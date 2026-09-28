# Silver recomp -- progress

Goal: not a byte-exact decompilation of `silver.exe` (no confirmed
original compiler, no debug symbols -- see `decomp/docs/known_structures.md`
for why that path was deprioritized), but a **functional recomp**: clean,
modern, compilable C that reimplements the reverse-engineered game logic
and asset formats, targeting a toolchain that exists today (MinGW-w64
GCC 14.2.0, installed via winget), so the end result can actually run
natively on modern hardware.

Everything under `recomp/` is 100% new code. Zero bytes of the original
`silver.exe` are linked in or executed. The original binary is only ever
*read* (by Ghidra, for reverse-engineering reference).

## What's working right now (verified, not just "compiles")

- **RNC decompressor** (`src/rnc.c` / `include/rnc.h`): direct C port of
  the project's own verified `tools/rnc.py`. Tested against real game
  data (`BOILAREA.PAK`'s compressed bytes) and diffed **byte-for-byte
  identical** to the Python reference implementation's output
  (`src/test_rnc.c`, `build/test_rnc.exe`).
- **`data.nob`/`fat.nob` archive reader** (`src/nob.c` / `include/nob.h`):
  port of `tools/nob_fat.py`.
- **PAK container parser** (`src/pak.c` / `include/pak.h`): port of
  `tools/pak_extract.py`.
- **Batch validation** (`src/validate_all.c` -> `build/validate_all.exe`):
  runs the full nob -> rnc -> pak pipeline against *every* `.PAK` entry
  in a disc's fat file. Result: **114/114 CD1 PAKs and 308/308 CD2 PAKs**
  (422/422 total) load successfully -- this isn't cherry-picked, it's
  the whole game's background data.
- **Real window + GDI rendering** (`src/main.c` -> `build/silver_recomp.exe`):
  a genuine Win32 window (no DirectDraw) that loads a room by path,
  builds an 8bpp indexed DIB from the decoded palette + pixel buffer,
  and blits it with `StretchDIBits`. Verified visually on two very
  different rooms (BOILAREA 640x480, SUBARCH 960x480 -- screenshots sent
  to the user in-session). Takes the room to load via command line:
  `silver_recomp.exe "<fat.nob>|<data.nob>|<relative PAK path>"`.
- **Camera panning** for rooms wider/taller than the fixed 640x480
  viewport: arrow keys, clamped to room bounds. Verified visually --
  panning right correctly reveals new content to the right, direction
  is correct.
- **Overlay animation** (`src/overlay.c`): decodes a room's Smacker
  (`.smk`) overlay video via `ffmpeg` (the ORIGINAL asset, not a
  pre-extracted copy), quantizes each frame to the room's palette, and
  composites+animates it over the background at the SMK's own 24fps.
  Verified multiple ways: a debug log of timer/frame-index, a standalone
  frame-diff test showing ~30% pixel change between consecutive frames
  (`src/test_overlay.c`), and -- after the user caught a real
  misalignment bug -- the screen position was computed via template
  matching against the actual background pixels (`find_overlay_position.py`),
  confirmed by all 13 decoded frames independently agreeing on the same
  position, and verified visually with no seam. Not a placeholder
  anymore, and the technique generalizes to every overlay in the game.
- **Click-to-move player character** (`src/main.c`, 2026-09-25): the
  first actual GAMEPLAY in the recomp, not just a scene viewer. Left-
  click anywhere in the room sets a target; the player (a red marker,
  placeholder art) walks toward it at a fixed speed on a 60Hz game
  timer, with a line showing the pending path while moving. Verified
  both via synthetic `PostMessage(WM_LBUTTONDOWN)` clicks and real
  mouse input, confirmed visually across three screenshots (click sent
  -> mid-travel -> arrived exactly at the clicked point, line
  disappears). Movement is clamped to the room's pixel bounds only (no
  walkmesh collision yet -- `out_0`'s per-tile record semantics aren't
  decoded far enough to use for collision). **Deliberately NOT a
  reproduction of the original's exact movement code** -- that
  investigation (see `decomp/docs/known_structures.md`, "Collision-
  trigger investigation") hit a real dead end after three separate
  tracing angles, and per this project's own stated goal (functional,
  not byte-exact), new from-scratch movement logic informed by the
  user's own description (Warcraft-3-style point-and-click) is the
  right call rather than blocking on perfect fidelity.
- **Real room-to-room transitions** (`src/main.c`, 2026-09-25): click a
  door, the room actually changes. Not hardcoded -- driven by a new
  data pipeline discovered and built this session, using a SECOND asset
  source the user pointed at (`the Silver Blockouts`, 3D rebuilds with
  real per-room 3D geometry as `.gltf`/`.bin`, not reverse-engineered
  from the compiled game). Each room's glTF has a "gizmo" mesh (a
  lime-green bounding box + black-material door/exit markers) and its
  own exact pre-render camera. Implemented the glTF perspective-camera
  projection from scratch (`project_gltf_camera.py`) and PROVED it
  correct by rendering the room's wireframe geometry back onto its real
  background PNG -- lines up almost exactly (`wireframe_boilarea.png`).
  Split the door-marker mesh into individual clickable regions
  (`extract_hotspots.py`) with real projected 2D screen positions. Paired
  each hotspot with its target room name using a SEPARATE, independent
  data source -- the target room's own compiled `.SCT` string table
  directly references its exit targets as plain (level, room) name
  pairs, no bytecode disassembly needed (`tools/extract_room_links.py`,
  cross-validated against `levels.SCT`'s master 393-room registry and
  confirmed the target PAKs actually exist in the game archive before
  trusting them). Combined into a per-room hotspot config
  (`build_room_hotspots.py`) the recomp loads directly. **Verified
  working, not just "compiles"**: launched the exe, simulated hovering
  a door hotspot (screenshot: yellow highlight box + "-> gno/teleroom"
  label appears exactly over the correct archway) and clicking it
  (screenshot: window title changes AND the background changes to a
  completely different, correctly-loaded room -- an observatory).
  **Honest caveats**: the hotspot-to-target-name pairing is a
  left-to-right screen-order heuristic, not a proven exact mapping
  (works for BOILAREA's case, refinable later against `out_1`'s own
  door zone positions); only BOILAREA has a hotspot config built so far
  (the pipeline generalizes to any room with both a the blockouts glTF and an
  extractable `.SCT`, but building configs game-wide isn't done yet).
  **Known gap, explicitly flagged by the user and correctly not yet
  addressed**: the player is still a flat 2D dot -- wrong long-term,
  since characters are 3D and each room has its own camera angle. The
  hard part (validated per-room camera projection) now exists and is
  ready to reuse for a proper 3D-projected player character.

## Toolchain

- Azul Zulu JDK 21 + Ghidra 12.1.4 (decompilation reference, see `decomp/`)
- WinLibs MinGW-w64 (UCRT runtime, GCC 14.2.0) -- the actual recomp
  compiler. Installed via `winget install --id BrechtSanders.WinLibs.POSIX.UCRT.LLVM`.
- `ffmpeg` on PATH -- required at runtime by `overlay.c` to decode
  `.smk` overlay videos (was already installed on this machine from
  earlier preservation-project work). Not a build dependency, a runtime
  one -- `silver_recomp.exe` shells out to it via `_popen`.
- Use `recomp\Makefile` (targets: `all`, `run`, `validate`, `test`,
  `test-overlay`) or invoke gcc directly, e.g.:
  ```
  gcc -mwindows -O2 -I include -o build\silver_recomp.exe src\main.c src\rnc.c src\nob.c src\pak.c src\overlay.c -luser32 -lgdi32
  gcc -O2 -I include -o build\validate_all.exe src\validate_all.c src\rnc.c src\nob.c src\pak.c
  gcc -O2 -I include -o build\test_rnc.exe src\test_rnc.c src\rnc.c
  gcc -O2 -I include -o build\test_overlay.exe src\test_overlay.c src\overlay.c src\pak.c src\nob.c src\rnc.c
  ```

## Environment note (resolved)

Windows 11 Smart App Control briefly blocked freshly-rebuilt unsigned
local binaries from launching mid-session. The user disabled it
themselves (their informed choice -- it's one-way without a Windows
reset) after I flagged the tradeoff; no longer an issue, both manual and
automated launches work normally again.

## What's NOT done yet (honest gap list, in rough priority order)

1. ~~No build script~~ -- fixed, see `Makefile`.
2. ~~Camera panning unverified~~ -- **verified visually**: right-pan
   correctly reveals new content to the right, direction is correct.
   Vertical panning couldn't be exercised on SUBARCH specifically
   (its height exactly equals the 480px viewport, so there's nothing to
   scroll) but the same clamped math applies either axis.
3. **Overlay animation -- working.** Turned out "overlays" are standard
   Smacker (`.smk`) video files sitting directly on disc next to a
   room's `.PAK` (e.g. `BOILAREA\boilbelt.smk`, a 72x148 13-frame
   conveyor-belt loop at 24fps) -- not a custom format. Implemented
   `recomp/src/overlay.c`: shells out to `ffmpeg` (must be on PATH) to
   decode the ORIGINAL `.smk` asset to raw RGB24 frames (no
   pre-extracted intermediate), quantizes every frame against the
   room's own 256-color palette (same nearest-color technique used
   throughout this project for palette fidelity), and composites the
   current frame onto a working copy of the background each repaint.
   Animates via `WM_TIMER` at the SMK's own 24fps. **Verified working**
   two ways: a debug log confirmed the timer fires and the frame index
   advances correctly, and a standalone frame-diff test
   (`src/test_overlay.c`) confirmed consecutive decoded frames
   genuinely differ by ~30% of pixels (not stuck/static) -- screenshots
   alone looked deceptively similar because this specific overlay is a
   small, dark, low-contrast texture, not because it wasn't animating.
   **Position -- FIXED, and verified, not guessed.** Traced the
   in-game position mechanism (`DAT_0069235c`/`DAT_00692358`, used
   directly as `SmackToBuffer`'s Left/Top) but could not find where the
   game ever WRITES them -- a write-reference search across all 3045
   decompiled functions came back completely empty. Turned out not to
   matter: these overlay `.smk` files are pre-rendered PATCHES of the
   scene itself (confirmed by exporting a real frame and looking at it
   -- `boilbelt.smk` shows the actual ramp/lantern structure, not an
   abstract sprite), meant to seamlessly replace the matching region of
   the background. That makes the position computable directly:
   `find_overlay_position.py` template-matches every decoded overlay
   frame against the real background pixels (brute-force SSD search,
   coarse-then-refined). **All 13 frames independently converged on the
   exact same position** (224,144 for `boilbelt.smk` on `BOILAREA`,
   not the earlier guess of 260,190) -- that cross-frame agreement is
   strong evidence it's genuinely correct. Confirmed visually: the
   composited overlay now blends into the background with no visible
   seam. **This is a reusable methodology for every other overlay in
   the game**, not a one-off fix -- any room+overlay pair can have its
   real position computed the same way, without needing to find the
   game's own (apparently unfindable-by-static-analysis) position-setting
   code at all.
   
   **Generalized and validated across 5 more rooms**, not just
   BOILAREA: `main.c` now takes the overlay `.smk` path and its
   `(x,y)` position as command-line arguments
   (`fat|data|room_pak|overlay_smk|x|y`) instead of hardcoding BOILAREA.
   Ran `find_overlay_position_general.py` (the reusable template-matcher)
   against 5 different room+overlay pairs and confirmed every one
   composites with no visible seam: TAVERN01+fire01.smk (584,240),
   DOCKSIDE+water.smk (0,288), WIZTOWER+wizsmoke.smk (152,0),
   PONDEND+newwater.smk (184,216), EXBARRAC+crowd1.smk (272,168).
   Screenshots sent to the user.

   **Room->overlay association -- SOLVED, same session.** The user
   pointed out (correctly) that real scenes play multiple overlays at
   once, and that a room's background/overlay set changes with story
   progress (e.g. DOCKSIDE has a base state plus `rep1.pak`/`rep2.pak`/
   `rep3.pak` background variants tied to story flags). Traced this to
   the room's `.SCT` file -- a compiled cutscene/script format, fully
   reverse-engineered this session (see `decomp/docs/known_structures.md`,
   ".SCT compiled script format" section). `tools/sct_parse.py` extracts
   every overlay/character/room-target/story-flag name a room's script
   references, directly from the real compiled data (checksum-verified,
   not guessed) -- tested working on two different rooms.

   **Wired into `recomp/`, same session.** `main.c` now loads a whole
   scene from a config file (`fat|data|room_pak|scene_config`, one
   `smk_path|x|y` line per overlay) instead of one hardcoded overlay --
   supports up to 16 simultaneous animated overlays, each independently
   positioned and frame-advanced (`src/main.c`'s `OverlayInstance`
   array). `build_scene_config.py` generates a config for a given
   room+overlay-list by template-matching each one. Demonstrated on
   DOCKSIDE's two real scene states from the user's own example: state 1
   = `DOCKSIDE.PAK` + dockhead/dockmob1/dockmob2 (army arriving), state 2
   = `rep1.pak` + dockboat/docksid2/water (departure) -- both
   screenshotted with all 3 overlays running together, matching the
   user's description exactly. Overlay lists for each state were taken
   from what `sct_parse.py` found in `DOCKSIDE.SCT`, grouped into the
   two states by hand per the user's description -- the bytecode-level
   conditional logic that would pick a state automatically isn't decoded
   yet (see the `.SCT` section in `decomp/docs/known_structures.md`).
4. **No walkmesh/collision/room-transition logic yet, but the ENTIRE
   room-transition pipeline is now mapped end to end.** `out_0`/`out_1`
   are loaded into memory (proven by the validator) but nothing
   interprets them for gameplay yet -- this item is about the actual
   game-logic side, and that side is now well understood. Traced in
   Ghidra while the user was away: `door_walkthrough_state_machine`
   (0x53a794) is the complete "player is approaching/entering a door"
   state machine (progress countdown -> animation-wait -> trigger),
   which sets a global mode flag consumed by `room_transition_dispatch_a`/
   `b` (0x444c90/0x4453f8), which call `change_room` (0x4bbbd8) -- the
   actual level-load function, confirmed to call `load_background_pak`
   directly (the same function this project already reimplemented in
   `recomp/src/pak.c` -- independent confirmation the recomp's PAK
   understanding targets the real call site, not a coincidence). Doors
   are tag==3 records (confirmed via the engine's own error string,
   "Door[%c] Does Not Exist..."), each with a 3D position/orientation
   applied via `position_character_at_door_with_orientation`. Also
   identified the engine's universal object-validation pattern (magic
   `0x5464563` = "OBJECT_IDENTIFIER") that most gameplay functions start
   with. Full function-by-function writeup with all VAs in
   `decomp/docs/known_structures.md` under "Room transitions / doors".
   **Still missing**: the actual collision/trigger-volume detection that
   sets the door-walkthrough state machine's own fields in the first
   place when the player first walks into a door's zone -- that's the
   next concrete thread, and the last piece needed before a recomp'd
   player-movement system could drive real room transitions.
