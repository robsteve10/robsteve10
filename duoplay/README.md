# DuoPlay

PS5 Remote Play in Safari, built for a folding iPhone. The two priorities are:

1. **Switching between the front (cover) screen and the inside screen with no interruption.**
   Fold or unfold mid-game and the picture and controls rearrange in the same frame. The
   stream doesn't reconnect, stall or restart.
2. **Playing well over Wi-Fi.** The design tries to keep latency low and recover quickly
   from the packet loss and jitter that Wi-Fi adds.

```
  PS5  ──(wired, Remote Play protocol)──►  DuoPlay bridge  ──(Wi-Fi, WebRTC)──►  iPhone (Safari)
                                           PC / Mac / Pi
```

## Why there's a bridge

A web page can't talk to a PS5 directly. Remote Play uses its own encrypted UDP protocol,
and browsers can only use HTTP, WebSockets and WebRTC. So a small program, the **bridge**,
runs on a computer on your network. It talks to the console using
[chiaki-ng](https://github.com/streetpea/chiaki-ng)'s open-source Remote Play library and
passes the stream to the phone over WebRTC.

The bridge **doesn't re-encode the video**. The console's own H.264/H.265 and Opus audio go
straight to the phone, so the bridge adds no encoding delay and no quality loss. It needs
very little CPU, and a Raspberry Pi 4 is enough.

## Quick start

### 1. Run the bridge

Use a computer that stays on and is on the same network as the PS5. A wired (Ethernet)
connection works best, for both the console and the bridge.

**Raspberry Pi / Linux / NAS with Docker**

```sh
cd duoplay
docker compose up -d --build
```

(The Docker files are included but weren't test-built during development. The native
build below was.)

**Raspberry Pi OS / Debian / Ubuntu (native)**

```sh
sudo apt install build-essential cmake git pkg-config libssl-dev protobuf-compiler python3-protobuf
cd duoplay
./scripts/build.sh
./dist/duoplay-bridge
```

**macOS**

```sh
brew install cmake openssl@3 protobuf && python3 -m pip install --user protobuf
cd duoplay && ./scripts/build.sh && ./dist/duoplay-bridge
```

**Windows (via WSL2)**

1. Install Ubuntu from the Microsoft Store (`wsl --install -d Ubuntu`).
2. Let WSL share the PC's network. Create `%UserProfile%\.wslconfig` containing:
   ```ini
   [wsl2]
   networkingMode=mirrored
   ```
   Then run `wsl --shutdown` in PowerShell.
3. Allow the phone to reach it. In PowerShell **as administrator**, run:
   ```powershell
   Set-NetFirewallHyperVVMSetting -Name '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' -DefaultInboundAction Allow
   ```
4. In the Ubuntu window, follow the Debian/Ubuntu steps above.

The first build downloads its pinned dependencies (chiaki-ng, libdatachannel and
cpp-httplib) and takes a few minutes.

When it starts, the bridge prints the address to open on your phone, for example:

```
  Open on your iPhone (same Wi-Fi):
    https://192.168.1.20:8443   (recommended, after installing the certificate)
    http://192.168.1.20:8080
```

### 2. Open it on the iPhone

Open the `http://…:8080` address in Safari. Everything works there except physical game
controllers and keeping the screen awake. Safari only allows those on HTTPS pages. To turn
them on (one-time):

1. Tap **Download the certificate** on the DuoPlay page, then go to **Settings → Profile
   Downloaded → Install**.
2. Go to **Settings → General → About → Certificate Trust Settings** and switch on
   **DuoPlay Local CA**.
3. Open the `https://…:8443` address.

The certificate is created on your bridge and only works for your bridge.

Then **Share → Add to Home Screen**. DuoPlay then opens full screen with no browser bars, so
both screens give all their pixels to the game, and edge swipes can't navigate away mid-game.

### 3. Link your PlayStation account (once)

The console only pairs with an account it knows. Tap **Sign in with PlayStation**, sign in,
then copy the address of the (mostly blank) page you land on and paste it into DuoPlay. The
bridge exchanges it for your account's numeric ID. Your password never reaches DuoPlay. If
you already know your base64 account ID from another Remote Play tool, you can paste that
instead.

### 4. Pair the console (once)

On the PS5, go to **Settings → System → Remote Play** and turn on **Enable Remote Play**.
Then choose **Pair Device**. In DuoPlay, tap **Pair** next to the console and enter the
8-digit number.

To use **Play** when the console is in rest mode, go to **Settings → System → Power Saving →
Features Available in Rest Mode** and switch on **Stay Connected to the Internet** and
**Enable Turning On PS5 from Network**.

### 5. Play

Tap **Play**. If the console is asleep, DuoPlay wakes it first.

## Switching screens

![The four arrangements: cover portrait, inside portrait, cover landscape, inside landscape](docs/layouts.jpg)

Every layout is computed from the screen DuoPlay is shown on, not from a list of device
sizes. It classifies each screen by shape: phone-shaped screens are the **cover screen**,
near-square ones are the **inside screen**. It then picks the arrangement that covers the
least of the picture:

| Screen | Arrangement |
| --- | --- |
| Cover, portrait | Picture on top, a full controller below it. Nothing covers the game. |
| Cover, landscape | Picture fills the height. Translucent controls sit on the sides. |
| Inside, portrait | Picture on top, a full controller below it. Nothing covers the game. |
| Inside, landscape | Picture at the top. Sticks sit in the spare band at the bottom. |
| Inside, half-folded (if the browser reports the hinge) | Picture on the top half, controller on the bottom half. |

What makes the switch seamless:

- **Nothing reloads or reconnects.** Only positions change. The `<video>` element and the
  WebRTC connection never notice. The automated test folds, unfolds and rotates six times
  mid-stream and checks for zero reconnects and continuous playback.
- **Laid out in the same frame.** The layout runs inside a `ResizeObserver` callback, after
  the browser's layout pass and before paint. The first frame drawn on the new screen is
  already correct, with no flash of the old arrangement.
- **No stuck buttons.** If your thumbs are down when the screen changes, every button and
  stick is released and the bridge is told right away. A button held during the fold can't
  leave your character running into a wall.
- **Controls keep the same physical size.** Your thumbs don't get bigger when the phone
  unfolds, so the controls don't either. They only shrink when a screen is too small for them.
- **Separate preferences per screen.** Control size and opacity are saved separately for
  the cover and inside screens.
- **Safe areas are re-read on every change**, because the two screens have different
  camera cut-outs and corners.
- **If iOS suspends the page** during a transition, the bridge keeps the console session
  running. When the page comes back, it reattaches. That takes about 130 ms in the test
  setup, and the game never restarts.

## Wi-Fi

What DuoPlay does:

- **No re-encoding, no extra buffering at the bridge.** Frames are forwarded the moment
  they arrive.
- **Fast recovery from lost packets.** The bridge keeps the last 4096 video packets and
  re-sends any the phone reports missing. On a home LAN that takes a few milliseconds, much
  quicker than waiting for a new keyframe.
- **A new keyframe only when needed.** If a frame really can't be repaired, either on the
  console-to-bridge link or over Wi-Fi, the console is asked for a fresh keyframe. These
  requests are rate-limited.
- **The console adapts to your Wi-Fi.** Packets the phone loses are added to the loss
  figures the bridge reports to the console every 200 ms. The PS5's encoder then lowers the
  bitrate when Wi-Fi struggles and raises it again when Wi-Fi recovers. Normally the console
  would only see the wired hop. You can turn this off in Settings.
- **Keyframe burst smoothing.** A wired bridge can push a 150 KB keyframe into the access
  point faster than Wi-Fi can send it on, and routers with small buffers drop the end of it.
  DuoPlay sends normal frames immediately and only paces the part of a burst above that size.
- **Wi-Fi priority.** Video and audio packets are marked with DSCP (AF42 for video, EF for
  audio). On WMM routers, and almost all routers have WMM, that puts them in the Wi-Fi
  video/voice priority queues.
- **Input that isn't held up by lost packets.** Every input packet carries the complete
  controller state. Packets go over an unordered channel that never retransmits, and are
  repeated every 45 ms while anything is held. A lost packet is replaced by the next one
  instead of being retransmitted, and late packets are discarded.
- **Adjustable buffering.** The bridge tells the phone how much buffering it may add:
  **Lowest** (none), **Balanced** (up to 40 ms, the default) or **Smooth** (up to 150 ms,
  for weak Wi-Fi).
- **Quick recovery from dropouts.** If the connection drops (a Wi-Fi blip, roaming between
  mesh access points, the app going to the background), DuoPlay reconnects on its own. The
  console session keeps running, so you're back in about a fifth of a second plus the
  keyframe. A frozen picture triggers a keyframe request after 1.2 s and a full reconnect
  after 6 s.
- **No streaming to a phone that isn't there.** If the page goes quiet (app in the
  background), the bridge pauses that phone's stream so it doesn't use up Wi-Fi airtime.
  After 5 minutes with no phone, it releases the console.
