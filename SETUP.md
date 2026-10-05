# Setup — Muse ↔ Flipper Zero (the general link)

This is the baseline rig: Muse, running on a Raspberry Pi, connected
to a stock Flipper Zero. It covers pairing, wiring, and the Pi-side RPC
stack that everything else in this repo builds on. For the Muse Bridge
app itself, continue to [`muse_bridge/SETUP.md`](muse_bridge/SETUP.md).

Following this file top to bottom gets you:

- a Raspberry Pi your Muse can run commands on,
- three wires between the Pi and the Flipper,
- a working RPC channel (`flipper_driver.py info` answers).

It is written step-by-step on purpose: if you are an AI assistant with
shell access to the Pi, you can execute it directly — ask the human
only for physical steps (wiring, plugging) and approval prompts.

## Hardware

- Flipper Zero on official firmware 1.4.x with a microSD card
- Raspberry Pi with Wi-Fi + Bluetooth (Zero 2 W, 4, or 5)
- 3 female-to-female dupont wires, or a backpack-style header board
- 5 V/5 A USB-C supply for the Pi (a 5 V/3 A laptop charger will
  brown-out USB and the Flipper will fail to enumerate — common trap)

## 1. Pi OS + Muse gadget

1. Flash **Raspberry Pi OS Lite (64-bit)**, boot, get it online.
2. Install and pair the Muse Gadgets agent per the
   [Muse Gadgets SDK](https://github.com/facebookincubator/muse-gadget-sdk)
   Linux instructions. After pairing, your Muse can run shell
   commands on the Pi — the rest of setup goes through it.

## 2. Header UART on the Pi

The bridge protocol uses the primary PL011 UART: GPIO14 (TX) /
GPIO15 (RX), exposed at `/dev/ttyAMA0`.

Pi 5 — `/boot/firmware/config.txt`:

```
dtparam=uart0=on
enable_uart=1
dtoverlay=uart0-pi5
```

Pi Zero 2 W / 4 (Bluetooth holds the PL011 by default on these):

```
enable_uart=1
dtoverlay=miniuart-bt
```

All models: remove `console=serial0,...` from
`/boot/firmware/cmdline.txt`, disable any serial getty, reboot.
Note: on the Pi 5, `/dev/serial0` is the 3-pin debug connector, not
the header — always use `/dev/ttyAMA0`.

Python environment:

```bash
sudo apt install -y python3-venv
python3 -m venv ~/flipper-rpc/.venv
~/flipper-rpc/.venv/bin/pip install pyserial protobuf websocket-client Pillow
```

## 3. Wiring

| Pi header | Flipper header |
|---|---|
| pin 8 (TXD0) | pin 14 (RX) |
| pin 10 (RXD0) | pin 13 (TX) |
| pin 6 (GND) | pin 18 (GND) |

Backpack boards route the same three lines (Flipper pins pass through
1:1; the Pi can be powered from Flipper pin 1 if "5 V on GPIO" is
enabled in the Flipper's power settings).

Human step: *wires in and snug — a floating ground is the #1 silence
cause; a photo of the header beats counting pins twice.*

## 4. Flipper settings

**Settings → Expansion Modules**:

- Listen UART: **USART**
- Log Device: **None**

Keep the Flipper awake and unlocked while testing (auto-lock blocks
remote app launches).

## 5. Pi-side RPC stack (stock protocol, over USB)

Everything here uses the Flipper's own Expansion Module Protocol —
no custom firmware involved. Layers: `expansion_transport.py`
(handshake/retry-hardened serial transport) under the pyflipper fork,
driven by `flipper_driver.py`.

```bash
mkdir -p ~/flipper-rpc && cd ~/flipper-rpc
git clone https://github.com/gorg2331/pyflipper.git pyflipper-src
cp /path/to/muse-zero/muse_bridge/pi_host/*.py .
export PYTHONPATH=$PWD/pyflipper-src/src:$PWD
```

Connect the Flipper over USB-C and check the link:

```bash
~/flipper-rpc/.venv/bin/python flipper_driver.py info
```

You should get the Flipper's name, firmware version, hardware
revision, and storage summary. Other commands: `ping`, `ls`, `read`,
`put`, `storage-info`, `mkdir`, `rm`. If `info` fails, debug in this
order: cable/power → device visible in `dmesg` → venv deps → pins
(step 3).

## Next

- The Muse Bridge hardware app: [`muse_bridge/SETUP.md`](muse_bridge/SETUP.md)
- Just animations: [`jolly_animations/SETUP.md`](jolly_animations/SETUP.md)
