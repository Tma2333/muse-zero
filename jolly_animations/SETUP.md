# Setup — putting the Jolly animations on a Flipper

This folder holds 33 finished desktop animation sets (128×64, 36
frames each, 3 fps, duration 48) in the stock Flipper format — no
custom firmware, no tools to run. Push them on any of these ways:

## Option A — SD card (easiest, no software)

1. Power the Flipper off and remove the microSD (or use USB storage
   mode if your firmware offers it).
2. Copy **every `Jolly*` folder in this directory** plus
   `manifest.txt` into `/ext/dolphin/` on the card.
   - Already have a `manifest.txt` there? Open both and append the
     `Name:` blocks from ours instead of overwriting yours.
3. Card back in, power on.

## Option B — qFlipper / mobile app

1. Connect the Flipper and open the file manager.
2. Navigate to `SD Card/dolphin/`.
3. Upload the `Jolly*` folders and merge `manifest.txt` as above
   (download yours, append our blocks, re-upload).

## Option C — from a Muse Zero Pi rig

If you've done the root SETUP, the Pi driver does it:
`flipper_driver.py mkdir` + `put` each folder into `/ext/dolphin/`,
then `put` the merged manifest. Restart the Flipper afterward.

## After installing

The desktop idle animation weighted-picks from the manifest, so Jolly
appears in rotation within a minute or two of idle — no settings to
change on stock firmware (there is no animation picker; the manifest
is the picker). The firmware keeps a few built-in fallbacks in the
pool, so other animations still show occasionally.

## Why the weights are huge

Each entry's `Weight:` in `manifest.txt` is 1,000,000, and that's
deliberate. The pick is a raffle: all eligible weights are summed and
the winner is drawn in proportion. Alongside the manifest, the
firmware hardcodes three built-in fallback animations — TV (weight
3), BadBattery (3), NoSd (6) — that the manifest cannot remove.

Ordinary weights would give those built-ins a steady share of picks
(TV especially can park the desktop for a long stretch once chosen).
At 1,000,000 per Jolly entry, the pool is 33,000,000 vs the built-ins'
12 combined — a ~0.00004% chance they'll ever be drawn. All 33
entries share the same weight, so they're evenly matched among
themselves; the big number is there purely to swamp the built-ins.
If you'd rather see them occasionally, lower all weights together —
ratios are all that matter.

Previews of all 33 are in [`previews/`](previews/).
