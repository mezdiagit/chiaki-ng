# AI Remote API (low-latency control + frames)

`chiaki-ng` can act as the interface between an autonomous AI and the console:
the AI sends controller input over a localhost API and reads the live screen
image back with minimal latency. Disabled by default; enable only when needed.

Long-term target: autonomous offline play (e.g. FC 27) driven by an external
AI process on the same machine. The game-understanding / decision part lives
outside `chiaki-ng`; this page documents the transport the AI should use.

## Enable

GUI: `Settings → General → Automation API (AI control)`:

- `Automation API` checked
- `Automation TCP Port` (default `19090`, `0` = off)
- `Automation UDP Port` (default `19091`, `0` = off)
- `Automation Token` (empty = none; recommended when exposing beyond localhost).
  The token guards HTTP only. UDP has no auth by design and always stays on
  `127.0.0.1`, even when HTTP listens on LAN.
- `Automation Exclusive Input` (unchecked = automation input is OR-ed with
  physical controllers/keyboard; checked = automation input replaces them).
- `Publish Frames` checked, `Frame Size` default `960x540`

CLI (overrides stored settings):

```bash
chiaki --enable-automation --automation-port 19090 --automation-udp-port 19091 \
  --automation-token secret stream 'My PS5' 192.168.1.10
```

Env overrides (useful for headless AI bring-up):

```bash
CHIAKI_AUTOMATION_ENABLED=1 CHIAKI_AUTOMATION_PORT=19090 \
CHIAKI_AUTOMATION_UDP_PORT=19091 CHIAKI_AUTOMATION_TOKEN=secret \
CHIAKI_AUTOMATION_LISTEN=127.0.0.1 chiaki
```

`CHIAKI_AUTOMATION_LISTEN=0.0.0.0` binds the HTTP API to LAN (understood
risk). Default binds `127.0.0.1` only. UDP input always binds `127.0.0.1`
regardless of this setting. `CHIAKI_AUTOMATION_EXCLUSIVE=1` enables exclusive
input without touching profiles.

## Window modes (no longer forced fullscreen)

`Settings → Video`:

- `Window Type`: video scaling (`Stream Resolution`, `Custom Resolution`,
  `Adjust Resolution Manually`, `Fullscreen (video fit)`, `Zoom`, `Stretch`).
  This no longer forces the window chrome.
- `Stream Window Mode`: `Windowed (custom size)` / `Maximized (not fullscreen)`
  / `Fullscreen`. Applies at connect and live via `POST /api/window_mode`.

CLI: `--windowed` / `--maximized` / `--fullscreen` (+ `--zoom` / `--stretch`
for video scaling). `F11` and double-click still toggle fullscreen.

## Control fast path (UDP, for 60 Hz AI loops)

Send 20-byte little-endian datagrams to `127.0.0.1:<udp_port>`:

```
u32 magic 0x43484941 ('CHIA'), u32 seq, u32 buttons, u8 l2, u8 r2,
i16 left_x, i16 left_y, i16 right_x, i16 right_y
```

- `seq`: monotonic; `0` = unsequenced (always accepted). Stale/duplicate seqs
  are dropped.
- `buttons`: bitmask of `ChiakiControllerButton` + analog bits
  (`CROSS=1<<0`, `MOON=1<<1`, `BOX=1<<2`, `PYRAMID=1<<3`,
  `DPAD_LEFT=1<<4`, `DPAD_RIGHT=1<<5`, `DPAD_UP=1<<6`, `DPAD_DOWN=1<<7`,
  `L1=1<<8`, `R1=1<<9`, `L3=1<<10`, `R3=1<<11`, `OPTIONS=1<<12`,
  `SHARE=1<<13`, `TOUCHPAD=1<<14`, `PS=1<<15`, `L2=1<<16`, `R2=1<<17`).
- Sticks: `-32768..32767`. No ACK, fire-and-forget. Input is OR-ed with
  physical controllers/keyboard on the next feedback send (coalesced, ~1 ms),
  unless exclusive mode is on (see below).
- Drop counters (`received/accepted/dropped_stale/dropped_bad_magic/...`) are
  visible in `GET /api/status` under `udp`, so AI loops can detect a stale-seq
  or wrong-port misconfiguration instead of guessing.

Python example:

