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
- Click to walk, double-click to run. The A* pathfinding runs over a
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
- Several scripts per room, each a grid of actions: 3 columns by default
  (more can be added), as many rows as needed. The rows play one after the
  other; the actions of a row all start together and the next row starts
  when they are all finished.
- Actions:
  - **Wait** a given time (the game goes on);
  - **Background**: the room shows another picture (only the picture);
  - **Music**: a playlist of tracks, each looped or not; it replaces the
    music playing and goes on during the next rows;
  - **Sound**: played 1 + *repeat* times; the row can wait for its end;
  - **Ambience**: an atmosphere sound, looped or not;
  - **Stop sound**: one of the script's sounds / ambiences, all of them,
    or the music;
  - **Place character**: any character of the game appears at a point
    clicked in the room, facing a direction;
  - **Move character**: David or a placed character walks / runs to a
    point clicked in the room, or through a connector (for David: until
    he is in the other room).
- While a script plays, the player can only left-click to skip the current
  row -- not a row that moves a character. Esc stops it (testing).
- A script plays when David arrives through a connector that names it (the
  connector wizard asks for it; also "Arrival script..." on a connector),
  or, entering the room otherwise, the room's first script marked *auto*.
- Choosers for sounds (with Listen) and pictures (with a preview); points
  are picked right in the room. Saved in `data/rooms/`.

**Movesets** -- how a character walks
- The 13 clips a character walks with (stand, walk, run, the starts and
  the turns), shown as a graph with the selected clip playing on the
  character. The **Human** preset gives David's clips; any slot can take
  another clip or "no animation" without changing the preset.
- Per character, in `data/movesets/`. From the animation viewer
  (Moveset...) or a script's character actions.

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
| F11 | fullscreen |
| A | animation viewer (Tab inside it: choose the character) |
| S | scripts of the room (F5 play, F6 play from the row, Ins / Del rows and cells, right-click a cell) |
| Left-click during a script | skip the current row (not while a character is moving) |
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
  movesets/<model>.cfg                a character's moveset (only when changed)
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

- Animated scenery (e.g. conveyor belts, flames) and the intro videos
  aren't played yet. Music and sounds only play from scripts.
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