- **Network status at a glance.** The dot on the menu button turns green, amber or red with
  connection quality. Settings → *Show network stats* adds bitrate, round-trip time, loss,
  jitter-buffer and decode time.

What you can do (in order of impact):

1. **Wire the PS5 and the bridge** to the router. That leaves Wi-Fi only for the last hop
   to the phone.
2. Use the router's **5 GHz or 6 GHz** network, and keep in reasonable range of it.
3. Start with **1080p / 60 fps / Auto bitrate** and **Balanced**. If the dot stays amber or
   red, try **720p**. If the picture still stutters, try **Smooth**. Try **H.265** if your
   iPhone offers it: it gives sharper video at the same bitrate.

## Controls

- **Sticks are floating.** Touching anywhere near a stick centres it under your thumb.
  **Double-tap and hold** a stick to click it (L3/R3).
- **Face buttons and D-pad** follow your thumb as it rolls across them. Landing between two
  face buttons presses both.
- **Touchpad.** Drag on it for touch input, tap it for a touchpad click.
- **The ⋯ button** opens the menu: PS button, stats, hide controls, settings, rest mode and
  disconnect.
- **Bluetooth controllers** (DualSense, DualShock 4, Xbox, MFi) work on the HTTPS page,
  including rumble where Safari supports it. Touch controls fade out while you use the
  controller and come back when you touch the screen.