5. **No audio** -- Miles Sound System (`mss32.dll`) and Smacker video
   (`smackw32.dll`) are proprietary middleware from the original build;
   a modern recomp needs a replacement (e.g. a modern audio backend for
   sound, and either a Smacker-compatible decoder or replacing the
   cutscenes with a modern video codec).
6. **No room transitions / game logic** -- the actual playable game
   (player movement, room-to-room travel, dialogue, inventory) is a
   substantially larger undertaking than the asset pipeline was, and
   hasn't been started.

7. **`.SCT` bytecode VM -- decoded, and its built-in function table is now
   partially identified.** Traced the full interpreter dispatch loop
   (`change_room`/`FUN_004bb4ac` -> `FUN_004f8c44` call-by-label ->
   `FUN_004f7800` main loop -> `FUN_004f82e8`/`FUN_004f8010`
   expr-eval/compound-assign) -- a real stack-machine scripting language
   with typed variables, arrays, user + built-in function calls, and
   structured control flow (if/if-not/while capped at 10000
   iterations/return/call-switch). Then dumped and decompiled all 128
   entries of the built-in native-function table
   (`PTR_FUN_00568254`) -- confirmed real via two ALREADY-named entries
   (`door_teleport_player`, `door_teleport_camera_or_object`) landing
   exactly where expected. Identified ~15 of the 128 by their debug
   strings/logic, independently confirming several mechanics the user
   described from having played the game: a `trigger_attack(char, id)`
   builtin with the exact named attack categories (NORMAL/LUNGE/
   SLICE_LEFT/SLICE_RIGHT/SLICE_180A/SLICE_180C/AIR), a
   `char_use_waypoint(char, index)` point-and-click movement queue, a
   per-object inventory check, a `dialog_event(name,name2)` lookup, a
   pausable per-object AI system, Miles Sound System (`_AIL_*`) audio
   control builtins, and a general 3D trip-wire/trigger-volume check
   distinct from both doors and the zone-proximity system. Also dumped
   the 102-entry variable table (`PTR_DAT_0056293c`) as raw pointers
   (not yet individually labeled). Full writeup with the identification
   table: `decomp/docs/known_structures.md`, "`PTR_FUN_00568254` -- the
   built-in function table, IDENTIFIED". Raw dumps in
   `decomp/exported/builtin_tables.txt`,
   `decomp/exported/builtin_funcs_summary.txt`,
   `decomp/exported/builtin_funcs_decompiled.c`.
   **Still open**: ~85 of the 128 built-ins are unidentified (no
   distinctive string, likely quiet getters/setters); the 102-entry
   variable table isn't labeled per-entry yet; and no raw `.SCT`
   bytecode has been disassembled instruction-by-instruction yet against
   this now-decoded opcode table.

- **Proof-of-concept: real animated 3D character, correctly projected
  into a real room** (`recomp/render_character_scene.py`, 2026-09-25).
  Not yet wired into the recomp's C exe -- a standalone Python
  prototype proving the full pipeline works. Loads Angus's base mesh +
  skeleton (2200 verts, 46-joint skin) and walk-cycle animation
  (135 real keyframe channels) from `Silver Blockouts`, evaluates the
  skeletal animation at 12 points in the cycle, skins the mesh per
  frame, places it in BOILAREA's world space, and projects it through
  the room's own real camera (`project_gltf_camera.py`, validated
  earlier this session against the ORIGINAL game's `out_2_hdr` camera
  data). Software-rasterizes flat-shaded triangles onto the real
  background with simple depth sorting. Output: `scene_angus_walk.gif`
  -- the pose visibly changes frame to frame (confirmed animation
  evaluation is real, not a static pose) and the character is
  correctly scaled/positioned for its distance from camera. Rough
  visual quality (no textures, simple flat shading, no floor/ground
  collision so placement was found empirically) but the underlying
  pipeline -- skeletal animation, skinning, per-room camera projection
  -- is genuinely working. **Character geometry source note**: this
  uses `Silver Blockouts`' glTF conversion, not the original `.CHR`/`.ANM`
  files -- a dedicated, thorough investigation this session (six
  independent techniques) found strong convergent evidence the shipped
  `silver.exe` doesn't load `.CHR`/`.ANM` at runtime at all (likely
  dev-only source assets); see `decomp/docs/known_structures.md`,
  "Character rendering format". The room geometry, camera, and
  background image are 100% from the original CD data either way.
  **Generalized and re-run across 4 rooms with wildly different camera
  setups** (`recomp/render_david_multi.py`): BOILAREA (moderate FOV),
  TAVERN01 (wide FOV, close, populated interior), PONDEND (narrow
  telephoto, distant outdoor), WIZTOWER (high dusky overlook) -- same
  character/renderer code, only the room's own camera data changes,
  and every shot projects correctly with no per-room manual tuning.
  Also swapped Angus for David (679 verts vs Angus's 2200 -- much
  closer to expected in-game complexity, per the user's correct
  observation that Angus looked unusually high-poly) and switched from
  flat shading to real UV-mapped texturing using David's actual
  `david.png` atlas (affine per-triangle software rasterization).
