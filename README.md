# USB-DAC-Auto-Reinitializer

This is a macOS launch daemon that repairs a USB DAC/AMP or other audio device which fails to reinitialize after
a hub switch, KVM switch or a stalled device after being plugged in.

## What this App Script fixes

If you were to unplug a USB DAC and then replug it back in to a Mac computer,
the expected result is the DAC resumed its connection, and playback resumes.

This application fixes an issue where *some* USB DAC amps fail to reinitialize, causing audio applications to stall or break in unusual ways.

It also addresses a USB DAC that stops playing audio after a KVM or USB hub switch flips back to your Mac,
so you don't have to physically unplug and replug it every time.

## In A Nutshell

`dac-reinit` performs a virtual re-plug. The daemon awaits for your specific DAC to
appear on the USB bus. When the even occurs, it fires off a USB re-enumeration.

macOS treats it as a removal and then connection, and selects the device as the default output again.

This should allow you to dock and undock your mac throughout the day, or use USB switches 
to control multiple computers with the same Keyboard, Mouse and headphones.

## Requirements

- macOS (admin rights)

- The Xcode command line tools, for `clang` ::
```bash
xcode-select --install
```

# How to Install

### Agentically:

Point your agent to this repo, ask it to judge it's safey, then pull down the files and provide the user with the install command.

This repo's documentation should be sufficient for the agent to troubleshoot issues as well.

### Manually:

If you don't have XCode select and haven't run the xcode-select from above, do that first.

*Make sure your device is already plugged in before you proceed!*

Run the installer with no options. It lists the connected USB audio devices and
asks you to pick one:

```bash
./install.sh
```

To select the device yourself and skip all prompts, use `--match` and `-y`:

```bash
./install.sh --match "FiiO K5 Pro" -y
```

The installer compiles the two binaries, generates the launchd plist, and
installs everything. The daemon starts immediately and starts again on every
boot.

### Installer options

| Option | Default | Description |
| --- | --- | --- |
| `--match <name>` | (interactive prompt) | Manually pick your USB device by name. The match is case-insensitive. |
| `--settle-ms <N>` | `400` (time in miliseconds)| The delay after the device appears, before the re-enumeration. |
| `--cooldown-ms <N>` | `3000` (time in miliseconds)| The period after a re-enumeration in which the daemon ignores new arrivals. |
| `-y`, `--yes` | off | Do not prompt. With more than one USB audio device, you must also pass `--match`. |

## Visually verify

You can optionally run this in terminal to watch your USB connect and disconnect events in team time.

Good for checking and debugging.

```bash
sudo tail -f /var/log/dac-reinit.log
```

Example log output:

```
2026-08-27 12:44:01 watching for "FiiO K5 Pro" (settle 400ms, cooldown 3000ms)
2026-08-27 12:45:18 DAC appeared -- settling 400ms
2026-08-27 12:45:19 re-enumerate attempt 1: OK
2026-08-27 12:45:19 done (cooldown 3000ms from now)
```

## Uninstall

```bash
./uninstall.sh
```

The log file stays in place. To remove the log too, use `--purge`:

```bash
./uninstall.sh --purge
```

## What and where gets installed to your Mac

| Path | Description |
| --- | --- |
| `/usr/local/sbin/dac-reinitd` | The daemon. |
| `/usr/local/sbin/usb-reenumerate` | The manual command line tool. |
| `/Library/LaunchDaemons/local.dacreinit.plist` | The launchd job. |
| `/var/log/dac-reinit.log` | The log. It holds stdout and stderr. |

## Reload after a configuration change

If you edit the installed plist, restart the daemon:

```bash
sudo launchctl bootout system/local.dacreinit && sudo launchctl bootstrap system /Library/LaunchDaemons/local.dacreinit.plist
```

The simpler alternative is to run `./install.sh` again with the new values.

## Troubleshooting

**No sound, and the log is empty.** The daemon did not see the device. Confirm
that the name matches.

```bash
ioreg -w0 -l -r -c IOUSBHostDevice | grep '"USB Product Name"'
```

Then install again and pick your device.

**The log shows `re-enumerate attempt 1: failed` with a `0x...` code.** The
seize failed. This is almost always a permission problem. Confirm that the
daemon runs as root. If you ran `usb-reenumerate` by hand, add `sudo`.

**The log shows `DAC already present at startup -- not acting`.** This is
correct. The daemon does not repair a device that was connected before it
started.

**Sound returns, but only after a long delay.** Increase the settle time. Some
devices need more time on the bus before they accept a re-enumeration:

```bash
./install.sh --match "FiiO K5 Pro" --settle-ms 1000 -y
```

**The daemon fires more than once per switch.** Increase the cooldown with
`--cooldown-ms`.

## Diagnostics

`tools/dac-snapshot.sh` captures the USB tree, the USB audio interfaces, and
the Core Audio devices. Take one snapshot in the broken state and one in the
working state, then compare them:

```bash
./tools/dac-snapshot.sh broken
```

```bash
./tools/dac-snapshot.sh working
```

```bash
diff ~/Desktop/dac-broken.txt ~/Desktop/dac-working.txt
```

## Repository layout

| Path | Description |
| --- | --- |
| `src/dac-reinitd.c` | The daemon. It registers an IOKit match notification and re-enumerates on a hit. |
| `src/usb-reenumerate.c` | The standalone command line tool. |
| `templates/local.dacreinit.plist.in` | The launchd plist template. |
| `tools/dac-snapshot.sh` | The diagnostic dump. |
| `install.sh` | The build system and the installer. |
| `uninstall.sh` | The remover. |
| `build/` | Generated output. Do not edit it by hand. |

## TL;DR for how it works

1. The daemon registers a new `kIOFirstMatchNotification` for `IOUSBHostDevice`.
2. On startup it drains the devices that are already present, and ignores them.
3. When a new USB device connects, the daemon compares the `USB Product Name` property against the match string.
4. On a hit, the daemon waits for the settle period.
5. The daemon calls `USBDeviceOpenSeize` and `USBDeviceReEnumerate`. It makes up to four attempts, 500ms apart.
6. The cooldown starts after the attempts finish. The re-enumeration makes the device drop and return, which looks like a fresh connection. The cooldown
   stops the daemon from triggering on its own work.