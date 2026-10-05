# Setup

This guide brings a Muse Zero rig up from parts to a verified
Muse ↔ Flipper hardware link. It is written step-by-step on purpose:
if you are an AI assistant (e.g. Muse with shell access to the Pi),
you can execute it directly — ask the human only for the physical steps
(wiring, plugging, pressing) and for approval prompts.

## You need

- Flipper Zero on official firmware 1.4.x with a microSD card
- A Raspberry Pi with Wi-Fi and Bluetooth (Zero 2 W, 4, or 5 all work;
  the backpack target is a Zero 2 W)
- 3 jumper wires (female-to-female dupont), or an Elecrow/Chrismettal
  style backpack board
- A proper 5 V/5 A USB-C supply for the Pi (a laptop charger that only
  offers 5 V/3 A will brown-out the Pi's USB ports and the Flipper will
  fail to enumerate — this bites everyone once)
- A computer for the one-time FAP build (or build on the Pi itself)

## 1. Pi OS and the Muse gadget

1. Flash **Raspberry Pi OS Lite (64-bit)** and boot with network up.
2. Install and pair the Muse Gadgets agent by following the
   [Muse Gadgets SDK](https://github.com/facebookincubator/muse-gadget-sdk)
   instructions for Linux. When pairing finishes, your Muse can run
   shell commands on the Pi — everything below happens through it.

## 2. Enable the Pi's header UART

The bridge uses the primary PL011 UART on GPIO14 (TX) / GPIO15 (RX).

Raspberry Pi 5 — in `/boot/firmware/config.txt`:

```
dtparam=uart0=on
enable_uart=1
dtoverlay=uart0-pi5
```

Pi Zero 2 W / Pi 4 (Bluetooth shares the PL011 by default; hand the
full UART to the header and give Bluetooth the mini-UART):

```
enable_uart=1
dtoverlay=miniuart-bt
```

On all models: remove any `console=serial0,...` from
`/boot/firmware/cmdline.txt` and disable the serial getty
(`sudo systemctl disable --now serial-getty@ttyAMA0` if present).
Reboot. The port is `/dev/ttyAMA0` — note that on the Pi 5
`/dev/serial0` points at the debug connector, not the header.

Python side:

```bash
sudo apt install -y python3-venv
python3 -m venv ~/flipper-rpc/.venv
~/flipper-rpc/.venv/bin/pip install pyserial protobuf websocket-client Pillow
```

## 3. Wire the Flipper

| Pi GPIO header | Flipper header |
|---|---|
| pin 8 (TXD0) | pin 14 (RX) |
| pin 10 (RXD0) | pin 13 (TX) |
| pin 6 (GND) | pin 18 (GND) |

(Backpack boards route the same three lines; on those, Flipper pins
1–18 pass through to breakout headers and the Pi is powered from
Flipper pin 1 — enable "5V on GPIO" in the Flipper's power settings if
you want the whole rig to run off the Flipper battery.)

Human step: *wires in, double-check against the table — a mis-seated
ground is the #1 cause of silence.*

## 4. Flipper settings for the bridge

In **Settings → Expansion Modules** (or the UART settings your
firmware exposes):

- Listen UART: **USART**
- Log Device: **None**

Leave the Flipper awake and unlocked when running tests
(auto-lock blocks app launches).

## 5. Pi-side RPC stack (setup and storage)

The Pi uses the Flipper's Expansion Module Protocol (protobuf RPC) over
USB for app launches and file management. The transport and driver in
this repo's `pi/` are layered on the pyflipper fork
(<https://github.com/gorg2331/pyflipper>):

```bash
mkdir -p ~/flipper-rpc && cd ~/flipper-rpc
git clone https://github.com/gorg2331/pyflipper.git pyflipper-src
cp /path/to/muse-zero/bridge/pi-host/*.py .
export PYTHONPATH=$PWD/pyflipper-src/src:$PWD
~/flipper-rpc/.venv/bin/python flipper_driver.py info
```

Connect the Flipper to the Pi by USB-C first. `info` should print the
Flipper name, firmware version, hardware revision, and storage. Other
driver commands include `ping`, `ls`, `read`, `put`, and `storage-info`.
Keep this directory layout — the test harness (step 7) expects it and
reads `FLIPPER_RPC_DIR` if you put it elsewhere.

## 6. Build the bridge app

```bash
pip install ufbt           # or use a project venv
ufbt update --branch=1.4.3 # match your firmware branch
ufbt build                 # in bridge/flipper-app/ → dist/muse_bridge.fap
```

## 7. Install and talk to it

Upload `dist/muse_bridge.fap` to `/ext/apps/Tools/muse_bridge.fap` on
the Flipper (via the driver, qFlipper, or Muse's file transfer), then
launch the app (Apps → Tools → Muse Bridge) or from the Pi.

Self-tests (run on the Pi, app running):

```bash
cd bridge/flipper-app/tools
FLIPPER_RPC_DIR=~/flipper-rpc ~/flipper-rpc/.venv/bin/python bridge_hil.py --case C04
```

`--case C04` exercises the wire codec end-to-end (bootstrap ping and
device info over the UART protocol at 230400 baud). The harness has
progressively deeper cases (sessions, actions, cancellation, recovery,
GPIO, ADC, notifications, IR receive); work upward only as your bench
fixtures allow (a witness wire for GPIO, a known voltage for ADC, a
real remote for IR).

## Notes and limitations

- The app currently auto-exits after 90 s (development behaviour) and
  holds the UART while open; stock RPC returns when it exits.
- Sessions live in RAM: relaunching the app resets them, by design.
- The bridge advertises only capabilities qualified on hardware
  (see `GET_CAPABILITIES`) — trust the list, not the docs.
- Transmit features (IR TX, emulation) are absent from this build on
  purpose, not merely disabled.