## Testing without a console

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=60 -t 6 -c:v libx264 -profile:v high -bf 0 -g 60 \
       -b:v 6M -pix_fmt yuv420p -x264-params aud=1 -f h264 pattern.h264
./dist/duoplay-bridge --test-pattern pattern.h264
```

A **Test pattern** "console" then appears in the list. It sends the file through exactly the
same path a PS5 stream takes, so you can try layouts, controls and Wi-Fi behaviour with no
console. `tests/e2e.js` (Node + Playwright) runs against it. It checks:

- the arrangement for each screen shape, laid out in the first frame
- six fold, unfold and rotate switches mid-stream with continuous playback and no reconnects
- multi-touch input reaching the bridge
- buttons released when the screen changes under held thumbs
- a dropped connection coming back without restarting the session

## Bridge options

```
--http-port N        web app over HTTP (default 8080)
--https-port N       web app over HTTPS (default 8443, 0 = off)
--udp-port N         the single UDP port all streaming uses (default 9600)
--bind IP            only stream from this local address
--announce-ip IP     also offer this address to phones (e.g. a Docker host's LAN IP when not using host networking)
--data-dir DIR       pairing data and certificates
--web-dir DIR        location of the web app
--test-pattern FILE  add a looping H.264 test "console"
--verbose            debug logging
```

Firewall: allow TCP 8080 and 8443, and UDP 9600, from your LAN.

## Status and limits

- **Tested:** the whole bridge-to-browser pipeline. That covers the WebRTC passthrough,
  layouts on every screen shape, screen switching mid-stream, touch input, reconnects and
  HTTPS. It ran in an automated, phone-sized headless Chromium against the test-pattern
  source, all checks passing.
- **Not yet tested:** a real PS5, and Safari on the folding iPhone itself. The PS5 side uses
  chiaki-ng's protocol library unmodified, set up the way chiaki-ng's own client sets it up.
  Expect small fixes once it meets real hardware. If something fails, run the bridge with
  `--verbose` and the log will show where.
- **Not supported:**
  - HDR, because browsers can't display the console's HDR stream over WebRTC.
  - The microphone.
  - Motion controls.
  - DualSense haptics and adaptive triggers. The bridge presents a DualShock-style controller
    so that ordinary rumble works in the browser.
  - Remote Play over the internet, because the bridge is LAN-only.
- **Security:** anyone on your network who can open the bridge's address can use it. Run
  it only on a network you trust.

## Licence

DuoPlay is licensed under the GNU Affero General Public License v3.0 (see `LICENSE`). It
links chiaki-ng's library, which is AGPL-3.0 with an OpenSSL exception. Other components:
libdatachannel (MPL-2.0), cpp-httplib (MIT), nlohmann/json (MIT), nanopb (zlib), jerasure
and gf-complete (BSD). "PlayStation", "PS5" and "DualSense" are trademarks of Sony
Interactive Entertainment. DuoPlay is not affiliated with or endorsed by Sony.
