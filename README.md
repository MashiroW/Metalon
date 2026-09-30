# Silver Remaster

A fan-made, playable re-creation of **Silver** (Infogrames / Spiral House,
1999) for Windows. Every room is shown with its original pre-rendered
picture, and David is a real-time 3D character walking in the room's 3D
blockout: he's hidden behind scenery, climbs stairs, finds his way around
obstacles, and plays the game's own animations.

It runs entirely from the **"Silver Blockouts"** (3D rebuilds of every
room, David and his animations, sprites) plus this project's own data.
No file of the original game is needed.

## Features

**Rooms**
- 278 rooms, loaded straight from the glTF blockouts and pictures.
- Camera aligned on the picture: the original game's camera roll is
  applied, and 95 rooms are calibrated against their original depth masks.
- Windowed (1:1) or fullscreen, with a 4:3 picture that scrolls when the
  room is bigger. Tool panels sit in the side bars.
- About 140 fps on a 144 Hz monitor. David is drawn at screen resolution.

**David**
- The blockouts' skinned model with the game's animation clips: stand, walk, run.
- Start and turn clips on every change of direction of 45 degrees or more:
  - standing still, facing the target: `towalk` / `torun`;
  - left / right: `towalka` / `towalkc` when walking, `run90a` / `run90c`
    when running;
  - behind, via his left or right: `to180a` / `to180c` when walking,
    `run180a` / `run180c` when running.

  A turn is committed: repeated orders don't restart it. A new direction
  early in a turn restarts the right one from where his body points.
- Click to walk, double-click to run. He comes out of a scene connector
  the way he went in: running if he ran to it, walking otherwise.
- Through a connector: the picture fades to black, the sounds and
  ambiences fade out (the music goes on), the game's spinning "Silver"
  loading animation plays for 2 seconds, then the new room fades in. The A* pathfinding runs over a
  multi-level walkable grid, with a body/step/slope model: stairs yes,
  walls and steep slopes no.
- Adjustable in the tool panel:
  - per room: hitbox radius, step height, slope, foot support, headroom,
    wall-click reach;
  - for David, in all rooms: walk / run speed, turn speed while running,
    animation speed, shadow size.

**Scene editor** (E)
- Draw rectangles, circles and polygons on the picture. Each shape can
  have any of these roles:
  - **red zone**: David never goes there;
  - **green zone**: forced walkable;
  - **foreground**: that part of the picture is drawn over David;
  - **scene connector**: a door to another room. A step-by-step wizard
    sets the doorstep, the arrival point and the other side, and the link
    works both ways.
- Resize handles, move, undo, delete; roles from the buttons or a
  right-click.
- Saved per room in `data/rooms/`.

