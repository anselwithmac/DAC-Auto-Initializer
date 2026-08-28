# USB-DAC-Auto-Reinitializer

This is a macOS launch daemon that repairs a USB DAC/AMP or other audio device which fails to reinitialize after
a hub switch, KVM switch or a stalled device after being plugged in.

## What this App Script fixes

If you were to unplug a USB DAC and then replug it back in to a Mac computer,
the expected result is the DAC resumed its connection, and playback resumes.

This application fixes an issue where *some* USB DAC amps fail to reinitialize, causing audio applications to stall or break in unusual ways.

It also addresses a USB DAC that stops playing audio, or audio becomes glitchy after a KVM or USB hub switch flips back to your Mac,
so you don't have to physically unplug and replug it every time.

## In A Nutshell

`dac-reinit` performs a virtual re-plug. The daemon waits for your DAC to appear
on the USB bus. When that happens, it does three things:

1. The app moved the output channel to your mac's preferred fallback.
2. It re-enumerates the DAC over USB, which is the software equivalent of a replug.
3. Then it puts everything back to how it was. In most cases, this puts it back on your DAC automatically.

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

The installer compiles the three binaries, generates the launchd plist, and
installs everything. The daemon starts immediately and starts again on every
boot.

### Installer options

| Option | Default | Description |
| --- | --- | --- |
| `--match <name>` | (interactive prompt) | Manually pick your USB device by name. The match is case-insensitive. |
| `--settle-ms <N>` | `400` (time in miliseconds)| The delay after the device appears, before the re-enumeration. |
| `--cooldown-ms <N>` | `3000` (time in miliseconds)| The period after a re-enumeration in which the daemon ignores new arrivals. |
| `--park <name>` | your built-in speakers | The output device to hold audio on during the repair. The installer detects this for you. |
| `--restore-timeout-ms <N>` | `20000` (time in miliseconds)| How long to wait for the DAC to come back before giving up on the restore. |
| `-y`, `--yes` | off | Do not prompt. With more than one USB audio device, you must also pass `--match`. |

## Visually verify

You can optionally run this in terminal to watch your USB connect and disconnect events in team time.

Good for checking and debugging.

```bash
sudo tail -f /var/log/dac-reinit.log
```

Example log output:

```
2026-08-27 12:44:01 watching for "FiiO K5 Pro" (settle 400ms, cooldown 3000ms, park "MacBook Pro Speakers")
2026-08-27 12:45:18 DAC appeared -- settling 400ms
2026-08-27 12:45:19   parked on "MacBook Pro Speakers" (was AppleUSBAudioEngine:...:FiiO K5 Pro:143400:1)
2026-08-27 12:45:19 re-enumerate attempt 1: OK
2026-08-27 12:45:21   restored the default output
2026-08-27 12:45:21 done (cooldown 3000ms from now)
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
| `/usr/local/sbin/dac-audio-park` | Moves the sound output and puts it back. Runs as you, not as root. |
| `/Library/LaunchDaemons/local.dacreinit.plist` | The launchd job. |
| `/var/log/dac-reinit.log` | The log. It holds stdout and stderr. |

## Reload after a configuration change

If you edit the installed plist, restart the daemon:

```bash
sudo launchctl bootout system/local.dacreinit && sudo launchctl bootstrap system /Library/LaunchDaemons/local.dacreinit.plist
```

The simpler alternative is to run `./install.sh` again with the new values.

## Troubleshooting

Start with the log. It answers most questions on its own:

```bash
sudo tail -40 /var/log/dac-reinit.log
```

A healthy repair looks like this, and takes about one and a half seconds:

```
DAC appeared -- settling 400ms
  parked on "MacBook Pro Speakers" (was "AppleUSBAudioEngine:...:FiiO K5 Pro:143400:1")
re-enumerate attempt 1: OK
  restored the default output
