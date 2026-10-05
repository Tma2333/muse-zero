# Animations

Drop-in Flipper Zero desktop animations: the complete **Jolly** pack —
33 little-robot idle animations — plus the tools that convert sprite
sheets into Flipper animation sets.

## Just want them on your Flipper?

1. Copy every folder in [`jolly-pack/`](jolly-pack/) and
   `jolly-pack/manifest.txt` onto your Flipper's SD card at
   `/ext/dolphin/` (merge with what's there; if you already have a
   `manifest.txt`, append the `Name:` blocks instead of overwriting).
2. Reboot the Flipper (or eject/reinsert the SD).
3. Done — the desktop idle animation picks from the manifest at
   random; Jolly shows up on its own rotation (roughly every ~48 s a
   new pick is made; the firmware also keeps a few built-in fallback
   animations in the pool, so Jolly shares the stage occasionally).

Each set is 36 frames at 3 fps with a 48-tick duration, 128×64
monochrome, in the standard `frame_N.bm` + `meta.txt` format read by
stock firmware 1.4.x — no custom firmware needed.

One retired first draft lives in [`retired/`](retired/) for the
record; it's not in the manifest.

## Making your own

`tools/package_animation.py` converts frames into the `.bm` format
(raw 1-bit XBM-style bitmaps, inverted, with a `\x00` prefix byte) and
writes `meta.txt`. `tools/WORKFLOW.md` documents the full pipeline
used for this pack: sprite-sheet asset guidelines (pure black-and-
white line art, generous gutters, consistent baseline), glyph
extraction, staging, and frame packaging. The meta fields that matter
most: **Passive frames** (>0), **Frames order**, **Frame rate**
(we use 3), and **Duration** (we use 48). With 0 active frames,
Active cycles and Active cooldown must be 0.

After generating a set, add a block to `manifest.txt`:

```
Name: YourSet
Min butthurt: 0
Max butthurt: 14
Min level: 1
Max level: 3
Weight: 1000000
```

(Values above mirror this pack's; the firmware weighted-picks among
entries matching the dolphin's current level/butthurt ranges.)
