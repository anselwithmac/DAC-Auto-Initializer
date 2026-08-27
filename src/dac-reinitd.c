// dac-reinitd -- event-driven USB DAC re-initialiser.
//
// Registers an IOKit matching notification so the kernel tells us the instant
// the DAC appears on the bus (no polling, no latency), then forces a USB
// re-enumeration to clear the stalled isochronous audio stream.
//
//   dac-reinitd --match "FiiO K5 Pro" [--settle-ms 400] [--cooldown-ms 3000]
//               [--park "MacBook Pro Speakers"] [--no-park]
//
// Three steps per arrival, in this order:
//
//   1. Park the default output on a live device.
//   2. Re-enumerate the DAC.
//   3. Restore the default output.
//
// Step 2 alone repairs the device but leaves every playing application stalled
// until the user presses play. Steps 1 and 3 are what stop that: they keep a
// live output device underneath those applications while the DAC's Core Audio
// object is destroyed and rebuilt. See src/dac-audio-park.c for the full
// reasoning and the measurements.
//
// Must run as root: seizing the device from the audio driver requires it. The
// park and restore cannot run here, because the default output device belongs
// to the logged-in user's coreaudiod session and a launchd system daemon lives
// outside it. Those two steps run through "launchctl asuser" instead.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER_PATH "/usr/local/sbin/dac-audio-park"

static const char *g_needle      = NULL;
static const char *g_park        = "MacBook Pro Speakers";
static const char *g_helper      = HELPER_PATH;
static unsigned    g_settle_ms   = 400;
static unsigned    g_cooldown_ms = 3000;
static unsigned    g_attempts    = 4;
static unsigned    g_restore_timeout_ms = 20000;
static double      g_suppress_until = 0.0;
static int         g_armed       = 0;
static int         g_do_park     = 1;

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


// ------------------------------------------------------- user session ------

// The uid that owns the console, i.e. whoever is logged in and looking at the
// screen. /dev/console is owned by that user, which avoids pulling in
// SystemConfiguration for one lookup.
static int console_user(uid_t *uid, char *name, size_t n) {
    struct stat st;
    struct passwd *pw;

    if (stat("/dev/console", &st) != 0) return -1;
    if (st.st_uid == 0) return -1;                 // nobody logged in
    pw = getpwuid(st.st_uid);
    if (!pw) return -1;
    if (!strcmp(pw->pw_name, "_mbsetupuser")) return -1;   // setup assistant
    *uid = st.st_uid;
    snprintf(name, n, "%s", pw->pw_name);
    return 0;
}

