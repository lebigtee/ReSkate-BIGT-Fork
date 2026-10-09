# Boneyard

skate 3's bail mini-game in ReSkate: you fall, and you see the bones you broke, according to how
hard and where you hit.

- **As the bail starts**: as in skate 3, the fall goes into slow motion and the picture moves in
  jerks (a new picture 40 times a second) to give it weight. Alone, the game itself slows down
  (half speed, 1.2 s, then a gradual return). In multiplayer, where the game's speed must not
  change, it is a replay: the overlay replays the start of the fall at half speed on your screen,
  then fades back to live (1 s in all). Nobody else is affected.
- **During the bail**: skate 3's X-ray. A real skeleton (skull, rib cage, spine, pelvis, limb
  bones, hands, feet) is drawn over the skater, see-through, bone by bone on its own joints. Your
  skater stays fully visible underneath. Bones are pale pink, lit down their middle and darker at
  their edges so they look round, in the soft blue glow of a lit film (which can be turned off);
  each one that breaks turns orange (Cracked), then red (Fractured, Compound fracture, Shattered),
  with a flash. All the text shown is in English.
- **All through the bail**: the screen reddens toward its edges and corners as in skate 3, and a
  "BONEYARD" panel at the bottom right, a radiograph's film headed by a strip of the X-ray collage
  drawn for the mod, gives, live, the number of broken bones, the speed at the bail, the drop, the
  list of bones with their severity, and whose film it is.

Nothing to set up: it is on from the start. Nothing changes in the physics, the score or the
skater's outfit: everything is read-only and drawn by ReSkate's overlay, except the slow motion at
the start of a bail alone, which then puts the game's speed back exactly as it was. In
multiplayer, everyone sees only their own bails, and only on their own screen (the slow motion
there is a visual replay).

## In the pause menu

The game's pause menu › ReSkate tab **CUSTOM STUFF** › **PLAYER** › **BONEYARD**:
`Boneyard: On/Off`, `Bail slow motion: On/Off`, `Bail stutter: On/Off` and `X-ray glow: On/Off`
(the last three only change with the Boneyard on). They go through the `boneyard` commands below
and are kept the same way.

## Commands (console `~`, optional)

| Command | Effect |
|---|---|
| `boneyard status` | State: on, X-ray, sensitivity, number of bails, whether the physics state is followed |
| `boneyard on` / `boneyard off` | Turns the Boneyard on or off |
| `boneyard xray 0\|1\|always` | The X-ray during bails; `always` shows it all the time (to check it fits the skater) |
| `boneyard slowmo 0\|1` | The slow motion at the start of a bail (on): the game alone, a visual replay in multiplayer |
| `boneyard stutter 0\|1` | The jerky picture at the start of a bail alone (on; the multiplayer replay always has it) |
| `boneyard glow 0\|1` | The glow round the X-ray's bones (on) |
| `boneyard sensitivity <0.25-4>` | How fragile the bones are: 1 = normal, 2 = bones break twice as easily |
| `boneyard duration <0.5-30>` | Seconds the panel stays once the skater is up (1.5 by default) |
| `boneyard drop [metres]` | Teleports the skater into the air (12 m by default) to test a fall; follows the session's teleport rules |
| `boneyard last` | The last bail in detail: the impact measured on each bone |
| `boneyard probe on\|off` | Records each bail with the game's raw contacts (reverse engineering), in `%LOCALAPPDATA%\ReSkate\boneyard\probe\` |
| `boneyard demo` | A 5 s fake bail on the current pose, to see the display without falling |

The commands answer to their first name too (`meat demo` is `boneyard demo`).

Settings are kept in `%LOCALAPPDATA%\ReSkate\boneyard\boneyard.json`. Settings kept under the
feature's first name are read from there until the first change saves them here.

## How it works

- **The skeleton**: the skater's animation pose (`Animation/Dingo/AnimBase_Default_Skeleton`, 395
  joints, read as the first-person view reads it), composed into world coordinates every tick.
  The joints' parents are in `boneyard_skeleton.h`, taken from the game's data by
  `dingosdk_boneyard_skeleton_dump` (CMake option `DINGOSDK_BUILD_FORK_TESTS`).
- **The bail**: the first of these signs: the No Bail hook's wipeout counter
  (`watched_physics_state`), a falling physics state (300 to 399), or a slam seen on the skeleton
  alone (the pelvis suddenly loses 4 m/s or more, then the torso leans more than 60° from
  upright). With **No Bail** on there is no bail, so no Boneyard.
- **A bone's impact**: the speed its two ends suddenly lose, measured over 1/30 s windows (the same
  at 30 or 144 frames a second), in m/s. A limb the ragdoll whips keeps its speed and does not
  count; a speed impossible for a body (over 30 m/s) neither. The hit that starts the bail counts.
