// dac-reinitd -- event-driven USB DAC re-initialiser.
//
// Registers an IOKit matching notification so the kernel tells us the instant
// the DAC appears on the bus (no polling, no latency), then forces a USB
// re-enumeration to clear the stalled isochronous audio stream.
//
//   dac-reinitd --match "FiiO K5 Pro" [--settle-ms 750] [--cooldown-ms 10000]
//
// The re-enumeration makes the device drop off the bus and return. macOS treats
// that as a removal followed by an arrival, and re-selects the device as the
// default output on its own, which is enough to get applications back onto it.
//
// Must run as root: seizing the device from the audio driver requires it.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static const char *g_needle      = NULL;
static unsigned    g_settle_ms   = 750;
static unsigned    g_cooldown_ms = 3000;
static unsigned    g_attempts    = 4;
static double      g_suppress_until = 0.0;
static int         g_armed       = 0;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void logmsg(const char *fmt, ...) {
    char stamp[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stdout, "%s ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int name_matches(io_service_t dev, const char *needle) {
    CFStringRef cfname = IORegistryEntryCreateCFProperty(
        dev, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
    if (!cfname) return 0;
    char buf[512] = {0};
    Boolean ok = CFStringGetCString(cfname, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cfname);
    return ok && strcasestr(buf, needle) != NULL;
}

// Seize the device from the audio driver and ask the USB stack to
// re-enumerate the port. This is what physically replugging does.
static int reenumerate_once(void) {
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return -1;

    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
        return -1;

    io_service_t dev;
    int rc = -1;
    while ((dev = IOIteratorNext(iter))) {
        if (!name_matches(dev, g_needle)) { IOObjectRelease(dev); continue; }

        IOCFPlugInInterface **plugin = NULL;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            dev, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) { IOObjectRelease(dev); continue; }

        IOUSBDeviceInterface **usb = NULL;
        HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&usb);
        (*plugin)->Release(plugin);
        if (hr || !usb) { IOObjectRelease(dev); continue; }

        kr = (*usb)->USBDeviceOpenSeize(usb);
        if (kr != KERN_SUCCESS) {
            logmsg("  USBDeviceOpenSeize failed: 0x%x", kr);
            (*usb)->Release(usb);
            IOObjectRelease(dev);
            continue;
        }

        kr = (*usb)->USBDeviceReEnumerate(usb, 0);
        if (kr == KERN_SUCCESS) rc = 0;
        else logmsg("  USBDeviceReEnumerate failed: 0x%x", kr);

        (*usb)->USBDeviceClose(usb);
        (*usb)->Release(usb);
        IOObjectRelease(dev);
        break;
    }
    IOObjectRelease(iter);
    return rc;
}

static void device_appeared(void *refcon, io_iterator_t iter) {
    (void)refcon;
    int hit = 0;
    io_service_t dev;
    // Always drain, or the notification will not re-arm.
    while ((dev = IOIteratorNext(iter))) {
        if (name_matches(dev, g_needle)) hit = 1;
        IOObjectRelease(dev);
    }
    if (!hit) return;

    if (!g_armed) {                       // initial drain at startup
        logmsg("DAC already present at startup -- not acting");
        return;
    }
    if (now_s() < g_suppress_until) {     // our own re-enumeration coming back
        logmsg("DAC appeared (cooldown -- ignoring)");
        return;
    }

    logmsg("DAC appeared -- settling %ums", g_settle_ms);
    usleep(g_settle_ms * 1000);

    int ok = -1;
    for (unsigned a = 1; a <= g_attempts; a++) {
        ok = reenumerate_once();
        logmsg("re-enumerate attempt %u: %s", a, ok == 0 ? "OK" : "failed");
        if (ok == 0) break;
        usleep(500 * 1000);
    }

    // Start the cooldown only once the attempts above have finished. The
    // re-enumeration makes the device drop and reappear, which is
    // indistinguishable from a fresh connect, so timing the cooldown any
    // earlier would let the daemon retrigger on its own handiwork.
    g_suppress_until = now_s() + g_cooldown_ms / 1000.0;
    logmsg("done (cooldown %ums from now)", g_cooldown_ms);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--match") && i + 1 < argc)            g_needle = argv[++i];
        else if (!strcmp(argv[i], "--settle-ms") && i + 1 < argc)   g_settle_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cooldown-ms") && i + 1 < argc) g_cooldown_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--attempts") && i + 1 < argc)    g_attempts = atoi(argv[++i]);
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!g_needle) {
        fprintf(stderr, "usage: %s --match <USB Product Name substring> "
                        "[--settle-ms N] [--cooldown-ms N] [--attempts N]\n", argv[0]);
        return 2;
    }

    IONotificationPortRef notify = IONotificationPortCreate(kIOMainPortDefault);
    if (!notify) { fprintf(stderr, "IONotificationPortCreate failed\n"); return 1; }
    CFRunLoopAddSource(CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(notify),
                       kCFRunLoopDefaultMode);

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceAddMatchingNotification(
        notify, kIOFirstMatchNotification, IOServiceMatching(kIOUSBDeviceClassName),
        device_appeared, NULL, &iter);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceAddMatchingNotification failed: 0x%x\n", kr);
        return 1;
    }

    device_appeared(NULL, iter);   // drain pre-existing devices, arms the notification
    g_armed = 1;

    logmsg("watching for \"%s\" (settle %ums, cooldown %ums)",
           g_needle, g_settle_ms, g_cooldown_ms);

    CFRunLoopRun();
    return 0;
}