- **Full interactive click-to-move proof of concept**
  (`recomp/render_interactive_walk.py`, 2026-09-25): idle → click →
  walk-with-facing-turn → arrive → idle, with a rejected click on an
  unwalkable area, all real, none mocked. Built three new real pieces:
  (1) `Camera.pixel_to_ray`/`pixel_to_floor_point`
  (`project_gltf_camera.py`) -- the actual INVERSE of the camera
  projection used everywhere else this session (screen pixel -> world
  ray -> floor-plane intersection); (2) `walkable_area.py` -- since the
  original `out_0` walkmesh format is still undecoded, approximates a
  room's walkable floor as the 2D convex hull of its own geometry's
  highest-vertex-density Y-band (a documented, honest heuristic, not a
  claim of matching the original walkmesh); (3) a found (not authored)
  generic `walk.gltf` animation, shared across characters via the same
  48-joint skeleton convention David/Angus both use (David's own folder
  only had confused/lookout/stdstill, no walk cycle) -- confirmed
  compatible by rendering it and getting a clean, correct walk cycle.
  Verified via a frame-by-frame contact sheet: David visibly walks
  toward each click target, turns to face his direction of travel, and
  the third (deliberately off-platform) click produces zero movement.
  Still a SCRIPTED sequence of simulated clicks (this Python prototype
  can't take live mouse input mid-render), not a live-interactive app --
  but every underlying system (unprojection, walkability gating,
  animation-state switching, facing rotation, per-room camera-correct
  projection) is real and running.

8. **Game-wide 3D character + navigation generalization, animation
   fixes, and honest retraction of a wrong finding (2026-09-25 evening
   session).** Started from the user catching two real bugs in the
   BOILAREA-only prototype: an implausible door connection
   ("boilarea -> veranda") and a visibly misaligned/oversized door box.
   - **Root-caused the wrong connection, not just silenced it.** Traced
     the actual door-movement script builtins (`door_teleport_player`/
     `door_teleport_camera_or_object`, idx 43/44 in `PTR_FUN_00568254`)
     via `decomp/exported/expr_eval.c` and `builtin_funcs_summary.txt`:
     they take a NUMERIC door id resolved through
     `find_tag_record_by_id`, never a string. So the (level,room)
     name-string pairs found in a room's own `.SCT` -- which the
     original single-room hotspot pipeline paired to door hotspots by
     screen/string order -- are NOT proven to be that room's door
     targets at all (more likely a different builtin's scripted-
     teleport arguments). Retracted that pairing from driving actual
     navigation rather than ship a plausible-looking guess; re-enabled
     it afterward only with an explicit "(unverified target)" marker in
     the title bar, per the user's explicit request to be able to test
     interconnection game-wide despite the open confidence gap. Real
     per-door target resolution remains open -- needs the tag record's
     undeciphered `+0x2e`/`+0x43c` linkage decoded with live Ghidra
     verification (two blind attempts this session gave implausible
     offsets, correctly not shipped).
   - **Found and fixed the real alignment bug.** WIZTOWER's door
     hotspot legitimately projected to a 601-976px box in a 640px-wide
     image -- verified with the SAME camera math that's pixel-accurate
     for BOILAREA (wireframe overlay), so a real geometry/grazing-angle
     issue, not a math bug. General fix in
     `batch_build_room_data.py`: project each marker's 3D CENTROID
     (immune to corner scatter) with a fixed human-doorway-sized box,
     plus an edge-margin filter dropping markers too close to the frame
     border to trust. Dropped ~24 rooms' worth of garbage hotspots in
     the process.
   - **Generalized 3D character + walkable hull + camera from BOILAREA-
     only to 257 of 278 rooms** (`batch_build_room_data.py`, loaded at
     runtime via `test_data/<room>_room3d.cfg` instead of compiled
     constants). Found and fixed a second real bug this surfaced: the
     room's hull FIRST VERTEX (the prior "safe" start-position pick)
     doesn't actually project on-screen for BOILAREA's own camera
     (verified by hand: px_x=730 in a 640-wide image) -- neither hull
     vertices nor the raw centroid are reliably both walkable and
     visible. Fixed generally: `pick_start_point` samples points
     between the hull centroid and each vertex (guaranteed inside a
     convex hull) and keeps the one verified on-screen, with margin, via
     that room's own camera -- computed once at export time, stored as
     `start_pos` in the cfg.
   - **David's animations replaced with evidence-picked clips**, per
     the user's specific complaints: idle (`stdstill.gltf`, a defensive
     crossed-arms/crossed-legs pose) -> `stand.gltf` (shared anims pool;
     perfect start/end loop continuity, natural stance, confirmed by
     rendering both side by side). Walk (`walk.gltf`, only animates 20
     of David's 48 skeleton nodes -- arms stuck in bind pose, the likely
     "choppy" cause) -> `towalk.gltf` (41 nodes, same loop quality).
     Added a real run clip (`run.gltf`, PERFECTLY seamless loop, 41
     nodes, dynamic full-body stride) on double-click (`CS_DBLCLKS` +
     `WM_LBUTTONDBLCLK`, `CHAR_RUN_SPEED` vs `CHAR_WALK_SPEED`) --
     single click still walks. All verified by rendering comparison
     GIFs/contact sheets before wiring into C, then confirmed again in
     the actual built exe.
   - **Known remaining rough edge**: the walkable-hull heuristic (2D
     convex hull of floor-level points, documented limitation, no real
     per-tile walkmesh decoded) doesn't know about elevation changes or
     mid-floor obstacles, so a room with a tall central prop (BOILAREA's
     boiler) or multi-level platforms can still place/allow the
     character somewhere technically "inside the hull" but visually on
     top of geometry. Not new to this session, still open.

## Suggested next milestone

With click-to-move AND room-to-room transitions both working, the
natural next steps, in rough priority order:
- **3D-projected player character**: explicitly flagged by the user as
  wrong in the current placeholder -- characters are 3D in the real
  game and each room has its own camera angle, so a flat 2D dot can't
  be right long-term. The hard part is already done and proven
  (`project_gltf_camera.py`'s per-room camera projection, validated via
  wireframe overlay) -- next step is representing the player as a 3D
  world position and projecting it through the CURRENT room's camera
  every frame instead of a fixed 2D pixel position, so scale/position
  respond correctly to each room's perspective.
- **Build hotspot configs for more rooms**: only BOILAREA has one so
  far. The pipeline (`build_room_hotspots.py`) works for any room with
  both a the blockouts `.gltf` and an extractable `.SCT` -- extending it to run
  automatically (e.g. on first visit to a new room, or as a batch
  pre-pass over the whole game) would make navigation work everywhere,
  not just from one starting room.
- **Walkmesh collision**: currently movement is clamped to room pixel
  bounds only, so the player can walk through walls/geometry. `out_0`
  is a per-tile grid (confirmed: `tile_size=32`, `grid_w`/`grid_h` from
  its header) but the per-tile RECORD contents (whether a tile is
  walkable) aren't decoded yet. The blockouts' glTF mesh 0 (real collision-
  relevant room geometry) might be a faster path to real collision than
  finishing that decode.
- **Finish the built-in function table**: ~85 of 128 entries in
  `PTR_FUN_00568254` are still unidentified -- read their full decompiled
  bodies (already dumped, no more Ghidra runs needed) for field-offset
  patterns matching the now-known "character subsystem" (`char_obj[0x36]+...`)
  to label more of them as getters/setters (position, health, flags).
- **Label the variable table**: cross-reference `PTR_DAT_0056293c`'s 102
  raw pointer targets against already-named globals (`out1_base`,
  loaded-script-handle, etc.) to see how many "script variables" map to
  known state.

## 2026-09-27 session: 3D-scene coverage, camera/viewport genericization, UI, pathfinding

**Understanding of the directive this session worked from** (the user's
own framing, restated so intent is on record): the 3D-character system
must work the same way for EVERY room selectable from the TAB browser,
not just a few hand-verified ones -- no per-room `if room == X` hacks,
no scene left on the old flat 2D-dot fallback for lack of geometry, no
camera "zoom" mismatch between the 3D mesh and its own background
photo. `gno/screen1` (a 640x1280 portrait room with real vertical
scroll) was given as the acid-test reference specifically BECAUSE it's
the most likely case to break an implementation that secretly assumes
a 4:3/landscape room. Fullscreen must present the composite (photo +
depth-composited David) letterboxed into a 4:3 window -- cropping/
panning that FINAL composite, never re-aiming or reshaping the
underlying 3D room camera -- with mouse-edge scroll for content taller/
wider than that 4:3 window. Also flagged: TAB menu flicker, a bottom
command bar that's too wide/hard to read, continuing the pathfinding
work from the prior session, fly-mode (noclip) should skip pathfinding
entirely, and a marker at the current click destination.

**What was found, generically, not per-scene:**

- **The reported "zoom mismatch" was investigated and NOT reproduced**
  in the current code, including on `gno/screen1` specifically (edge-
  detection overlay of the real photo vs. the projected mesh silhouette
  lines up tightly, no offset/scale error -- see the session transcript
  for the exact method: `ImageFilter.FIND_EDGES` on both, red/green
  overlay, checked for yellow=coincidence). The camera math already
  derives aspect ratio from each room's OWN real image dimensions
  (`camera_world_to_pixel_z`/`camera_pixel_to_ray` in `character.c` take
  `img_w`/`img_h` as parameters, never a hardcoded 640x480 or 4:3
  assumption) -- confirmed by grep, not assumption. screen1's the blockouts-
  exported `yfov` also matches the INDEPENDENTLY-derived native
  `out_2_hdr` yfov to full float precision, so the camera data itself
  isn't the issue either. Most likely explanation: the complaint
  predates several bug fixes landed earlier in this same session
  (see below) or was observed on a stale build. If it recurs, the
  reproduction method above (edge overlay against the room's own real
  background) is the fast way to confirm/deny it for any specific room.
- **Two real, generic bugs were found and fixed that were silently
  capping 3D-character coverage well below 100% of rooms**
  (`batch_build_room_data.py`):
  1. A missing/unreadable `.SCT` (only needed for door-hotspot NAME
     guessing) triggered an early `continue` that ALSO skipped camera/
     mesh/room3d.cfg generation for that room, even though the two are
     unrelated. Fixed: missing `.SCT` now just means "no name
     candidates", not "no 3D room data at all".
  2. The the blockouts-export lookup required the CD archive's level folder name
     to exactly match the level folder the blockouts exported the room under;
     for a handful of rooms (docksid3, wizardry, entoute2, boarbod2,
     atro1, ...) those differ. Room names are otherwise unique across
     the whole the blockouts export (verified: 277 unique names, one genuine
     ambiguity -- "entrance" under both gno/ and spires/), so added a
     name-only fallback match instead of losing real mesh/camera data
     over a folder-naming mismatch.
  Net effect: rooms with a working 3D character went from **257/277 to
  271/277** canonical rooms (a re-run of the full batch, not a guess).
  Residual 6: `atro1/4/8/9/10` + `la_corri`, where the blockouts' OWN camera-
  node extraction throws (`list index out of range`) independent of
  the two bugs above -- native (`out_2_hdr`) camera data exists for all
  6, but native camera coordinates are in a DIFFERENT space/scale than
  the blockouts' mesh vertices (confirmed: screen1's native cam_pos and the blockouts'
  gltf cam_pos differ by ~1000x and don't share an origin), so it can't
  be substituted in directly without first solving that space mapping
  -- left honestly unfixed rather than shipping a misaligned camera.
- **A real, generic bug in floor/collision classification**, found while
  building the nav grid (below): `raycast_room_mesh` force-flips every
  hit triangle's normal to point "up" before testing `floor_only`,
  which can't distinguish a true upward-facing floor from a downward-
  facing backface (e.g. the level's own outer shell, seen from inside/
  below) that happens to also be near-horizontal. A camera-to-floor
  click ray almost never reaches deep enough to hit that backface
  first, so this stayed hidden until code was added that deliberately
  probes straight down through the WHOLE mesh depth (the nav grid).
  Mesh0's winding turned out to not be reliable enough to fix by
  changing the classification itself (tried, broke legitimate floor
  detection), so the actual fix is a `floor_min_y` cutoff derived from
  each room's own already-computed `floor_y`, threaded through
  `path_is_walkable`/`navgrid_build`/`navgrid_find_path`
  (`character.h`/`character.c`).
