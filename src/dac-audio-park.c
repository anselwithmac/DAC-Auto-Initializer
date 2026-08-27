// dac-audio-park: move the default output device out of the way, then put it
// back.
//
//   dac-audio-park park    --park "MacBook Pro Speakers"
//   dac-audio-park restore --match "FiiO K5 Pro" --prev-uid "<uid>"
//   dac-audio-park show
//
// Why this exists as a separate binary:
//
// A re-enumeration destroys and rebuilds the DAC's Core Audio object. When it
// lands within a second or two of the device arriving, the HAL reports the
// teardown and the rebuild as one atomic swap with no gap between them. No
// live output device exists at that instant, so every application rendering to
// the DAC is cut off mid-rebuild. Those applications then sit stalled until a
// user presses play, which is the symptom this whole project was built around.
//
// Parking the default output on a real device first gives them somewhere to
// keep rendering while the swap happens. Measured: with the park, an already
// stalled player resumes on its own in about 2.6 seconds. Without it, it never
// resumes.
//
// This runs unprivileged, and it must: the default output device belongs to
// the logged-in user's coreaudiod session. A launchd system daemon lives
// outside that session and cannot set it. dac-reinitd invokes this through
// "launchctl asuser".
//
// Progress goes to stderr. stdout carries machine-readable output only, so the
// daemon can read PREV_UID back.

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAXDEV 128

static int g_verbose = 1;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!g_verbose) return;
    fprintf(stderr, "  park: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

// ------------------------------------------------------------- properties --

static void get_str(AudioObjectID id, AudioObjectPropertySelector sel,
                    char *out, size_t n)
{
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 sz = (UInt32)sizeof s;

    out[0] = '\0';
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &s) == noErr && s) {
        CFStringGetCString(s, out, (CFIndex)n, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

static void dev_name(AudioObjectID id, char *out, size_t n)
{
    if (id == kAudioObjectUnknown) { snprintf(out, n, "<none>"); return; }
    get_str(id, kAudioObjectPropertyName, out, n);
    if (!out[0]) snprintf(out, n, "<id %u>", (unsigned)id);
}

static void dev_uid(AudioObjectID id, char *out, size_t n)
{
    out[0] = '\0';
    if (id != kAudioObjectUnknown)
        get_str(id, kAudioDevicePropertyDeviceUID, out, n);
}

// Output channel count. This is what separates a real output device from an
// input-only one, and from a device that is listed but not usable yet.
static int out_channels(AudioObjectID id)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration,
                                     kAudioObjectPropertyScopeOutput,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioBufferList *bl;
    int ch = 0;

    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) != noErr || !sz)
        return 0;
    bl = malloc(sz);
    if (!bl) return 0;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
            ch += (int)bl->mBuffers[i].mNumberChannels;
    free(bl);
    return ch;
}

static int device_list(AudioObjectID *v, int max)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    int n;

    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz) != noErr)
        return 0;
    n = (int)(sz / sizeof(AudioObjectID));
    if (n <= 0) return 0;
    if (n > max) n = max;
    sz = (UInt32)(n * (int)sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, v) != noErr)
        return 0;
    return (int)(sz / sizeof(AudioObjectID));
}

static AudioObjectID find_by_name(const char *needle)
{
    AudioObjectID v[MAXDEV];
    int n = device_list(v, MAXDEV);

    for (int i = 0; i < n; i++) {
        char nm[192];
        dev_name(v[i], nm, sizeof nm);
        // Case-insensitive substring: USB descriptors carry trailing spaces.
        if (strcasestr(nm, needle) && out_channels(v[i]) > 0) return v[i];
    }
    return kAudioObjectUnknown;
}

static AudioObjectID find_by_uid(const char *uid)
{
    AudioObjectID v[MAXDEV];
    int n;

    if (!uid || !uid[0]) return kAudioObjectUnknown;
    n = device_list(v, MAXDEV);
    for (int i = 0; i < n; i++) {
        char u[256];
        dev_uid(v[i], u, sizeof u);
        if (u[0] && !strcmp(u, uid) && out_channels(v[i]) > 0) return v[i];
    }
    return kAudioObjectUnknown;
}

// ---------------------------------------------------------------- default --