// Run the helper inside the logged-in user's audio session. "launchctl asuser"
// moves the process into that user's bootstrap namespace, which is what lets it
// see the same coreaudiod; "sudo -u" then drops root. Neither prompts, because
// we are already root.
//
// out, when given, receives the helper's stdout. Its stderr is left alone so
// its progress lands in our log.
static int run_asuser(uid_t uid, const char *user, const char *const *args,
                      char *out, size_t outsz) {
    char uidbuf[32];
    const char *argv[24];
    int n = 0, fd[2] = {-1, -1}, status = 0;
    pid_t pid;

    snprintf(uidbuf, sizeof(uidbuf), "%u", (unsigned)uid);
    argv[n++] = "/bin/launchctl";
    argv[n++] = "asuser";
    argv[n++] = uidbuf;
    argv[n++] = "/usr/bin/sudo";
    argv[n++] = "-u";
    argv[n++] = user;
    for (int i = 0; args[i] && n < 23; i++) argv[n++] = args[i];
    argv[n] = NULL;

    if (out) {
        out[0] = '\0';
        if (pipe(fd) != 0) return -1;
    }

    pid = fork();
    if (pid < 0) {
        if (out) { close(fd[0]); close(fd[1]); }
        return -1;
    }
    if (pid == 0) {
        if (out) { dup2(fd[1], STDOUT_FILENO); close(fd[0]); close(fd[1]); }
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (out) {
        size_t used = 0;
        ssize_t r;
        close(fd[1]);
        while (used + 1 < outsz && (r = read(fd[0], out + used, outsz - used - 1)) > 0)
            used += (size_t)r;
        out[used] = '\0';
        close(fd[0]);
    }
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

// Pull "PREV_UID=..." out of the helper's stdout.
static void parse_prev_uid(const char *buf, char *out, size_t n) {
    const char *p = strstr(buf, "PREV_UID=");
    size_t i = 0;

    out[0] = '\0';
    if (!p) return;
    p += strlen("PREV_UID=");
    while (*p && *p != '\n' && *p != '\r' && i + 1 < n) out[i++] = *p++;
    out[i] = '\0';
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

    // Step 1: park the default output, so applications have a live device to
    // keep rendering into while the re-enumeration destroys the DAC's audio
    // object. Without this they stall and need a manual play press.
    uid_t uid = 0;
    char user[128] = {0}, prev_uid[256] = {0}, obuf[512] = {0};
    int parked = 0;

    if (g_do_park) {
        if (console_user(&uid, user, sizeof(user)) != 0) {
            logmsg("  no console user -- skipping park/restore");
        } else {
            const char *args[] = { g_helper, "park", "--park", g_park, NULL };
            int rc = run_asuser(uid, user, args, obuf, sizeof(obuf));
            parse_prev_uid(obuf, prev_uid, sizeof(prev_uid));
            if (rc == 0) {
                parked = 1;
                logmsg("  parked on \"%s\" (was \"%s\")", g_park,
                       prev_uid[0] ? prev_uid : "the park device -- will restore to the DAC");
            } else {
                // Not fatal. A re-enumeration without the park is what the
                // daemon did for its whole life before this, and it still
                // repairs the device.
                logmsg("  park failed -- re-enumerating anyway");
            }
        }
    }

    // Step 2: the repair itself.
    int ok = -1;
    for (unsigned a = 1; a <= g_attempts; a++) {
        ok = reenumerate_once();
        logmsg("re-enumerate attempt %u: %s", a, ok == 0 ? "OK" : "failed");
        if (ok == 0) break;
        usleep(500 * 1000);
    }

    // Step 3: put the default output back. The helper waits for the device to
    // leave and return before it touches anything, because restoring to the
    // object that is about to be destroyed silently does nothing.
    if (parked) {
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%u", g_restore_timeout_ms);
        const char *args[] = { g_helper, "restore", "--match", g_needle,
                               "--prev-uid", prev_uid,
                               "--timeout-ms", tbuf, NULL };
        if (run_asuser(uid, user, args, NULL, 0) == 0)
            logmsg("  restored the default output");
        else
            logmsg("  restore FAILED -- default output is left on \"%s\"", g_park);
    }

    // Start the cooldown only once the work above has finished. The
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
        else if (!strcmp(argv[i], "--park") && i + 1 < argc)         g_park = argv[++i];
        else if (!strcmp(argv[i], "--helper") && i + 1 < argc)       g_helper = argv[++i];
        else if (!strcmp(argv[i], "--restore-timeout-ms") && i + 1 < argc)
                                                                    g_restore_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-park"))                      g_do_park = 0;
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (!g_needle) {
        fprintf(stderr, "usage: %s --match <USB Product Name substring>\n"
                        "         [--settle-ms N] [--cooldown-ms N] [--attempts N]\n"
                        "         [--park <output device name>] [--no-park]\n"
                        "         [--restore-timeout-ms N] [--helper <path>]\n", argv[0]);
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

    if (g_do_park && access(g_helper, X_OK) != 0) {
        logmsg("WARNING: helper \"%s\" is missing -- park/restore is disabled", g_helper);
        g_do_park = 0;
    }

    if (g_do_park)
        logmsg("watching for \"%s\" (settle %ums, cooldown %ums, park \"%s\")",
               g_needle, g_settle_ms, g_cooldown_ms, g_park);
    else
        logmsg("watching for \"%s\" (settle %ums, cooldown %ums, no park)",
               g_needle, g_settle_ms, g_cooldown_ms);

    CFRunLoopRun();
    return 0;
}