```python
import socket, struct
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq = 0
def send(buttons=0, l2=0, r2=0, lx=0, ly=0, rx=0, ry=0):
    global seq
    seq += 1
    s.sendto(struct.pack('<III B B h h h h', 0x43484941, seq, buttons, l2, r2, lx, ly, rx, ry),
             ('127.0.0.1', 19091))
# hold CROSS for ~100 ms
send(buttons=1 << 0)
```

## Control plane (HTTP JSON, `127.0.0.1:<tcp_port>`)

Auth (when token set): `X-Chiaki-Token: <token>` header or `?token=<token>`
(or `Authorization: Bearer <token>`).

- `GET /api/status` → `{session:{present,connected,host,bitrate,packet_loss,
  automation_active,automation_exclusive,automation_state,...},
  udp:{received,accepted,dropped_*}, last_input:{source,timestamp_us,age_us},
  frame:{seq,width,height,timestamp_us,shm_key}, window_mode, video_mode}`
- `POST /api/exclusive` `{"exclusive":true}` — when on, automation input
  replaces physical controllers/keyboard instead of OR-ing with them. Persists
  to settings; also readable in `GET` (no body) form.
- `GET /api/automation/log?limit=200` — last N automation inputs
  (`t_us,source,buttons,l2,r2,lx,ly,rx,ry[,udp_seq]`, capped at 1200) for
  training-data capture and debugging stuck-input issues.
- `POST /api/controller` full state (lowest-latency HTTP path):
  ```json
  {"buttons": 3, "l2_state": 0, "r2_state": 255,
   "left_x": 0, "left_y": -32767, "right_x": 0, "right_y": 0}
  ```
  `buttons` also accepts `["cross","r1"]`. Individual `"cross": true/false`
  keys are merged. Sticks accept ints or normalized floats (`-1..1`).
  `l2`/`r2` aliases for `l2_state`/`r2_state`.
- `POST /api/button` `{"button":"cross","pressed":true}`
- `POST /api/clear` release automation input (idempotent)
- `POST /api/pin` `{"pin":"1234"}`
- `POST /api/home`, `POST /api/mute` (`{"muted":true}` or toggle),
  `POST /api/video_mode` (`normal|zoom|stretch`),
  `POST /api/window_mode` (`windowed|maximized|fullscreen`),
  `POST /api/connect` (`{"index":0}`), `POST /api/disconnect` (`{"sleep":false}`),
  `POST /api/idr`

```bash
curl -H 'X-Chiaki-Token: secret' -X POST 127.0.0.1:19090/api/controller \
  -H 'Content-Type: application/json' -d '{"buttons":["cross"],"left_y":-1.0}'
```

## Vision fast path (shared memory + JPEG)

- Shared memory `chiaki-ng-frame-<pid>` (QSharedMemory, per-process key —
  discover it via `GET /api/frame.info`, never hardcode it): header
  `{magic, version, seq, timestamp_us, pts, width, height, stride, format=BGRA32}`
  + `width*height*4` BGRA bytes at the configured AI size (default 960x540,
  ~2 MB @60 fps). Zero JPEG cost; poll `seq` for new frames. The key changes
  per process so two instances never share a segment.
- `GET /api/frame.jpg?quality=80` → JPEG snapshot (cached per frame; headers
  `X-Frame-Seq/Width/Height/Timestamp-Us`). Poll at your control rate.
- `GET /api/frame.raw` → raw BGRA bytes + same headers.
- `GET /api/frame.info` → `{seq,width,height,timestamp_us,shm_key}`.

Frames are tapped on the decoder thread right after hw→sw transfer, before GUI
handoff, so AI latency is minimized. Downscale happens once via `libswscale`.
Frame `timestamp_us` values are monotonic (same clock as the decode/present
stats), so `last_input.age_us` vs frame age can be compared directly.

## Latency notes for AI play

- Prefer UDP input + SHM vision on the same machine (no TCP/JSON/JPEG in the
  hot loop).
- Keep the AI size small (960x540 default; 640x360 for faster models).
- Use `POST /api/idr` sparingly (recovery only); prefer wired/local network and
  720p/1080p profiles matching your model.
- Automation input is OR-ed by default; enable exclusive mode or release
  physical inputs (or call `/api/clear`) when the AI takes over to avoid
  fighting. The current automation state is echoed in `/api/status`.
- Capture training data from `GET /api/automation/log` (timestamped inputs)
  joined with SHM frame `seq` on your side.