static AudioObjectID default_out(void)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectID id = kAudioObjectUnknown;
    UInt32 sz = (UInt32)sizeof id;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &id);
    return id;
}

// The HAL accepts the write and applies it asynchronously, so a successful
// set() proves nothing. Read it back, and retry if it did not take.
static int set_default_verified(AudioObjectID id)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress b = { kAudioHardwarePropertyDefaultSystemOutputDevice,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    char nm[192];

    dev_name(id, nm, sizeof nm);
    for (int attempt = 1; attempt <= 3; attempt++) {
        OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL,
                                                 (UInt32)sizeof id, &id);
        if (st != noErr) { logmsg("set default output failed: %d", (int)st); return -1; }
        // Alerts and system sounds follow this one. Best effort.
        AudioObjectSetPropertyData(kAudioObjectSystemObject, &b, 0, NULL,
                                   (UInt32)sizeof id, &id);
        usleep(200 * 1000);
        if (default_out() == id) {
            logmsg("default output is now \"%s\" (attempt %d)", nm, attempt);
            return 0;
        }
        logmsg("default output did not take, retrying (attempt %d)", attempt);
    }
    logmsg("gave up setting default output to \"%s\"", nm);
    return -1;
}

// ------------------------------------------------------------- subcommands --

static int cmd_show(void)
{
    AudioObjectID v[MAXDEV];
    int n = device_list(v, MAXDEV);
    AudioObjectID cur = default_out();
    char nm[192], uid[256];

    dev_name(cur, nm, sizeof nm);
    dev_uid(cur, uid, sizeof uid);
    printf("DEFAULT_NAME=%s\nDEFAULT_UID=%s\n", nm, uid);
    for (int i = 0; i < n; i++) {
        int ch = out_channels(v[i]);
        if (!ch) continue;
        dev_name(v[i], nm, sizeof nm);
        dev_uid(v[i], uid, sizeof uid);
        printf("DEVICE\tch=%d\tname=%s\tuid=%s\n", ch, nm, uid);
    }
    return 0;
}

// The built-in speakers carry a stable UID on every Mac, but their display
// name is model-specific ("MacBook Pro Speakers", "Mac mini Speakers", ...).
// Resolve the configured name first, then fall back to the UID, so a plist
// generated on one Mac still works on another.
#define BUILTIN_SPEAKER_UID "BuiltInSpeakerDevice"

static AudioObjectID find_park(const char *park_name)
{
    AudioObjectID id = kAudioObjectUnknown;

    if (park_name && park_name[0]) {
        id = find_by_name(park_name);
        if (id != kAudioObjectUnknown) return id;
        logmsg("no output device named \"%s\", trying the built-in speakers", park_name);
    }
    return find_by_uid(BUILTIN_SPEAKER_UID);
}

static int cmd_park(const char *park_name)
{
    AudioObjectID prev = default_out(), park;
    char prev_uid[256], nm[192];

    dev_uid(prev, prev_uid, sizeof prev_uid);
    dev_name(prev, nm, sizeof nm);
    logmsg("current default output is \"%s\"", nm);

    park = find_park(park_name);
    if (park == kAudioObjectUnknown) {
        logmsg("no park device found (tried \"%s\" and the built-in speakers)", park_name);
        printf("PREV_UID=%s\n", prev_uid);
        fflush(stdout);
        return 1;
    }

    // If the default output is already the park device, there is no meaningful
    // "previous device" to go back to. Report an empty PREV_UID so restore
    // targets the DAC instead.
    //
    // This is what stops a failed restore from cascading. Without it, the next
    // run records the park device as the previous device, restores to it, and
    // the DAC can never be selected again without the user stepping in.
    if (prev == park) {
        logmsg("already on the park device -- restore will target the DAC");
        printf("PREV_UID=\n");
        fflush(stdout);
        return 0;
    }

    // Report the starting point before changing anything, so the caller can put
    // it back even if the park itself fails.
    printf("PREV_UID=%s\n", prev_uid);
    fflush(stdout);

    dev_name(park, nm, sizeof nm);
    logmsg("parking on \"%s\"", nm);
    return set_default_verified(park) == 0 ? 0 : 1;
}

