# Sprite Sheet → Flipper Animation Workflow (v2, validated 2026-10-03)

Supersedes the old walk-animation playbook. Rebuilt from scratch with the artist
after the v1–v5 extractions failed (size wobble, neighbor-pixel bleed,
props handed to the wrong frame). Guiding rule: **proof at every stage,
each stage approved before the next; package locally, never push to the
Flipper without the artist's explicit go.**

Reference implementation (JollyRobot, 36 frames, completed end-to-end):
`step1_grouping.py`, `step2_face.py`, `package_robot.py` in this folder.

## Asset spec (for newly generated sheets)
- Pure black line art on white, laid out as a grid of frames.
- Generous white gutters — at least ~half a sprite width between neighbors.
- ALL ink stays inside its own cell: props, sparkles, motion lines included.
  A prop that drifts past the midpoint toward the next sprite is ambiguous
  by construction (this is what broke the old drone sheet).
- Same character size every cell; feet on a consistent baseline.
- First and last frames = plain standing pose (loop seam + anchor calibration).

## Stage 1 — Glyph grouping (`step1_grouping.py <src> <out.png>`)
1. Threshold at gray < 180; drop specks < 8 px; connected components
   (`scipy.ndimage.label`) at NATIVE resolution. No cropping/scaling yet.
2. Bodies = tall glyphs (height ≥ 0.5 × max) that sit on the sheet's grid
   lattice: cluster candidate x- and y-centers (gap > 40 px) and keep the
   dominant clusters. A tall off-lattice prop (the thrown paper plane is
   nearly body-height!) is dropped here. Height AND pixel-area alone both
   fail — lattice position is the reliable signature.
3. Every glyph joins the body nearest in x within its row band.
4. Verifier: a glyph is "contested" if its distance to the 2nd-nearest body
   minus distance to the nearest is < 25 source px. Contested count must be
   0 (or be eyeballed on the proof) before proceeding.
5. Phase B (targeted, cheap): phase A only misfiles SMALL glyphs (≤32 px —
   dashes/sparkles trailing a prop past the boundary). Suspects = small
   glyphs whose bbox-gap to the runner-up sprite's ink is < 25 px and less
   than to their own sprite's ink. Only suspects get the expensive EDT
   ink-distance test; flip if the other sprite's ink is ≤ 0.6× as far.
   Everything else never pays for it (whole 6-sheet batch runs in ~2 s).
5. Proof: sheet painted one color per sprite + frame numbers. the artist checks
   every prop matches its character's color — especially travelers.

## Stage 2 — Stage framing (`step2_face.py <src> <Name>`)
- ONE global scale for the whole sheet: `47 / median body height`
  (target standing height ~47 px final). Never rescale per frame.
- Anchor (per sprite, at native res):
  - x = face center = ink centroid of glyphs lying INSIDE the body bbox
    (upper 62%). The face never merges with held props; the outline bbox
    does, and gets pulled (measured up to +5.5 final px of anchor error).
  - y = body feet (body glyph bbox bottom). Floor line = final row 62,
    horizontal center = x 64.
- Render at 4× supersampling (512×256), glyphs translated only; single
  BOX downscale to 128×64, threshold 50%. Output `sheets/<Name>/frame_NN.png`.
- Checks printed + proven on a contact sheet with guide lines: feet-row
  spread ≤ 1 px, no ink clipped at edges, body heights reported (pose
  variation is fine; scale is constant by construction).

## Stage 3 — Timing preview
- First pass: uniform 333 ms/frame GIF at 3× size — judge staging/motion.
  (3 fps is the artist's standing default pace, set 2026-10-04; 200 ms felt too
  fast on the real screen.)
- Then structure per animation (the artist's pattern): hold the standing pose
  at both ends, repeat the middle section if wanted. Preview again.
- No packaging until the GIF is approved.

## Stage 4 — Local package (`package_robot.py`, adapt per animation)
- Default Frame rate 3 (333 ms/frame) unless the approved preview used a
  different pace; the rate must quantize the approved segment durations.
  Bake holds/repeats as DUPLICATED frame files —
  the Flipper has no per-frame durations. `Passive frames` = total tick
  count; `Frames order` = 0..N-1; Active frames/cycles/cooldown = 0;
  Bubble slots 0; Duration 48 (≈4 full passes of a 36-frame sheet at 3 fps,
  so each pick loops ~4× before the desktop re-raffles).
- `.bm` format (firmware `icon.py`): 1-bit → invert → XBM bytes, stored as
  `\x00` + raw (1025 bytes per 128×64 frame).
- Round-trip-decode EVERY .bm and assert pixel-equality with its source.
- Output: `package/<Name>/` (frame_*.bm + meta.txt). LOCAL ONLY.
  Installing (copy to `/ext/dolphin/<Name>/` + manifest decision: pool vs
  replace) is a separate, separately-approved step.

## Known failure modes (designed against above)
- Per-frame crop+resize → size wobble. (Global scale + translation only.)
- Fixed slicing → travelers split/assigned across cells. (Glyph assignment.)
- Prop touching body → outline bbox pull. (Face anchor, not outline.)
- Assets too close together → ambiguous glyphs. (Contested check; if it
  fires, fix the asset or accept a flagged hand-check — no silent guesses.)