done (cooldown 3000ms from now)
```

To see the sound devices the way the helper sees them, with their UIDs:

```bash
/usr/local/sbin/dac-audio-park show
```

---

**Audio comes back, but the app stays stuck until you press play.**

The park did not happen. Look for the `parked on` line. If it is missing, look
just above it for `park failed` or `no console user`. The park is the step that
keeps applications alive, so without it they stall.

**The log shows `restore FAILED` and the sound output is stuck on the speakers.**

This is a safe failure, not a broken install. The sound output is left on the
speakers on purpose, so that the problem is audible instead of silent.

The next switch repairs it by itself. To fix it immediately instead, switch the
output back by hand, or run:

```bash
sudo /usr/local/sbin/usb-reenumerate "FiiO K5 Pro"
```

If it happens on every switch, your DAC takes longer than the timeout to return.
Raise it:

```bash
./install.sh --match "FiiO K5 Pro" --restore-timeout-ms 40000 -y
```

**I flipped the switch several times quickly and ended up on the speakers.**

Expected. A restore fails if the device leaves again while it is running,
because the device really is gone. The next switch puts the DAC back with no
action from you.

**The log shows `no console user -- skipping park/restore`.**

Nobody is logged in, so there is no sound session to change. The daemon still
repairs the device. The sound output moves when you log in.

**The whole thing takes several seconds.**

Most of that is not the daemon. The daemon needs about one and a half seconds.
The rest is your switch re-presenting the device on the USB bus, which took 7 to
57 seconds in testing. Compare the `DAC appeared` timestamp with the moment you
pressed the switch to see the split.

To shave the daemon's share, lower the settle delay. It runs before the park:

```bash
./install.sh --match "FiiO K5 Pro" --settle-ms 200 -y
```

Do not raise `--settle-ms` to make things faster. It adds delay.

**No sound, and the log is empty.**

The daemon did not see the device. Confirm the name:

```bash
ioreg -w0 -l -r -c IOUSBHostDevice | grep '"USB Product Name"'
```

Then install again and pick your device.

**The log shows `re-enumerate attempt 1: failed` with a `0x...` code.**

The seize failed, which is almost always a permission problem. Confirm that the
daemon runs as root. If you ran `usb-reenumerate` by hand, add `sudo`.

**The log shows `DAC already present at startup -- not acting`.**

This is correct. The daemon does not repair a device that was connected before
the daemon started.

**The daemon fires more than once per switch.**

Raise the cooldown with `--cooldown-ms`.

**The audio is still glitchy after a successful repair.**

The re-enumeration did not clear it. Run it again by hand and listen:

```bash
sudo /usr/local/sbin/usb-reenumerate "FiiO K5 Pro"
```

If that clears it and the daemon does not, the daemon is acting too early.
Raise the settle delay so the device is steady before it is seized:

```bash
./install.sh --match "FiiO K5 Pro" --settle-ms 1000 -y
```

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
| `src/dac-audio-park.c` | The sound output half. It parks the default output and restores it. |
| `src/usb-reenumerate.c` | The standalone command line tool. It takes a positional argument. |
| `templates/local.dacreinit.plist.in` | The launchd plist template. |
| `tools/dac-snapshot.sh` | The diagnostic dump. |
| `install.sh` | The build system and the installer. |
| `uninstall.sh` | The remover. |
| `build/` | Generated output. Do not edit it by hand. |

## How to reproduce the problem it fixes

The fault needs the other computer to be **powered on** and to have used the DAC.
A switch while that computer is off does not reproduce it.

1. Play audio on the Mac.
2. Press the switch to the other computer.
3. Play audio on that computer.
4. Press the switch back to the Mac.

The DAC then comes back glitchy and stuttering, and your player stays stuck.
macOS reports the device as perfectly healthy the whole time, which is why
nothing fixes it on its own.

## TL;DR for how it works

1. The daemon registers a new `kIOFirstMatchNotification` for `IOUSBHostDevice`.
2. On startup it drains the devices that are already present, and ignores them.
3. When a new USB device connects, the daemon compares the `USB Product Name` property against the match string.
4. On a hit, the daemon waits for the settle period.
5. The daemon moves the default output to the park device, through `launchctl asuser` so the change lands in your login session.
6. The daemon calls `USBDeviceOpenSeize` and `USBDeviceReEnumerate`. It makes up to four attempts, 500ms apart.
7. The daemon waits for the DAC to leave and return, then moves the default output back. It matches the device by UID, because the numeric id changes across a re-enumeration.
8. The cooldown starts after the restore finishes. The re-enumeration makes the device drop and return, which looks like a fresh connection. The cooldown
   stops the daemon from triggering on its own work.