static int cmd_restore(const char *match, const char *prev_uid,
                       unsigned gone_ms, unsigned timeout_ms, unsigned settle_ms)
{
    unsigned waited = 0;
    AudioObjectID dac = kAudioObjectUnknown, target;
    char nm[192];

    // Phase 1: wait for the device to leave. The re-enumeration is
    // asynchronous, so it is normal to arrive here before the teardown. It is
    // also possible the device already returned, so never treat this as fatal.
    while (waited < gone_ms) {
        if (find_by_name(match) == kAudioObjectUnknown) {
            logmsg("device left the list after %u ms", waited);
            break;
        }
        usleep(100 * 1000);
        waited += 100;
    }

    // Phase 2: wait for it to come back with usable output channels.
    waited = 0;
    while (waited < timeout_ms) {
        dac = find_by_name(match);
        if (dac != kAudioObjectUnknown) break;
        usleep(100 * 1000);
        waited += 100;
    }
    if (dac == kAudioObjectUnknown) {
        logmsg("\"%s\" did not return within %u ms", match, timeout_ms);
        return 1;
    }
    dev_name(dac, nm, sizeof nm);
    logmsg("\"%s\" is back after %u ms", nm, waited);

    usleep(settle_ms * 1000);

    // Re-resolve the target after the settle, and again on every failure.
    //
    // The object found above can be dead by now. A KVM can churn the device a
    // second time while we wait, and the AudioObjectID does not survive that.
    // Setting the default to a dead object fails silently and repeatedly --
    // the symptom is a retry loop against a device whose name comes back
    // empty. Always look the target up fresh, immediately before using it.
    //
    // Match on the UID first: it survives the re-enumeration, and the numeric
    // AudioObjectID does not. An empty prev_uid means the caller had no
    // meaningful previous device, so the DAC is the target.
    for (int attempt = 1; attempt <= 3; attempt++) {
        target = find_by_uid(prev_uid);
        if (target == kAudioObjectUnknown) target = find_by_name(match);
        if (target == kAudioObjectUnknown) {
            logmsg("target is not in the device list (attempt %d)", attempt);
            usleep(700 * 1000);
            continue;
        }
        dev_name(target, nm, sizeof nm);
        if (default_out() == target) {
            logmsg("default output is already \"%s\"", nm);
            return 0;
        }
        if (set_default_verified(target) == 0) return 0;
        logmsg("retrying with a fresh device lookup (attempt %d)", attempt);
        usleep(400 * 1000);
    }
    logmsg("could not restore the default output");
    return 1;
}

// --------------------------------------------------------------------- main --

int main(int argc, char **argv)
{
    const char *cmd, *park_name = "MacBook Pro Speakers";
    const char *match = NULL, *prev_uid = "";
    unsigned gone_ms = 1000, timeout_ms = 20000, settle_ms = 500;

    if (argc < 2) {
        fprintf(stderr,
            "usage: %s park    --park <name>\n"
            "       %s restore --match <name> [--prev-uid <uid>]\n"
            "                  [--gone-ms N] [--timeout-ms N] [--settle-ms N]\n"
            "       %s show\n", argv[0], argv[0], argv[0]);
        return 2;
    }
    cmd = argv[1];

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--park") && i + 1 < argc)            park_name = argv[++i];
        else if (!strcmp(argv[i], "--match") && i + 1 < argc)      match = argv[++i];
        else if (!strcmp(argv[i], "--prev-uid") && i + 1 < argc)   prev_uid = argv[++i];
        else if (!strcmp(argv[i], "--gone-ms") && i + 1 < argc)    gone_ms = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) timeout_ms = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--settle-ms") && i + 1 < argc)  settle_ms = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet"))                      g_verbose = 0;
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }

    if (!strcmp(cmd, "show"))    return cmd_show();
    if (!strcmp(cmd, "park"))    return cmd_park(park_name);
    if (!strcmp(cmd, "restore")) {
        if (!match) { fprintf(stderr, "restore needs --match\n"); return 2; }
        return cmd_restore(match, prev_uid, gone_ms, timeout_ms, settle_ms);
    }
    fprintf(stderr, "unknown command: %s\n", cmd);
    return 2;
}