- **The hits: the game's contacts.** The skater's physics core keeps one record per ragdoll part
  (24: head, neck, each side's clavicle, upper arm, forearm and hand, four spine parts, each side's
  thigh, shin, foot and toes, the pelvis, plus one off the body). The game copies them every frame
  into the animation's contact block (`[[core+0x3b8]+0x30]`, record i at `+0x2d0 + i*0x80`): the
  part's position (`+0x00`), contact under way (`+0x73`), and for its two contact points the speed
  at which it meets the surface (`+0x30`, `+0x34`, `+0x60`, `+0x64`). Each part is known by its
  position (the skeleton joint within 12 cm); a bone's impact is the hardest contact of its parts.
  If the layout no longer fits (another game build), the mod falls back to the skeleton estimate
  above. Found with `boneyard probe on` and the scripts in `Test/` (`probe_summary.py`,
  `probe_contacts.py`, `probe_replay.py`).
- **A confirmed slam**: with the game's contacts, a slam seen on the skeleton only counts if a body
  part other than the feet hit something at 3 m/s or more, and never while the skater is still on
  its board. A landing on the feet, even folded in two, is no bail.
- **Feet on the board** (skeleton estimate only): while the skater is on its board (and 0.15 s
  after), hits to the shins and ankles do not count.
- **Severity**: impact × the bone's fragility (`bones` in `boneyard.h`) × sensitivity,
  against the thresholds 8.5 / 10.4 / 12.7 / 15 m/s (Cracked, Fractured, Compound fracture,
  Shattered). Below Cracked, the bone is whole. Fragility also balances the body: hands and feet
  whip fastest in a ragdoll, so they are a little less fragile than the skull, ribs or pelvis,
  which move as one block.
- **End of the bail**: the skater gets back on its board, stands up, or is teleported (session
  marker, respawn); 60 s at most. Lying still is still the bail. The X-ray goes at once; the panel
  stays another 1.5 s (`boneyard duration`), then fades.
