# ADReolink

Native EPICS areaDetector driver for Reolink Ethernet cameras. It decodes the
camera's RTSP main stream with FFmpeg, publishes RGB1 NDArrays, and controls
PTZ/zoom/focus through the authenticated Reolink CGI API.

The driver is instance based. Multiple cameras run in one IOC without sharing
decoder, HTTP-session, reconnect, or command-queue state.

This repository also builds the production `reolinkIOC` executable. The two
beamline cameras run as independent driver instances in that one camera IOC;
they are no longer loaded into the main `4dh4` control IOC.

## Dependencies

- EPICS Base 7
- asyn
- ADCore R3-14 or compatible
- FFmpeg development libraries: libavformat, libavcodec, libavutil, libswscale
- libcurl development files
- nlohmann-json headers

On Fedora the intended packages are:

```text
ffmpeg-free-devel libcurl-devel json-devel
```

Use native system libraries for the IOC. Do not link the module against Conda
copies of FFmpeg or curl.

Copy `configure/RELEASE.local.example` to `configure/RELEASE.local`, adjust
paths if needed, then run `make`.

## Configuration

```text
ADReolinkConfig(port, host, channel, rtspPort, httpPort,
                usernameEnv, passwordEnv,
                maxBuffers, maxMemory, priority, stackSize)
```

`usernameEnv` and `passwordEnv` are environment-variable names, not
credentials. The driver reads their values once during IOC startup. An example
two-camera setup is in `iocBoot/ADReolink.iocsh`.

Store credentials outside version control, for example:

```sh
# /home/mitr_4dh4/.config/epics/reolink.env (mode 0600)
export REOLINK_CAM1_USER='...'
export REOLINK_CAM1_PASSWORD='...'
export REOLINK_CAM2_USER='...'
export REOLINK_CAM2_PASSWORD='...'
```

The standalone launcher `iocBoot/iocReolink/run-reolink-ioc` sources this file
before executing the IOC. Credentials
are held in process memory but are never exposed as PVs or deliberately
written to IOC logs.

## Standalone IOC operation

The installed user service is `reolinkioc.service`:

```sh
systemctl --user status reolinkioc.service
systemctl --user start reolinkioc.service
systemctl --user restart reolinkioc.service
systemctl --user stop reolinkioc.service
journalctl --user -u reolinkioc.service
```

The service restarts automatically after unexpected exits. SIGTERM performs a
normal EPICS shutdown. The main IOC remains managed by the existing `4dh4ioc`
launcher; that shortcut does not load camera credentials or camera records.

To let both IOCs coexist on one host, their CA and PVA server/search ports and
bind interface are supplied through the private
`~/.config/epics/network.env` deployment file. The repository contains only
the variable names. Both servers disable automatic beacon address lists and
use nonstandard search ports. GUI clients retain normal LAN discovery for
ordinary demo IOCs and read explicit production endpoints from the private
file.

```sh
MITR_EPICS_SERVER_INTERFACE=...
REOLINK_CA_SERVER_PORT=...
REOLINK_PVA_SERVER_PORT=...
REOLINK_PVA_BROADCAST_PORT=...
MITR_EPICS_CA_ADDR_LIST=...
MITR_EPICS_PVA_ADDR_LIST=...
```

The diffractometer-controls launcher applies these settings automatically.
This preserves deliberate address configuration, but it is network obscurity,
not authentication; firewall rules are required for a hard access boundary.

## Image pipeline

```text
RTSP/H.265 -> ADReolink RGB1 -> NDCodec/JPEG -> NDPva -> PyDM/p4p
```

`AcquirePeriod` controls publication rate, not the source-camera rate. The
default is 0.25 seconds (4 Hz). The decoder consumes the stream continuously
to maintain H.265 state and discards decoded frames that are not selected for
publication. There is no CA waveform image PV.

For the example configuration the viewer channels are:

- `pva://4dh4:Reolink1:Pva1:Image`
- `pva://4dh4:Reolink2:Pva1:Image`

After `iocInit`, configure each codec for compress/JPEG and quality 90:

```text
dbpf 4dh4:Reolink1:Codec1:Mode 0
dbpf 4dh4:Reolink1:Codec1:Compressor 1
dbpf 4dh4:Reolink1:Codec1:JPEGQuality 90
dbpf 4dh4:Reolink1:Codec1:EnableCallbacks 1
dbpf 4dh4:Reolink1:Pva1:EnableCallbacks 1
```

Repeat for Reolink2. The JPEG file plugin remains idle until its standard
`WriteFile` record is processed.

## Reliability and safety

RTSP uses TCP, five-second I/O timeouts, and 1/2/4/8/15-second reconnect
backoff. Acquisition remains requested through a network failure. Each camera
reconnects independently.

Every non-stop PTZ command starts a deadman timer (two seconds by default).
Expiry queues an explicit Stop. Stop commands have priority over movement;
pending absolute zoom/focus commands are coalesced.

Unsupported controls reject writes. Use the `*Supported_RBV` records to adapt
operator interfaces to each model.

`Refocus` is a momentary command available when zoom and autofocus are both
supported and autofocus is enabled. Reolink does not expose a distinct
one-shot autofocus operation, so the driver reads and reissues the camera's
current `ZoomPos`. This runs the normal autofocus cycle without changing the
requested magnification.

## Operator screen

`reolinkApp/op/PyDM/ADReolink.ui` is a reusable, single-camera PyDM screen.
Launch it with `P` and `R` macros, for example `P=4dh4:,R=Reolink1:`. It uses
PyDM's standard PVA image widget for `pva://${P}${R}Pva1:Image` and provides
basic acquisition, PTZ, lens, status, and related areaDetector screens. Sites
that need specialized rendering or a multi-camera layout can wrap the same PV
interface in a beamline-specific display. The beamline GUI provides its custom
two-camera JPEG viewer and an IOC panel that independently reports process
state, heartbeat, uptime, RTSP/control state, and publication rate for the
main and camera IOCs.
