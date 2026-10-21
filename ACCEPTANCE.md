# Commissioning and acceptance

## Host preparation

Install native build dependencies and rebuild the module and its standalone IOC:

```sh
sudo dnf install -y ffmpeg-free-devel libcurl-devel json-devel
cd /home/mitr_4dh4/Documents/GitHub/ADReolink
make clean install
```

Copy `iocBoot/reolink.env.example` to
`/home/mitr_4dh4/.config/epics/reolink.env`, populate it, and run
`chmod 600` on the result. The camera service launcher sources this file; the
main `4dh4ioc` launcher does not.

The `bluesky-server` environment already contains PyDM, p4p, and Pillow. Any
alternate GUI environment must provide all three.

## Camera preparation

Use the camera administration interface during an approved maintenance window:

1. Enable RTSP on `192.168.138.211`.
2. Keep both main streams at 3840 by 2160 using H.265.
3. Set both main streams to 10 frames per second.
4. Confirm the RTSP main stream is available on TCP port 554.

Do not expose camera encoding settings as operator PVs. They are commissioning
settings, not normal controls.

## IOC checks

Start with `REOLINK_AUTOSTART=0`, restart `reolinkioc.service`, and confirm both control
connections and capability summaries. Camera 1 should report PTZ but no
optical zoom/focus or variable speed. Camera 2 should report its PTZ, speed,
zoom, focus, and autofocus capabilities.

Start each stream individually, then together:

```text
caput 4dh4:Reolink1:Acquire 1
caput 4dh4:Reolink2:Acquire 1
```

Verify:

- `ArraySizeX_RBV=3840`, `ArraySizeY_RBV=2160`, and `ColorMode_RBV=RGB1`.
- Each `PublishedFPS_RBV` remains at least 3.5 at the 0.25-second period.
- Both PVA channels display through PyDM:
  - `pva://4dh4:Reolink1:Pva1:Image`
  - `pva://4dh4:Reolink2:Pva1:Image`
- Disconnecting one camera increments only its reconnect count and does not
  interrupt the other.
- Press/release PTZ operation sends Stop, and the deadman also stops movement
  after two seconds.
- Out-of-range and unsupported control writes alarm.
- `JPEG1:WriteFile` saves a 3840 by 2160 image after setting the standard file
  path/name records.

Independently restart each IOC and verify the other heartbeat continues. Run
both streams for 30 minutes while observing IOC CPU, resident memory,
frame age, and plugin queue counters. If successful, set
`REOLINK_AUTOSTART=1`, switch the GUI to the PVA channels, disable the ADURL
camera acquisition, remove credential-bearing URL autosave entries, and rotate
the camera passwords.
