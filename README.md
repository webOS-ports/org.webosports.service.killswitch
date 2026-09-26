# org.webosports.service.killswitch

Reports the hardware privacy switches a device has on the luna bus, so the
shell can show them, and runs whatever action a switch needs that the platform
does not already perform itself.

## Bus API

    luna://org.webosports.service.killswitch/getStatus   {"subscribe": true}

replies, and on every change pushes:

```json
{
  "returnValue": true,
  "switches": [
    { "id": "camera",     "state": "blocked", "label": "Camera",     "readable": true  },
    { "id": "cellular",   "state": "open",    "label": "Cellular",   "readable": true  },
    { "id": "microphone", "state": "unknown", "label": "Microphone", "readable": false }
  ]
}
```

`state` is `blocked` (engaged, hardware off), `open` (disengaged) or `unknown`.
`readable` distinguishes "cannot ever be read" from "not read yet", which are
different things to show a user.

**`unknown` is not a failure and must not be rendered as either of the other
two.** Some of these switches physically cut the line with no presence
detection at all. On the FLX1s the microphone switch drops the analogue level
from -46.1 dBFS to -86.1 dBFS - a 40 dB fall to the ADC noise floor - and
appears in no sysfs attribute, no input device and no property. Finding out
would mean opening the microphone, which is the one thing the switch exists to
prevent.

## Configuration

One group per switch in `/etc/killswitchd.conf`; a machine declares only what
it has. With no config the daemon exits immediately, so it is harmless
everywhere else.

```ini
[camera]
label=Camera
source=sysfs                                            ; sysfs | presence | unreadable
path=/sys/devices/platform/custom-keys/cam_switch
blocked-value=0
on-blocked=/usr/libexec/killswitchd/camera-blocked      ; optional
on-open=/usr/libexec/killswitchd/camera-open            ; optional
```

- `sysfs` - read `path`, engaged when the contents equal `blocked-value`.
- `presence` - engaged when `path` has gone away. For switches that cut power,
  where the absence of the device is the only reading available.
- `unreadable` - there is a switch and its position cannot be read.

An unreadable or unparsable entry is still reported, as `unknown`. A switch the
daemon cannot interpret is still a switch the device has.

Ready-made configs are in `conf/`. The PinePhone one is **unverified** - its
paths have not been checked on hardware.

## Why it polls

The FLX1s driver behind these switches never calls `sysfs_notify()`, so its
attributes cannot be `poll()`ed, and its input device is held under an
exclusive `EVIOCGRAB` by a MediaTek vendor HAL - an attempt to grab it returns
`EBUSY` - so evdev delivers nothing to any other reader. Presence-based
switches have nothing to wait on either. A one second poll costs almost nothing
and behaves the same on every backend.

## Detection and action are separate problems

On the FLX1s the vendor's `nvram@1.1-service` already watches the switches and
sets `persist.vendor.camera.disabled` / `persist.vendor.radio.disabled`. For
cellular that is enough: `mtkrild.rc` has init triggers on that property and
stops and restarts `vendor.ril-daemon-mtk`, ofono sees the binder service die
and re-attaches when it returns - verified end to end, so the config sets no
action.

For the camera there is **no** such trigger anywhere in the vendor image, so
the property flips and `camerahalserver` keeps running. Without the helper in
`on-blocked` the camera switch is inert on LuneOS. Which of those a machine
needs belongs in its config, not in this daemon.