- **The slow motion**: the engine setting `SimulationTime.TimeScale` (the trainer's game speed),
  changed by ReSkate itself on the game thread: down to half the player's speed in 0.12 s, held
  1.2 s, back up in 0.6 s, in steps of a tenth (some ten changes a bail, not one a frame). The
  speed before is put back exactly (the trainer's if the player had slowed it), unless someone
  changed it in between: it is then left. Never in a multiplayer session: this setting changes the
  game for everyone (your skater would be sent in slow motion), ReSkate locks it in sessions and
  the server controls the game's speed. There, the replay takes its place (below). A paused or
  unreadable game is not slowed. Speeds measured on the skeleton follow a game clock slowed with
  it: a bail in slow motion breaks the same bones.
- **The jerks**: purely visual. The overlay keeps a copy of the game's picture (on the GPU) 40
  times a second and puts it in place of the following pictures, before drawing over it; the X-ray
  is frozen with it to stay on the skater. The game itself runs at full speed: controls, physics
  and network do not change. The copy is let go 3 s after. `boneyard_picture.cpp` does it, and
  the replay below, on the overlay's command list before it draws.
- **The replay (multiplayer)**: `boneyard_replay.cpp`. The pictures of the start of the bail
  are kept on the GPU in 720p (a copy of the picture, then a reduction by a small shader,
  `boneyard_replay.hlsl`, precompiled in `boneyard_replay_shaders.h`), 80 times a second
  at most, in 24 slots reused once shown (`Test/replay_plan.py` simulates the plan: 22 at most at
  240 frames a second). Each frame the HUD picks which to show: the one of the moment the replay
  is at (half speed after a 0.12 s transition), drawn over the game's picture, then faded toward
  live from 0.72 to 1.02 s. The X-ray follows the replayed picture. The game runs at full speed
  meanwhile. GPU memory: up to 100 MB during the replay (depending on the frame rate; 70 MB around
  100 FPS in 1080p), of which 8 to 15 MB are kept during the session, ready for the next; let go
  10 s after. `Test/replay_test.cpp` checks it on WARP (Windows' software renderer) in the game's
  four possible picture formats.
- **The X-ray**: three drawings of the skeleton made for the mod, front, back and side, white on a
  transparent background. `Test/build_assets.ps1` removes their halo, cuts the side view in two
  (its arm alone, and the rest without the arm drawn over it) and arranges them in
  `assets/bones.png`, built into ReSkate.dll and added to the ImGui atlas at startup, like the
  chat's emotes. `boneyard_hud.cpp` cuts each drawing into bones (tables measured with
  `Test/skeleton_spec.py`): each bone is laid between two of the skater's joints, stretched by the
  perspective (shortened when it points at the camera), as wide as its real size at that distance,
  and tinted.
- **Front, and the skull from behind**: the body always keeps the front drawing, which the
  skater's joints turn with it (the full side and back views jumped too much when the skater
  turned). Only the skull switches to the back drawing when the camera is behind the skater, so as
  not to show a face: when its chest faces away from the camera (with a margin, so as not to
  flicker side-on). The chest's way comes from a cross product whose sign (the handedness of the
  game's frame) is learned as soon as front or back is clear. `Test/BoneyardPreview.cs` makes this
  choice and this drawing outside the game, on recorded poses (`boneyard probe`,
  `Test/probe_preview.py`) seen from several angles.
- **The panel**: a radiograph's film, plain and dark, labelled as a hospital labels its films
  (RADIOGRAPH, the bail's number, PATIENT and the player's name as the overlay's menu last read it,
  or JOHN DOE). A strip of `assets/panel.png`, the collage of X-rays drawn for the mod (cropped by
  `build_assets.ps1`), heads it; without the picture the panel goes without the strip. Grain, faint
  scan lines and a border worn through in places, the same for a whole bail, keep it rough.
- **Round bones and the glow**: each bone's sprite is cut lengthwise into strips, shaded from
  dark at its edges to light a little off its middle; under all of them, the glow is three soft
  layers wider than each bone, blue-white, or the colour of its break.
- **Stretched screens**: everything is placed in the window, as the rest of ReSkate's overlay;
  a game drawing a smaller picture than its window (1920×1080 stretched over a 3440×1440 screen)
  is handled once for the whole overlay, which scales what it draws to the picture
  (`fit_to_picture` in `Extension/UI/Overlay/overlay_render.cpp`). The HUD never lays out by the
  swapchain's size, or it would be scaled twice. The game projects its world for its picture's
  own shape, though: the skeleton is projected the same way, then stretched across as the picture
  is over the window (`picture_stretch`, which that scaling finds). The Death Race's flags too.

## Performance

- Game thread: about 10 µs a tick outside a bail (reading the skeleton and the contacts). The slow
  motion adds some ten calls to the engine setting a bail, timed in the log
  (`slow motion of bail N over: … longest … ms`).
- Overlay, during a bail: the drawing (X-ray, vignette, panel) and, during the jerks, one copy of
  the screen a frame on the GPU (around 0.1 ms in 1440p). The real cost is written in the log at
  the end of the jerks (`jerky picture over: … us a frame`). Outside a bail: nothing.
- Replay (multiplayer): one copy and one reduction a kept frame (80 a second at most), one
  full-screen draw a frame during 1 s; a few tenths of a millisecond of GPU a frame. Its
  preparation (pipeline) happens outside a bail, and its time is written in the log.
- ReSkate profiler zones (perf window): `tick/boneyard`, `overlay/boneyard`,
  `overlay/boneyard picture`, `overlay/boneyard replay`.

## Tuning the thresholds

Each bail writes two lines in `logs\ReSkate.log`:

```
Boneyard: bail 3 (slam) at 38.0 km/h, physics state 504.
Boneyard: bail 3 ended (immobile) after 2.4 s: 38.0 km/h, drop 4.1 m, hips 6.2 m/s; 2 broken: ...; peaks (m/s): ...
```

With `--log-level=debug` (or `"log_level": "debug"` in `ReSkateLauncher.settings.json`), each hit
of the bail and each physics state change are written too. If bones break too much or not
enough, change `boneyard sensitivity` or the thresholds (`severity_thresholds` in `boneyard.cpp`).

## Files

| File | Role |
|---|---|
| `boneyard.h` | The bones, the severities, the snapshot shared with the overlay |
| `boneyard.cpp` | Game thread: skeleton, impacts, bail, settings, commands |
| `boneyard_commands.cpp` | Registers the `boneyard` commands |
| `boneyard_view.cpp` | The snapshot the overlay reads (overlay library) |
| `boneyard_hud.cpp` | The drawing: the X-ray in sprites and the result panel |
| `boneyard_skeleton.h` | The parents of the animation skeleton's 395 joints |
| `boneyard_picture.cpp` | The jerks and the replay applied to the game's picture, before the overlay draws |
| `boneyard_replay.cpp`, `.hlsl` | The slow motion replay in a multiplayer session |
| `assets/bones.png` | The three skeleton drawings, arranged in one sheet (1230 × 917) |
| `assets/panel.png` | The X-ray collage a strip of which heads the panel (480 × 642) |
| `assets/templates/` | The templates to draw the skeleton and the panel on |
| `Test/build_assets.ps1`, `BoneyardAssets.cs` | Make the two pictures from the Procreate exports |
| `Test/skeleton_spec.py` | The joints measured on the drawings; prints the code's tables |
| `Test/BoneyardPreview.cs`, `probe_preview.py` | The preview outside the game on recorded poses |
| `Test/skeleton_dump.cpp` | The tool that reads the game's data: asset names, and an asset's fields with their offsets |
| `Test/replay_test.cpp`, `replay_plan.py` | The replay's test on WARP, and its plan's simulation |
| `Test/probe_*.py` | Reading `boneyard probe` recordings (the contacts' layout, real bails replayed) |

The skeleton drawings and the panel, its collage of X-rays included, were made for the mod by its
author, and are shared with the fork under its license.

## Known limits

- The thresholds are an estimate: they are tuned on real bails (see above).
- Made for the game build ReSkate 1.1.3 supports (Steam 25414733). A game update means checking
  the skeleton again (`dingosdk_boneyard_skeleton_dump`) and the contacts' layout (`boneyard probe`).