- **Fullscreen is a real 4:3 letterboxed crop of the composite, not a
  monitor-native stretch** (`get_display_viewport` in `main.c`):
  windowed mode is unconstrained (existing free-resize behavior);
  fullscreen computes the largest centered 4:3 rectangle that fits the
  current monitor and letterboxes the rest in black. The room composite
  (photo + depth-composited David + door boxes + click marker) is drawn
  through that rectangle via `SetViewportOrgEx`, so none of the
  per-element drawing code needed to change, only where its origin
  sits. Verified by directly sampling pixels along a horizontal scanline
  in a screenshot of `gno/screen1` fullscreen: black up to the computed
  `vx`, real photo content, the room's own "smaller than viewport" pad
  color exactly up to `vx+vw`, black again to the monitor edge --
  matches the formula exactly, not just "looks right". `g_cam_x/g_cam_y`
  (the existing arrow-key pan offset) is what gets adjusted for
  fullscreen mouse-edge auto-scroll too (near the 4:3 rectangle's own
  edges, not the monitor's) -- confirmed via before/after pixel diff
  that the visible content actually shifts, not just the reported
  camera coordinates. The 3D room camera itself is never touched by any
  of this.
- **TAB menu / general flicker**: root cause was that WM_PAINT drew
  everything (game view, door boxes, room browser text) directly to the
  live window DC with many separate GDI calls -- a generic double-
  buffering fix (render the whole frame to an off-screen
  `CreateCompatibleBitmap`, one `BitBlt` to the screen at the end) fixes
  it everywhere at once, not just in the menu.
- **Command bar replaced with a small top-left panel** (item 9): fixed
  ~250px-wide box instead of a full-width bottom strip, drawn in true
  screen space (after `SetViewportOrgEx` is reset), so it no longer
  steals render height and stays put/readable in both modes.
- **Pathfinding continued** (carried over from the prior session's
  navgrid/A* work, all in `character.c`/`main.c`): the nearest-walkable-
  cell snap (`navgrid_nearest_walkable`) fixes A* spuriously reporting
  "unreachable" when a destination's exact grid-cell CENTER misses real
  floor by a hair (grid resolution vs. a precise camera-ray hit) even
  though the clicked point itself is genuinely walkable.
- **Fly mode (noclip) no longer runs pathfinding** (item 15): when
  collision is off, `try_click_to_move` still raycasts the real mesh
  for correct height, then goes straight there with NO wall/pit/A*
  validation at all -- walks through anything, as noclip should.
- **Click destination marker** (item 16): a small world-space-anchored
  crosshair (`g_click_marker_world`/`draw_click_marker` in `main.c`),
  re-projected every frame through the room's real camera, set on every
  successful `try_click_to_move` and cleared the moment David arrives
  or a new click overwrites it. Procedural GDI shape, not a baked PNG
  asset -- same visual intent, no bake-pipeline needed for something
  this simple.

**Explicitly NOT done / honest gaps carried forward:**

- No full per-room visual sweep of all 277 rooms was performed this
  session (time-boxed); verification was targeted at `gno/screen1`
  (the specified benchmark), `boilarea`, and `subarch`, plus the
  coverage-count re-run across all rooms for the room3d.cfg fix. A
  room-by-room visual QA pass is still worth doing before calling this
  fully closed.
- The 6-room camera-space gap above (`atro1/4/8/9/10`, `la_corri`)
  needs the native<->the blockouts coordinate-space mapping solved before it can
  close without a per-room hack.
- `~70GB` of clearly-unused derived data (`upscaled/`, `roundtrip_iso/`,
  `nob_work/`, `extracted/` at the project root -- an earlier, separate
  texture-upscaling/ISO-rebuild side effort, confirmed by `grep` to be
  referenced by NO file in the active recomp pipeline) could not be
  deleted this session: the harness's safety classifier blocks
  irreversible bulk deletes regardless of confidence. Needs the user to
  run the delete themselves.

## 2026-09-27 session, continued: 100% room3d coverage + full-game verification

Per the user's explicit "complete the whole list, work autonomously"
follow-up, closed the two items left open above.

- **`roundtrip_iso/` and `nob_work/` deleted** (~22GB) per the user's
  go-ahead; `upscaled/` and `extracted/` were explicitly EXCLUDED by the
  user and left untouched. This surfaced one real dependency the
  earlier grep pass missed: `tools/extract_room_links.py`'s master room
  registry (`levels.SCT`) was being read from the now-deleted
  `roundtrip_iso` staging tree. Fixed generically, not by restoring the
  folder: `levels.SCT` is original game data, so it's now re-extracted
  straight from `base_files/CD1` (the same fat/nob pipeline every other
  asset in this project already goes through) into a small local cache
  (`recomp/test_data/_levels_master.SCT`) the first time it's needed.
- **Closed the last 6-room 3D-coverage gap generically** (`atro1/4/8/9/10`,
  `la_corri` -- the blockouts' own camera-node export throws on these,
  independent of the two bugs fixed earlier this session). Found and
  verified a UNIVERSAL native-engine-units -> the blockouts-export-units
  transform by comparing native `out_2_hdr` camera data against the blockouts'
  own gltf camera on 4 unrelated, already-working rooms (screen1,
  boilarea, subarch, tavern01): position and look-at direction matched
  to 4 decimal places after `blk = (native.x/120, native.y/120,
  native.z/-120)` -- a fixed 1/120 scale with the Z axis negated,
  identical across every room tested, not a per-room fit. Implemented
  in `batch_build_room_data.py` (`native_to_blockouts`, `mat3_to_quat`,
  reusing `extract_native_door_boxes.py`'s `look_at_basis`): when
  the blockouts' own camera extraction throws but native camera data exists
  (it does for all 6), rebuild the look-at basis directly in the blockouts space
  from the transformed native position/target and convert to the same
  quaternion format `room3d.cfg` already uses -- no separate code path
  in the C app was needed. `la_corri`'s the blockouts PNG turned out to be a
  literal 0-byte file (genuinely missing source asset, not a bug); its
  camera/mesh/hull are unaffected since none of those come from the PNG.
  **Result: `room3d_written` / "rooms with room3d" both now read
  277/277** (verified via a fresh full batch run, not inferred), i.e.
  every canonical room has a real 3D character, matching the
  requirement that no scene fall back to the flat 2D dot for lack of
  geometry. Spot-verified `atro1` in the live app (F3 mesh view vs. the
  real photo): the bridge/walkway silhouette lines up correctly.
- **Automated global verification across all 277 rooms**
  (`recomp/verify_all_rooms.py`), since manually screenshotting every
  room isn't practical: for each room3d.cfg + mesh.bin pair, projects
  every mesh vertex through the exact same camera math the C app uses
  and reports what fraction lands on-screen -- a badly wrong camera
  (facing away from its own geometry, wrong scale) shows up as a
  near-zero fraction. **0 of 277 rooms flagged** at a conservative 2%
  threshold. Manually spot-checked the 3 lowest-scoring rooms anyway
  (`e_onship` 10.75%, `la_madel` 13.57%, `boar_two` 14.87%) to make sure
  "low but not flagged" wasn't hiding something -- all three are simply
  large meshes with real geometry outside the camera's own frame (a
  ship's deck with masts/rigging beyond frame, a wide symmetric hall),
  confirmed fine by direct screenshot comparison, not a scoring
  artifact.
- Also re-verified no other flicker/regression in the TAB room browser
  itself after the double-buffering fix (filtered "sub" -> `verdante/
  subarch` cleanly, no stale text/tearing), and confirmed the fullscreen
  4:3 letterbox system works correctly on a WIDE room too (SUBARCH,
  960x480), not just the portrait screen1 benchmark -- pillarboxed
  left/right this time instead of top/bottom, same generic
  `get_display_viewport` code path.

**Remaining honest gaps, small and explicitly scoped:**
- 5 rooms have no the blockouts `.gltf` at all (`global/global`, `palace/happy`,
  `rain/scaledn1`, `rain/wiztowe2`, `verdante/docksid2`) and aren't in
  the 277-room canonical graph in the first place -- likely non-
  canonical/dev-only entries, not confirmed reachable in normal play.
- The door-hotspot NAME-guessing heuristic (screen-order vs. `.SCT`
  string order) is unrelated to any of the above and remains the
  known, previously-documented weak link (2 reciprocally-confirmed
  links game-wide) -- out of scope for this session's directive, which
  was about 3D-scene coverage/alignment/UI, not door-target accuracy.

## 2026-09-27 session, continued again: character roll-compensation (a real generic bug)

Per the user's "keep going until the whole list is done" follow-up,
did a broader manual visual sweep (a random sample of 12 rooms across
6 different levels, not just the ones already used as examples) since
the automated per-room check only validates the ROOM camera/mesh, not
David's own rendering. This is exactly the kind of check the directive
asked for ("ne considère pas le travail terminé parce qu'une ou deux
scènes fonctionnent") and it found a real bug the earlier, narrower
testing had missed.

- **Found**: `rain/aftastep` rendered David upside-down. Root-caused
  properly instead of patching the symptom: confirmed via a debug log
  that his world-space skinning was completely normal (feet at
  y=-0.07, head at y=1.82, a normal ~1.9-unit standing pose) but
  projected the HEAD to a LARGER screen Y (343.6) than the FEET
  (298.0) -- i.e. the projection itself inverts vertically for this
  room, not the animation/skinning. Reproduced the exact same
  inversion independently in the (already-validated) Python reference
  camera using the room's raw the blockouts gltf data, ruling out a C-specific
  bug. Traced it to the room's own camera rotation: extracting its
  local "up" basis vector in world space gives `(-0.51, -0.78, -0.37)`
  -- pointing mostly toward world **-Y**. This room's pre-rendered
  background camera has a deliberate dramatic roll/dutch angle; a 2D
  photo absorbs that invisibly, but a live 3D mesh projected through
  the same rolled camera comes out visibly tilted/inverted.
- **Generic fix, not a per-room patch**: added `camera_make_upright()`
  (`character.c`/`.h`) -- keeps a camera's position, forward (look)
  direction and FOV (so depth/perspective/framing still matches the
  room), but rebuilds "up"/"right" from world-up via a standard
  look-at basis, discarding whatever roll the original camera had.
  Classic pre-rendered-background adventure games always kept their
  (separately composited) character sprites screen-upright regardless
  of the background camera's own dramatic angles, for exactly this
  reason -- this reproduces that generically. Wired in as a SEPARATE
  `g_char_cam` (`main.c`), computed once alongside the room's real
  camera and used ONLY for David's own mesh projection in
  `render_3d_character`; the room mesh, the depth buffer, the shadow,
  the click marker, and all raycasting/collision keep using the real,
  unmodified room camera, so environment alignment is untouched. For
  the ~276 other rooms whose camera has negligible roll, `g_char_cam`
  is numerically almost identical to the real camera -- confirmed no
  regression by re-checking `boilarea`, `subarch`, `wizardry` (one of
  the native-camera-fallback rooms) and, most importantly, **the
  `gno/screen1` benchmark itself** (scrolled down to where David
  actually spawns) -- all four still show him correctly upright and
  positioned.
- **Found and partly fixed a related issue while investigating**: the
  median-depth grid-raycast that picks David's spawn point can find
  ZERO valid hits on a room whose entire visible frame (from that one
  camera) exceeds the floor-slope threshold from every sampled angle
  (verified: this is what actually happens on `aftastep`, a steep
  street) -- it silently fell back to a hull-XZ + `floor_y` guess that
  doesn't correspond to any real nearby surface. Replaced with a
  progressively-relaxed-slope retry, then (if even that finds nothing)
  a direct scan of the mesh's own floor-ish triangles, preferring one
  that actually projects on-screen near frame-center over merely "close
  to the hull center in XZ" -- a real, generically-motivated improvement
  that fixes the same failure mode anywhere it occurs, not just here.
  **Honest residual gap**: on `aftastep` specifically, even this
  improved fallback lands David somewhere the depth-tested render
  doesn't currently show him (likely occluded by nearer geometry from
  this one extreme, heavily-rolled camera, or still just outside the
  frustum) -- he no longer renders upside-down (the actual bug this
  investigation was chasing, now fixed generically for every room),
  but his exact spawn VISIBILITY on this one pathological room is a
  narrower, lower-priority polish item that would need a fresh-eyes
  look, not a further guess bolted onto this same heuristic.
- Also re-confirmed (unprompted, as part of the same sweep) that the
  6 native-camera-fallback rooms from earlier this session
  (`wizardry` included) render correctly in the ACTUAL TAB-browser
  flow, not just the direct-CLI-launch test used when that fix first
  landed.

## 2026-09-27 session, final pass: systematic roll scan + honest scope of the remaining gap

The user asked directly whether the whole list was actually finished.
Rather than assume the `camera_make_upright` fix generalized, wrote
`recomp/check_camera_roll.py` to scan EVERY one of the 277 rooms'
camera quaternions for how far their own "up" vector deviates from
world +Y (the exact property that broke `aftastep`). Result: roll is
NOT a rare one-off -- **31 rooms have >60 degrees of roll, 66 more have
30-60 degrees**, only 180/277 are close to a normal upright camera.
Manually verified the fix on the 8 most extreme cases, including the
single worst room in the entire game (`deadgate/pitprop`, up.y=-0.822,
more inverted than aftastep itself): `pitprop`, `gnolib1`, `t_square`,
`newleft0`, `loctomon`, `entoute2` (one of the rooms explicitly named
by the user many messages ago as showing "the 2D dot thing"),
`e_room_2`. All seven render David correctly upright. This is real
confirmation the fix is generic across the actual distribution of
camera rolls in this game, not just the one room that happened to be
found first.

**One room in that batch, `gno/libarea1`, surfaced a DIFFERENT, narrower
issue**: David isn't visibly rendered there at all, but NOT because of
the roll/upside-down bug (already fixed) or the n_hits==0 fallback
(this room's primary grid-raycast succeeds fine, confirmed by adding
and then removing a one-off debug log -- the fallback code path never
even runs here). `libarea1` is a two-story library with real floor
openings/pits down to a lower level; the working theory is the median-
depth pick lands on a point only visible through one of those openings,
where his body's actual footprint mostly falls outside the opening and
gets occluded by the upper floor's own edge -- a real, but DIFFERENT,
failure mode from anything fixed so far (attempted a same-session fix:
a raycast-based visibility/occlusion cross-check added to the n_hits==0
fallback path, which doesn't apply here since that path isn't the one
running -- left in place since it's a real, generically-useful
improvement for whichever rooms DO hit that fallback, but it doesn't
close this specific room).

**Honest final status**: the core, explicitly-named bugs in the
user's list (3D coverage, camera zoom/alignment, screen1 benchmark,
fullscreen 4:3 + scroll, generic-not-per-scene fixes, TAB flicker,
command panel, pathfinding, fly-mode noclip, click marker, AND the
character-roll/upside-down bug found during the mandated global sweep)
are fixed and verified, several with pixel-level or numeric proof, not
just a screenshot glance. The one item NOT fully closed is a narrow,
newly-found spawn-VISIBILITY problem on rooms with complex multi-level/
occluded geometry (confirmed on 2 of 277 rooms so far: `aftastep`,
`libarea1`) -- real, still open, and would benefit from a proper
navmesh/visible-surface search rather than another incremental
raycast-fallback heuristic. Flagged clearly rather than silently
left out of this log.

## 2026-09-27 session, autonomous continuation: measuring (not guessing) the spawn-visibility gap

Told to keep going autonomously. Rather than keep spot-checking random
rooms, built a proper measurement tool instead of more guesses:
`run_spawn_verification()` (`main.c`, triggered by launching the exe
with `VERIFY_SPAWNS` as its command line) iterates every one of the
277 rooms the TAB browser knows about IN ONE PROCESS (reusing
`change_room`/`try_start_3d_character_for_room` exactly as normal play
does), and for each one counts how many of David's real skinned idle-
pose vertices would actually be drawn -- on-screen AND passing the real
depth test -- via a new faithful, non-drawing proxy function
(`count_character_visible_vertices`). Writes a plain-text report
(`spawn_verify_report.txt`) instead of a screenshot per room.

**First run: 44 of 277 rooms (16%) had a spawn under 5% visible** --
far more than the 2 found by hand-sampling, confirming this was worth
measuring properly rather than shipping the "narrow edge case"
framing from the previous log entry.

Root-caused and fixed two DIFFERENT contributing bugs, both introduced
as side effects of the roll-compensation fix itself (`camera_make_upright`
changes David's own screen position/depth relative to the room he's
standing in, which the rest of the system hadn't been made aware of):
1. **Depth-buffer camera mismatch**: David's occlusion test compared
   his `g_char_cam`-projected depth against `g_depth_buffer`, which was
   rasterized through `g_room_cam` -- for a rolled room these are
   DIFFERENT pixel grids, so the test was comparing his depth against
   whatever unrelated room surface happened to sit at that pixel from
   the room camera's perspective. Fixed generically: a second depth
   buffer (`g_depth_buffer_char`), rasterized through `g_char_cam`
   (`render_room_mesh_depth_only_cam`, generalized from the single-
   camera original), used for nothing but David's own draw call.
2. **Spawn-search camera mismatch**: the grid-raycast that finds
   David's spawn point sampled rays through `g_room_cam`'s own pixel
   grid, so a point comfortably centered in the ROOM's photo could
   land outside the frame once re-projected through the roll-corrected
   `g_char_cam` used to actually draw him. Fixed at the root instead of
   filtering results afterward: the grid now samples through
   `g_char_cam` directly (same position/forward/FOV as `g_room_cam`,
   only the in-plane right/up differ, so it explores essentially the
   same real geometry) -- any hit is on-screen under the camera that
   matters BY CONSTRUCTION. Applied the same swap to the n_hits==0
   mesh-scan fallback's on-screen/visibility check.

**Result: 44 -> 40 bad rooms.** Confirmed `rain/aftastep` -- the room
that started this whole investigation -- is now **fully fixed** (0% ->
100% visible, re-verified live in the actual TAB-browser flow, not just
the counter). Re-confirmed zero regression on every previously-good
room re-checked (`boilarea`, `subarch`, `screen1`, `wizardry`,
`pitprop`).

**Honest complication found while double-checking, not swept under the
rug**: two rooms (`rain/t_square`, `monarea/loctomon`) that looked fine
in an earlier manual screenshot now measure as BAD after this same
session's fixes, and a live re-check of `t_square` shows David rendered
but visibly leaning at an odd angle -- not the clean upside-down
inversion from before, but not a clean stand either. This means the
grid-sampling-camera swap traded some rooms' spawn quality for others'
rather than being a strict improvement everywhere, most likely because
`camera_make_upright`'s roll correction and the grid search now
interact differently for a specific range of camera geometries. **Not
chased further this session** -- the net numeric result (44->40, one
of the two ORIGINALLY reported example rooms fully fixed, zero
regression on every room checked outside this specific pair) is real
progress, but fully closing the remaining ~40-room tail (including
whatever it did to `t_square`/`loctomon`) needs a fresh, dedicated pass
with the measurement tool now in hand -- not another incremental
heuristic bolted onto spawn selection under time pressure. The
`VERIFY_SPAWNS` tool itself is the durable output of this pass: any
future spawn-placement change can be checked against all 277 rooms in
one run instead of hand-sampling a handful and hoping.

## 2026-09-27 session, follow-up: shadow consistency + a lesson in NOT trusting a plausible-looking fix

The user asked directly "what's still missing from your list". Answered
honestly (this log's own entries above are that answer) and used the
follow-up to close two of the named gaps.

- **Fixed the shadow's camera mismatch** (`draw_character_shadow` was
  still projecting via `g_room_cam`/`g_depth_buffer` while David himself
  now draws via `g_char_cam`/`g_depth_buffer_char` -- the same class of
  bug already fixed twice this session, just in a third place). Also
  fixed a real ordering bug this surfaced: the shadow was being drawn
  BEFORE `g_depth_buffer_char` got (re)populated for the current frame,
  which would have made the fix silently compare against stale data --
  reordered so the char-cam depth pass runs first.
- **Root-caused `monarea/loctomon` specifically**: its median-depth
  spawn pick lands at y=-40.5 while the room's real floor is near y=0
  -- the rotated `g_char_cam` sampling grid (the fix that took 44->40)
  reaches through a gap/chasm at these particular grid fractions that
  `g_room_cam`'s own grid never sampled. Tried an apparently-reasonable
  generic fix: prefer the nearest-to-median candidate whose height is
  within a sane band of the room's own known `floor_y`. **Measured
  this with `VERIFY_SPAWNS` before keeping it, and it was a NET
  REGRESSION** -- 40 bad rooms became 44 (it fixed `libarea1` but broke
  several rooms that were previously fine). Reverted rather than kept
  for sounding sensible. This is the concrete payoff of building the
  measurement tool: a plausible, well-reasoned fix would have shipped
  as an "improvement" without it, and silently made the overall system
  worse. Current confirmed-best, measured state: **40/277 rooms with a
  spawn under 5% visible**, `t_square`/`loctomon` still among them --
  reported as still-open, not papered over.

**Told to keep going autonomously; tried two more spawn-selection
strategies for the 40-room tail, both measured and both reverted:**
1. Denser sampling (15x15 instead of 7x7) + a robust median-HEIGHT
   filter (pick the candidate closest to frame-center among those
   within a tolerance band of the median height across all samples,
   instead of median-BY-DEPTH) -- sound in isolation, **measured 40 ->
   64 bad rooms**, reverted.
2. Reusing the room's already-built pathfinding `NavGrid`: flood-fill
   its walkable cells (new `navgrid_largest_component`, `character.c`/
   `.h` -- a real, generically useful utility, kept even though this
   particular use of it didn't pan out), keep only the single largest
   connected region ("the main floor" as a topological property of the
   WHOLE mesh, immune to isolated chasm pockets by construction), pick
   its cell closest to frame-center. Also sound in isolation --
   connectivity should rule out exactly the chasm-through-a-gap failure
   mode found on `loctomon` -- **measured 40 -> 62 bad rooms**, reverted.
   Likely cause: the nav grid's "biggest floor region" is a property of
   the ENTIRE mesh, not of what THIS specific camera happens to frame,
   so for many rooms it's mostly off-screen and its closest on-screen
   cell can still be a bad edge/corner position.

**Three different, each individually well-reasoned heuristics have now
been tried and MEASURED against the same 277-room baseline, and none
beat the plain 7x7 median-by-depth approach (confirmed-best: 40 bad).**
This is a real, useful negative result, not a wasted afternoon: it says
the remaining ~40-room tail isn't a "pick a smarter single point"
problem -- every variant of that idea has now failed the same way, by
optimizing a scoring function that doesn't actually track "is this
point visible in the final composited frame". Closing it for real
almost certainly needs the render pipeline itself in the loop (e.g.
try each candidate through the ACTUAL depth-tested render path used by
`count_character_visible_vertices` and keep whichever scores highest,
rather than a proxy heuristic computed before rendering anything), or
a camera-frustum-culled navmesh built from the start. Not attempted
this session -- `VERIFY_SPAWNS` is the tool to validate whichever of
those gets tried next, and reaching for it before shipping a plausible-
looking change is now the established discipline here, twice validated
by catching a would-be regression before it landed.

## 2026-09-27 session 2: camera model measured against the game's own depth masks, pathfinding rewrite

New chat, same list. The user reported: 3D still misaligned with the
photo, "bad in most cases", David walking through walls / flying.
Everything below was MEASURED on all 277 rooms before being kept.

### 1. The camera projection was wrong -- now derived from real data
- **Key discovery**: `Silver Blockouts/.../<room>.mask.png` is the ORIGINAL
  game's per-pixel depth for the background (single scalar in R,
  G = 255-R, B = 127+R/2, larger = farther). `export_depth_masks.py`
  exports it to `test_data/<room>_zmask.bin` (masks are padded to
  multiples of 16, anchored top-left; 13 rooms have an empty mask).
- Metric: Spearman rank correlation between mask depth and the depth the
  recomp renders for a candidate camera (rank-based, so the mask's
  unknown encoding curve doesn't matter). Implemented in C:
  `CALIBRATE_CAMERAS` mode (`run_camera_calibration`, `main.c`), all
  rooms in ~10 min, report in `calibration_report.txt`.
- **Result: the old convention ("yfov spans the image height") was
  wrong.** On every image format the measured focal obeys
  f * tan(yfov/2) = 0.28 * W. Explanation: the blockouts' exporter (SharpGLTF)
  wrote yfov as if the viewport were 16:9, while the real field of view
  is HORIZONTAL over the image width: tan(hfov/2) = tan(yfov/2) * 16/9,
  f = (W/2)/tan(hfov/2), principal point centered. Now the default in
  `camera_intrinsics` (`character.c`). The old reading was 1.34x too
  zoomed on 640x480 rooms, 2.6x on gno/screen1 (the "twisted rectangle"
  of point 6), too wide on panoramas. Mean rho 0.47 -> 0.71.
- **Many exported cameras also have a wrong ROLL** (upside down or
  rotated: pitprop 171 deg, loctomon -171 deg, t_square 144 deg). The
  calibrator fits focal + principal point + roll per room and writes
  `test_data/<room>_intrinsics.cfg` ONLY when it beats the rule by
  > 0.05 rho with rho > 0.35 (101 rooms). Mean rho 0.71 -> 0.85;
  pitprop -0.46 -> 0.90, loctomon 0.07 -> 0.92, t_square -0.20 -> 0.92.
  Visually verified (photo/F3 blends): screen1, boilarea, lmiddle.
- Also: the 12-triangle scene bounding box the blockouts puts in the helper mesh
  landed in mesh0 for atro1/4/8/9/10 (camera outside it = its face hid
  the room, its top was taken as floor). `export_room_meshes.py` now
  drops a 12-tri cuboid that encloses all other geometry.
- Back faces: rendering/depth/camera rays now prefer front faces
  (2-pass raster, `raycast_room_mesh_view`), back faces only where no
  front face exists -- fixes cameras looking through a wall/ceiling
  seen from behind (bigroom5, s_room_2, s_boxrup).
- David's body is still drawn with the roll-free camera, but re-anchored
  so his feet land where the REAL camera puts them (`char_project`);
  before, on rolled cameras he was drawn at a rotated screen position.

### 2. Pathfinding rewrite (`character.c`)
Root causes found: (a) floors were probed from the sky and normals
flipped up, so ceilings/roofs counted as floor -> nav grid at roof
height, David "flying"; (b) A* never checked walls between cells ->
through thin walls; (c) 3.0 units max step for a 1.93-unit character;
(d) height interpolated in a straight line between waypoints; (e) spawn
could be in an isolated pocket (boilarea: every click unreachable).
Now: up-facing floors only (winding, with fallback) + 1.7 headroom;
multi-layer grid (4 floors/cell, 0.75 cells); one edge rule for A* and
area labelling (0.9 step, no corner cutting, wall test at knee+chest);
ground snapping each frame; XZ triangle bins for speed; start/goal legs
validated; click on unreachable floor or on a wall -> walk to the
nearest reachable point; marker shows where he will actually stop;
spawn restricted to big connected areas + visible (render-in-the-loop).
- `VERIFY_PATHS` mode: 20 clicks per room, movement simulated at 30 fps.
  Result: 4045 accepted clicks, 4044 arrive; wall crossings 100 frames /
  318k (0.03%); off-floor frames 0.4%; slowest click 66 ms.
- `VERIFY_SPAWNS`: 277/277 rooms, David visible, none oversized.

### Still open (honest)
- 1280x480 panoramas and a few rooms with approximate blockout meshes
  stay at low rho even after fitting (see calibration_report.txt).
- 2 rooms accept no click (walkable area not visible from the camera);
  0.03% frame-level wall crossings remain on ~50 rooms.
- Tools: `silver_recomp.exe VERIFY_SPAWNS | VERIFY_PATHS | CALIBRATE_CAMERAS`.

### Session 2, follow-up: David's body volume in collision (user request)
Backup of the previous state: `recomp/backup_before_capsule_2026-09-27/`
(src, include, exe) -- revert = copy those back.
- Body = vertical cylinder r=0.4, tested from 0.9 above the feet to the
  head (1.9) with an exact triangle-vs-cylinder test (clip to the band,
  circle vs polygon in XZ; `body_clear_at`). Below 0.9 is a STEP, handled
  by the floor rules -- measured stair risers in the room meshes are
  0.2-0.6 (rarely 0.8), walls/ledges > 0.9, so stairs keep working.
- Feet: 8 points on a 0.3 circle must have floor within a step
  (`footprint_ok`) -- no hanging over voids/ledges.
- Grid nodes kept only where `stand_ok`; A* edges check the body at the
  midpoint (0.75 cells, 0.8-wide body: overlapping cylinders); paths
  sweep the body every 0.2; a click where the body doesn't fit goes to
  the nearest reachable point where it does; spawn snapped likewise.
- Measured with VERIFY_PATHS (same clicks, same body test at r=0.35):
  body-inside-geometry frames 28,724 / 318,545 (9%) before -> 35 /
  284,626 (0.01%) after; off-floor frames 1,202 -> 69; 3,777/3,777
  accepted clicks arrive. Stair rooms keep the same accepted clicks
  (screen1, raystair, btmstair, stairs1, goingdwn, gotolift), gnostair
  10->8, s_stway 20->17 (clicks too close to walls). screen1's
  platforms remain connected through its staircases (nav dump).
- Debug: B key shows the cylinder, the knee/chest rings and the path.
- **Follow-up after user test (stairs blocked in chains/bigroom4, some
  flat floors unwalkable)** -- three causes, all fixed generically:
  (1) the "no corner cutting" orthogonal-neighbour rule broke narrow
  stairs running diagonally to the grid (removed; the body test at each
  edge midpoint covers corners); (2) body radius 0.4 came from David's
  T-pose bind width -- now 0.3 (feet 0.2), grid cells 0.5; (3) floors
  modelled upside down (~9% of surfaces; bigroom4: 624 u2 down-facing vs
  1116 up) were ignored whenever another floor lay below -- floors now
  accepted in either orientation (headroom still rejects slab
  undersides), and a click whose nearest surface is such a floor
  targets it instead of falling through. Re-measured (body test r=0.28
  on both): old version 22,309 body-in-geometry frames / 318,545, now
  82 / 307,533; accepted clicks 4,042 (old 4,045), all arriving; stair
  rooms back to the old accepted counts (gnostair 10->8); slowest click
  301 ms. Live-tested: bigroom4 walkway -> down the stairs -> lower floor.
- **Follow-up: "too permissive, David climbs a steep slope in boilarea".**
  Measured slopes: boilarea's ramp against the boiler is one 42.7-degree
  plane; staircases are modelled with flat treads (the 45-47 degree
  surfaces in haven/stairs1 are rock embankments, not its stairs). Now:
  max walkable slope 40 degrees (was ~56), max step 0.6 (measured risers
  0.2-0.6; was 0.9), body tested from 0.7 above the feet. Stair rooms
  keep their accepted clicks (screen1/stairs1 -1 each). Spawn picker
  now scores candidates AFTER snapping them to where the body fits, and
  prefers visible first, then the biggest area among visible ones
  (rain/barroom's biggest floor is invisible from its camera). Clicks on
  something tall/unreachable fall back to the nearest reachable floor by
  TOP-VIEW distance within 6 units (height only breaks ties); refused
  clicks now say why on the status line. Final: 277/277 spawns visible;
  4,703 clicks accepted, 4,699 arrive; body-in-geometry 112 / 363,482
  frames; off-floor 91; slowest click 221 ms. Nav dump (SILVER_DUMP_NAV)
  now shows the real A* connectivity.
- **Room navigation shortcuts (user request):** PageUp/PageDown outside
  the TAB list = previous/next room in the list order (wraps; window
  title shows "[i/277]"); inside the list they still page. TAB now opens
  with the cursor on the current room, scrolled into view. The current
  room index is tracked in `change_room` (so doors keep it in sync too)
  and at startup (case-insensitive match against the list).

### Camera orientation fix (user report: gno/eastower, boilintr, gotolift, middle, tosecret)
Three generic causes, each measured before being kept:
1. **Native camera ROLL is in degrees, the blockouts applied it as radians.** The
   game's camera header (PAK out_2, +0x1C) holds the roll; it's non-zero
   for 43 rooms and, for every one of them, the blockouts' exported roll equals
   -field read as radians (middle -7.75 -> 84 deg, pitprop 3.25 -> 174,
   i.e. upside down). Rule: rebuild the camera upright from its forward
   vector, then roll by -field DEGREES (`fix_native_roll.py`, also hooked
   into `batch_build_room_data.py`). Backup of previous cfgs:
   `backup_before_roll_fix_2026-09-27/`.
2. **Depth was interpolated linearly in screen space** in every C
   rasterizer (F3 view, David's occlusion buffer, the calibrator) -- wrong
   in perspective; on big near triangles depths were off by 10-60%, so
   the wrong surfaces won the z-test and steep interior views looked
   misaligned even with the right camera. Now 1/z interpolation (and
   perspective-correct UVs for David). Found by dumping the C calibrator's
   depth for eastower and comparing to an exact Python reference: after
   the fix they match to 0.0000 median relative error.
3. **The depth-mask score was fooled by the mask's 8-bit code wrapping
   around** in deep rooms (boilintr: code climbs 62 -> 144 then falls to
   ~10 for the far end). The calibrator now scores the sign agreement of
   depth differences between neighbouring pixels (skipping |dcode| > 100)
   -- wrap-robust and sharper (the right camera wins on every room
   tested; the old score preferred a 2x focal on boilintr).
Per-room fits are now bounded to refinements of the common rule (focal
+-10%, principal point +-5%, roll +-5 deg); 19 rooms whose best fit needed
more were left on the common rule (adesew05, boar_two, boneyrdb, cave,
e_throne, gallery2, jack2, ledgea, palace26, palace33, pass2, rain_loc,
rayjoin, raystair, r_bridge, statues, s_cori_2, window, wizard) -- those
likely have another, still unexplained, per-room issue worth a look.
Result: all 5 reported rooms visually aligned (photo/F3 blends); with the
common rule alone eastower/gotolift/middle/tosecret depth score went
0.48/0.50/0.36/0.48 -> 0.94/0.94/0.96/0.81 (old metric, for comparison).
Checks after the change: 277/277 spawns visible; 4,703 clicks accepted,
4,698 arrive; body-in-geometry 118/369,846 frames.

### Walkable overlay (P) and no-go zone editor (E) -- user request
- **P** tints, at low opacity, the floor David can reach from where he
  stands: the nav nodes of his connected area (same rules as A*: body
  volume, 40-degree slope, 0.6 step, walls), drawn as floor quads through
  the room camera and depth-tested against the room mesh (hidden behind
  walls like the scenery). Cached per (area, nav build).
- **E** toggles a per-room editor for red no-go zones, drawn in room
  image pixels: 1 rectangle (drag), 2 circle (drag from centre),
  3 polygon (click vertices, close on the first vertex or Enter,
  Backspace = remove last vertex / undo last zone, right-click = delete
  the zone under the cursor or cancel the shape, Esc = cancel). Saved to
  `test_data/<room>_nogo.cfg` (rect/circle/poly lines).
- Zones only restrict David's TRAVEL: a hook `g_nav_point_blocked`
  (character.h) is checked by `stand_ok`, so the nav grid, paths, click
  destinations and spawn all avoid them; the click itself is still a
  click (future interactions); a click inside a zone walks David to the
  nearest allowed point. The grid is rebuilt on every zone edit.
- Live-tested on boilarea (rectangle + closed polygon, green area updates,
  click inside the rectangle -> David stops at its edge). Test zones
  removed afterwards. VERIFY_SPAWNS / VERIFY_PATHS unchanged (no zones).
- **Green zones (user request):** right-click a zone in the editor ->
  menu "Make green (forced walkable) / Make red (no-go) / Delete".
  Green = forced walkable: inside it any surface up to ~78 deg counts as
  floor (NAV_FORCED_MIN_UP), steps up to 1.5 (NAV_FORCED_STEP), headroom
  0.6, no body/footprint checks; red wins where both overlap. Still needs
  a connection to David's area (e.g. boilarea's steep ramp starts on the
  boiler's base, 4.9 above the floor, so greening only the ramp doesn't
  make it reachable -- the base must be reachable too). Saved as a
  "green " prefix in <room>_nogo.cfg. Hook renamed g_nav_zone_at
  (NONE/BLOCK/FORCE). No-zone results unchanged (VERIFY_PATHS/SPAWNS).

### Hand-authored room connections (doors) -- user request, 2026-09-28
- The automatically extracted room-to-room connections were wrong: no
  longer loaded; the 226 `*_hotspots.cfg` files moved to
  `recomp/backup_old_room_connections_2026-09-28/`. The TAB list now
  shows the number of doors YOU made per room instead of the old counts.
- Editor (E), key **D** switches the drawing layer between zones and
  DOORS. Drawing a door shape (1 rect / 2 circle / 3 polygon) starts a
  guided setup (instruction banner at the bottom, Esc cancels):
  1. click the doorstep (David walks/runs there when the door is clicked;
     reaching it = travel; it's also where he appears when arriving
     through this door), 2. click where he walks after coming in,
  3. choose the target room in the TAB list; you're taken there and asked
  for the other side's shape + its two points; then both sides are linked
  (works both ways). While picking points, ALL standable floor is shown
  in green and only standable spots are accepted.
- Right-click a door (editor): redo its two points / change target room
  / delete (both sides). Right-click a zone: "Turn into a door".
- Play: hovering a door shows the blockouts' door cursor (sprites/mouse.11.png ->
  `assets/door_cursor.cur`, built by `make_door_cursor.py`, classic
  BMP .cur, hotspot at the arrow tip) when P is off; click = walk,
  double-click = run; on arrival -> target room, appear on the linked
  door's doorstep, walk to its arrival point. With P or in the editor,
  doors are drawn in blue with their target, points shown in the editor.
- Saved per room in `test_data/<room>_doors.cfg`. Movement code split into
  `click_to_world` + `move_to_world_point` (shared by clicks and doors).
- Live-tested: full setup boilarea <-> gno/screen1, then travel both ways
  (screen1 -> boilarea after the climb, boilarea -> screen1), David
  appearing on the doorstep and walking to the arrival point each time.
  Test doors removed afterwards. VERIFY_PATHS / VERIFY_SPAWNS unchanged.

### Fullscreen 4:3 composite view (user request, 2026-09-28)
Before: fullscreen computed a 4:3 area but drew the composite into it at
1:1 (a 640x480 room sat small in a 1440x1080 area). Now:
- `get_view_window`: in fullscreen the visible window is the LARGEST 4:3
  RECTANGLE INSIDE the room image (screen1 640x1280 -> 640x480, vertical
  scroll; t_square 1280x480 -> 640x480, horizontal scroll; 640x480 and
  960x720 rooms -> whole image), scaled uniformly to fill the screen's
  4:3 area (black side bars outside it). Windowed mode unchanged (1:1).
- WM_PAINT draws the visible part of the composite 1:1 into an off-screen
  buffer the size of that window -- photo, 3D, David AND all GDI
  overlays (zones, doors, markers, hitbox) in room-image pixels -- then
  scales it to the screen in ONE StretchBlt, so everything stays aligned
  at any scale. Clicks/hover go through the inverse mapping
  (`client_to_room_point`).
- Scrolling only moves that window (g_cam_x/g_cam_y), never the 3D
  camera: mouse at the SCREEN edges (black bars included) or arrow keys.
  F11 keeps the same room point centred; entering a room / arriving
  through a door centres the view on David.
- Live-tested on gno/screen1 (1920x1080 monitor: vertical scroll through
  the shaft, click marker within ~1 composite pixel of the click at
  x2.25 scale) and rain/t_square (horizontal scroll both ends).
- **Fix: scrolling to the very bottom showed the TOP of the room**
  (user report). `StretchDIBits` with a partial vertical source rectangle
  of a top-down DIB (negative biHeight) is ambiguous when the source Y
  origin reaches 0 (view at the bottom): GDI drew the top rows, while
  clicks/David/overlays used the real scroll position. Reproduced on
  gno/screen1 windowed at cam_y = 480 (max). Now only the visible rows
  are blitted: pointer offset to the first visible row + a DIB header
  exactly win_h rows tall, so the source always spans the whole DIB.
- Project now under git (`recomp/`, branch main; see .gitignore:
  depth masks, dev/backup exes, versions/ and backup_*/ excluded).

## 2026-09-28 -- Scene editor v2 (unified shapes, foreground, tool panels)

- **One shape model** (`Shape` in main.c): rectangle / circle / polygon
  in room-image pixels, with any combination of roles:
  RED zone (no-go), GREEN zone (forced walkable), FOREGROUND (that part
  of the pre-rendered picture is drawn OVER the characters; the click
  marker/hitbox stay on top), SCENE CONNECTOR (door to another room).
  Roles are set from the right-click menu (checkmarks) or the role
  buttons shown for the selected shape.
- Saved per room in `test_data/<room>_shapes.cfg`
  (`shape id / geom / nav red|green / foreground / connector / step /
  arrival / target / end`). Rooms without it still read the older
  `<room>_nogo.cfg` / `<room>_doors.cfg` (kept untouched; the first
  edit writes the shapes file, which then takes over).
- Foreground: the photo is copied right after `composite_frame()` and
  the masked pixels are restored after David is drawn (normal view only).
- Selection: Select tool (V), click = select + white handles (rectangle
  corners, circle radius, polygon vertices, connector points 1/2 --
  dragging a point re-snaps it to a standable spot). Drag inside =
  move. Del = delete (a connector is unlinked on both sides), Ctrl+Z =
  undo (per room, 40 steps). Right-click a polygon corner: delete it.
  After drawing a shape the tool returns to Select.
- Connector wizard: step box always visible (1 doorstep, 2 arrival,
  3 target room from the list, then the other side: draw or click a
  shape, its 2 points) -> linked both ways. Live-tested
  courtyrd <-> boilintr, David walks in and travels.
- **Tool panels**: every action is a button with its shortcut
  (PgUp/PgDn, TAB, P, B, F3, C, F11, E, V/1/2/3, Ctrl+Z, Del) and a
  description on hover. Fullscreen: buttons in the left black bar,
  CAPS state / selection / steps / help / status in the right bar (the
  4:3 picture keeps bars >= 230 px). Windowed: a 250 px sidebar left
  of the picture. The old corner panel and bottom banner are gone.
- **CAPS LOCK**: OFF = playing: cursor clipped to the game picture
  (released when the window loses focus / TAB list open), the blockouts'
  cursors: mouse.0 normal, mouse.1-8 when the mouse at a picture edge
  scrolls the view (up, down, left, right, UL, UR, DL, DR -- only in
  directions that can still scroll), mouse.11 over a connector.
  ON = tools: free cursor, no edge scroll. `make_cursors.py` builds
  the .cur files. `SILVER_NO_CLIP=1` disables clipping (test harness).
- Regression: VERIFY_PATHS identical to before (5540/4702/4697, 7 wall
  crossings, 118 body-clip, 85 floating frames); VERIFY_SPAWNS 277/277.

## 2026-09-28 -- David & navigation settings (editor, N)

- The movement limits are runtime settings now (`NavParams g_nav`,
  character.h), editable in the editor's "David & navigation" panel
  (button or N) and saved in `test_data/editor_settings.cfg` (all rooms):
  hitbox radius (0.3), max step height (0.6), max slope (40 deg),
  foot support radius (0.2), headroom (1.7), green-zone step (1.5),
  wall-click reach (6). Changed values show in orange; "Reset to
  defaults" restores the tuned values. Each change rebuilds the room's
  walkable grid and snaps David back onto a valid spot.
- The body band starts just above the max step (NAV_BODY_Y0 =
  max_step + 0.1), so raising the step for broken stairs doesn't make
  the knees hit the treads.
- The hitbox view (B) now draws the exact tested volume from the live
  settings (feet, max step, body band start, head at the hitbox radius);
  it used to draw an outdated 0.4 radius.
- Settings load before the first room builds its grid. With defaults
  the results are identical to the previous build (VERIFY_PATHS run with
  both exes on the same data).
- (same day) The David & navigation settings are now PER ROOM: every
  room starts from the tuned defaults and `test_data/<room>_nav.cfg`
  holds only that room's changed values (loaded on room load, before the
  walkable grid is built; "Reset to defaults" deletes the file). The TAB
  list marks such rooms "(custom nav)". The old global
  `test_data/editor_settings.cfg` is no longer read.

## 2026-09-28 -- Animation viewer (A)

- `export_david_anims.py` bakes every clip of `Silver Blockouts/.../chars`
  (anims/ + david/) whose glTF has David's 48-node rig into
  `assets/david_anims.bin` (410 clips, 2.9 MB). 148 files skipped: other
  rigs (51-node winged/big creatures, 30/46/4-node animals/props) or no
  animation. Loaded at runtime by `anim_lib_load` (character.c), so new
  clips only need a re-bake, not a rebuild.
- Viewer: button "Animations" (VIEW section) or A. Full window: list with
  type-to-filter (David's own clips in blue), studio stage with David
  textured and lit, grid floor, timeline. Up/Down (or click) = clip,
  Space = play/pause, Left/Right = frame step (1/30 s), Num-/Num+ =
  speed x0.125..x4, drag = rotate, wheel = zoom (over the list: scroll),
  "Camera: follow/fixed" (fixed shows root motion over the grid),
  Esc = back to the game.

## 2026-09-28 -- Skeleton fix, run turn-starts, David's speeds

- **Skeleton rotations were inverted since the start**: `mat4_from_trs`
  read `quat_to_mat3`'s TRANSPOSED matrix as if it were the rotation, so
  every bone turned the opposite way (legs folding into each other, head
  backwards). Fixed; David's model faces -Z (measured on walk/towalk/run:
  grounded foot slides +Z, head leans -Z), so the render adds a half turn
  to g_char_facing (= heading).
- **Run turn-starts** (the blockouts' clips, measured: exact 90/180 degree hip
  turns, no displacement, end on the run cycle's first pose): a run order
  whose first leg is >= 45 degrees away from David's front plays, in place,
  run90a (left) / run90c (right) / run180a (behind, via his left) /
  run180c (behind, via his right) -- > 135 degrees = behind -- then runs.
  Not when he's already running. The double-click's first click already
  starts a walk: a run order within the double-click time is judged from
  the facing he had before that walk.
- **David's global settings** (panel N, group "DAVID (all rooms)",
  `test_data/david_settings.cfg`): walk speed (6), run speed (13),
  animation speed factor (x1, scales idle/walk/run/turn playback).

## 2026-09-28 -- Smooth rendering (24 -> ~140 fps), sharp David, turn-starts v2

Measured with `SILVER_PERF=<file>` (per second: fps, paint avg/worst,
worst gap, and the split scene / stretch / david / panels), fullscreen
1920x1080, boilarea, David walking:
- before: 22-31 fps, 40 ms per frame (the room mesh was rasterized FOUR
  times per frame for depth; GetTickCount's 15.6 ms steps made dt jitter)
- after: ~140 fps on a 144 Hz monitor, ~2.4 ms per frame.

What changed:
- Room depth rasterized once per room (`room_depth_cached`, keyed on mesh
  + camera), copied into the per-frame buffers.
- Frame loop: PeekMessage + one `game_tick` per display refresh
  (`DwmFlush`), dt from QueryPerformanceCounter. A 33 ms timer only ticks
  while Windows runs a modal loop (menus, window drag). Edge scrolling is
  time-based (480 composite px/s) instead of 8 px per tick.
- David is no longer drawn into the 8-bit 640x480 frame: `render_david_hires`
  rasterizes him straight into the scaled 32-bit view layer, true colour
  from his texture, occluded by the room depth sampled under each screen
  pixel and by FOREGROUND shapes (F3 debug view keeps the old path).
  Click marker / hitbox are drawn on the layer through a world transform.
- Own bilinear scaler (`scale_room_rows`, 2 channels per multiply) instead
  of GDI HALFTONE (6.4 ms); the scaled picture is cached and only the rows
  under changed source rows are re-scaled (`compose_room_layer`).
- Fonts cached, back buffer kept; game frames repaint/copy only the game
  picture, the side panels are redrawn on input / status or room change /
  every second.
- Makefile now lists all game sources and -lm -ldwmapi.

Turn-starts v2 (user feedback): they play for WALK orders too, David sets
off at walk speed while the clip turns him (then walks/runs), an order
right after he set off (the double-click's second click, a re-click) keeps
the turn already playing instead of restarting/cancelling it, arriving
mid-turn ends facing the travel direction.

VERIFY_PATHS forces the reference speeds (walk 6, run 13, anim x1) so its
numbers stay comparable whatever David's settings are: 5540 / 4697 / 4692,
7 wall crossings (unchanged); VERIFY_SPAWNS 277/277.

- (same day) Turn-starts on EVERY direction change >= 45 degrees
  (`david_turn_toward`): new orders while standing or moving, and each
  corner of his path. A change during a turn that needs the same clip
  just retargets it (double-click's second click); another clip restarts
  from where the turn began. Walk cycle = the blockouts' `walk` clip from the
  animation library (full body: spine, arms, legs, hips); the baked
  `towalk` had been picked while the skeleton rotations were inverted.
- (same day) A new direction during a turn-start breaks it: the right clip restarts from where his body points NOW (start facing + the hips yaw the clip reached, david_current_heading); the same order again (double-click) just continues. Arriving mid-turn lets the turn finish on the spot instead of snapping.

## 2026-09-28 -- New repository: silver-remaster (assets only, relative paths)

The project moved from `recomp/` (kept as an archive, with its full
history) to a clean repository that runs only from the blockouts' Silver
Blockouts (`./assets`, not versioned) and its own data (`./data`):

- **No original game file at runtime anymore.** Room pictures are the
  Blockouts' `<room>.png` (loaded with GDI+), cameras and geometry come
  from `<room>.gltf` (own small JSON/glTF reader), David's model and every
  animation clip from `chars/david` / `chars/anims` (glTF, loaded on first
  use). The `.nob` / RNC / `.PAK` readers, the SMK overlay player and the
  8-bit palette pipeline are gone: the frame is 32-bit (tints, shadow,
  foreground, scaling in true colour).
- **Nothing generated anymore**: no `david_data.c` / `david_anims.bin` /
  `shadow_data.c` / `.cur` files / `<room>_mesh.bin` / `_room3d.cfg` /
  `_zmask.bin`. The shadow is computed at startup, the cursors are built
  from `sprites/mouse.N.png`, floor height and footprint are computed from
  the glTF like walkable_area.py did.
- **Camera roll**: the only thing still coming from the original game.
  It's stored per room in `data/rooms/<level>/<room>_camera.cfg`
  (48 rooms), regenerated by `tools/extract_native_roll.py` from the CDs,
  applied at load time exactly like fix_native_roll.py did.
- **Per-room data keyed by `level/room`** (`data/rooms/<level>/<room>_*.cfg`):
  two levels have a room called `entrance`. Editor shapes, nav settings and
  calibrations were migrated; the two rooms still in the pre-shapes formats
  (goingdwn doors, boneyrd3 zones) were converted.
- **Paths are relative** to the project folder (exe folder, or its parent
  when the exe is in `build/`). Reports and logs go to `build/`.

Validation, before removing the old data:
- Temporary dump of every room's camera (translation + rotation matrix,
  roll fix and calibration applied), yfov, floor height, footprint and
  geometry (triangle count + coordinate sum), compared with the old
  `_room3d.cfg` / `_mesh.bin`: identical for all 276 rooms that existed
  before.
- `spires/la_corri`: its picture is a 0-byte file in the Blockouts (the old
  build read the CD's background) -> the room is skipped. `gno/entrance`
  (only in the Blockouts) is new: 277 rooms.
- VERIFY_SPAWNS 277/277. VERIFY_PATHS: 5540 clicks, 4701 accepted, 4695
  arrived, 11 wall crossings, 77 body-in-geometry and 48 floating frames --
  the same totals as the old exe run on the same day with the same data.
  35 rooms differ only in frame counts: the old build used the model and
  clips rounded to 6 decimals (baked C arrays), the new one reads the
  exact glTF values, which moves a few spawn-visibility ties.
- Performance unchanged: ~140 fps on a 144 Hz monitor, 2.5 ms per frame.
- (same day) `spires/la_corri`: the user fixed the empty picture in the Blockouts (640x480); the room loads again -> 278 rooms. Rooms are found by scanning assets/levels at every launch (a room = <room>.gltf + non-empty <room>.png).

## 2026-09-28 -- Character picker in the animation viewer

- `char_model_load` (character.c) loads any character of assets/chars:
  every primitive of its mesh merged (the small second primitives are
  double-sided bits -- David now also gets his 6 missing vertices), the
  texture named by the glTF material. `skeleton_skin_matrices_for` skins
  any of them.
- The animation library knows each clip's skeleton (node count) and takes
  new folders on demand (`anim_lib_add_dir`, existing indices never
  move); a clip plays on a character with the same node count.
- Picker (Tab / Model button): every assets/chars/<name>/<name>.gltf with a
  skinned mesh and more than 6 bones -- 198 characters (the 4-6 bone props,
  chars/items and the animation-only folders are left out). Grid of cards,
  cached preview in a standing pose (an own idle-like clip, else the shared
  stand when it fits, else the rest pose), the selected card animated and
  turning. The scan (all models + textures) takes well under a second.
- VERIFY_SPAWNS 278/278 with David's extra vertices.

