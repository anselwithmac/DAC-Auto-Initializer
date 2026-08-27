// usb-reenumerate: force a USB device to re-enumerate, i.e. the software
// equivalent of unplugging and replugging it.
//
//   sudo usb-reenumerate <substring of USB Product Name>
//
// Requires root: it seizes the device away from whatever driver holds it
// (for a DAC, that's the USB audio driver) and asks the USB stack to
// re-enumerate the port.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int name_matches(io_service_t dev, const char *needle) {
    CFStringRef cfname = IORegistryEntryCreateCFProperty(
        dev, CFSTR("USB Product Name"), kCFAllocatorDefault, 0);
    if (!cfname) return 0;
    char buf[512] = {0};
    Boolean ok = CFStringGetCString(cfname, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cfname);
    if (!ok) return 0;
    return strcasestr(buf, needle) != NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <substring of USB Product Name>\n", argv[0]);
        return 2;
    }
    const char *needle = argv[1];

    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) { fprintf(stderr, "IOServiceMatching failed\n"); return 1; }

    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices failed\n");
        return 1;
    }

    io_service_t dev;
    int found = 0, rc = 1;
    while ((dev = IOIteratorNext(iter))) {
        if (!name_matches(dev, needle)) { IOObjectRelease(dev); continue; }
        found = 1;

        IOCFPlugInInterface **plugin = NULL;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            dev, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID,
            &plugin, &score);
        if (kr != KERN_SUCCESS || !plugin) {
            fprintf(stderr, "IOCreatePlugInInterfaceForService failed: 0x%x\n", kr);
            IOObjectRelease(dev);
            continue;
        }

        IOUSBDeviceInterface **usb = NULL;
        HRESULT hr = (*plugin)->QueryInterface(
            plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&usb);
        (*plugin)->Release(plugin);
        if (hr || !usb) {
            fprintf(stderr, "QueryInterface failed\n");
            IOObjectRelease(dev);
            continue;
        }

        // Seize: the audio driver already has the device open exclusively.
        kr = (*usb)->USBDeviceOpenSeize(usb);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "USBDeviceOpenSeize failed: 0x%x (are you root?)\n", kr);
            (*usb)->Release(usb);
            IOObjectRelease(dev);
            continue;
        }

        kr = (*usb)->USBDeviceReEnumerate(usb, 0);
        if (kr == KERN_SUCCESS) {
            printf("re-enumerated\n");
            rc = 0;
        } else {
            fprintf(stderr, "USBDeviceReEnumerate failed: 0x%x\n", kr);
        }

        (*usb)->USBDeviceClose(usb);
        (*usb)->Release(usb);
        IOObjectRelease(dev);
    }
    IOObjectRelease(iter);

    if (!found) { fprintf(stderr, "no USB device matching \"%s\"\n", needle); return 3; }
    return rc;
}