**Scripts** (S) -- the room's cutscenes
- Every room has a **Default** script (it can't be deleted or renamed):
  it plays automatically when David enters, and its first block is the
  room's background music -- *None* until chosen.
- Several scripts per room, each a grid of actions: 3 columns by default
  (more can be added), as many rows as needed. The rows play one after the
  other; the actions of a row all start together and the next row starts
  when they are all finished.
- Actions:
  - **Wait** a given time (the game goes on);
  - **Background**: the room shows another picture (only the picture);
  - **Music**: a playlist of tracks, each looped or not; it replaces the
    music playing and goes on during the next rows. If the music playing
    is already its first track (the same music as the room before), it
    goes on without restarting. A fade out (0.5-10 s) makes the music
    playing fade away first, then the new playlist starts (with *None*:
    the music just fades out). Every new script starts with a Music
    block set to *None* (no music) in its first cell;
  - **Sound**: played 1 + *repeat* times; the row can wait for its end;
  - **Ambience**: an atmosphere sound, looped or not;
  - **Stop sound**: one of the script's sounds / ambiences, all of them,
    or the music -- at once or fading out;
  - **Place character**: any character of the game appears at a point
    clicked in the room, facing a direction;
  - **Move character**: David or a placed character walks / runs to a
    point clicked in the room, or through a connector (for David: until
    he is in the other room);
  - **Animate character**: a character plays any animation made for its
    skeleton (chosen with a live preview), a number of times or over and
    over while the rest of the row lasts -- e.g. talking during a line --,
    or looped until replaced (kept after the script, until another
    animation, a "Back to normal", or a move order); at the game's
    animation speed or at its own (x0.1-x4). A clip followed by another
    one on the next row chains with no idle pose in between; "Freeze on
    the last frame" keeps its last pose until the rest of the row is over;
  - **Character settings**: a character becomes an ally / an enemy, its
    AI on / off, takes out a weapon / shield (or puts it away) as with the
    radial menu. "Place character" chooses its side and AI too;
  - "Animate character" and "Sound" can have a random pool: an animation
    takes another one of it at random at every loop (a number of times,
    while the row lasts, until replaced), a sound one each time it plays;
  - **Overlays** can come from another room too ("Import from another
    room...": one of its overlays or all of them; their frames stay in that
    room's folder, their place is set here; an imported one can be taken
    out again);
  - **Speak**: a character says a line (a sound -- the room's own lines
    are listed first) with its portrait shown until the line is over. Each
    character's portrait (sprites `bigports.1-71`) is chosen the first time
    it speaks and kept for it everywhere (`data/portraits.cfg`);
  - **Overlays**: the room's environmental animations (the original
    game's videos: fires, water, doors, crowds...), already at their place
    on the picture, each one looped, played once (stays on its last frame,
    or disappears), frozen on a chosen frame, hidden or left as it is --
    with an animated preview. Their places can be nudged or picked in the
    room (they belong to the room: `data/rooms/<level>/<room>_overlays.cfg`,
    from `tools/find_overlay_positions.py`);
  - **Change room**: like going through a connector -- the room, the
    connector David comes in by, and the script that plays there.
- While a script plays, the player can only left-click to skip the current
  row -- not a row that moves a character. Esc stops it (testing).
- Camera action: the view slides (over the chosen seconds) to David, to
  a point of the room, or stays, and zooms (x1-x3, or the room's own
  zoom). Each room's zoom is a setting (N in the editor: Camera zoom).
- **Advanced timeline**: select rows (Shift+click / Shift+arrows), then
  "Make timeline". In it the actions start at their own time and can
  overlap: each is a block as tall as it lasts (instant ones are thin
  lines; a character walking somewhere or an animation "until the others
  end" is shown with its last measured length, or an estimate), the
  columns are lanes. Drag an action up / down to set when it starts
  (0.05 s steps, Shift: 0.01) or sideways to another lane; click / double
  click an empty spot to add one there. Make it taller / shorter by
  dragging its bottom edge (the handle), with "+ Row" / "- Row", or Ins /
  Delete row on one of its rows. Zoom in / out changes the seconds per row.
  A sound, a line or an animation can be cut: drag its top edge down to
  start further into it, its bottom edge up to end it earlier (a wait or
  a camera slide: its length); orange dots mark a cut end.
  Blocks of a lane never go into each other: a dragged block (or group)
  or edge stops against its neighbour, an action made longer (another
  file, more repeats...) pushes the next ones of its lane later. Instant
  ones may sit on a block's edge, not inside it.
- Groups of actions: Ctrl+click adds / removes one, Shift+click takes all
  the actions of the rows from the selected one, a rectangle drawn from an
  empty spot takes the ones it touches. Drag one of them to move them all
  (in plain rows: by whole cells, green where they'll go, red if they
  can't; in a timeline: in time and across lanes). A group is either
  plain rows or one timeline, never both. Del clears it (asks), Esc
  unselects it. When playing, the zone is over as soon as all its actions
  are -- not when its rows are -- and the rows after it go on. "Back to
  rows" turns it back into plain rows.
- A script plays when David arrives through a connector that names it (the
  connector wizard asks for it; also "Arrival script..." on a connector),
  or, entering the room otherwise, the room's first script marked *auto*.
- Choosers for sounds (with Listen) and pictures (with a preview); points
  are picked right in the room. Saved in `data/rooms/`.
- The sounds are listed by kind: the room's own **lines** (its characters'
  dialogue, in its folder), the **music** (the 39 soundtrack tracks), the
  **sound effects** (the rest of `assets/sound`), the **other rooms'
  lines** -- the likeliest kind first (music for a Music action, the
  room's lines for Speak / Sound / Ambience, effects for attack sounds).

**Radial menu** (right click in the game)
- 8 slots around the click (kept inside the picture), each with its
  ornament on the ring; the view doesn't scroll while it's open.
- Food, Orbs, Ranged, Magical, Backpack (Inventory, Potions, Keys),
  Shields, Weapons, Specials. Right click goes back one level (closes it
  at the top). A small arc points at the slot under the mouse.
- Picking an item equips it: its slot on the first ring then shows its
  icon (Weapons, Shields, Magical, Ranged, Orbs, Specials).
- Sounds: pieopen when a ring opens, pieclse when it closes, pieslice on
  hover, pieselct on a pick. Drawing or changing a weapon plays swrdeqp1
  or swrdeqp2.
- Weapons: short sword, broad sword, battle axe, long sword, war hammer,
  mace, bastard sword, dual knightly swords -- drawn or put away with
  `sheatmp` (the one held: put away).
- Shields: taken out / put back with `shldquip`, worn on the back of the
  left hand. A broken shield falls in pieces and is gone for good.
- Specials: Web of Death, Reaper, Cleaver, Scythe, Falcon, Hurricane,
  Berserker, Armageddon -- their animation, with a blue trail behind the
  blades.
- The other items are there for later (no effect yet).

**Magic** (radial menu: Orbs, Magical, Backpack > Potions)
- Orbs (Fire, Ice, Health, Earth, Acid, Lightning, Time, Light): taken out
  with `magkup` (hands raised, the element's burst of `equipfx`, orbup),
  held up (`magkbob`), the orb's sprite spinning in the hand; the same orb
  again: put away (magkup backward). Combat preset **Orb Magic**: magkaim,
  mgkair, mgk90a/c, mgk180a/c. The spell leaves at the cast gesture,
  toward a foe in front: a glow (`star`) with its trail (`twinkle`) and
  blast (`xplode` / `equipfx`), a 3D bolt for acid (`acidbolt`) and ice
  (`iceshard`), a rolling rock for earth. Health heals, Time slows the foes
  around, ice slows its target.
- Wands and staves (Ice wand, Lightning Staff; also firewand, wandbolt...):
  held like weapons, combat preset **Wand Magic** (fcast, conjure,
  cast90a/c, cast180a/c) casting their element. The fire sword burns.
- Potions: drunk (`drink`, the bottle in the left hand) -- health, strength
  (double damage 20 s), enchanted armour (half damage 30 s), absolute
  protection (10 s), chaos (one of them, or slowed); thrown (`throw`, an
  arc) -- exploding vials (a fire blast), gas cloud vials (poison).
- AI preset "Caster - keeps its distance".

**Attack mode** (Ctrl held)
- A click: one of the three attacks of the moveset, at random, toward the
  point clicked.
- The button held and the mouse swung: up = thrust, left / right = side
  blows, down = a blow turning around via the left or the right.
- Every blow moves him with it (hitbox diameters: attacks 1, thrust 4,
  left 1.25, right 1.5, down 1.5 back -- as far as the floor allows).
- Right click: a dodge back (`dodgeb`, 4 hitbox diameters). Right button
  held: the shield up (`shldup`, `shldhold` while held, `shlddown`).
- The weapon's combat preset (chosen with the weapon): **Single Swords** (rchop / rchopp /
  rchoppp, lstab, headchop, revslice, rchp180a/c) or **Double Swords** for
  the dual swords (rchoptwo, stabtwo, rslice2, hedchop2 -- `lslice2`
  isn't in the blockouts --, rlc2180a/c).

**Movesets** -- how a character walks and fights
- Walking tab: the 13 clips a character walks with (stand, walk, run, the
  starts and the turns), shown as a graph with the selected clip playing
  on the character. The **Human** preset gives David's clips.
- Combat tab: the 8 attacks of attack mode, each with its step. The
  combat preset follows the weapon held unless one is chosen.
- Each blow's sounds ("Sounds: ... edit..."): like a little script,
  sounds and waits in order from the start of the blow, with a ruler
  over the clip and "Play with sounds" to check they match; saved with
  the combat preset (default: one swing of the pool, at once).
- Sounds tab: pools of swing sounds, hit sounds (kept for when something
  can be hit) and grunts when hit, with the chance a grunt plays.
- **Equipped weapon** / **Equipped shield** (top right): what the
  character holds (a pair of weapons like fuge's `dualswrd`: one in each
  hand) and wears on its left arm.
- Presets for each tab (Human, Single Swords, Double Swords, David's
  sounds, and your own in `data/presets/`): choose, save (optionally as
  the character's default), delete (asks first). Any slot can take
  another clip or "no animation" without changing its preset.
- **Model presets** bundle the three tabs and the equipment: save them
  for the character, make one its default, apply or delete them.
- Per character, in `data/movesets/`. From the animation viewer
  (Moveset...) or a script's character actions.

**Combat, reactions, AI** (moveset screen: Reactions, AI, Stats tabs)
- Two sides: David and his allies, the enemies. A blow lands when a blade
  touches the hitbox of a character of the other side (a cylinder around
  it, as tall as its model), once per blow: the attacker's damage, its
  hit sound, the target's grunt and "Hit" reaction. A raised shield blocks.
- Health gone: an enemy plays "Death" and stays down for good; an ally
  (David too) is "Knocked down", then "Lying down" (looped) until no enemy
  is left, then "Gets up" with its health back (David at most 10 s later).
- Every character has its floor shadow, sized to its build (David's is
  the "Shadow size" setting; the others scale with their width).
- Reactions tab: Hit, Dodge, Death, Knocked down, Lying down, Getting up (presets
  Human, Fuge -- fuge dies with his own fugedie).
- AI tab (presets; built in: "Fuge - dual blades"): close in to the
  nearest opponent, strike from a distance, walk or run, a pause between
  attacks, a chance to dodge our blows.
- Stats tab (presets): health, damage per blow, boss -- a boss enemy's
  health bar shows at the top of the screen while it's alive.
- Model presets bundle walking, reactions, combat, sounds, AI, stats and
  the equipment.

**Animation viewer** (A)
- Plays every clip made for the shown character's skeleton: its own
  folder's clips first, then the shared ones of `assets/chars/anims`
  (410 for David).
- Filter by name, play / pause, frame by frame, speed, rotate and zoom.
- **Character picker** (Tab or the Model button): the 198 characters
  and creatures of the game in a grid, each card with a 3D preview (the
  selected card is animated). Mouse (click, double-click, wheel) or
  keyboard (arrows, PgUp/PgDn, Enter, Esc, type to filter).

## Setup

1. **Windows 10/11** and a MinGW-w64 `gcc` on the PATH (e.g. WinLibs).
2. **Assets.** Copy the *content* of the `Silver Blockouts` folder into
   `./assets`, so that you get `assets/chars`, `assets/levels`,
   `assets/sprites`, `assets/sound` (music and sounds of the scripts), and
   optionally `assets/intro`, which isn't used yet. They are not part of this repository.
3. **Build:** `mingw32-make` (the name of `make` in WinLibs) → `build/silver_remaster.exe`
4. **Run:** `build/silver_remaster.exe [level/room]`, for example
   `build/silver_remaster.exe gno/screen1`. It starts in `gno/boilarea`
   by default.

All paths are relative to the project folder: the exe's folder, or its
parent when the exe sits in `build/`.

## Controls

Every tool is also a button in the side panel, with its shortcut shown.

| | |
|---|---|
| Click / double-click | walk / run there |
| Mouse at the edge of the picture | scroll the view |
| **Caps Lock** | off = playing (mouse kept in the picture), on = tools (mouse free for the panels) |
| PgUp / PgDn, TAB | previous / next room, room list (type to filter) |
| P | walkable area (green) |
| B | David's hitbox and path |
| C | collisions on/off (fly mode) |
| F3 | raw 3D blockout instead of the picture |
| F4 (editor, placing a script's character: position, facing, destination) | free camera: fly anywhere in the room's blockout (no picture, the floor he can stand on green) to put a connector's points, or a script character's, where the room's camera doesn't look (the script's other characters are marked; kept for the next pick) -- the keys where W A S D are on a QWERTY keyboard (Z Q S D on an AZERTY one) / arrows, Q / E (A / E) down / up, Shift faster, right button dragged to look, wheel forward / back; F4 / Esc back |
| F11 | fullscreen |
| A | animation viewer (Tab inside it: choose the character) |
| S | scripts of the room (F5 play, F6 play from the row, Ins / Del rows and cells, right-click a cell) |
| Left-click during a script | skip the current row (not while a character is moving) |
| Right click | the radial menu (weapons, shields, specials...) |
| Side panel: Master / Music / Sounds & voices / Ambiences | the volumes (- / +, 10 % steps; a click on the name mutes it); music at 80 % to begin with, kept in data/audio.cfg |
| Ctrl held | attack mode: click = attack, button held + swing up / left / right / down = the other blows, right click = dodge, right button held = shield |
| Ctrl+K | break the shield (test: nothing hits yet) |
| Ctrl+H | David is hit: a grunt, by the moveset's chance (test) |
| E | scene editor: V select, 1 rectangle, 2 circle, 3 polygon, Ctrl+Z undo, Del delete, right-click = roles |
| N (editor) | David & navigation settings |

## Project layout

```
src/, include/   the game (C, Win32 + GDI/GDI+)
  main.c         window, rooms, rendering, editor, scripts screen & player, UI
  character.c    David (glTF model + animation clips), camera, collisions, pathfinding
  script.c       scripts: the grid of actions and its file
  audio.c        music, sounds, ambiences (.ogg, mixed on a thread)
  gltf.c json.c  glTF loading
  image.c        PNG loading (GDI+)
  third_party/   stb_vorbis.c (Ogg Vorbis decoder, public domain)
assets/          the Silver Blockouts (not versioned)
data/            this project's data (versioned)
  david.cfg                         David's settings (all rooms)
  rooms/<level>/<room>_camera.cfg     original game's camera roll (tools/extract_native_roll.py)
  rooms/<level>/<room>_intrinsics.cfg camera calibration (CALIBRATE_CAMERAS)
  rooms/<level>/<room>_shapes.cfg     scene editor shapes
  rooms/<level>/<room>_nav.cfg        per-room navigation settings (only changed values)
  rooms/<level>/<room>_scripts.cfg    the room's scripts
  rooms/<level>/<room>_overlays.cfg   the room's overlays: place, size, frame rate
  movesets/<model>.cfg                a character's moveset and right-hand item (only when changed)
  presets/<name>.cfg                  moveset presets saved from the moveset screen
  portraits.cfg                       the portrait each character speaks with
tools/           scripts that need the original CDs (see tools/README.md)
docs/PROGRESS.md development history: measurements, decisions, what was tried
build/           silver_remaster.exe
```

## Developer modes

- `silver_remaster.exe VERIFY_PATHS`: walks David to 20 points in every
  room and reports wall crossings, clipping and refusals
  (`build/path_verify_report.txt`).
- `silver_remaster.exe VERIFY_SPAWNS`: checks that David starts visible
  in every room.
- `silver_remaster.exe CALIBRATE_CAMERAS`: fits each room's camera
  against its original depth mask and writes the `_intrinsics.cfg` files.
- `SILVER_PERF=<file>`: logs fps and frame timings every second.
  `SILVER_NO_CLIP=1` never locks the mouse (for automated tests).

## Known limitations

- The intro videos aren't played yet. Music, sounds and the rooms'
  animations only play from scripts (an "auto" script plays when David
  enters the room).
- Scene connectors are authored by hand in the editor. The game's own
  room-to-room links aren't decoded.

## Credits

- **Silver** © 1999 Infogrames / Spiral House. This is a non-commercial
  fan project; you need your own copy of the resources.
- A **mysterious person**, who prefers to remain anonymous, for their
  support with the Silver Blockouts (3D rebuilds of the rooms, David and
  his animations). Thank you.
- **stb_vorbis** by Sean Barrett (public domain): the Ogg Vorbis decoder.
- **silvie** by Lucas Petitiot (GPL-3.0): the RNC / PAK formats the tools
  port.
