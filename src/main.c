/* Silver Remaster -- the game window, rooms, David, the scene editor.
   Everything is read from ./assets (the Silver Blockouts: room
   pictures, 3D blockouts, David and his animations, sprites) and ./data
   (this project's own per-room data), both relative to the project
   folder -- see README.md. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <windowsx.h>
#include <dwmapi.h>
#include "character.h"
#include "gltf.h"
#include "image.h"
#include "audio.h"
#include "script.h"

/* ---------------- project paths ----------------
   The project folder is the exe's folder, or its parent when the exe
   sits in build/ (the normal layout). */
static char g_root[MAX_PATH];
static void init_root(void) {
    GetModuleFileNameA(NULL, g_root, sizeof(g_root));
    char *s = strrchr(g_root, '\\');
    if (s) *s = 0;
    s = strrchr(g_root, '\\');
    if (s && _stricmp(s + 1, "build") == 0) *s = 0;
}
/* g_root + "\" + printf-formatted relative path ('/' become '\'). */
static void root_path(char *out, size_t n, const char *fmt, ...) {
    char rel[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(rel, sizeof(rel), fmt, ap); va_end(ap);
    for (char *p = rel; *p; p++) if (*p == '/') *p = '\\';
    snprintf(out, n, "%s\\%s", g_root, rel);
}
/* Creates the folders leading to `path` (for files written in data/). */
static void ensure_parent_dir(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 3; *p; p++) {
        if (*p != '\\') continue;
        *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\';
    }
}
/* "level/room" -> its parts */
static void split_label(const char *label, char *level, size_t nl, char *room, size_t nr) {
    const char *s = strchr(label, '/');
    if (!s) { snprintf(level, nl, "%s", ""); snprintf(room, nr, "%s", label); return; }
    snprintf(level, nl, "%.*s", (int)(s - label), label);
    snprintf(room, nr, "%s", s + 1);
}

static struct { uint32_t width, height; } g_hdr; /* current room picture size */
static uint32_t *g_dib_pixels = NULL;    /* the room's pre-rendered picture, 0x00RRGGBB, top-down */
static uint32_t *g_render_pixels = NULL; /* working copy: picture + shadow + tints, rebuilt each frame */
static uint32_t g_dib_stride = 0;        /* pixels per row */
static uint32_t *g_dib_own = NULL;       /* the room's own picture while a script shows another one (background_set) */
static int g_loaded = 0;
static char g_status[512] = "no background loaded";

/* Raw 3D debug view (per explicit user request: show the actual mesh0
   environment geometry -- the same data they found they could open
   directly in Blender -- through the real per-room camera, with David
   in it, as an alternative to the pre-rendered 2D background photo).
   Defaults ON; a key toggles back to the normal background. Shares one
   depth buffer between the room mesh and David so they correctly
   occlude each other (a real improvement over the background mode,
   where David always draws on top of everything). */
static struct { int active; float t, dur, fx, fy, fz, tx, ty, tz; } g_camslide; /* a script's camera slide (camera_slide) */
static float g_view_zoom = 1.0f; /* the view's zoom (get_view_window) */
static int g_view_mode_3d = 0; /* 0 = real photo + depth-composited David (default/normal play), 1 = raw flat-shaded mesh debug view (F3) */
static float *g_depth_buffer = NULL; /* g_dib_stride * g_hdr.height floats, camera-space depth, indexed by g_room_cam's own pixel grid */
static float *g_depth_buffer_char = NULL; /* room-mesh-only depth through g_room_cam, used for David's occlusion/shadow and
    spawn scoring. Kept separate from g_depth_buffer because that one also receives the F3 debug view's color pass and David's
    own fragments. David's body pixels are re-anchored into g_room_cam's pixel grid by char_project, so the grids match. */

/* Collision toggle, per explicit user request: David is never meant to
   fly or clip through walls (collision ON, default) but sometimes it's
   useful to freely fly the camera-click target around for debugging/
   exploring (collision OFF -- flat-plane click, ignores mesh entirely,
   same as the pre-raycast behavior). Real stairs still work fine with
   collision ON: each tread is its own small floor-like (up-facing)
   triangle, connected to the next by the riser, so the floor-normal
   test alone already accepts them -- no separate stair-case logic
   needed. */
static int g_collision_enabled = 1;

/* Room list (TAB): every room found in assets/levels. Type to filter,
   arrows to move, Enter to go, Esc to cancel. */
#define MAX_MAP_ROOMS 400
typedef struct {
    char label[96];   /* "level/room" = assets/levels/<level>/<room>/<room>.gltf + .png */
} MapRoomEntry;
static MapRoomEntry g_map_rooms[MAX_MAP_ROOMS];
static int g_map_room_count = 0;
static int g_map_filtered[MAX_MAP_ROOMS];
static int g_map_filtered_count = 0;
static int g_map_mode = 0;
static char g_map_filter[64] = "";
static int g_map_filter_len = 0;
static int g_map_selected = 0;
static int g_map_scroll = 0;
static int g_current_map_room = -1; /* index in g_map_rooms of the room on screen, -1 = not in the list */
#define MAP_VISIBLE_ROWS 18

static int map_label_cmp(const void *a, const void *b) { return strcmp(((const MapRoomEntry *)a)->label, ((const MapRoomEntry *)b)->label); }
/* Every room of the game: a folder assets/levels/<level>/<room> holding
   <room>.gltf (3D blockout + camera) and <room>.png (the picture). */
static void scan_room_list(void) {
    char pat[1024];
    root_path(pat, sizeof(pat), "assets/levels/*");
    g_map_room_count = 0;
    WIN32_FIND_DATAA lv;
    HANDLE hl = FindFirstFileA(pat, &lv);
    if (hl == INVALID_HANDLE_VALUE) return;
    do {
        if (!(lv.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || lv.cFileName[0] == '.') continue;
        char rpat[1024];
        root_path(rpat, sizeof(rpat), "assets/levels/%s/*", lv.cFileName);
        WIN32_FIND_DATAA rm;
        HANDLE hr = FindFirstFileA(rpat, &rm);
        if (hr == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(rm.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || rm.cFileName[0] == '.') continue;
            char g[1024], p[1024];
            root_path(g, sizeof(g), "assets/levels/%s/%s/%s.gltf", lv.cFileName, rm.cFileName, rm.cFileName);
            root_path(p, sizeof(p), "assets/levels/%s/%s/%s.png", lv.cFileName, rm.cFileName, rm.cFileName);
            WIN32_FILE_ATTRIBUTE_DATA pa;
            if (GetFileAttributesA(g) == INVALID_FILE_ATTRIBUTES || !GetFileAttributesExA(p, GetFileExInfoStandard, &pa) ||
                (pa.nFileSizeLow == 0 && pa.nFileSizeHigh == 0)) continue; /* no picture (spires/la_corri's is empty in the blockouts' export) */
            if (g_map_room_count >= MAX_MAP_ROOMS) break;
            snprintf(g_map_rooms[g_map_room_count].label, sizeof(g_map_rooms[0].label), "%s/%s", lv.cFileName, rm.cFileName);
            for (char *q = g_map_rooms[g_map_room_count].label; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
            g_map_room_count++;
        } while (FindNextFileA(hr, &rm));
        FindClose(hr);
    } while (FindNextFileA(hl, &lv));
    FindClose(hl);
    qsort(g_map_rooms, g_map_room_count, sizeof(MapRoomEntry), map_label_cmp);
}

/* case-insensitive substring test */
static int ci_strstr(const char *hay, const char *needle) {
    if (!*needle) return 1;
    size_t hn = strlen(hay), nn = strlen(needle);
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == nn) return 1;
    }
    return 0;
}

static void map_recompute_filter(void) {
    g_map_filtered_count = 0;
    for (int i = 0; i < g_map_room_count; i++) {
        if (ci_strstr(g_map_rooms[i].label, g_map_filter)) {
            g_map_filtered[g_map_filtered_count++] = i;
        }
    }
    if (g_map_selected >= g_map_filtered_count) g_map_selected = g_map_filtered_count > 0 ? g_map_filtered_count - 1 : 0;
    g_map_scroll = 0;
}

/* Puts the list cursor on the current room and scrolls it into view
   (roughly centered), used when TAB opens the list. */
static void map_select_current_room(void) {
    g_map_selected = 0;
    for (int k = 0; k < g_map_filtered_count; k++)
        if (g_map_filtered[k] == g_current_map_room) { g_map_selected = k; break; }
    g_map_scroll = g_map_selected - MAP_VISIBLE_ROWS / 2;
    if (g_map_scroll > g_map_filtered_count - MAP_VISIBLE_ROWS) g_map_scroll = g_map_filtered_count - MAP_VISIBLE_ROWS;
    if (g_map_scroll < 0) g_map_scroll = 0;
}
static int map_index_of_label(const char *label);
static HCURSOR g_cursor_arrow = NULL, g_cursor_door = NULL;

/* David: the blockouts' skinned model (assets/chars/david) animated with the
   clips of assets/chars/anims, projected through the room's camera. */
static int g_has_3d_character = 0;
static RoomCamera g_room_cam;
static RoomCamera g_char_cam; /* roll-free variant of g_room_cam, David's mesh projection only -- see camera_make_upright */
/* FREE CAMERA (F4, the scene editor / a script's point picking): the room's
   3D blockout seen from a camera flown anywhere -- no picture -- so the
   points of a connector (or a script) can be put where the room's own
   camera doesn't look. It starts as the room's camera (same lens), then:
   W A S D / arrows to fly, Q / E down / up, Shift faster, the right button
   dragged to look around, the wheel forward / back. F4 / Esc: back. */
static int g_free_cam = 0;
static RoomCamera g_fcam;
static float g_fcam_yaw = 0.0f, g_fcam_pitch = 0.0f;
static int g_fcam_look = 0, g_fcam_lx = 0, g_fcam_ly = 0;
static const RoomCamera *view_cam(void) { return g_free_cam ? &g_fcam : &g_room_cam; }
static void fcam_update(void) {
    float cp = cosf(g_fcam_pitch), f[3] = { sinf(g_fcam_yaw) * cp, sinf(g_fcam_pitch), cosf(g_fcam_yaw) * cp };
    float r[3] = { -f[2], 0.0f, f[0] }, rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-6f) { r[0] = 1.0f; rl = 1.0f; }
    r[0] /= rl; r[2] /= rl;
    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    float b[3] = { -f[0], -f[1], -f[2] };
    float *R = g_fcam.R; /* view -> world: its columns are right, up, back */
    R[0] = r[0]; R[1] = u[0]; R[2] = b[0];
    R[3] = r[1]; R[4] = u[1]; R[5] = b[1];
    R[6] = r[2]; R[7] = u[2]; R[8] = b[2];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) g_fcam.Rinv[i * 3 + j] = R[j * 3 + i];
}
static void fcam_toggle(void) {
    g_fcam_look = 0;
    if (g_free_cam) { g_free_cam = 0; snprintf(g_status, sizeof(g_status), "back to the room's camera"); return; }
    g_fcam = g_room_cam; /* from where the picture is seen, same lens */
    float fx = -g_room_cam.R[2], fy = -g_room_cam.R[5], fz = -g_room_cam.R[8];
    g_fcam_yaw = atan2f(fx, fz);
    g_fcam_pitch = asinf(fy < -1.0f ? -1.0f : fy > 1.0f ? 1.0f : fy);
    fcam_update();
    g_free_cam = 1;
    snprintf(g_status, sizeof(g_status), "free camera: W A S D / arrows fly, Q / E down / up, Shift faster, right button dragged: look, wheel: forward / back. F4 / Esc: back");
}
/* every game step: the keys held fly it. 1 if it moved */
static int fcam_tick(float dt) {
    if (!g_free_cam || !GetFocus()) return 0; /* (the keys: this window's) */
    #define KEY(k) ((GetKeyState(k) & 0x8000) != 0)
    float fwd = (float)(KEY('W') || KEY(VK_UP)) - (float)(KEY('S') || KEY(VK_DOWN));
    float side = (float)(KEY('D') || KEY(VK_RIGHT)) - (float)(KEY('A') || KEY(VK_LEFT));
    float up = (float)KEY('E') - (float)KEY('Q');
    float speed = KEY(VK_SHIFT) ? 24.0f : 8.0f;
    #undef KEY
    if (fwd == 0.0f && side == 0.0f && up == 0.0f) return 0;
    float cp = cosf(g_fcam_pitch), f[3] = { sinf(g_fcam_yaw) * cp, sinf(g_fcam_pitch), cosf(g_fcam_yaw) * cp };
    float r[3] = { -cosf(g_fcam_yaw), 0.0f, sinf(g_fcam_yaw) };
    for (int i = 0; i < 3; i++) g_fcam.translation[i] += (f[i] * fwd + r[i] * side) * speed * dt;
    g_fcam.translation[1] += up * speed * dt;
    return 1;
}

/* Characters in the room: David (g_actors[0], always there) and the ones a
   script brings in (ACT_PLACE), each with its own model, position, path,
   animation state and moveset (see MOVESETS). The movement code works on
   the CURRENT character g_act -- David, except between actor_begin() and
   actor_end() -- through the g_char_* names below.
   Moveset slots: the turn-starts / start transitions (the blockouts' anims),
   played before the walk/run cycle takes over -- rules in move_clip_for --
   and the three cycles. run90a/c and run180a/c were measured: they turn
   the hips exactly 90/180 degrees without moving him and end in the run
   cycle's first pose. */
enum { MC_RUN90A, MC_RUN90C, MC_RUN180A, MC_RUN180C, MC_TOWALKA, MC_TOWALKC, MC_TOWALK, MC_TORUN, MC_TO180A, MC_TO180C,
       MC_STAND, MC_WALK, MC_RUN,
       /* reactions: a blow taken, a dodge, dying (an enemy), knocked down and getting up again (an ally) */
       MC_HIT, MC_DODGE, MC_DEATH, MC_DOWN, MC_DOWNLOOP, MC_GETUP,
       /* combat (attack mode, Ctrl held): a click = one of the 3 attacks at random, a held swing = the other 5 */
       MC_ATK1, MC_ATK2, MC_ATK3, MC_SW_UP, MC_SW_LEFT, MC_SW_RIGHT, MC_SW_BACKL, MC_SW_BACKR, MC_COUNT };
#define MC_FIRST_COMBAT MC_ATK1
#define MC_FIRST_REACT MC_HIT
/* The human skeleton's hands: the wrists 19 (right) and 23 (left), the
   grips in their palms 42 (right) and 40 (left) -- 41, on the back of the
   left hand, is most likely a shield's. Not every model has the grips
   where David does (fuge: 42 hangs from the root), so grip_node checks. */
#define RIGHT_HAND_NODE 42
#define RIGHT_WRIST_NODE 19
#define LEFT_HAND_NODE 40
#define LEFT_WRIST_NODE 23
#define MAX_CHAR_WAYPOINTS 48
#define MAX_ACTORS 16
/* An attack's SOUNDS, like a little script: in order from the start of the
   blow, sounds (they play at once and don't wait) and waits (in seconds of
   the animation). No list: one of the swings of its sound pool, at once. */
enum { CUE_SOUND, CUE_SWING, CUE_WAIT };
#define CUE_MAX 16
#define COMBAT_SLOTS (MC_COUNT - MC_FIRST_COMBAT)
typedef struct { int type; float sec; char file[96]; } SoundCue;
typedef struct { int set; int n; SoundCue c[CUE_MAX]; } CueList;
typedef struct { CueList list; int i; float wait; int on; } CuePlayer;
typedef struct {
    int used;
    CharModel *model;
    int place_id;              /* id of the script action that brought it in (0 = David) */
    float pos[3], target[3], facing;
    int moving;
    int walk_mode;             /* 0 = idle clip, 1 = walk clip, 2 = run clip */
    float anim_t;
    int turn_clip;             /* library clip playing, -1 = none */
    float turn_t, turn_to;     /* turn_to: facing once the turn is done */
    int turn_from_stand;       /* the clip playing was picked by the standing-start rules */
    int turn_interrupted;      /* the clip playing already replaced another one (a new direction mid-turn) */
    float waypoints[MAX_CHAR_WAYPOINTS][3];
    int waypoint_count, waypoint_idx;
    int pending_door;          /* connector it walks to (goes through it on arrival), -1 = none */
    int clips[MC_COUNT];       /* its moveset, resolved: library clip per slot, -1 = no animation */
    int clips_gen;             /* g_moveset_gen they were resolved at */
    int play_clip;             /* script "Animate character": clip playing over its stand / walk pose, -1 = none */
    float play_t;
    int play_left;             /* times still to play (this one included), -1 = until the script stops it */
    int play_attack;           /* the clip playing is an attack (attack mode) */
    int play_hold;             /* a script loop (ANIM_LOOP): kept after its script, until replaced or a move order */
    float play_turn;           /* facing change once it ends (the swings that turn the body) */
    char weapon[48];           /* what it holds (starts as its moveset's right_hand; the radial menu changes it) */
    char shield[48];           /* its shield, "" = none */
    CharModel *item;           /* held in the right hand, NULL = nothing */
    CharModel *item_l;         /* held in the left hand (the second blade of a pair) */
    CharModel *shield_m;       /* on the back of the left hand */
    Mat4 hand, hand_l, hand_s; /* the hands' grip bones / the shield's bone in the pose last computed (actor_project) */
    int hand_ok, hand_l_ok, hand_s_ok;
    int stepping;              /* the clip playing moves it from step_from to step_to (attacks, dodge) */
    float step_from[3], step_to[3];
    int play_next, play_next_loop; /* then this clip (looping or not), -1 = none */
    int play_event;            /* at play_event_at s into the clip: EV_WEAPON / EV_SHIELD becomes event_arg */
    float play_event_at;
    char event_arg[48];
    int trail;                 /* the clip leaves a blue trail behind its blades (specials) */
    int guard;                 /* shield: 0 down, 1 raising / held, 2 lowering */
    float play_speed;          /* the clip's own speed (script "Animate character"), 0 = the game's animation speed */
    int play_script;           /* the clip is a script's "Animate character" */
    int play_ended;            /* ...and it's over: held on its last frame until the script goes on (no idle in between) */
    int play_freeze;           /* ...held until its row is over ("Freeze on the last frame") */
    int hold_step;             /* the script step (g_run.step) it belongs to */
    CuePlayer cue;             /* the attack's sounds playing */
    /* combat */
    int side;                  /* SIDE_ALLY (David's side) / SIDE_ENEMY: blows only land on the other side */
    int hp, hp_max;            /* health (its stats) */
    int dead, down;            /* health gone: an enemy dead for good / an ally knocked down (until the enemies are beaten) */
    float down_t;              /* how long it's been down */
    int hold_last;             /* the clip playing stays on its last frame once over (dying, knocked down) */
    int ai_on;                 /* its AI plays (a script can turn it off) */
    float ai_cool, ai_repath;  /* AI: until its next attack / its next new path */
    int blow;                  /* the clip playing is a blow (it hits what its blades touch) */
    unsigned hit_mask;         /* ...the characters it has hit already */
    int dodging;               /* its dodge is playing: blows miss it */
    float blade[2][2][3], blade_prev[2][2][3]; /* its blades in the world (base, tip) in the last two poses drawn */
    int blade_ok[2], blade_prev_ok[2];
    /* magic */
    int play_rev;              /* the clip plays backward (an orb put away: magkup the other way) */
    int cast_pending, cast_clip, cast_elem; /* a spell leaves its hands at cast_at s into cast_clip */
    float cast_at;
    float slow_t, str_t, armour_t, invuln_t, poison_t, poison_tick, buff_fx_t; /* slowed (time / ice), stronger, armoured, untouchable, poisoned */
    CharModel *potion_m;       /* a potion in its hand (drinking: left, throwing: right) */
    int pot_right, pot_pending, pot_clip, pot_kind;
    float pot_at;
} Actor;
enum { SIDE_ALLY, SIDE_ENEMY };
static int g_atk_queued = -1;  /* attack mode: the next attack, asked for while one plays */
static void david_attack(int slot);
enum { EV_NONE, EV_WEAPON, EV_SHIELD };
static void actor_play_end(Actor *a);
static void trail_sample(Actor *a);
static int weapon_is_orb(const char *w); /* MAGIC */
static void orb_glow_draw(Actor *a, uint32_t *px, int W, int H, float sc);
static int grip_node(const CharModel *m, int left); /* MOVESETS */
static int shield_node(const CharModel *m);
static double perf_now_ms(void);
static void render_mesh_hires(const CharModel *m, float *vx, float *vy, const float *vz, const int *vis,
                              uint32_t *px, int W, int H, float sc, float *zb, uint32_t *zst, uint32_t stamp);
static Actor g_actors[MAX_ACTORS];
static Actor *g_act = &g_actors[0];
#define DAVID_ACTOR (&g_actors[0])
#define g_char_pos (g_act->pos)
#define g_char_target (g_act->target)
#define g_char_facing (g_act->facing)
#define g_char_moving (g_act->moving)
#define g_char_walk_mode (g_act->walk_mode)
#define g_char_anim_t (g_act->anim_t)
#define g_char_turn_clip (g_act->turn_clip)
#define g_char_turn_t (g_act->turn_t)
#define g_char_turn_to (g_act->turn_to)
#define g_char_turn_from_stand (g_act->turn_from_stand)
#define g_char_turn_interrupted (g_act->turn_interrupted)
#define g_char_waypoints (g_act->waypoints)
#define g_char_waypoint_count (g_act->waypoint_count)
#define g_char_waypoint_idx (g_act->waypoint_idx)
#define g_pending_door (g_act->pending_door)
static void actor_begin(Actor *a) { g_act = a; }
static void actor_end(void) { g_act = DAVID_ACTOR; }
/* David's speeds -- global settings (editor "David & navigation" panel,
   "DAVID (ALL ROOMS)" group, saved in data/david.cfg) */
static float g_david_walk_speed = 6.0f;  /* world units/sec, matches the Python prototype */
static float g_david_run_speed = 13.0f;  /* run.gltf's own stride is visibly ~2x walk's cadence */
static float g_david_anim_speed = 1.0f;  /* playback factor for ALL his animations */
static float g_david_run_turn_pct = 50.0f; /* speed during a turn-start while running (run90a/c, run180a/c), % of run speed */
static float g_david_shadow_radius = 0.7f; /* half-width of his floor shadow, world units (was a fixed 0.45) */
#define CHAR_WALK_SPEED (g_david_walk_speed)
#define CHAR_RUN_SPEED (g_david_run_speed)
#define TURN_INTERRUPT_WINDOW 0.3f /* a new direction restarts the turn only in its first 30% */
static void actor_resolve_clips(Actor *a); /* MOVESETS */
#define CHAR_RADIUS 0.4f /* coarse capsule half-width for path/wall checks */

/* Multi-waypoint queue for A*-routed moves (see try_click_to_move):
   g_char_target is always the CURRENT leg's destination; the rest of
   the route waits in g_char_waypoints and advance_character pulls the
   next one off on arrival, same run/walk mode throughout. A direct,
   unobstructed click never touches it -- it's just a single-target move
   like before, queue count 0. */

/* Small on-screen marker at the current click-to-move destination
   (item 16): appears where the player clicked, disappears once David
   actually reaches it OR a new click replaces it (set_click_marker is
   called from every successful try_click_to_move branch, overwriting
   whichever target was active). Stored in WORLD space and re-projected
   every frame (not a fixed screen position) so it stays correctly
   anchored if the view scrolls. */
static int g_click_marker_active = 0;
static float g_click_marker_world[3];
static int script_playing(void); /* SCRIPTS */
static void script_draw_portraits(uint32_t *px, int W, int H, float sc);
static void radial_draw(uint32_t *px, int W, int H, int ox, int oy);
static void radial_draw_text(HDC hdc);
static void blend_sprite(uint32_t *px, int W, int H, const uint32_t *spr, int sw, int sh, int x0, int y0, float sc);
static inline uint32_t lerp_rgb(uint32_t a, uint32_t b, uint32_t w);

static void set_click_marker(const float world[3]) {
    if (g_act != DAVID_ACTOR || script_playing()) return; /* the player's own orders only */
    g_click_marker_world[0] = world[0];
    g_click_marker_world[1] = world[1];
    g_click_marker_world[2] = world[2];
    g_click_marker_active = 1;
}

/* Floor height + footprint of the current room (computed from its
   glTF, see compute_floor_and_hull; only a spawn fallback). */
#define MAX_ROOM_HULL 64
static float g_room_hull[MAX_ROOM_HULL][2];
static int g_room_hull_count = 0;
static float g_room_floor_y = 0.0f;

/* The room's environment geometry (glTF mesh 0 as a triangle soup):
   click raycasts, collisions, pathfinding, David's occlusion. */
static float *g_room_mesh_tris = NULL; /* triangle_count * 9 floats */
static int g_room_mesh_tri_count = 0;

/* Coarse walkability grid over the current room's mesh, built once per
   room load -- powers A* pathfinding for clicks a straight line can't
   reach (see try_click_to_move / navgrid_find_path in character.c). */
static NavGrid g_nav_grid;
static int g_nav_gen = 0; /* bumped on every nav grid (re)build -> P overlay cache */
static void shapes_load(const char *label); /* scene editor shapes (zones, foreground, connectors) */
static void room_settings_load(const char *label); /* per-room David & navigation settings */

static int g_rdc_tris;
static void free_room_mesh(void) {
    g_rdc_tris = -1; /* room depth cache: stale */
    free(g_room_mesh_tris);
    g_room_mesh_tris = NULL;
    g_room_mesh_tri_count = 0;
    navgrid_free(&g_nav_grid);
    trimesh_invalidate_cache();
}

/* The blockouts' exports carry a 12-triangle cuboid enclosing the whole scene
   (usually in the helper mesh 1, in a few rooms -- atro1/4/8/9/10 -- in
   mesh 0). It isn't environment geometry: its near face would hide the
   whole room and its top would read as a floor far above the real one.
   Detected by shape: a single 12-triangle primitive whose bounds contain
   every other primitive's bounds. */
static int is_bounding_box_helper(const float *pos, int n, int tris, const float *lo_others, const float *hi_others) {
    if (tris != 12 || !lo_others) return 0;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) { lo[k] = fminf(lo[k], pos[i * 3 + k]); hi[k] = fmaxf(hi[k], pos[i * 3 + k]); }
    for (int k = 0; k < 3; k++) if (!(lo[k] <= lo_others[k] + 1.0f && hi[k] >= hi_others[k] - 1.0f)) return 0;
    return 1;
}

static int cmp_pt(const void *a, const void *b) {
    const double *p = (const double *)a, *q = (const double *)b;
    if (p[0] != q[0]) return p[0] < q[0] ? -1 : 1;
    if (p[1] != q[1]) return p[1] < q[1] ? -1 : 1;
    return 0;
}
static double hull_cross(const double *o, const double *a, const double *b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); }

/* Floor height + walkable footprint (only a fallback for the spawn): the
   most populated 0.5-unit height band in the lower half of the room's
   vertices, then the convex hull (XZ) of the vertices within 1 unit of
   it (all vertices if fewer than 3). */
static void compute_floor_and_hull(const float *pts, int n) {
    g_room_hull_count = 0; g_room_floor_y = 0.0f;
    if (n <= 0) return;
    double ymin = 1e30, ymax = -1e30;
    for (int i = 0; i < n; i++) { ymin = fmin(ymin, pts[i * 3 + 1]); ymax = fmax(ymax, pts[i * 3 + 1]); }
    double ymid = (ymin + ymax) / 2;
    int cap = 64, nb = 0; long *bkey = malloc(sizeof(long) * cap); int *bcnt = malloc(sizeof(int) * cap);
    for (int i = 0; i < n; i++) {
        double y = pts[i * 3 + 1];
        if (y > ymid) continue;
        long key = (long)rint(y / 0.5); /* round half to even, like Python's round */
        int j = 0;
        while (j < nb && bkey[j] != key) j++;
        if (j == nb) { if (nb == cap) { cap *= 2; bkey = realloc(bkey, sizeof(long) * cap); bcnt = realloc(bcnt, sizeof(int) * cap); } bkey[nb] = key; bcnt[nb] = 0; nb++; }
        bcnt[j]++;
    }
    int best = 0;
    for (int j = 1; j < nb; j++) if (bcnt[j] > bcnt[best]) best = j;
    g_room_floor_y = nb ? (float)(bkey[best] * 0.5) : (float)ymin;
    free(bkey); free(bcnt);
    double *p = malloc(sizeof(double) * 2 * n);
    int m = 0;
    for (int i = 0; i < n; i++) if (fabs(pts[i * 3 + 1] - (double)g_room_floor_y) < 1.0) { p[m * 2] = pts[i * 3]; p[m * 2 + 1] = pts[i * 3 + 2]; m++; }
    if (m < 3) { m = 0; for (int i = 0; i < n; i++) { p[m * 2] = pts[i * 3]; p[m * 2 + 1] = pts[i * 3 + 2]; m++; } }
    qsort(p, m, sizeof(double) * 2, cmp_pt);
    int u = 0; /* unique */
    for (int i = 0; i < m; i++) if (u == 0 || cmp_pt(p + i * 2, p + (u - 1) * 2) != 0) { p[u * 2] = p[i * 2]; p[u * 2 + 1] = p[i * 2 + 1]; u++; }
    double *h = malloc(sizeof(double) * 2 * (2 * u + 2));
    int k = 0;
    for (int i = 0; i < u; i++) { while (k >= 2 && hull_cross(h + (k - 2) * 2, h + (k - 1) * 2, p + i * 2) <= 0) k--; h[k * 2] = p[i * 2]; h[k * 2 + 1] = p[i * 2 + 1]; k++; }
    for (int i = u - 2, t = k + 1; i >= 0; i--) { while (k >= t && hull_cross(h + (k - 2) * 2, h + (k - 1) * 2, p + i * 2) <= 0) k--; h[k * 2] = p[i * 2]; h[k * 2 + 1] = p[i * 2 + 1]; k++; }
    if (u > 2) k--; /* last point repeats the first */
    else k = u;
    if (k > MAX_ROOM_HULL) k = MAX_ROOM_HULL;
    for (int i = 0; i < k; i++) { g_room_hull[i][0] = (float)h[i * 2]; g_room_hull[i][1] = (float)h[i * 2 + 1]; }
    g_room_hull_count = k;
    free(p); free(h);
}

/* The room's environment geometry (glTF mesh 0, every primitive except
   the scene bounding-box helper) as a triangle soup, + floor/hull. */
static int load_room_geometry(const Gltf *g) {
    free_room_mesh();
    const JsonValue *prims = json_get(json_at(json_get(g->root, "meshes"), 0), "primitives");
    int np = json_len(prims);
    if (np <= 0) return 0;
    float **pos = calloc(np, sizeof(float *)); uint32_t **idx = calloc(np, sizeof(uint32_t *));
    int *nv = calloc(np, sizeof(int)), *ni = calloc(np, sizeof(int));
    int total_v = 0, total_t = 0, c;
    for (int i = 0; i < np; i++) {
        const JsonValue *pr = json_at(prims, i);
        pos[i] = gltf_read_floats(g, json_int(json_get(json_get(pr, "attributes"), "POSITION"), -1), &nv[i], &c);
        if (pos[i] && c != 3) { free(pos[i]); pos[i] = NULL; }
        idx[i] = gltf_read_uints(g, json_int(json_get(pr, "indices"), -1), &ni[i], &c);
        if (!pos[i]) nv[i] = 0;
        if (!idx[i]) ni[i] = 0;
        total_v += nv[i];
    }
    float *all = malloc(sizeof(float) * 3 * (total_v ? total_v : 1));
    int av = 0;
    for (int i = 0; i < np; i++) { if (nv[i]) memcpy(all + av * 3, pos[i], sizeof(float) * 3 * nv[i]); av += nv[i]; }
    compute_floor_and_hull(all, av);
    for (int i = 0; i < np; i++) {
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }; int others = 0;
        for (int j = 0; j < np; j++) if (j != i) for (int v = 0; v < nv[j]; v++) { others = 1; for (int k = 0; k < 3; k++) { lo[k] = fminf(lo[k], pos[j][v * 3 + k]); hi[k] = fmaxf(hi[k], pos[j][v * 3 + k]); } }
        if (is_bounding_box_helper(pos[i], nv[i], ni[i] / 3, others ? lo : NULL, hi)) { ni[i] = 0; continue; }
        total_t += ni[i] / 3;
    }
    float *tris = malloc(sizeof(float) * 9 * (total_t ? total_t : 1));
    int t = 0;
    for (int i = 0; i < np; i++)
        for (int q = 0; q + 2 < ni[i]; q += 3)
            for (int k = 0; k < 3; k++) {
                uint32_t vi = idx[i][q + k];
                if (vi >= (uint32_t)nv[i]) vi = 0;
                memcpy(tris + (size_t)t * 9 + k * 3, pos[i] + (size_t)vi * 3, sizeof(float) * 3);
                if (k == 2) t++;
            }
    for (int i = 0; i < np; i++) { free(pos[i]); free(idx[i]); }
    free(pos); free(idx); free(nv); free(ni); free(all);
    g_room_mesh_tris = tris;
    g_room_mesh_tri_count = t;
    trimesh_invalidate_cache();
    return t > 0;
}

/* Walkable grid (needs the room's navigation settings loaded first). */
static void build_room_nav(void) {
    if (g_room_mesh_tri_count <= 0) return;
    navgrid_build(&g_nav_grid, g_room_mesh_tris, g_room_mesh_tri_count, 0.5f, g_room_floor_y - 12.0f);
    g_nav_gen++;
}

/* The original game's camera roll: stored in degrees in its camera
   header, applied by the blockouts' export as radians with the opposite sign.
   Camera = the blockouts' position/forward, rebuilt upright, rolled by -roll
   degrees about the view axis (measured best on the depth masks, see
   docs/PROGRESS.md). q: glTF [x y z w]. */
static void correct_camera_roll(float q[4], float native_roll_deg) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                       { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                       { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    double fwd[3] = { -R[0][2], -R[1][2], -R[2][2] };
    double r0[3] = { -fwd[2], 0.0, fwd[0] }; /* fwd x (0,1,0) */
    double n = sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
    if (n < 1e-6) return;
    for (int k = 0; k < 3; k++) r0[k] /= n;
    double u0[3] = { r0[1] * fwd[2] - r0[2] * fwd[1], r0[2] * fwd[0] - r0[0] * fwd[2], r0[0] * fwd[1] - r0[1] * fwd[0] };
    double a = -native_roll_deg * 3.14159265358979 / 180.0, ca = cos(a), sa = sin(a);
    double M[3][3];
    for (int k = 0; k < 3; k++) {
        M[k][0] = r0[k] * ca + u0[k] * sa;
        M[k][1] = -r0[k] * sa + u0[k] * ca;
        M[k][2] = -fwd[k];
    }
    double tr = M[0][0] + M[1][1] + M[2][2], qx, qy, qz, qw;
    if (tr > 0) { double s = sqrt(tr + 1.0) * 2; qw = 0.25 * s; qx = (M[2][1] - M[1][2]) / s; qy = (M[0][2] - M[2][0]) / s; qz = (M[1][0] - M[0][1]) / s; }
    else if (M[0][0] > M[1][1] && M[0][0] > M[2][2]) { double s = sqrt(1.0 + M[0][0] - M[1][1] - M[2][2]) * 2; qw = (M[2][1] - M[1][2]) / s; qx = 0.25 * s; qy = (M[0][1] + M[1][0]) / s; qz = (M[0][2] + M[2][0]) / s; }
    else if (M[1][1] > M[2][2]) { double s = sqrt(1.0 + M[1][1] - M[0][0] - M[2][2]) * 2; qw = (M[0][2] - M[2][0]) / s; qx = (M[0][1] + M[1][0]) / s; qy = 0.25 * s; qz = (M[1][2] + M[2][1]) / s; }
    else { double s = sqrt(1.0 + M[2][2] - M[0][0] - M[1][1]) * 2; qw = (M[1][0] - M[0][1]) / s; qx = (M[0][2] + M[2][0]) / s; qy = (M[1][2] + M[2][1]) / s; qz = 0.25 * s; }
    q[0] = (float)qx; q[1] = (float)qy; q[2] = (float)qz; q[3] = (float)qw;
}

/* ---- Spawn visibility scoring (render-in-the-loop) ----
   David's skinned idle-pose vertex offsets are computed once and cached;
   scoring a candidate spawn point is then just "translate, project
   through char_project, depth-test against g_depth_buffer_char" -- exactly
   what count_character_visible_vertices / VERIFY_SPAWNS measure, so the
   spawn picker optimizes the real criterion instead of a proxy. */
/* Projects one of David's body vertices. His BODY is drawn through the
   roll-free g_char_cam (so he stays upright on dutch-angle cameras), but
   the result is translated in screen space so his ANCHOR (feet/root,
   `anchor`) lands exactly where the room's real camera puts it --
   otherwise, on any rolled camera, g_char_cam's in-plane rotation would
   draw him at a rotated screen position, detached from the floor he
   actually stands on in the photo (verified: palace/s_room_2, a near
   top-down camera, drew him on top of a side wall). Both cameras share
   position and view axis, so the depth is identical and the ROOM
   camera's depth buffer is the right one to test against. */
static int char_project(const float anchor[3], const float world[3], float *px, float *py, float *pz) {
    int w = (int)g_hdr.width, h = (int)g_hdr.height;
    if (!camera_world_to_pixel_z(&g_char_cam, world, w, h, px, py, pz)) return 0;
    float ax_r, ay_r, az_r, ax_c, ay_c, az_c;
    if (camera_world_to_pixel_z(&g_room_cam, anchor, w, h, &ax_r, &ay_r, &az_r) &&
        camera_world_to_pixel_z(&g_char_cam, anchor, w, h, &ax_c, &ay_c, &az_c)) {
        *px += ax_r - ax_c;
        *py += ay_r - ay_c;
    }
    return 1;
}

static void camera_apply_roll(RoomCamera *cam, float deg);
#define SPAWN_GOOD_VIS_PCT 30
static void render_room_mesh_depth_only_cam(const RoomCamera *cam, float *depth_buf);
static float g_david_idle_offsets[DAVID_MAX_VERTS][3];
static int g_david_idle_offsets_ready = 0;

static void ensure_david_idle_offsets(void) {
    if (g_david_idle_offsets_ready) return;
    NodeOverride overrides[DAVID_MAX_NODES];
    anim_sample_idle(0.0f, overrides);
    static Mat4 skin_mats[DAVID_MAX_JOINTS];
    skeleton_compute_skin_matrices(overrides, skin_mats);
    for (int vi = 0; vi < DAVID_VERTEX_COUNT; vi++) {
        float acc[3] = {0, 0, 0};
        for (int k = 0; k < 4; k++) {
            float wt = david_weights[vi][k];
            if (wt <= 0) continue;
            float sp[3];
            mat4_vec3(&skin_mats[david_joints_idx[vi][k]], david_positions[vi], sp);
            acc[0] += sp[0]*wt; acc[1] += sp[1]*wt; acc[2] += sp[2]*wt;
        }
        g_david_idle_offsets[vi][0] = acc[0];
        g_david_idle_offsets[vi][1] = acc[1];
        g_david_idle_offsets[vi][2] = acc[2];
    }
    g_david_idle_offsets_ready = 1;
}

/* (Re)rasterizes the room's depth (through the real room camera -- see
   char_project) into g_depth_buffer_char. Must run before
   spawn_visibility_at. */
static void spawn_prepare_visibility_depth(void) {
    if (!g_depth_buffer_char) return;
    size_t npix = (size_t)g_dib_stride * g_hdr.height;
    for (size_t p = 0; p < npix; p++) g_depth_buffer_char[p] = 1e29f;
    render_room_mesh_depth_only_cam(&g_room_cam, g_depth_buffer_char);
}

/* Number of David's idle-pose vertices that would be drawn (on-screen
   and unoccluded) if he stood at (x,y,z). */
static int spawn_visibility_at(float x, float y, float z) {
    ensure_david_idle_offsets();
    int w = (int)g_hdr.width, h = (int)g_hdr.height;
    int visible_count = 0;
    float anchor[3] = { x, y, z };
    for (int vi = 0; vi < DAVID_VERTEX_COUNT; vi++) {
        float world[3] = { g_david_idle_offsets[vi][0]+x, g_david_idle_offsets[vi][1]+y, g_david_idle_offsets[vi][2]+z };
        float px, py, pz;
        if (!char_project(anchor, world, &px, &py, &pz)) continue;
        if (px < 0 || px >= w || py < 0 || py >= h) continue;
        size_t idx = (size_t)(int)py * g_dib_stride + (int)px;
        if (g_depth_buffer_char && pz >= g_depth_buffer_char[idx]) continue;
        visible_count++;
    }
    return visible_count;
}

/* Spawn-picker score: visible vertex count, but a candidate so close to
   the camera that David's projected height exceeds SPAWN_MAX_HEIGHT_PCT
   of the 4:3 view height (width*3/4) is scaled down proportionally --
   fully "visible" but a giant filling the frame is not a good spawn. */
#define SPAWN_MAX_HEIGHT_PCT 50
static int spawn_score_at(float x, float y, float z) {
    int vis = spawn_visibility_at(x, y, z);
    if (vis == 0) return 0;
    float min_py = 1e30f, max_py = -1e30f;
    float anchor[3] = { x, y, z };
    for (int vi = 0; vi < DAVID_VERTEX_COUNT; vi += 4) {
        float wp[3] = { g_david_idle_offsets[vi][0]+x, g_david_idle_offsets[vi][1]+y, g_david_idle_offsets[vi][2]+z };
        float px, py, pz;
        if (!char_project(anchor, wp, &px, &py, &pz)) continue;
        if (py < min_py) min_py = py;
        if (py > max_py) max_py = py;
    }
    float hpct = max_py > min_py ? (max_py - min_py) * 100.0f / (g_hdr.width * 0.75f) : 0.0f;
    if (hpct > SPAWN_MAX_HEIGHT_PCT) vis = (int)(vis * SPAWN_MAX_HEIGHT_PCT / hpct);
    return vis;
}

/* Last-resort candidate set: centroids of the mesh's own floor-ish
   triangles that project on-screen through g_room_cam, scored with the
   same visibility count. Ties go to the one closest to frame center.
   Returns the best score (0 if nothing usable). */
static int spawn_best_floor_triangle(float *ox, float *oy, float *oz) {
    int w = (int)g_hdr.width, h = (int)g_hdr.height;
    int stride = g_room_mesh_tri_count > 6000 ? g_room_mesh_tri_count / 6000 + 1 : 1;
    int best_vis = 0;
    float best_d2 = 1e30f;
    for (int t = 0; t < g_room_mesh_tri_count; t += stride) {
        const float *v0 = g_room_mesh_tris + (size_t)t * 9;
        const float *v1 = v0 + 3, *v2 = v0 + 6;
        float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
        float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        float nlen = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nlen < 1e-8f || fabsf(n[1] / nlen) < 0.55f) continue; /* same slope limit as click-to-move */
        float c[3] = { (v0[0]+v1[0]+v2[0])/3.0f, (v0[1]+v1[1]+v2[1])/3.0f, (v0[2]+v1[2]+v2[2])/3.0f };
        float px, py, pz;
        if (!camera_world_to_pixel_z(&g_room_cam, c, w, h, &px, &py, &pz)) continue;
        if (px < 0 || px >= w || py < 0 || py >= h) continue;
        if (g_nav_grid.comp_count > 0) {
            int big = 0;
            for (int k = 0; k < g_nav_grid.comp_count; k++) if (g_nav_grid.comp_size[k] > big) big = g_nav_grid.comp_size[k];
            if (navgrid_area_size_at(&g_nav_grid, c[0], c[1], c[2]) * 4 < big) continue; /* isolated pocket */
        }
        int v = spawn_score_at(c[0], c[1], c[2]);
        float ddx = px - w*0.5f, ddy = py - h*0.5f, d2 = ddx*ddx + ddy*ddy;
        if (v > best_vis || (v == best_vis && v > 0 && d2 < best_d2)) {
            best_vis = v; best_d2 = d2; *ox = c[0]; *oy = c[1]; *oz = c[2];
        }
    }
    return best_vis;
}

/* Camera, geometry and David for a room, from its glTF
   (assets/levels/<level>/<room>/<room>.gltf) + this project's per-room
   data (data/rooms/<level>/<room>_camera.cfg: the original game's camera
   roll; _intrinsics.cfg: the calibrated projection). Returns 1 on success. */
static int try_start_3d_character_for_room(const char *label) {
    char level[64], room[64], path[1024], line[256];
    split_label(label, level, sizeof(level), room, sizeof(room));
    root_path(path, sizeof(path), "assets/levels/%s/%s/%s.gltf", level, room, room);
    Gltf g;
    if (!gltf_load(path, &g)) return 0;
    float cam_t[3] = {0}, cam_r[4] = {0, 0, 0, 1}, yfov = 1.0f;
    int have_cam = 0;
    const JsonValue *nodes = json_get(g.root, "nodes");
    for (int i = 0; i < json_len(nodes) && !have_cam; i++) {
        const JsonValue *n = json_at(nodes, i);
        int ci = json_int(json_get(n, "camera"), -1);
        if (ci < 0) continue;
        for (int k = 0; k < 3; k++) cam_t[k] = (float)json_num(json_at(json_get(n, "translation"), k), 0.0);
        for (int k = 0; k < 4; k++) cam_r[k] = (float)json_num(json_at(json_get(n, "rotation"), k), k == 3 ? 1.0 : 0.0);
        yfov = (float)json_num(json_get(json_get(json_at(json_get(g.root, "cameras"), ci), "perspective"), "yfov"), 1.0);
        have_cam = 1;
    }
    int geo = have_cam && load_room_geometry(&g);
    gltf_free(&g);
    if (!have_cam || g_room_hull_count < 3) return 0;
    (void)geo;

    float native_roll = 0.0f;
    root_path(path, sizeof(path), "data/rooms/%s/%s_camera.cfg", level, room);
    FILE *fc = fopen(path, "r");
    if (fc) { while (fgets(line, sizeof(line), fc)) sscanf(line, "native_roll %f", &native_roll); fclose(fc); }
    if (fabsf(native_roll) > 1e-6f) correct_camera_roll(cam_r, native_roll);
    camera_init(&g_room_cam, cam_t, cam_r, yfov);
    {   /* measured intrinsics, if this room was calibrated against its
           original depth mask (see run_camera_calibration) */
        root_path(path, sizeof(path), "data/rooms/%s/%s_intrinsics.cfg", level, room);
        FILE *fi = fopen(path, "r");
        if (fi) {
            float fo = 0, fcx = 0, fcy = 0, froll = 0;
            while (fgets(line, sizeof(line), fi)) {
                sscanf(line, "focal %f", &fo);
                sscanf(line, "cx %f", &fcx);
                sscanf(line, "cy %f", &fcy);
                sscanf(line, "roll %f", &froll);
            }
            fclose(fi);
            if (fo > 0) { g_room_cam.focal = fo; g_room_cam.cx = fcx; g_room_cam.cy = fcy; }
            if (froll != 0) camera_apply_roll(&g_room_cam, froll);
        }
    }
    camera_make_upright(&g_room_cam, &g_char_cam);
    room_settings_load(label); /* before the walkable grid is built */
    shapes_load(label);
    build_room_nav();

    /* fallback start (the camera-ray search below normally decides):
       halfway between two opposite hull points, on the floor */
    int hull_count = g_room_hull_count;
    float cx = (g_room_hull[0][0] + g_room_hull[hull_count/2][0]) / 2.0f;
    float cz = (g_room_hull[0][1] + g_room_hull[hull_count/2][1]) / 2.0f;
    float cy = g_room_floor_y;

    /* Pick the start position by raycasting a grid of screen points
       against the real mesh and keeping the FARTHEST successful floor
       hit. A direct raycast is the simplest correct answer here: since
       it returns the NEAREST triangle along the ray, a successful
       floor_only hit is BY CONSTRUCTION the real visible surface at
       that pixel (never occluded, never a hidden basement slab -- both
       real failure modes hit while developing this: a single largest-
       floor-triangle heuristic can pick geometry occluded by the
       boiler, or below the real floor entirely). Taking the FARTHEST
       of several candidates (not just the first hit) avoids the other
       failure mode found here: a near-camera hit makes David look like
       a giant relative to the rest of the scene. No per-room manual
       authoring needed, same as the rest of this walkability system. */
    /* NOTE: a "largest connected navgrid component, closest cell to
       frame center" strategy was tried here (reusing navgrid_largest_
       component -- see character.c) and measured WORSE (40 -> 62 bad
       rooms via VERIFY_SPAWNS) than the plain ray-grid approach below.
       Reverted. Likely cause: the nav grid's connectivity is a property
       of the WHOLE mesh, not what THIS camera actually frames, so "the
       biggest floor region" is often mostly off-screen for this shot,
       and picking its closest on-screen cell can still be a bad corner/
       edge position. Left as a cautionary note (see docs/PROGRESS.md) rather
       than re-attempted blind -- three different heuristics have now
       been tried and measured against this same 40-bad baseline without
       beating it; the remaining tail likely needs a genuinely different
       approach (e.g. camera-frustum-culled navmesh), not a fourth
       variant of "smarter point selection". */
    if (g_room_mesh_tri_count > 0) {
        int w = (int)g_hdr.width, h = (int)g_hdr.height;
        float hit_x[49], hit_y[49], hit_z[49], hit_depth[49];
        int n_hits = 0;
        /* Progressively relax the floor-slope threshold if the first,
           strict pass finds nothing -- a real failure mode on rooms
           whose ENTIRE visible surface from this camera is a steep
           street/ramp (verified case: rain/aftastep, a diagonal
           cobblestone street where every grid sample exceeded 0.55
           from every angle): falling back silently to a hull-XZ +
           floor_y guess put David's Y at a value that didn't correspond
           to ANY real nearby surface, badly distorting his projected
           pose (looked "upside down" -- an extreme-perspective artifact
           of a badly wrong 3D position, not a camera or rendering bug).
           A relaxed-but-still-raycasted point is always a real surface
           point, so it can't produce that failure mode.

           NOTE: a denser 15x15 grid + median-HEIGHT filtering was tried
           here (see PROGRESS.md, "a lesson in NOT trusting a plausible-
           looking fix, part 2") and measured WORSE (40 -> 64 bad rooms
           via VERIFY_SPAWNS) than this plain 7x7 + median-DEPTH version
           -- reverted. Don't re-attempt that specific shape of fix
           without re-measuring; the plain version below is the
           confirmed-best state. */
        static const float SLOPE_FALLBACKS[] = {0.55f, 0.35f, 0.15f, -0.5f};
        for (size_t pass = 0; pass < sizeof(SLOPE_FALLBACKS)/sizeof(SLOPE_FALLBACKS[0]) && n_hits == 0; pass++) {
            float min_up_dot = SLOPE_FALLBACKS[pass];
            for (int gy = 1; gy <= 7 && n_hits < 49; gy++) {
                for (int gx = 1; gx <= 7 && n_hits < 49; gx++) {
                    float spx = w * gx / 8.0f, spy = h * gy / 8.0f;
                    float origin[3], dir[3], hit[3], normal[3];
                    /* Sample through the room's REAL camera: David's
                       anchor is projected through it too (char_project),
                       so a hit framed in the photo is where he's drawn. */
                    camera_pixel_to_ray(&g_room_cam, spx, spy, w, h, origin, dir);
                    if (!raycast_room_mesh_view(origin, dir, g_room_mesh_tris, g_room_mesh_tri_count,
                                            1 /* floor_only */, min_up_dot, hit, normal)) continue;
                    float dx = hit[0]-origin[0], dy = hit[1]-origin[1], dz = hit[2]-origin[2];
                    hit_x[n_hits] = hit[0]; hit_y[n_hits] = hit[1]; hit_z[n_hits] = hit[2];
                    hit_depth[n_hits] = sqrtf(dx*dx+dy*dy+dz*dz);
                    n_hits++;
                }
            }
        }
        if (n_hits > 0) {
            /* MEDIAN depth among successful hits -- not nearest (which
               made David look like a giant, right up against the
               camera) or farthest (tiny, barely visible at the far
               edge of the room) -- a typical, well-framed distance.
               Simple insertion sort, n_hits <= 49. */
            for (int a = 1; a < n_hits; a++) {
                float dk = hit_depth[a], xk = hit_x[a], yk = hit_y[a], zk = hit_z[a];
                int b = a - 1;
                while (b >= 0 && hit_depth[b] > dk) {
                    hit_depth[b+1]=hit_depth[b]; hit_x[b+1]=hit_x[b]; hit_y[b+1]=hit_y[b]; hit_z[b+1]=hit_z[b];
                    b--;
                }
                hit_depth[b+1]=dk; hit_x[b+1]=xk; hit_y[b+1]=yk; hit_z[b+1]=zk;
            }
            int mid = n_hits / 2;
            cx = hit_x[mid]; cy = hit_y[mid]; cz = hit_z[mid];

            /* Render-in-the-loop check (see PROGRESS.md: three proxy
               heuristics were measured and all lost to plain median-
               depth, because none of them tracked "is he actually drawn").
               Score the median pick with the SAME depth-tested vertex
               count VERIFY_SPAWNS uses; only when it's genuinely poorly
               visible, fall back to whichever grid candidate scores best,
               ties broken by closeness to the median depth so framing/
               scale stays as close to the median choice as possible. */
            spawn_prepare_visibility_depth();
            /* Render-in-the-loop + walkable-area check. Each candidate
               gets (a) the same depth-tested visibility score VERIFY_SPAWNS
               measures and (b) the size of the connected walkable area it
               belongs to (navgrid_area_size_at: same step/wall rules as
               A*). Only candidates in a big area are eligible -- spawning
               in an isolated pocket (verified: gno/boilarea, on a walled
               platform) made every click in the room unreachable. Among
               eligible, well-visible candidates, keep the one closest to
               the median depth (natural framing/scale). */
            int area[49], score[49], amax = 0;
            for (int k = 0; k < n_hits; k++) {
                /* move each candidate to where David's body actually fits
                   BEFORE scoring it, so visibility is measured at the
                   position he will really spawn at */
                if (!stand_ok(hit_x[k], hit_y[k], hit_z[k], g_room_mesh_tris, g_room_mesh_tri_count)) {
                    float sp[3];
                    if (navgrid_snap(&g_nav_grid, hit_x[k], hit_y[k], hit_z[k], sp)) { hit_x[k] = sp[0]; hit_y[k] = sp[1]; hit_z[k] = sp[2]; }
                }
                area[k] = navgrid_area_size_at(&g_nav_grid, hit_x[k], hit_y[k], hit_z[k]);
                score[k] = spawn_score_at(hit_x[k], hit_y[k], hit_z[k]);
                if (area[k] > amax) amax = area[k];
            }
            /* Visible first, then biggest walkable area among the visible
               candidates (an area nobody can see is useless as a spawn --
               verified: rain/barroom's largest floor is entirely hidden
               from its camera), then closest to the median depth. */
            int vis_amax = 0;
            for (int k = 0; k < n_hits; k++)
                if (score[k] * 100 >= DAVID_VERTEX_COUNT * SPAWN_GOOD_VIS_PCT && area[k] > vis_amax) vis_amax = area[k];
            int pick = -1, pick_d = 1 << 30, best_any = -1, best_any_s = -1;
            for (int k = 0; k < n_hits; k++) {
                if (score[k] > best_any_s) { best_any_s = score[k]; best_any = k; }
                if (score[k] * 100 < DAVID_VERTEX_COUNT * SPAWN_GOOD_VIS_PCT) continue;
                if (area[k] * 2 < vis_amax) continue;
                if (abs(k - mid) < pick_d) { pick_d = abs(k - mid); pick = k; }
            }
            (void)amax;
            if (pick < 0) pick = best_any;
            if (pick >= 0) { cx = hit_x[pick]; cy = hit_y[pick]; cz = hit_z[pick]; }
            if (pick < 0 || score[pick] * 100 < DAVID_VERTEX_COUNT * SPAWN_GOOD_VIS_PCT) {
                /* grid poor: also try the mesh's own floor triangles */
                float fx, fy, fz;
                int fv = spawn_best_floor_triangle(&fx, &fy, &fz);
                if (fv > (pick >= 0 ? score[pick] : 0)) { cx = fx; cy = fy; cz = fz; }
            }
        } else {
            /* Camera-ray grid found nothing even at the most permissive
               slope threshold -- can happen when the room's walkable
               floor only occupies a small sliver of THIS camera's frame
               (verified case: rain/aftastep, an elevated camera mostly
               framing rooftops/walls with the real street just a corner
               a 7x7 fixed-fraction grid can genuinely miss). Fall back
               to scanning the mesh's own triangles directly for a
               floor-like one (same classification, no camera/screen
               dependency at all), keeping whichever is closest to the
               hull-based XZ estimate -- guarantees David spawns ON REAL
               GEOMETRY rather than a synthetic hull/floor_y combination
               that doesn't correspond to any actual surface (the actual
               root cause of the "upside down"-looking spawn: his Y was
               simply nowhere near the mesh at his X/Z, not a camera or
               render bug). */
            int w2 = (int)g_hdr.width, h2 = (int)g_hdr.height;
            float best_visible_d2 = 1e30f, best_onscreen_d2 = 1e30f, best_any_d2 = 1e30f;
            int have_visible = 0, have_onscreen = 0, have_any = 0;
            float visible_x=cx, visible_y=cy, visible_z=cz;
            float onscreen_x=cx, onscreen_y=cy, onscreen_z=cz;
            float any_x=cx, any_y=cy, any_z=cz;
            for (int t = 0; t < g_room_mesh_tri_count; t++) {
                const float *v0 = g_room_mesh_tris + (size_t)t * 9;
                const float *v1 = v0 + 3, *v2 = v0 + 6;
                float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
                float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
                float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                float nlen = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                if (nlen < 1e-8f) continue;
                float ny = n[1] / nlen;
                if (ny < 0) ny = -ny; /* same "flip to up-ish" convention as raycast_room_mesh */
                if (ny < 0.15f) continue; /* still require something floor-ish, not a bare wall */
                float ccx = (v0[0]+v1[0]+v2[0]) / 3.0f;
                float ccy = (v0[1]+v1[1]+v2[1]) / 3.0f;
                float ccz = (v0[2]+v1[2]+v2[2]) / 3.0f;
                float centroid[3] = {ccx, ccy, ccz};
                /* Prefer a candidate that actually projects ON-SCREEN
                   through the real camera (closest to frame center) --
                   "closest to the hull's XZ" alone (the only fallback
                   before this) can pick a real floor point the camera
                   doesn't even show, leaving David positioned outside
                   the visible frustum with no other symptom than
                   "invisible", not obviously a bad spawn. */
                float px, py, pz;
                if (camera_world_to_pixel_z(&g_room_cam, centroid, w2, h2, &px, &py, &pz) &&
                    px >= 0 && px < w2 && py >= 0 && py < h2) {
                    float ddx = px - w2*0.5f, ddy = py - h2*0.5f;
                    float d2 = ddx*ddx + ddy*ddy;
                    if (d2 < best_onscreen_d2) { best_onscreen_d2 = d2; onscreen_x=ccx; onscreen_y=ccy; onscreen_z=ccz; have_onscreen=1; }
                    /* On-screen alone isn't enough -- a floor triangle
                       from a lower level (verified case: gno/libarea1,
                       a two-story library whose floor has real gaps/pits
                       over a lower level) can project inside the frame
                       while sitting BEHIND nearer geometry from the
                       camera's real viewpoint, leaving David occluded
                       and invisible despite an "on-screen" spawn point.
                       Cross-check by raycasting THROUGH g_room_cam (the
                       camera this on-screen test just used) from that
                       exact pixel and confirming THIS triangle's centroid
                       is genuinely the nearest surface there (within a
                       small tolerance for centroid-vs-exact-hit-point
                       rounding), same real-mesh raycast used everywhere
                       else in this system -- just aimed through the
                       camera that actually matters for what gets drawn. */
                    float origin[3], dir[3], hit[3], normal[3];
                    camera_pixel_to_ray(&g_room_cam, px, py, w2, h2, origin, dir);
                    if (raycast_room_mesh_view(origin, dir, g_room_mesh_tris, g_room_mesh_tri_count, 1, 0.15f, hit, normal)) {
                        float hdx = hit[0]-ccx, hdy = hit[1]-ccy, hdz = hit[2]-ccz;
                        float hd2 = hdx*hdx + hdy*hdy + hdz*hdz;
                        if (hd2 < 4.0f && d2 < best_visible_d2) { best_visible_d2 = d2; visible_x=ccx; visible_y=ccy; visible_z=ccz; have_visible=1; }
                    }
                }
                float ddx2 = ccx - cx, ddz2 = ccz - cz;
                float d2b = ddx2*ddx2 + ddz2*ddz2;
                if (d2b < best_any_d2) { best_any_d2 = d2b; any_x=ccx; any_y=ccy; any_z=ccz; have_any=1; }
            }
            if (have_visible) { cx=visible_x; cy=visible_y; cz=visible_z; }
            else if (have_onscreen) { cx=onscreen_x; cy=onscreen_y; cz=onscreen_z; }
            else if (have_any) { cx=any_x; cy=any_y; cz=any_z; }
            spawn_prepare_visibility_depth();
            int cur_vis = spawn_score_at(cx, cy, cz);
            if (cur_vis * 100 < DAVID_VERTEX_COUNT * SPAWN_GOOD_VIS_PCT) {
                float fx, fy, fz;
                if (spawn_best_floor_triangle(&fx, &fy, &fz) > cur_vis) { cx = fx; cy = fy; cz = fz; }
            }
        }
    }

    if (g_room_mesh_tri_count > 0 && !stand_ok(cx, cy, cz, g_room_mesh_tris, g_room_mesh_tri_count)) {
        float sp[3];
        if (navgrid_snap(&g_nav_grid, cx, cy, cz, sp)) { cx = sp[0]; cy = sp[1]; cz = sp[2]; }
    }
    g_has_3d_character = 1;
    g_char_pos[0] = cx;
    g_char_pos[1] = cy;
    g_char_pos[2] = cz;
    g_char_target[0] = cx;
    g_char_target[1] = cy;
    g_char_target[2] = cz;
    g_char_facing = 0.0f;
    g_char_moving = 0;
    g_char_walk_mode = 0;
    g_char_anim_t = 0.0f;
    g_char_turn_clip = -1;
    return 1;
}

/* Sets a new walk/run target at a world point ALREADY validated as real
   floor (by raycast_room_mesh against the room's actual environment
   mesh -- see the WM_LBUTTONDOWN/DBLCLK handlers -- or, as a fallback
   for the few rooms without mesh data, by walkable_point_in_hull).
   `run` picks the run clip + faster speed (double-click) vs walk
   (single click). */
/* EVERY change of direction (a new order, standing or already moving, and
   each corner of his path) goes through here. The new direction relative
   to his front: front (< 45 deg), left / right (45..135), back-left /
   back-right (> 135).
     run:  front -> straight into the run cycle, left run90a, right run90c,
           back-left run180a, back-right run180c
     walk: front -> straight into the walk cycle, left towalka, right
           towalkc, back-left to180a, back-right to180c
   Standing still, the start transitions take over: front -> torun (run) /
   towalk (walk); the other directions use the rules above.
   Returns the library clip, -1 = none (just turn him). */
static int move_clip_for(float from, float to, int run, int standing) {
    float delta = to - from; /* > 0 = to his LEFT (facing = atan2(dx, dz), Y up) */
    while (delta > 3.14159265f) delta -= 6.2831853f;
    while (delta < -3.14159265f) delta += 6.2831853f;
    float ad = fabsf(delta);
    actor_resolve_clips(g_act);
    const int *mc = g_act->clips;
    if (ad < 0.785398f) return standing ? mc[run ? MC_TORUN : MC_TOWALK] : -1;
    if (ad < 2.356194f) return mc[delta > 0 ? (run ? MC_RUN90A : MC_TOWALKA) : (run ? MC_RUN90C : MC_TOWALKC)];
    return mc[delta > 0 ? (run ? MC_RUN180A : MC_TO180A) : (run ? MC_RUN180C : MC_TO180C)];
}
static int move_clip_is_front_start(int clip) {
    return clip >= 0 && (clip == g_act->clips[MC_TOWALK] || clip == g_act->clips[MC_TORUN]);
}
static float wrap_angle(float a);
static float wrap_angle(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
/* Yaw of the hips (node 2) in a turn clip at time t, model space. */
static float turn_clip_hip_yaw(int clip, float t) {
    NodeOverride ov[DAVID_MAX_NODES];
    anim_lib_sample_for(clip, t, g_act->model, ov);
    if (!ov[2].has_r) return 0.0f;
    const float *q = ov[2].r; /* x y z w; forward = q * (0,0,-1) * q^-1 */
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float fx = -(2 * (x * z + w * y)), fz = -(1 - 2 * (x * x + y * y));
    return atan2f(fx, fz);
}
/* Where his body actually points right now (mid-turn: start facing +
   how far the clip has turned the hips so far). */
static float david_current_heading(void) {
    if (g_char_turn_clip < 0) return g_char_facing;
    return wrap_angle(g_char_facing + wrap_angle(turn_clip_hip_yaw(g_char_turn_clip, g_char_turn_t) -
                                                  turn_clip_hip_yaw(g_char_turn_clip, 0.0f)));
}
/* Starts `clip` (from move_clip_for) toward `heading`. The front starts
   (towalk/torun) don't turn him, so he faces the new way right away. */
static void david_start_move_clip(int clip, float from, float heading, int from_stand) {
    g_char_turn_from_stand = from_stand;
    g_char_turn_interrupted = 0;
    if (clip < 0) { g_char_turn_clip = -1; g_char_facing = heading; return; }
    g_char_facing = move_clip_is_front_start(clip) ? heading : from;
    g_char_turn_clip = clip;
    g_char_turn_t = 0.0f;
    g_char_turn_to = heading; /* g_char_facing switches to it when the clip ends */
}
/* standing: 1 = new order while stopped, 0 = new order while moving,
   -1 = corner of his path */
#define PATH_CORNER_MIN_TURN 1.047198f /* 60 deg */
static void david_turn_toward(float heading, int run, int standing) {
    if (standing < 0) {
        /* along a wall the nav-cell path zigzags in 45 degree steps: those
           small corners just turn him (a clip already playing carries on
           toward the new heading) instead of firing towalka/run90a...
           which would also drop a run to walk speed every half unit */
        float ref = g_char_turn_clip >= 0 ? g_char_turn_to : g_char_facing;
        if (fabsf(wrap_angle(heading - ref)) < PATH_CORNER_MIN_TURN) {
            if (g_char_turn_clip >= 0) g_char_turn_to = heading;
            else g_char_facing = heading;
            return;
        }
        standing = 0;
    }
    if (g_char_turn_clip >= 0) {
        /* A turn-start is COMMITTED: spamming orders must not restart it
           again and again (he'd slide around replaying its first frames). */
        float dur = anim_lib_duration(g_char_turn_clip);
        float progress = dur > 0 ? g_char_turn_t / dur : 1.0f;
        /* same order (double-click's 2nd click; its path is re-planned from
           where he now stands, so its first leg can point a bit differently) */
        if (fabsf(wrap_angle(heading - g_char_turn_to)) < 0.524f) {
            g_char_turn_to = heading;
            /* walk order turned into a run (or back): swap to the other
               mode's version (towalk -> torun, towalka -> run90a, to180c ->
               run180c...), with the same standing/moving rules the clip
               playing was picked by, CONTINUING at the same point of the
               turn (the pairs turn by the same angle) */
            float from = move_clip_is_front_start(g_char_turn_clip) ? heading : g_char_facing;
            int clip = move_clip_for(from, heading, run, g_char_turn_from_stand);
            if (clip >= 0 && clip != g_char_turn_clip) {
                int interrupted = g_char_turn_interrupted;
                david_start_move_clip(clip, from, heading, g_char_turn_from_stand);
                g_char_turn_t = progress * anim_lib_duration(clip);
                g_char_turn_interrupted = interrupted;
            }
            return;
        }
        /* a new direction: early in the clip (once per turn) it restarts
           the right clip from where his body points NOW; otherwise the
           order just redirects his path and the clip plays out -- when it
           ends, advance_character turns him toward wherever he's heading
           by then (david_turn_after_clip) */
        if (!g_char_turn_interrupted && progress < TURN_INTERRUPT_WINDOW) {
            float cur = david_current_heading();
            david_start_move_clip(move_clip_for(cur, heading, run, standing), cur, heading, standing);
            g_char_turn_interrupted = 1;
        }
        return;
    }
    david_start_move_clip(move_clip_for(g_char_facing, heading, run, standing), g_char_facing, heading, standing);
}
/* A turn-start just ended while he's moving: orders (or path corners)
   that came in while it was committed may point him elsewhere -- turn
   toward the leg he's actually on now (a clip again if it's >= 45 deg). */
static void david_turn_after_clip(void) {
    float dx = g_char_target[0] - g_char_pos[0], dz = g_char_target[2] - g_char_pos[2];
    if (dx * dx + dz * dz < 1e-6f) return;
    david_turn_toward(atan2f(dx, dz), g_char_walk_mode == 2, 0);
}

static void set_character_target(float wx, float wy, float wz, int run) {
    if (g_act->dead || g_act->down) return; /* it can't move */
    if (g_act->play_attack) { g_act->play_clip = -1; g_act->play_attack = 0; g_act->play_turn = 0.0f; g_act->stepping = 0; g_act->trail = 0; g_act->cue.on = 0; } /* a move order ends an attack */
    if (g_act->play_hold) { g_act->play_clip = -1; g_act->play_hold = 0; } /* ...and a looped script animation */
    float dx = wx - g_char_pos[0], dz = wz - g_char_pos[2];
    if (dx * dx + dz * dz < 1e-6f) return; /* already there */
    g_char_target[0] = wx;
    g_char_target[1] = wy;
    g_char_target[2] = wz;
    int standing = !g_char_moving;
    g_char_moving = 1;
    g_char_walk_mode = run ? 2 : 1;
    david_turn_toward(atan2f(dx, dz), run, standing);
}

/* Resolves a screen click to a real target: raycasts the room's actual
   environment mesh (glTF mesh 0 -- real
   walls/floor/props, the same data the user found they could open in
   Blender) and only accepts a hit whose surface faces mostly "up"
   (floor, not a wall or a prop's side). Falls back to the coarse 2D
   hull for the handful of rooms without exported mesh data, rather
   than refusing to move David there at all. Returns 1 if a valid
   target was set.

   If the direct line to the destination isn't walkable (a wall, a
   pit, too steep a step), tries A* over the room's nav grid instead
   of just refusing the move -- real obstacle-avoiding pathfinding
   rather than "walk in a straight line or don't move". Some clicks
   will still be legitimately unreachable (an isolated pocket of the
   room, or off the walkable footprint entirely) and get rejected same
   as before. */
static int g_click_fail = 0; /* why the last click was refused (VERIFY_PATHS diagnostics) */
static int try_click_to_move(float px, float py, int run);
/* Tells the player WHY a click did nothing (status line), instead of a
   silent no-op. */
static void report_click_refusal(void) {
    const char *why = "no path from here";
    if (g_click_fail == 1) why = "nothing walkable under the cursor";
    else if (g_click_fail == 2) why = "David doesn't fit there and nothing reachable nearby";
    else if (g_click_fail == 12) why = "that spot isn't standable";
    else if (g_click_fail == 13) why = "that area isn't connected to where David is";
    snprintf(g_status, sizeof(g_status), "click ignored: %s", why);
}
/* Walks David to a WORLD point with the full navigation logic (straight
   line if the body fits all the way, else A*; unreachable -> nearest
   reachable point). Used by floor clicks and by door clicks / door
   arrivals. Returns 1 if a walk was started (or he's already there). */
static int move_to_world_point(const float target[3], int run) {
    float hit[3] = { target[0], target[1], target[2] };
    g_char_waypoint_count = 0;
    g_char_waypoint_idx = 0;
    if (!g_collision_enabled || g_room_mesh_tri_count <= 0) {
        set_character_target(hit[0], hit[1], hit[2], run);
        set_click_marker(hit);
        return 1;
    }
    float floor_min_y = g_room_floor_y - 12.0f;
    if (!stand_ok(hit[0], hit[1], hit[2], g_room_mesh_tris, g_room_mesh_tri_count)) {
        /* David's body wouldn't fit exactly there (against a wall,
           under something low, feet over an edge): aim for the
           nearest point where it does */
        float near_pt[3];
        if (!navgrid_nearest_reachable(&g_nav_grid, g_char_pos, hit, 6.0f, near_pt)) { g_click_fail = 2; return 0; }
        hit[0] = near_pt[0]; hit[1] = near_pt[1]; hit[2] = near_pt[2];
    }
    if (path_is_walkable(g_char_pos, hit, g_room_mesh_tris, g_room_mesh_tri_count,
                          CHAR_RADIUS, NAV_MAX_STEP, floor_min_y)) {
        set_character_target(hit[0], hit[1], hit[2], run);
        set_click_marker(hit);
        return 1;
    }
    int n = navgrid_find_path(&g_nav_grid, g_room_mesh_tris, g_room_mesh_tri_count,
                               g_char_pos, hit, g_char_waypoints, MAX_CHAR_WAYPOINTS, floor_min_y);
    if (n <= 0) {
        /* somewhere David can't stand or can't reach (top of a prop, a
           floor behind a railing, another level): walk to the closest
           reachable floor point instead, like classic point-and-click
           games */
        float near_pt[3];
        if (navgrid_nearest_reachable(&g_nav_grid, g_char_pos, hit, 8.0f, near_pt)) {
            hit[0] = near_pt[0]; hit[1] = near_pt[1]; hit[2] = near_pt[2];
            n = navgrid_find_path(&g_nav_grid, g_room_mesh_tris, g_room_mesh_tri_count,
                                  g_char_pos, hit, g_char_waypoints, MAX_CHAR_WAYPOINTS, floor_min_y);
        }
    }
    if (n <= 0) { g_click_fail = 10 + g_nav_last_fail; return 0; } /* genuinely unreachable from here -- normal, not a bug */
    g_char_waypoint_count = n;
    /* first waypoint that is actually somewhere else (a leg of ~zero
       length would make set_character_target think he's already there
       and cancel the whole walk) */
    g_char_waypoint_idx = 0;
    while (g_char_waypoint_idx < n - 1) {
        float wx = g_char_waypoints[g_char_waypoint_idx][0] - g_char_pos[0];
        float wz = g_char_waypoints[g_char_waypoint_idx][2] - g_char_pos[2];
        if (wx * wx + wz * wz > 0.01f) break;
        g_char_waypoint_idx++;
    }
    set_character_target(g_char_waypoints[g_char_waypoint_idx][0], g_char_waypoints[g_char_waypoint_idx][1],
                         g_char_waypoints[g_char_waypoint_idx][2], run);
    set_click_marker(g_char_waypoints[n - 1]); /* where he will actually stop */
    return 1;
}

/* Screen click (room image pixel) -> the world floor point it designates
   (floor under the cursor; a wall/prop side -> the reachable floor at its
   foot; a floor modelled upside down is honoured). Returns 0 if nothing
   usable is under the cursor. */
/* Clicking a wall/prop side sends David to the reachable floor nearest
   to the point clicked on it, searched within this many world units
   (editor: "Wall-click reach", per room -- see room_settings_load).
   Too small: clicks on tall walls/props are refused; too big: a click on
   far scenery can send him somewhere unexpected. */
static float g_wall_click_reach = 6.0f;
static float g_room_zoom = 1.0f; /* the room's camera zoom (per room, scene editor) */
static char g_settings_room[128] = ""; /* room whose settings are loaded */
static void room_settings_path(char *out, size_t n, const char *room) {
    root_path(out, n, "data/rooms/%s_nav.cfg", room);
}
/* Every tunable of David's movement, shown in the editor's settings
   panel (N). `nav` = the walkable grid must be rebuilt after a change. */
typedef struct {
    const char *key, *label; float *v; float def, step, min, max; const char *fmt; int nav; const char *desc;
    int global; /* 1 = David, all rooms (data/david.cfg); 0 = per room */
} TunableSetting;
static TunableSetting g_settings[] = {
    { "hitbox_radius", "Hitbox radius", &g_nav.body_radius, 0.3f, 0.05f, 0.1f, 0.8f, "%.2f", 1,
      "Radius of David's body cylinder (units). Smaller = he fits through narrower gaps and gets closer to walls; bigger = he keeps more distance and can clip less." },
    { "max_step", "Max step height", &g_nav.max_step, 0.6f, 0.05f, 0.2f, 1.5f, "%.2f", 1,
      "Highest step David climbs (units). Raise it for stairs with tall or broken steps; too high and he climbs low walls, crates and ledges." },
    { "max_slope", "Max slope (deg)", &g_nav.max_slope_deg, 39.997f, 2.0f, 10.0f, 75.0f, "%.0f", 1,
      "Steepest floor David walks on. Raise it for ramps / rough rock slopes; too high and he walks up embankments and props." },
    { "foot_radius", "Foot support", &g_nav.foot_radius, 0.2f, 0.05f, 0.0f, 0.5f, "%.2f", 1,
      "How far around his feet there must be floor (units). Lower it for gaps/holes in stairs and thin walkways; higher keeps him away from ledges." },
    { "headroom", "Headroom", &g_nav.headroom, 1.7f, 0.1f, 0.8f, 2.5f, "%.1f", 1,
      "Free height needed above a floor (units). Lower it if he refuses low passages; too low and he walks under tables and slabs." },
    { "green_step", "Green zone step", &g_nav.forced_step, 1.5f, 0.1f, 0.5f, 3.0f, "%.1f", 1,
      "Max step inside GREEN zones (units): lets you force a climb in one place without changing the whole game." },
    { "camera_zoom", "Camera zoom", &g_room_zoom, 1.0f, 0.1f, 1.0f, 3.0f, "x%.1f", 0,
      "How close the view is in this room (x1 = the whole picture as usual). Scripts can zoom too (Camera action)." },
    { "wall_click_reach", "Wall-click reach", &g_wall_click_reach, 6.0f, 1.0f, 1.0f, 40.0f, "%.0f", 0,
      "Clicking a wall or prop sends David to the walkable floor closest to that point, searched up to this distance (units). Lower = far clicks refused." },
    { "walk_speed", "Walk speed", &g_david_walk_speed, 6.0f, 0.5f, 1.0f, 20.0f, "%.1f", 0,
      "How fast David walks (units per second), all rooms.", 1 },
    { "run_speed", "Run speed", &g_david_run_speed, 13.0f, 0.5f, 2.0f, 30.0f, "%.1f", 0,
      "How fast David runs (units per second), all rooms.", 1 },
    { "run_turn_speed", "Run turn speed", &g_david_run_turn_pct, 50.0f, 5.0f, 10.0f, 100.0f, "%.0f%%", 0,
      "Speed while David changes direction during a run (run90a/c, run180a/c turn-starts), as % of run speed, all rooms.", 1 },
    { "shadow_size", "Shadow size", &g_david_shadow_radius, 0.7f, 0.05f, 0.1f, 2.0f, "%.2f", 0,
      "Size of the soft shadow under David's feet (half-width in units), all rooms.", 1 },
    { "anim_speed", "Animation speed", &g_david_anim_speed, 1.0f, 0.05f, 0.25f, 3.0f, "x%.2f", 0,
      "Playback speed of ALL David's animations (idle, walk, run, turn-starts), all rooms. Walk/run speeds are separate.", 1 },
};
#define SETTING_COUNT ((int)(sizeof(g_settings) / sizeof(g_settings[0])))
/* PER ROOM: every room starts from the tuned defaults; its
   data/rooms/<level>/<room>_nav.cfg (written by the editor) holds only the values
   changed for that room. Called on every room load, before the room
   builds its walkable grid. */
static void room_settings_load(const char *room_lower) {
    g_free_cam = 0; g_fcam_look = 0;
    snprintf(g_settings_room, sizeof(g_settings_room), "%s", room_lower);
    for (int i = 0; i < SETTING_COUNT; i++) if (!g_settings[i].global) *g_settings[i].v = g_settings[i].def;
    char path[600]; room_settings_path(path, sizeof(path), room_lower);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256], key[64]; float v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%63s %f", key, &v) != 2) continue;
            for (int i = 0; i < SETTING_COUNT; i++)
                if (!g_settings[i].global && strcmp(key, g_settings[i].key) == 0 && v >= g_settings[i].min && v <= g_settings[i].max) *g_settings[i].v = v;
        }
        fclose(f);
    }
    nav_params_changed();
    g_view_zoom = g_room_zoom; /* the room's own zoom */
    g_camslide.active = 0;
}
static const char *david_settings_path(void) { static char p[1024]; root_path(p, sizeof(p), "data/david.cfg"); return p; }
#define DAVID_SETTINGS_PATH (david_settings_path())
static void david_settings_load(void) {
    FILE *f = fopen(DAVID_SETTINGS_PATH, "r");
    if (!f) return;
    char line[256], key[64]; float v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63s %f", key, &v) != 2) continue;
        for (int i = 0; i < SETTING_COUNT; i++)
            if (g_settings[i].global && strcmp(key, g_settings[i].key) == 0 && v >= g_settings[i].min && v <= g_settings[i].max) *g_settings[i].v = v;
    }
    fclose(f);
}
static void david_settings_save(void) {
    FILE *f = fopen(DAVID_SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "# Silver Remaster: David's global settings (all rooms)\n");
    for (int i = 0; i < SETTING_COUNT; i++) if (g_settings[i].global) fprintf(f, "%s %.3f\n", g_settings[i].key, *g_settings[i].v);
    fclose(f);
}
static int setting_is_default(int i) { return fabsf(*g_settings[i].v - g_settings[i].def) <= 1e-4f; }
static int room_settings_changed_count(void) {
    int n = 0;
    for (int i = 0; i < SETTING_COUNT; i++) n += !g_settings[i].global && !setting_is_default(i);
    return n;
}
static void room_settings_save(void) {
    if (!g_settings_room[0]) return;
    char path[600]; room_settings_path(path, sizeof(path), g_settings_room);
    if (room_settings_changed_count() == 0) { remove(path); return; } /* all defaults: no file */
    ensure_parent_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Silver Remaster: David & navigation settings for this room (only values differing from the defaults)\n");
    for (int i = 0; i < SETTING_COUNT; i++) if (!g_settings[i].global && !setting_is_default(i)) fprintf(f, "%s %.3f\n", g_settings[i].key, *g_settings[i].v);
    fclose(f);
}

static int click_to_world(float px, float py, float out[3]) {
    float origin[3], dir[3];
    camera_pixel_to_ray(&g_room_cam, px, py, (int)g_hdr.width, (int)g_hdr.height, origin, dir);
    float hit[3], normal[3];
    if (!raycast_room_mesh_view(origin, dir, g_room_mesh_tris, g_room_mesh_tri_count,
                                1 /* floor_only */, 0.55f /* ~56 degrees max slope */, hit, normal)) {
        /* Clicked a wall/prop side rather than floor: with collision on,
           head for the reachable floor closest to where the click landed
           on it (the foot of the wall). Nothing hit at all (sky) = 0. */
        float any_hit[3], any_n[3], near_pt[3];
        if (g_collision_enabled &&
            raycast_room_mesh_view(origin, dir, g_room_mesh_tris, g_room_mesh_tri_count, 0, 0.0f, any_hit, any_n) &&
            navgrid_nearest_reachable(&g_nav_grid, g_char_pos, any_hit, g_wall_click_reach, near_pt)) {
            hit[0] = near_pt[0]; hit[1] = near_pt[1]; hit[2] = near_pt[2];
        } else {
            g_click_fail = 1;
            return 0;
        }
    }
    if (g_collision_enabled) {
        /* The front-face-preferring ray skips surfaces seen from behind
           (walls/ceilings the pre-rendered camera looks through), but a
           FLOOR modelled upside down is also "seen from behind" from
           above -- the click then fell through to whatever lay below it.
           If the plain nearest surface under the cursor is horizontal and
           is a real nav floor, that's what was clicked. */
        float nh[3], nn[3];
        if (raycast_room_mesh(origin, dir, g_room_mesh_tris, g_room_mesh_tri_count, 0, 0.0f, nh, nn) &&
            nn[1] >= 0.55f && nh[1] > hit[1] + 0.3f &&
            navgrid_node_at(&g_nav_grid, nh[0], nh[1], nh[2]) >= 0) {
            float fy;
            if (floor_below(nh[0], nh[2], nh[1] + 0.05f, 0.1f, g_room_mesh_tris, g_room_mesh_tri_count, &fy) && fabsf(fy - nh[1]) < 0.1f) {
                hit[0] = nh[0]; hit[1] = nh[1]; hit[2] = nh[2];
            }
        }
    }
    out[0] = hit[0]; out[1] = hit[1]; out[2] = hit[2];
    return 1;
}

static int g_door_travel_request = -1; /* set when he reaches it; handled by the game timer */
static int g_door_travel_run = 0;      /* he was running when he reached it: he comes out running */

static int try_click_to_move(float px, float py, int run) {
    g_click_fail = 0;
    g_pending_door = -1; /* any normal click cancels a walk to a door */
    if (g_room_mesh_tri_count > 0) {
        float w[3];
        if (!click_to_world(px, py, w)) return 0;
        return move_to_world_point(w, run);
    }
    /* No mesh data at all for this room (a handful, see
       try_start_3d_character_for_room): flat-plane intersection at the
       room's average floor height, with no hull gating at all when
       collision is explicitly off -- lets David go anywhere for free
       debugging/exploring, exactly what a "disable collisions" toggle
       should do. When it's ON but there's just no mesh, the hull is
       still checked as the best remaining approximation. */
    g_char_waypoint_count = 0;
    g_char_waypoint_idx = 0;
    float world[3];
    if (!camera_pixel_to_floor_point(&g_room_cam, px, py, (int)g_hdr.width, (int)g_hdr.height,
                                      g_room_floor_y, world)) {
        return 0;
    }
    if (g_collision_enabled && !walkable_point_in_hull(world[0], world[2], g_room_hull, g_room_hull_count)) return 0;
    set_click_marker(world);
    set_character_target(world[0], g_room_floor_y, world[2], run);
    return 1;
}

/* Where the walk/run cycle starts once `clip` (the turn-start just
   finished) hands over: after to180c / run180c the walk / run cycle's
   first pass starts from its middle; it then loops as usual. */
static float cycle_start_after(int clip) {
    if (clip < 0) return 0.0f;
    if (clip == g_act->clips[MC_RUN180C]) return anim_lib_duration(g_act->clips[MC_RUN]) * 0.5f;
    if (clip == g_act->clips[MC_TO180C]) return anim_lib_duration(g_act->clips[MC_WALK]) * 0.5f;
    return 0.0f;
}

/* The equipment change of the clip playing (sheathing, taking a shield):
   a weapon taken out or changed rings (swrdeqp1 / swrdeqp2). */
static void orb_equip_fx(Actor *a);
static void actor_apply_event(Actor *a) {
    if (a->play_event == EV_WEAPON) {
        snprintf(a->weapon, sizeof(a->weapon), "%s", a->event_arg);
        if (weapon_is_orb(a->weapon)) { audio_play("orbup.ogg", AUDIO_SOUND, 0, 0); orb_equip_fx(a); }
        else if (a->weapon[0]) audio_play(rand() % 2 ? "swrdeqp1.ogg" : "swrdeqp2.ogg", AUDIO_SOUND, 0, 0);
    } else snprintf(a->shield, sizeof(a->shield), "%s", a->event_arg);
    a->play_event = EV_NONE; a->clips_gen = 0;
}

/* The clip playing over the pose is over: its turn (a blow that turned
   the body), its step, its event if not done yet, then what comes next. */
static void actor_play_end(Actor *a) {
    a->facing = wrap_angle(a->facing + a->play_turn);
    a->play_turn = 0.0f;
    if (a->stepping) { memcpy(a->pos, a->step_to, sizeof(float) * 2); a->pos[2] = a->step_to[2]; a->stepping = 0; }
    if (a->play_event) actor_apply_event(a);
    a->trail = 0;
    a->play_clip = -1; a->play_hold = 0; a->play_script = 0; a->play_ended = 0; a->play_freeze = 0;
    if (a->play_next >= 0) { /* chained (shield: raised, then held) */
        a->play_clip = a->play_next; a->play_t = 0.0f; a->play_left = a->play_next_loop ? -1 : 1; a->play_speed = 0.0f;
        a->play_next = -1;
        return;
    }
    a->blow = 0; a->dodging = 0;
    if (a->play_attack) {
        a->play_attack = 0;
        if (a == DAVID_ACTOR && g_atk_queued >= 0) { int q = g_atk_queued; g_atk_queued = -1; david_attack(q); }
    }
}

static int advance_character(float dt) {
    if (g_act->play_clip >= 0) { /* a clip over its own pose (attacks, script animations...): its times, then back */
        Actor *a = g_act;
        float d = anim_lib_duration(a->play_clip);
        if (!a->play_ended) a->play_t += dt * (a->play_speed > 0.0f ? a->play_speed : g_david_anim_speed);
        if (a->stepping && d > 0.0f) { /* the step taken with it, eased */
            float k = fminf(1.0f, a->play_t / d);
            k = k * k * (3.0f - 2.0f * k);
            a->pos[0] = a->step_from[0] + (a->step_to[0] - a->step_from[0]) * k;
            a->pos[2] = a->step_from[2] + (a->step_to[2] - a->step_from[2]) * k;
            float gy;
            if (g_room_mesh_tri_count > 0 && floor_below(a->pos[0], a->pos[2], a->pos[1], NAV_MAX_STEP, g_room_mesh_tris, g_room_mesh_tri_count, &gy)) a->pos[1] = gy;
        }
        if (a->play_event && a->play_t >= a->play_event_at) actor_apply_event(a); /* e.g. the weapon changes in the middle of the sheathing */
        if (a->play_ended) { if (a->play_t > d - 0.001f) a->play_t = d > 0.001f ? d - 0.001f : 0.0f; } /* a script clip over: held where it is, until the script goes on */
        else if (d <= 0.0f) { if (a->play_script || a->hold_last) { a->play_ended = 1; a->play_t = 0.0f; } else actor_play_end(a); }
        else if (a->play_t >= d) {
            if (a->play_left > 0 && --a->play_left == 0) {
                if (a->play_script || a->hold_last) { a->play_ended = 1; a->play_t = d - 0.001f; } else actor_play_end(a);
            }
            else a->play_t = fmodf(a->play_t, d);
        }
    }
    if (!g_char_moving) {
        g_char_anim_t += dt * g_david_anim_speed;
        if (g_char_turn_clip >= 0) { /* arrived mid-turn: the turn finishes on the spot */
            g_char_turn_t += dt * g_david_anim_speed;
            if (g_char_turn_t >= anim_lib_duration(g_char_turn_clip)) { g_char_turn_clip = -1; g_char_facing = g_char_turn_to; g_char_anim_t = 0.0f; }
        }
        return 1;
    }
    /* turn-start playing: he already sets off, at WALK speed, while the
       clip turns his body; then the walk/run clip takes over */
    int turning = g_char_turn_clip >= 0;
    if (turning) {
        g_char_turn_t += dt * g_david_anim_speed;
        if (g_char_turn_t >= anim_lib_duration(g_char_turn_clip)) {
            g_char_anim_t = cycle_start_after(g_char_turn_clip); /* the cycle starts from the pose the turn ends in */
            g_char_turn_clip = -1;
            g_char_facing = g_char_turn_to;
            david_turn_after_clip();
            turning = g_char_turn_clip >= 0;
        }
    }
    /* a start from standing still sets off at walk speed; a turn while
       already running (a new order, a path corner around a prop) slows
       to the "Run turn speed" share of his run speed; walking keeps its pace */
    float speed = (g_char_walk_mode == 2) ? CHAR_RUN_SPEED : CHAR_WALK_SPEED;
    if (turning) {
        if (g_char_turn_from_stand) speed = CHAR_WALK_SPEED;
        else if (g_char_walk_mode == 2) speed = CHAR_RUN_SPEED * g_david_run_turn_pct * 0.01f;
    }
    float step = speed * dt;
    /* Along walls the A* path is a chain of short legs (one per nav cell,
       string-pulling can't skip them): the distance left after reaching a
       waypoint carries on into the next leg, so he keeps his full speed
       instead of losing the rest of the frame at every waypoint. */
    for (;;) {
    float dx = g_char_target[0] - g_char_pos[0];
    float dz = g_char_target[2] - g_char_pos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist <= step || dist < 1e-4f) {
        g_char_pos[0] = g_char_target[0];
        g_char_pos[1] = g_char_target[1];
        g_char_pos[2] = g_char_target[2];
        if (g_char_waypoint_idx < g_char_waypoint_count - 1) {
            /* more of the A*-routed path left -- head for the next leg
               instead of stopping, same run/walk mode as the whole move. */
            step -= dist;
            g_char_waypoint_idx++;
            float *wp = g_char_waypoints[g_char_waypoint_idx];
            int run = (g_char_walk_mode == 2);
            g_char_target[0] = wp[0]; g_char_target[1] = wp[1]; g_char_target[2] = wp[2];
            david_turn_toward(atan2f(wp[0] - g_char_pos[0], wp[2] - g_char_pos[2]), run, -1); /* path corner */
            g_char_walk_mode = run ? 2 : 1;
            if (step > 1e-5f) continue;
            if (!turning) g_char_anim_t += dt * g_david_anim_speed;
            return 1;
        }
        int was_running = g_char_walk_mode == 2;
        g_char_moving = 0;
        g_char_walk_mode = 0;
        g_char_anim_t = 0.0f;
        g_char_waypoint_count = 0;
        if (g_act == DAVID_ACTOR) g_click_marker_active = 0; /* reached it -- marker disappears (item 16) */
        if (g_pending_door >= 0) {
            if (g_act == DAVID_ACTOR) { g_door_travel_request = g_pending_door; g_door_travel_run = was_running; } /* he goes to the other room */
            else g_act->used = 0;                                            /* another character: leaves the room */
            g_pending_door = -1;
        }
        break;
    }
    {
        float frac = step / dist;
        g_char_pos[0] += dx * frac;
        g_char_pos[2] += dz * frac;
        float gy;
        float snap_step = (g_nav_zone_at && g_nav_zone_at(g_char_pos[0], g_char_pos[1], g_char_pos[2]) == NAV_ZONE_FORCE)
                          ? NAV_FORCED_STEP : NAV_MAX_STEP;
        if (g_collision_enabled && g_room_mesh_tri_count > 0 &&
            floor_below(g_char_pos[0], g_char_pos[2], g_char_pos[1], snap_step,
                        g_room_mesh_tris, g_room_mesh_tri_count, &gy) &&
            fabsf(gy - g_char_pos[1]) <= snap_step) {
            /* feet on the real floor under him every frame (stairs,
               ramps) instead of a straight 3D line between waypoints
               that can cut through geometry or float above it */
            g_char_pos[1] = gy;
        } else {
            g_char_pos[1] += (g_char_target[1] - g_char_pos[1]) * frac;
        }
        if (!turning) g_char_anim_t += dt * g_david_anim_speed;
    }
    break;
    }
    return 1;
}

/* Fills a triangle directly into g_render_pixels, sampling the
   character's texture (affine per-triangle UV interpolation, matching the
   Python prototype's draw_textured_triangle) instead of a flat color. */
static void fill_triangle_textured(const CharModel *m, float x0, float y0, float x1, float y1, float x2, float y2,
                                    float z0, float z1, float z2,
                                    float u0, float v0, float u1, float v1, float u2, float v2,
                                    float *depth_buf) {
    int minx = (int)floorf(fminf(x0, fminf(x1, x2)));
    int maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2)));
    int maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= (int)g_hdr.width) maxx = (int)g_hdr.width - 1;
    if (maxy >= (int)g_hdr.height) maxy = (int)g_hdr.height - 1;
    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(denom) < 1e-6f) return;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            float w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / denom;
            float w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / denom;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f) continue;
            size_t idx = (size_t)py * g_dib_stride + px;
            /* perspective-correct: depth is linear in 1/z across the screen,
               not in z (linear-z made large near triangles report depths
               off by 10-60%, i.e. wrong surfaces winning the z-test --
               measured on gno/eastower against an exact reference) */
            float z = 1.0f / (w0 / z0 + w1 / z1 + w2 / z2);
            if (depth_buf && z >= depth_buf[idx]) continue; /* occluded by room geometry or a nearer character triangle */
            float u = (w0 * u0 / z0 + w1 * u1 / z1 + w2 * u2 / z2) * z;
            float v = (w0 * v0 / z0 + w1 * v1 / z1 + w2 * v2 / z2) * z;
            int iu = (int)u; if (iu < 0) iu = 0; if (iu >= m->tex_w) iu = m->tex_w - 1;
            int iv = (int)v; if (iv < 0) iv = 0; if (iv >= m->tex_h) iv = m->tex_h - 1;
            const uint8_t *tc = m->tex_rgb + ((size_t)iv * m->tex_w + iu) * 3;
            g_render_pixels[idx] = ((uint32_t)tc[0] << 16) | ((uint32_t)tc[1] << 8) | tc[2];
            if (depth_buf) depth_buf[idx] = z;
        }
    }
}

/* The pose of a character right now: the turn-start playing, else its
   walk / run / stand cycle (rest pose for a slot set to "no animation"). */
static void actor_pose(Actor *a, NodeOverride *overrides) {
    actor_resolve_clips(a);
    int clip = a->turn_clip;
    float t = a->turn_t;
    if (a->play_clip >= 0) { clip = a->play_clip; t = a->play_t; if (a->play_rev) { float d = anim_lib_duration(clip); t = d > 0.001f ? d - 0.001f - fminf(t, d - 0.001f) : 0.0f; } } /* a script animation wins */
    if (clip < 0) { clip = a->clips[a->walk_mode == 2 ? MC_RUN : a->walk_mode == 1 ? MC_WALK : MC_STAND]; t = a->anim_t; }
    if (clip >= 0 && anim_lib_fits(clip, a->model->node_count)) anim_lib_sample_for(clip, t, a->model, overrides);
    else memset(overrides, 0, sizeof(NodeOverride) * DAVID_MAX_NODES);
}

/* A character's animated vertices projected into ROOM-image pixels (+ depth). */
static void actor_project(Actor *a, float *px_buf, float *py_buf, float *z_buf, int *visible) {
    const CharModel *m = a->model;
    NodeOverride overrides[DAVID_MAX_NODES];
    actor_pose(a, overrides);
    static Mat4 skin_mats[DAVID_MAX_JOINTS];
    skeleton_skin_matrices_for(m, overrides, skin_mats);
    a->hand_ok = (a->item || weapon_is_orb(a->weapon) || (a->potion_m && a->pot_right)) && skeleton_node_global(grip_node(m, 0), &a->hand);
    a->hand_l_ok = (a->item_l || (a->potion_m && !a->pot_right)) && skeleton_node_global(grip_node(m, 1), &a->hand_l);
    a->hand_s_ok = a->shield_m && skeleton_node_global(shield_node(m), &a->hand_s);

    /* facing = direction it's heading (atan2(dx, dz)); the models face -Z
       (measured on David: in the walk cycles the grounded foot slides
       toward +Z and the head leans toward -Z), hence the half turn */
    float cf = cosf(a->facing + 3.14159265f), sf = sinf(a->facing + 3.14159265f);
    for (int vi = 0; vi < m->vertex_count; vi++) {
        float acc[3] = {0, 0, 0};
        for (int k = 0; k < 4; k++) {
            float w = m->weights[vi][k];
            if (w <= 0) continue;
            int j = m->joints_idx[vi][k];
            float sp[3];
            mat4_vec3(&skin_mats[j], m->positions[vi], sp);
            acc[0] += sp[0] * w; acc[1] += sp[1] * w; acc[2] += sp[2] * w;
        }
        float world[3] = { acc[0] * cf + acc[2] * sf + a->pos[0], acc[1] + a->pos[1], -acc[0] * sf + acc[2] * cf + a->pos[2] };
        visible[vi] = char_project(a->pos, world, &px_buf[vi], &py_buf[vi], &z_buf[vi]);
    }
}

/* The item in a character's right (left) hand, projected like its body
   (after actor_project, which found the hands). 0 if it holds nothing. */
static int actor_project_item(Actor *a, int left, float *px_buf, float *py_buf, float *z_buf, int *visible) {
    /* left: 0 right hand, 1 left hand, 2 the shield, 3 a potion (in the hand it's in) */
    const CharModel *it = left == 3 ? a->potion_m : left == 2 ? a->shield_m : left ? a->item_l : a->item;
    int hand_r = left == 0 || (left == 3 && a->pot_right);
    if (left == 0 && a->potion_m && a->pot_right) return 0; /* throwing: the potion instead of the weapon */
    if (!it || !(left == 2 ? a->hand_s_ok : hand_r ? a->hand_ok : a->hand_l_ok)) return 0;
    const Mat4 *hand = left == 2 ? &a->hand_s : hand_r ? &a->hand : &a->hand_l;
    float cf = cosf(a->facing + 3.14159265f), sf = sinf(a->facing + 3.14159265f);
    for (int vi = 0; vi < it->vertex_count; vi++) {
        float h[3];
        mat4_vec3(hand, it->positions[vi], h);
        float world[3] = { h[0] * cf + h[2] * sf + a->pos[0], h[1] + a->pos[1], -h[0] * sf + h[2] * cf + a->pos[2] };
        visible[vi] = char_project(a->pos, world, &px_buf[vi], &py_buf[vi], &z_buf[vi]);
    }
    return 1;
}

/* F3 debug view only: the characters rasterized into the 8-bit room frame. */
static void render_3d_character(void) {
    if (!g_has_3d_character) return;
    static float px_buf[DAVID_MAX_VERTS], py_buf[DAVID_MAX_VERTS], z_buf[DAVID_MAX_VERTS];
    static int visible[DAVID_MAX_VERTS];
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *a = &g_actors[k];
        if (!a->used) continue;
        const CharModel *m = a->model;
        actor_project(a, px_buf, py_buf, z_buf, visible);
        /* the Z-buffer test in fill_triangle_textured handles self-occlusion,
           the other characters AND the room geometry: draw order doesn't matter */
        for (int pass = 0; pass < 4; pass++) {
        if (pass > 0) { if (!actor_project_item(a, pass - 1, px_buf, py_buf, z_buf, visible)) continue; m = pass == 3 ? a->shield_m : pass == 2 ? a->item_l : a->item; } /* then what it holds */
        for (int t = 0; t < m->index_count / 3; t++) {
            int i0 = m->indices[t * 3], i1 = m->indices[t * 3 + 1], i2 = m->indices[t * 3 + 2];
            if (!visible[i0] || !visible[i1] || !visible[i2]) continue;
            fill_triangle_textured(m, px_buf[i0], py_buf[i0], px_buf[i1], py_buf[i1], px_buf[i2], py_buf[i2],
                                    z_buf[i0], z_buf[i1], z_buf[i2],
                                    m->uvs_px[i0][0], m->uvs_px[i0][1],
                                    m->uvs_px[i1][0], m->uvs_px[i1][1],
                                    m->uvs_px[i2][0], m->uvs_px[i2][1],
                                    g_depth_buffer_char);
        }
        }
    }
}


/* Two-pass "prefer front faces" depth rule (same rule as
   raycast_room_mesh_view, applied per pixel): pass 0 rasterizes only
   triangles facing the camera; pass 1 rasterizes back faces, but only
   into pixels no front face covered. Back-pass depths are stored NEGATED
   while the passes run so the two kinds can be told apart without an
   extra buffer; depth_resolve_two_pass flips them back afterwards.
   Returns 1 if the fragment was written. */
static int depth_test_two_pass(float *d, float z, int back_pass) {
    if (!back_pass) {
        if (z >= *d) return 0;
        *d = z;
        return 1;
    }
    if (*d >= 0.0f && *d < 1e29f) return 0; /* a front face owns this pixel */
    if (*d < 0.0f && z >= -*d) return 0;    /* nearer back face already there */
    *d = -z;
    return 1;
}

static void depth_resolve_two_pass(float *depth_buf) {
    size_t npix = (size_t)g_dib_stride * g_hdr.height;
    for (size_t i = 0; i < npix; i++) if (depth_buf[i] < 0.0f) depth_buf[i] = -depth_buf[i];
}

/* 1 if triangle (v0,v1,v2) faces the camera at cam_pos (glTF CCW). */
static int tri_faces_point(const float *v0, const float *v1, const float *v2, const float cam_pos[3]) {
    float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
    float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
    float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
    return n[0]*(v0[0]-cam_pos[0]) + n[1]*(v0[1]-cam_pos[1]) + n[2]*(v0[2]-cam_pos[2]) < 0.0f;
}

/* Flat-shaded, depth-tested triangle fill for the raw 3D debug view's
   room geometry (no texture -- mesh0 doesn't carry material data,
   just positions, so a solid per-triangle shade is the honest option
   rather than inventing texture coordinates). */
static void fill_triangle_flat_depth(float x0, float y0, float x1, float y1, float x2, float y2,
                                      float z0, float z1, float z2, uint32_t color, int back_pass) {
    int minx = (int)floorf(fminf(x0, fminf(x1, x2)));
    int maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2)));
    int maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= (int)g_hdr.width) maxx = (int)g_hdr.width - 1;
    if (maxy >= (int)g_hdr.height) maxy = (int)g_hdr.height - 1;
    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(denom) < 1e-6f) return;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            float w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / denom;
            float w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / denom;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f) continue;
            size_t idx = (size_t)py * g_dib_stride + px;
            /* perspective-correct: depth is linear in 1/z across the screen,
               not in z (linear-z made large near triangles report depths
               off by 10-60%, i.e. wrong surfaces winning the z-test --
               measured on gno/eastower against an exact reference) */
            float z = 1.0f / (w0 / z0 + w1 / z1 + w2 / z2);
            if (!depth_test_two_pass(&g_depth_buffer[idx], z, back_pass)) continue;
            g_render_pixels[idx] = color;
        }
    }
}

/* Optional raw 3D debug view (F3): renders the room's ACTUAL mesh0
   environment geometry (walls/floor/props -- the same data the user
   found they could open directly in Blender) flat-shaded, instead of
   the real pre-rendered background photo. Useful for sanity-checking
   the mesh/camera alignment directly; normal play always uses
   render_room_mesh_depth_only + the real photo instead (see
   composite_frame/WM_PAINT) since there's no real texture/UV data to
   shade the mesh with -- the photo already IS the room's real look. */
static void render_room_mesh_3d(void) {
    if (!g_loaded) return;
    memset(g_render_pixels, 0, (size_t)g_dib_stride * g_hdr.height * sizeof(uint32_t)); /* black */
    size_t npix = (size_t)g_dib_stride * g_hdr.height;
    for (size_t i = 0; i < npix; i++) g_depth_buffer[i] = 1e29f;

    if (g_room_mesh_tri_count <= 0) return; /* no mesh data for this room -- just the clear color */

    static const float light_dir[3] = { 0.35f, 0.82f, 0.45f }; /* pre-normalized-ish, good enough */
    const RoomCamera *vc = view_cam(); /* the room's camera, or the free one */
    uint8_t base_r = 150, base_g = 145, base_b = 135; /* neutral stone-gray -- mesh0 carries no material colors */

    for (int pass = 0; pass < 2; pass++)
    for (int t = 0; t < g_room_mesh_tri_count; t++) {
        const float *v0 = g_room_mesh_tris + (size_t)t * 9;
        const float *v1 = v0 + 3;
        const float *v2 = v0 + 6;
        if (tri_faces_point(v0, v1, v2, vc->translation) == pass) continue; /* pass 0: fronts, pass 1: backs */
        float px0, py0, pz0, px1, py1, pz1, px2, py2, pz2;
        int vis0 = camera_world_to_pixel_z(vc, v0, (int)g_hdr.width, (int)g_hdr.height, &px0, &py0, &pz0);
        int vis1 = camera_world_to_pixel_z(vc, v1, (int)g_hdr.width, (int)g_hdr.height, &px1, &py1, &pz1);
        int vis2 = camera_world_to_pixel_z(vc, v2, (int)g_hdr.width, (int)g_hdr.height, &px2, &py2, &pz2);
        if (!vis0 || !vis1 || !vis2) continue;

        float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
        float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        float nlen = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nlen < 1e-8f) continue;
        n[0]/=nlen; n[1]/=nlen; n[2]/=nlen;
        float ndotl = n[0]*light_dir[0] + n[1]*light_dir[1] + n[2]*light_dir[2];
        if (ndotl < 0) ndotl = -ndotl; /* light both faces -- mesh0's winding/normal direction isn't guaranteed consistent */
        float shade = 0.35f + 0.65f * ndotl; /* ambient floor so nothing goes pure black */
        if (shade > 1.0f) shade = 1.0f;
        uint32_t color = ((uint32_t)(base_r * shade) << 16) | ((uint32_t)(base_g * shade) << 8) | (uint32_t)(base_b * shade);
        if (g_free_cam && fabsf(n[1]) >= 0.55f) /* free camera: the floor David can stand on, green */
            color = ((uint32_t)(70 * shade) << 16) | ((uint32_t)(175 * shade) << 8) | (uint32_t)(80 * shade);

        fill_triangle_flat_depth(px0, py0, px1, py1, px2, py2, pz0, pz1, pz2, color, pass);
    }
    depth_resolve_two_pass(g_depth_buffer);
}

/* Depth-ONLY rasterization of the room's real mesh, no color writes --
   used every frame during normal play so David's skinned model
   correctly occludes behind pillars/props/walls even though the
   VISIBLE background stays the real pre-rendered photo (composite_frame,
   called right before this). Since the photo and this mesh share the
   exact same per-room camera, a mesh depth value at a given pixel
   corresponds to the real surface visible at that same pixel in the
   photo -- so this is a correct z-buffer for the photo itself, not an
   approximation. */
static void fill_triangle_depth_only(float x0, float y0, float x1, float y1, float x2, float y2,
                                      float z0, float z1, float z2, float *depth_buf, int back_pass) {
    int minx = (int)floorf(fminf(x0, fminf(x1, x2)));
    int maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2)));
    int maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= (int)g_hdr.width) maxx = (int)g_hdr.width - 1;
    if (maxy >= (int)g_hdr.height) maxy = (int)g_hdr.height - 1;
    float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(denom) < 1e-6f) return;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float fx = (float)px + 0.5f, fy = (float)py + 0.5f;
            float w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / denom;
            float w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / denom;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f) continue;
            size_t idx = (size_t)py * g_dib_stride + px;
            /* perspective-correct: depth is linear in 1/z across the screen,
               not in z (linear-z made large near triangles report depths
               off by 10-60%, i.e. wrong surfaces winning the z-test --
               measured on gno/eastower against an exact reference) */
            float z = 1.0f / (w0 / z0 + w1 / z1 + w2 / z2);
            depth_test_two_pass(&depth_buf[idx], z, back_pass);
        }
    }
}

/* Rasterizes the room mesh's depth ONLY, through `cam`, into `depth_buf`
   (both explicit, not always the "default" g_room_cam/g_depth_buffer --
   see g_depth_buffer_char: whenever a room's camera has real roll,
   g_char_cam projects world points to different pixels than g_room_cam,
   so David's occlusion test needs depth rasterized through THAT SAME
   camera, not the room's own). */
static void render_room_mesh_depth_only_cam(const RoomCamera *cam, float *depth_buf) {
    if (g_room_mesh_tri_count <= 0 || !depth_buf) return;
    int w = (int)g_hdr.width, h = (int)g_hdr.height;
    for (int pass = 0; pass < 2; pass++)
    for (int t = 0; t < g_room_mesh_tri_count; t++) {
        const float *v0 = g_room_mesh_tris + (size_t)t * 9;
        const float *v1 = v0 + 3;
        const float *v2 = v0 + 6;
        if (tri_faces_point(v0, v1, v2, cam->translation) == pass) continue; /* pass 0: fronts, pass 1: backs */
        float px0, py0, pz0, px1, py1, pz1, px2, py2, pz2;
        int vis0 = camera_world_to_pixel_z(cam, v0, w, h, &px0, &py0, &pz0);
        int vis1 = camera_world_to_pixel_z(cam, v1, w, h, &px1, &py1, &pz1);
        int vis2 = camera_world_to_pixel_z(cam, v2, w, h, &px2, &py2, &pz2);
        if (!vis0 || !vis1 || !vis2) continue;
        fill_triangle_depth_only(px0, py0, px1, py1, px2, py2, pz0, pz1, pz2, depth_buf, pass);
    }
    depth_resolve_two_pass(depth_buf);
}


/* The room's depth through its camera never changes while the room is
   shown: rasterized ONCE (it used to be redone twice per frame, the
   biggest cost of a frame), rebuilt only when the mesh or camera change. */
static float *g_room_depth_cache = NULL;
static size_t g_room_depth_cache_n = 0;
static const float *g_rdc_mesh = NULL;
static int g_rdc_tris = -1;
static RoomCamera g_rdc_cam;
static const float *room_depth_cached(void) {
    size_t npix = (size_t)g_dib_stride * g_hdr.height;
    if (g_room_depth_cache && g_room_depth_cache_n == npix && g_rdc_mesh == g_room_mesh_tris &&
        g_rdc_tris == g_room_mesh_tri_count && memcmp(&g_rdc_cam, &g_room_cam, sizeof(RoomCamera)) == 0)
        return g_room_depth_cache;
    if (g_room_depth_cache_n != npix) {
        free(g_room_depth_cache);
        g_room_depth_cache = (float *)malloc(npix * sizeof(float));
        g_room_depth_cache_n = g_room_depth_cache ? npix : 0;
        if (!g_room_depth_cache) return NULL;
    }
    for (size_t i = 0; i < npix; i++) g_room_depth_cache[i] = 1e29f;
    render_room_mesh_depth_only_cam(&g_room_cam, g_room_depth_cache);
    g_rdc_mesh = g_room_mesh_tris; g_rdc_tris = g_room_mesh_tri_count; g_rdc_cam = g_room_cam;
    return g_room_depth_cache;
}

/* David's floor shadow: a soft dark ellipse (64x32, smoothstep falloff,
   max alpha 150, generated once -- shadow_alpha), projected onto
   the real floor point directly beneath him, alpha-blended into the
   already-composited frame and depth-tested against the room mesh so
   it doesn't bleed onto a closer pillar/prop between it and the
   camera. Sized in WORLD space (not a fudged screen-space constant) by
   projecting two points radius_world apart through the same per-room
   camera everything else uses, so it scales correctly with distance
   like any other piece of real geometry would. Shrinks and fades with
   his height above that floor point -- currently always ~0 since
   click-to-move always lands him exactly on the floor, but this is
   exactly the hook a future jump/fall-into-a-pit mechanic needs, and
   costs nothing to have ready now. */
#define SHADOW_TEX_W 64
#define SHADOW_TEX_H 32
static uint8_t shadow_tex_alpha[SHADOW_TEX_H][SHADOW_TEX_W];
static void shadow_alpha_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    float cx = (SHADOW_TEX_W - 1) / 2.0f, cy = (SHADOW_TEX_H - 1) / 2.0f;
    for (int y = 0; y < SHADOW_TEX_H; y++) for (int x = 0; x < SHADOW_TEX_W; x++) {
        float nx = (x - cx) / (SHADOW_TEX_W / 2.0f), ny = (y - cy) / (SHADOW_TEX_H / 2.0f);
        float d = sqrtf(nx * nx + ny * ny), t = 1.0f - d;
        shadow_tex_alpha[y][x] = d >= 1.0f ? 0 : (uint8_t)(150.0f * (t * t * (3 - 2 * t)));
    }
}
/* a model's rest footprint: its height and its widest reach from its axis (cached) */
static void model_extent(const CharModel *m, float *height, float *reach) {
    static const CharModel *ms[64]; static float hs[64], rs[64]; static int n = 0;
    for (int i = 0; i < n; i++) if (ms[i] == m) { *height = hs[i]; *reach = rs[i]; return; }
    float h = 0.0f, r2 = 0.0f;
    for (int v = 0; v < m->vertex_count; v++) {
        const float *p = m->positions[v];
        if (p[1] > h) h = p[1];
        float d = p[0] * p[0] + p[2] * p[2];
        if (d > r2) r2 = d;
    }
    *height = h > 0.0f ? h : 1.8f; *reach = r2 > 0.0f ? sqrtf(r2) : 0.4f;
    if (n < 64) { ms[n] = m; hs[n] = *height; rs[n] = *reach; n++; }
}
static void draw_character_shadow(void) {
    shadow_alpha_init();
    if (!g_has_3d_character || g_room_mesh_tri_count <= 0 || !g_loaded || !g_act->model) return;
    /* its size: David's (the setting) scaled by how wide this one is next to him */
    float mh, reach, dh, dreach, radius = g_david_shadow_radius;
    model_extent(g_act->model, &mh, &reach);
    if (g_act != DAVID_ACTOR && DAVID_ACTOR->model) {
        model_extent(DAVID_ACTOR->model, &dh, &dreach);
        float k = dreach > 1e-3f ? reach / dreach : 1.0f;
        if (k < 0.4f) k = 0.4f;
        if (k > 4.0f) k = 4.0f;
        radius *= k;
    }
    /* the floor under it, looked for from its own height (not from far above: a floor overhead would catch it) */
    float up = mh * 0.6f;
    if (up < 1.0f) up = 1.0f;
    if (up > 6.0f) up = 6.0f;
    float origin[3] = { g_char_pos[0], g_char_pos[1] + up, g_char_pos[2] };
    float down[3] = { 0.0f, -1.0f, 0.0f };
    float hit[3], normal[3];
    if (!raycast_room_mesh(origin, down, g_room_mesh_tris, g_room_mesh_tri_count, 1, 0.55f, hit, normal)) return;
    if (hit[1] < g_room_floor_y - 12.0f) return; /* a deep false-positive well below the real floor, not David's actual shadow spot */
    float drop = g_char_pos[1] - hit[1];
    if (drop < 0.0f) drop = 0.0f;

    /* Projected via the room's real camera: the shadow lies on the real
       floor, and David's anchor (feet) is projected through this same
       camera by char_project, so the shadow stays under his feet. */
    int w_img = (int)g_hdr.width, h_img = (int)g_hdr.height;
    float px, py, depth;
    if (!camera_world_to_pixel_z(&g_room_cam, hit, w_img, h_img, &px, &py, &depth)) return;
    float edge_world[3] = { hit[0] + radius, hit[1], hit[2] };
    float epx, epy, edepth;
    if (!camera_world_to_pixel_z(&g_room_cam, edge_world, w_img, h_img, &epx, &epy, &edepth)) return;
    float screen_radius = fabsf(epx - px);
    if (screen_radius < 1.0f) return;

    float shrink = 1.0f / (1.0f + drop * 0.5f);
    float darkness = 1.0f / (1.0f + drop * 0.8f); /* fades out faster than it shrinks */
    float w = screen_radius * 2.0f * shrink;
    float h = w * ((float)SHADOW_TEX_H / (float)SHADOW_TEX_W);
    if (w < 2.0f || h < 1.0f) return;

    int x0 = (int)(px - w / 2.0f), x1 = (int)(px + w / 2.0f);
    int y0 = (int)(py - h / 2.0f), y1 = (int)(py + h / 2.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w_img) x1 = w_img - 1;
    if (y1 >= h_img) y1 = h_img - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            size_t idx = (size_t)y * g_dib_stride + x;
            if (g_depth_buffer_char && depth > g_depth_buffer_char[idx] + 0.05f) continue; /* something closer occludes the floor here */
            float u = (x + 0.5f - (px - w / 2.0f)) / w;
            float v = (y + 0.5f - (py - h / 2.0f)) / h;
            int tu = (int)(u * SHADOW_TEX_W), tv = (int)(v * SHADOW_TEX_H);
            if (tu < 0 || tu >= SHADOW_TEX_W || tv < 0 || tv >= SHADOW_TEX_H) continue;
            int a = shadow_tex_alpha[tv][tu];
            if (a <= 0) continue;
            float blend = (a / 255.0f) * darkness;
            if (blend <= 0.0f) continue;
            uint32_t px0 = g_render_pixels[idx];
            uint32_t nr = (uint32_t)(((px0 >> 16) & 255) * (1.0f - blend));
            uint32_t ng = (uint32_t)(((px0 >> 8) & 255) * (1.0f - blend));
            uint32_t nb = (uint32_t)((px0 & 255) * (1.0f - blend));
            g_render_pixels[idx] = (nr << 16) | (ng << 8) | nb;
        }
    }
}

static void composite_frame(void) {
    if (!g_loaded) return;
    memcpy(g_render_pixels, g_dib_pixels, (size_t)g_dib_stride * g_hdr.height * sizeof(uint32_t));
}

/* Milestone 2: camera panning for rooms wider/taller than the fixed
   viewport -- the first bit of interactivity, and the first thing that
   actually uses the room dimensions for something beyond a one-shot
   static blit. Viewport is a fixed 640x480 (the game's normal room
   frame size; SUBARCH-style wide rooms are the ones meant to scroll). */
/* VIEWPORT_W/H are now a MAX CAP (typical desktop screen headroom), not
   a fixed size -- per explicit user request the window adapts to each
   room's own actual background shape (rooms vary a lot: 640x480,
   960x480 wide panoramas, 1000x480, etc, see resize_window_to_room)
   instead of always cropping to 640x480 and panning. Only rooms bigger
   than the cap still pan. */
#define VIEWPORT_W 1280
#define VIEWPORT_H 800
#define TOOL_SIDEBAR_W 250 /* windowed: tool panel left of the picture */
#define TOOL_MIN_BAR 230   /* fullscreen: minimum width of each black side bar (tool panels) */
#define PAN_STEP 24
static int g_cam_x = 0, g_cam_y = 0;
static int g_mouse_client_x = -1, g_mouse_client_y = -1; /* raw client coords, for fullscreen edge-scroll */
static HWND g_hwnd = NULL;
static int g_is_fullscreen = 0;
static RECT g_windowed_rect;       /* saved window rect to restore on exiting fullscreen */
static LONG_PTR g_windowed_style;  /* saved window style (WS_OVERLAPPEDWINDOW etc.) */

/* True exclusive-feeling fullscreen (F11): a borderless popup window
   sized to cover the whole monitor, the standard approach for a
   regular GDI app (no separate exclusive-fullscreen API needed here).
   Saves the current windowed rect/style to restore on toggling back. */
static void toggle_fullscreen(HWND hwnd) {
    if (!g_is_fullscreen) {
        g_windowed_style = GetWindowLongPtrA(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_windowed_rect);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoA(mon, &mi);
        SetWindowLongPtrA(hwnd, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
        g_is_fullscreen = 1;
    } else {
        SetWindowLongPtrA(hwnd, GWL_STYLE, g_windowed_style);
        SetWindowPos(hwnd, NULL, g_windowed_rect.left, g_windowed_rect.top,
                     g_windowed_rect.right - g_windowed_rect.left, g_windowed_rect.bottom - g_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);
        g_is_fullscreen = 0;
    }
}

/* Resizes the actual OS window's client area to match the currently
   loaded room's real background dimensions (capped at VIEWPORT_W/H for
   rooms wider/taller than a typical screen, which still pan). Called
   whenever a room loads, including room-to-room transitions, so a wide
   SUBARCH-style scene actually LOOKS wide instead of being cropped to
   a fixed 640x480 window. */
static void resize_window_to_room(void) {
    if (!g_hwnd || !g_loaded) return;
    if (IsZoomed(g_hwnd) || g_is_fullscreen) return; /* respect the user's own fullscreen/maximize choice --
        don't un-maximize/un-fullscreen them just because a new room loaded (point 2 of the user's request) */
    int view_w = (int)g_hdr.width < VIEWPORT_W ? (int)g_hdr.width : VIEWPORT_W;
    int view_h = (int)g_hdr.height < VIEWPORT_H ? (int)g_hdr.height : VIEWPORT_H;
    if (view_h < 800) view_h = 800; /* room for the tool sidebar's buttons */
    RECT wr = {0, 0, view_w + TOOL_SIDEBAR_W, view_h};
    AdjustWindowRect(&wr, (DWORD)GetWindowLongPtrA(g_hwnd, GWL_STYLE), FALSE);
    SetWindowPos(g_hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

/* Flat 2D stand-in for rooms without 3D data (none at the moment). */
#define PLAYER_SPEED 180.0 /* pixels/second */
#define PLAYER_RADIUS 10
static double g_player_x = 320.0, g_player_y = 240.0;
static double g_player_target_x = 320.0, g_player_target_y = 240.0;
static int g_player_moving = 0;
#define GAME_TIMER_ID 2
#define GAME_TIMER_MS 16 /* ~60Hz */

static void clamp_to_room(double *x, double *y) {
    double minx = PLAYER_RADIUS, miny = PLAYER_RADIUS;
    double maxx = (double)g_hdr.width - PLAYER_RADIUS;
    double maxy = (double)g_hdr.height - PLAYER_RADIUS;
    if (maxx < minx) maxx = minx;
    if (maxy < miny) maxy = miny;
    if (*x < minx) *x = minx;
    if (*x > maxx) *x = maxx;
    if (*y < miny) *y = miny;
    if (*y > maxy) *y = maxy;
}

static void set_player_target(double x, double y) {
    clamp_to_room(&x, &y);
    g_player_target_x = x;
    g_player_target_y = y;
    g_player_moving = 1;
}

/* Advances the player toward its target at PLAYER_SPEED, called once
   per game tick. Returns 1 if the player actually moved (so the
   caller knows whether a repaint is needed). */
static int advance_player(double dt_seconds) {
    if (!g_player_moving) return 0;
    double dx = g_player_target_x - g_player_x;
    double dy = g_player_target_y - g_player_y;
    double dist = sqrt(dx * dx + dy * dy);
    double step = PLAYER_SPEED * dt_seconds;
    if (dist <= step || dist < 0.001) {
        g_player_x = g_player_target_x;
        g_player_y = g_player_target_y;
        g_player_moving = 0;
    } else {
        g_player_x += dx / dist * step;
        g_player_y += dy / dist * step;
    }
    return 1;
}

static void draw_player(HDC hdc, int cam_x, int cam_y) {
    int sx = (int)g_player_x - cam_x;
    int sy = (int)g_player_y - cam_y;
    HBRUSH body = CreateSolidBrush(RGB(220, 40, 40));
    HBRUSH old = (HBRUSH)SelectObject(hdc, body);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 220, 220));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    Ellipse(hdc, sx - PLAYER_RADIUS, sy - PLAYER_RADIUS, sx + PLAYER_RADIUS, sy + PLAYER_RADIUS);
    if (g_player_moving) {
        MoveToEx(hdc, sx, sy, NULL);
        int tx = (int)g_player_target_x - cam_x;
        int ty = (int)g_player_target_y - cam_y;
        LineTo(hdc, tx, ty);
    }
    SelectObject(hdc, old);
    SelectObject(hdc, oldPen);
    DeleteObject(body);
    DeleteObject(pen);
}

/* Small click-destination marker (item 16): a simple animated-looking
   crosshair/diamond at the world point David is currently walking to,
   re-projected through the room's real camera every frame (so it
   tracks correctly if the view scrolls) and cleared the moment he
   arrives or a new click overwrites it (see set_click_marker /
   advance_character). Procedural GDI shape rather than a baked bitmap
   -- same visual intent (a marker that appears and disappears), no
   extra asset/bake step needed for something this simple. Only
   meaningful for rooms with a real 3D camera (g_has_3d_character). */
static void draw_click_marker(HDC hdc, int cam_x, int cam_y) {
    if (!g_click_marker_active || !g_has_3d_character) return;
    float px, py, depth;
    if (!camera_world_to_pixel_z(&g_room_cam, g_click_marker_world, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &depth)) return;
    int sx = (int)px - cam_x, sy = (int)py - cam_y;
    const int r = 7;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 240, 80));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, sx - r, sy, NULL); LineTo(hdc, sx - 2, sy);
    MoveToEx(hdc, sx + 2, sy, NULL); LineTo(hdc, sx + r, sy);
    MoveToEx(hdc, sx, sy - r, NULL); LineTo(hdc, sx, sy - 2);
    MoveToEx(hdc, sx, sy + 2, NULL); LineTo(hdc, sx, sy + r);
    HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, nullBrush);
    Ellipse(hdc, sx - 3, sy - 3, sx + 3, sy + 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

/* Debug overlay (B key): David's collision volume as the pathfinding
   sees it -- a CHAR_RADIUS cylinder from his feet to his head (cyan),
   the two heights where walls are tested (knee 0.6 / chest 1.3, see
   segment_blocked_by_wall; magenta), and the remaining planned path
   (green). Projected through char_project, the same mapping as his
   drawn body, so it stays glued to him on rolled cameras too. Drawn on
   top, no depth test, so it's visible even behind geometry. */
static int g_show_collision = 0;
static void draw_collision_box(HDC hdc, int cam_x, int cam_y) {
    if (!g_show_collision || !g_has_3d_character) return;
    const int SEG = 20;
    /* feet, max step (anything lower is climbable), body band start, head
       -- the exact volume stand_ok tests, from the live settings */
    const float heights[4] = { 0.0f, NAV_MAX_STEP, NAV_BODY_Y0, NAV_BODY_Y1 };
    const float CHAR_RADIUS_DRAWN = g_nav.body_radius;
    const COLORREF cols[4] = { RGB(0, 230, 255), RGB(255, 60, 255), RGB(255, 60, 255), RGB(0, 230, 255) };
    COLORREF body = g_collision_enabled ? RGB(0, 230, 255) : RGB(140, 140, 140);
    for (int hi = 0; hi < 4; hi++) {
        HPEN pen = CreatePen(PS_SOLID, (hi == 1 || hi == 2) ? 1 : 2, g_collision_enabled ? cols[hi] : body);
        HPEN oldPen = (HPEN)SelectObject(hdc, pen);
        int started = 0;
        for (int k = 0; k <= SEG; k++) {
            float a = 6.2831853f * k / SEG;
            float w[3] = { g_char_pos[0] + cosf(a) * CHAR_RADIUS_DRAWN, g_char_pos[1] + heights[hi], g_char_pos[2] + sinf(a) * CHAR_RADIUS_DRAWN };
            float px, py, pz;
            if (!char_project(g_char_pos, w, &px, &py, &pz)) { started = 0; continue; }
            int sx = (int)px - cam_x, sy = (int)py - cam_y;
            if (!started) { MoveToEx(hdc, sx, sy, NULL); started = 1; } else LineTo(hdc, sx, sy);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, body);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    for (int k = 0; k < 4; k++) { /* vertical edges */
        float a = 6.2831853f * k / 4 + 0.785398f;
        float b[3] = { g_char_pos[0] + cosf(a) * CHAR_RADIUS_DRAWN, g_char_pos[1], g_char_pos[2] + sinf(a) * CHAR_RADIUS_DRAWN };
        float t[3] = { b[0], b[1] + heights[3], b[2] };
        float bx, by, bz, tx, ty, tz;
        if (!char_project(g_char_pos, b, &bx, &by, &bz) || !char_project(g_char_pos, t, &tx, &ty, &tz)) continue;
        MoveToEx(hdc, (int)bx - cam_x, (int)by - cam_y, NULL);
        LineTo(hdc, (int)tx - cam_x, (int)ty - cam_y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    if (g_char_moving) { /* remaining path, through the real room camera (it's on the floor) */
        HPEN gp = CreatePen(PS_SOLID, 2, RGB(60, 255, 90));
        HPEN op = (HPEN)SelectObject(hdc, gp);
        float px, py, pz;
        if (camera_world_to_pixel_z(&g_room_cam, g_char_pos, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) {
            MoveToEx(hdc, (int)px - cam_x, (int)py - cam_y, NULL);
            int first = g_char_waypoint_count > 0 ? g_char_waypoint_idx : 0;
            int last = g_char_waypoint_count > 0 ? g_char_waypoint_count - 1 : -1;
            if (last < 0) {
                if (camera_world_to_pixel_z(&g_room_cam, g_char_target, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz))
                    LineTo(hdc, (int)px - cam_x, (int)py - cam_y);
            }
            for (int i = first; i <= last; i++) {
                if (!camera_world_to_pixel_z(&g_room_cam, g_char_waypoints[i], (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) continue;
                LineTo(hdc, (int)px - cam_x, (int)py - cam_y);
            }
        }
        SelectObject(hdc, op);
        DeleteObject(gp);
    }
}

/* The on-screen rectangle (client coordinates) the room composite
   (real photo + depth-composited David, see WM_PAINT) is drawn into.
   Windowed: the WHOLE client area -- no aspect constraint, matches the
   existing free-resize behavior, no status bar strip stealing space
   anymore (see the corner command panel instead). Fullscreen: the
   game's own asset pipeline is built around 4:3, so fullscreen crops
   to the largest centered 4:3 rectangle that fits the monitor instead
   of stretching the composite to the monitor's own (usually 16:9)
   aspect or free-forming per room -- letterboxed with black bars,
   with mouse-edge scrolling (see WM_MOUSEMOVE/GAME_TIMER) panning the
   COMPOSITE inside that fixed 4:3 window when a room is bigger than
   it, exactly like the existing arrow-key pan, never by moving the
   underlying 3D camera. Generic across every room regardless of its
   own background image's aspect ratio (extremely tall or wide alike --
   see gno/screen1, a 640x1280 portrait room used to validate this). */
#define GAME_ASPECT_W 4
#define GAME_ASPECT_H 3
static void get_view_window_1(int *win_w, int *win_h, float *scale, int *ox, int *oy);
static void get_display_viewport(int *out_x, int *out_y, int *out_w, int *out_h) {
    RECT rc = {0, 0, VIEWPORT_W, VIEWPORT_H};
    if (g_hwnd) GetClientRect(g_hwnd, &rc);
    int full_w = rc.right, full_h = rc.bottom;
    if (full_w < 1) full_w = 1;
    if (full_h < 1) full_h = 1;
    if (!g_is_fullscreen) {
        /* windowed: the tool sidebar sits on the left of the picture */
        *out_x = TOOL_SIDEBAR_W; *out_y = 0; *out_w = full_w - TOOL_SIDEBAR_W; *out_h = full_h;
        if (*out_w < 1) *out_w = 1;
        return;
    }
    /* fullscreen: the 4:3 picture, leaving side bars at least
       TOOL_MIN_BAR wide for the tool panels (1920x1080: 1440x1080) */
    int w = full_w - 2 * TOOL_MIN_BAR, h = w * GAME_ASPECT_H / GAME_ASPECT_W;
    if (h > full_h) { h = full_h; w = full_h * GAME_ASPECT_W / GAME_ASPECT_H; }
    *out_x = (full_w - w) / 2;
    *out_y = (full_h - h) / 2;
    *out_w = w; *out_h = h;
}

/* The part of the room COMPOSITE (photo + 3D + David + overlays, all in
   room-image pixels) shown on screen, and how it is scaled.
   - Windowed: 1 composite pixel = 1 screen pixel, as much of the room as
     the client area allows (unchanged behaviour).
   - Fullscreen: the window is the LARGEST 4:3 RECTANGLE THAT FITS INSIDE
     the room image (640x480 for gno/screen1 640x1280 -> vertical scroll,
     640x480 for a 1280x480 panorama -> horizontal scroll, the whole image
     for 640x480 / 960x720 rooms), scaled up uniformly to fill the
     screen's 4:3 area: no distortion, no bars inside the 4:3 area.
   The 3D camera never moves: scrolling only moves this window over the
   already-composited frame (g_cam_x/g_cam_y). */
/* The view's zoom (x1 = the whole window as above): the room's own (scene
   editor, "Camera zoom"), or a script's (ACT_CAMERA). A zoom shows less of
   the room, bigger, in the same place on the screen. */
static void get_view_window(int *win_w, int *win_h, float *scale, int *ox, int *oy) {
    get_view_window_1(win_w, win_h, scale, ox, oy);
    if (g_view_zoom <= 1.001f) return;
    int dw = (int)(*win_w * *scale + 0.5f), dh = (int)(*win_h * *scale + 0.5f);
    *win_w = (int)(*win_w / g_view_zoom); *win_h = (int)(*win_h / g_view_zoom);
    if (*win_w < 1) *win_w = 1;
    if (*win_h < 1) *win_h = 1;
    float sx = (float)dw / *win_w, sy = (float)dh / *win_h;
    *scale = sx < sy ? sx : sy;
}
static void get_view_window_1(int *win_w, int *win_h, float *scale, int *ox, int *oy) {
    int vx, vy, vw, vh;
    get_display_viewport(&vx, &vy, &vw, &vh);
    int W = g_loaded ? (int)g_hdr.width : vw, H = g_loaded ? (int)g_hdr.height : vh;
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (!g_is_fullscreen) {
        *win_w = W < vw ? W : vw; *win_h = H < vh ? H : vh;
        *scale = 1.0f; *ox = vx; *oy = vy;
        return;
    }
    if (W * GAME_ASPECT_H >= H * GAME_ASPECT_W) { *win_h = H; *win_w = H * GAME_ASPECT_W / GAME_ASPECT_H; }
    else { *win_w = W; *win_h = W * GAME_ASPECT_H / GAME_ASPECT_W; }
    if (*win_w > W) *win_w = W;
    if (*win_h > H) *win_h = H;
    if (*win_w < 1) *win_w = 1;
    if (*win_h < 1) *win_h = 1;
    float sx = (float)vw / (float)*win_w, sy = (float)vh / (float)*win_h;
    *scale = sx < sy ? sx : sy;
    int dw = (int)(*win_w * *scale + 0.5f), dh = (int)(*win_h * *scale + 0.5f);
    *ox = vx + (vw - dw) / 2;
    *oy = vy + (vh - dh) / 2;
}

/* Mouse events arrive in raw client coordinates, which only equal
   room-composite coordinates in windowed mode; in fullscreen the 4:3
   viewport is offset/letterboxed inside the client area (see
   get_display_viewport), so clicks/hover need that offset subtracted
   before adding the room scroll position -- otherwise hotspot hit-
   testing and click-to-move both aim at the wrong point whenever the
   letterbox bars are showing. */
static void client_to_room_point(int client_x, int client_y, int *out_x, int *out_y) {
    int win_w, win_h, ox, oy; float sc;
    get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    *out_x = (int)floorf((client_x - ox) / sc) + g_cam_x;
    *out_y = (int)floorf((client_y - oy) / sc) + g_cam_y;
}

static void clamp_camera(void);
/* Scrolls the visible window so David is centred (clamped to the room):
   used when a room loads and after walking through a door, so e.g. in
   gno/screen1 you land on the part of the shaft where he actually is. */
static void center_view_on_david(void) {
    if (!g_loaded || !g_has_3d_character || !g_hwnd) return;
    float px, py, pz;
    if (!camera_world_to_pixel_z(&g_room_cam, g_char_pos, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) return;
    int win_w, win_h, ox, oy; float sc;
    get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    g_cam_x = (int)px - win_w / 2;
    g_cam_y = (int)py - win_h / 2 - win_h / 10; /* a bit of headroom above him */
    clamp_camera();
}

/* The view centred on a room point, at a zoom (clamped to the room). */
static void view_set(float cx, float cy, float zoom) {
    g_view_zoom = zoom < 1.0f ? 1.0f : zoom;
    int win_w, win_h, ox, oy; float sc;
    get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    g_cam_x = (int)(cx - win_w / 2.0f + 0.5f); g_cam_y = (int)(cy - win_h / 2.0f + 0.5f);
    clamp_camera();
}
/* the room point at the centre of the view */
static void view_center(float *cx, float *cy) {
    int win_w, win_h, ox, oy; float sc;
    get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    *cx = g_cam_x + win_w / 2.0f; *cy = g_cam_y + win_h / 2.0f;
}
/* A script's camera: a smooth slide of the view to a point and a zoom (g_camslide). */
static void camera_slide(float tx, float ty, float tz, float dur) {
    float cx, cy; view_center(&cx, &cy);
    g_camslide.fx = cx; g_camslide.fy = cy; g_camslide.fz = g_view_zoom;
    g_camslide.tx = tx; g_camslide.ty = ty; g_camslide.tz = tz < 1.0f ? 1.0f : tz;
    g_camslide.t = 0.0f; g_camslide.dur = dur;
    g_camslide.active = 1;
    if (dur <= 0.0f) { g_camslide.active = 0; view_set(tx, ty, g_camslide.tz); }
}
/* game_tick: 1 if the view moved */
static int camera_tick(float dt) {
    if (!g_camslide.active) return 0;
    g_camslide.t += dt;
    float k = g_camslide.t >= g_camslide.dur ? 1.0f : g_camslide.t / g_camslide.dur;
    k = k * k * (3.0f - 2.0f * k); /* eased */
    view_set(g_camslide.fx + (g_camslide.tx - g_camslide.fx) * k, g_camslide.fy + (g_camslide.ty - g_camslide.fy) * k,
             g_camslide.fz + (g_camslide.tz - g_camslide.fz) * k);
    if (g_camslide.t >= g_camslide.dur) g_camslide.active = 0;
    return 1;
}

static void clamp_camera(void) {
    /* Uses the LIVE display viewport (4:3-cropped in fullscreen, the
       free client area in windowed), not a fixed VIEWPORT_W/H -- the
       window is freely resizable/maximizable (point 2 of the user's
       request), so how much of the room is visible (and therefore how
       far panning can go) changes with the actual current window/
       viewport size. */
    int view_w = VIEWPORT_W, view_h = VIEWPORT_H;
    if (g_hwnd) {
        int ox, oy; float sc;
        get_view_window(&view_w, &view_h, &sc, &ox, &oy); /* in COMPOSITE pixels */
    }
    int max_x = (int)g_hdr.width - view_w;
    int max_y = (int)g_hdr.height - view_h;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (g_cam_x < 0) g_cam_x = 0;
    if (g_cam_y < 0) g_cam_y = 0;
    if (g_cam_x > max_x) g_cam_x = max_x;
    if (g_cam_y > max_y) g_cam_y = max_y;
}

/* =====================================================================
   ROOM OVERLAYS: the environmental animations of a room (the original
   game's .smk videos, exported as <name>.<n>.png in the room's folder).
   Each is a patch of the room's own picture, drawn over it at its place
   -- found by tools/find_overlay_positions.py and kept, with its frame
   rate, in data/rooms/<level>/<room>_overlays.cfg. Nothing plays by
   itself: the scripts' "Overlays" action loops them, plays them once,
   freezes them on a frame or hides them.
   ===================================================================== */
enum { OVS_HIDDEN, OVS_LOOP, OVS_ONCE, OVS_ONCE_HIDE, OVS_FROZEN };
#define MAX_ROOM_OVERLAYS 32
typedef struct {
    char name[32];
    int frames, x, y, w, h;
    char bg[48];              /* the picture it was matched on (the room's own, or a variant like rep1) */
    char conf[8];             /* sure / guess / none / set (moved by hand) */
    float fps;
    int state;                /* OVS_* */
    int frame, finished;      /* finished: a "once" came to its end */
    float t;
    uint32_t **img;           /* frames, loaded the first time they're drawn (0xAARRGGBB) */
    int *iw, *ih;
} RoomOverlay;
static RoomOverlay g_ov[MAX_ROOM_OVERLAYS];
static int g_ov_n = 0;
static char g_ov_room[128] = "";
static int g_ov_show = -1;    /* shown at its place whatever its state (its position being picked) */

static void overlays_free(void) {
    for (int i = 0; i < g_ov_n; i++) {
        RoomOverlay *o = &g_ov[i];
        if (o->img) for (int f = 0; f < o->frames; f++) free(o->img[f]);
        free(o->img); free(o->iw); free(o->ih);
    }
    g_ov_n = 0;
}
static void overlays_path(char *out, size_t n, const char *label) { root_path(out, n, "data/rooms/%s_overlays.cfg", label); }
static void overlays_load(const char *label) {
    overlays_free();
    snprintf(g_ov_room, sizeof(g_ov_room), "%s", label);
    char path[1024], line[512];
    overlays_path(path, sizeof(path), label);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f) && g_ov_n < MAX_ROOM_OVERLAYS) {
        RoomOverlay o; memset(&o, 0, sizeof(o));
        o.fps = 15.0f;
        if (sscanf(line, "overlay %31s %d %d %d %d %d %47s %7s %f", o.name, &o.frames, &o.x, &o.y, &o.w, &o.h, o.bg, o.conf, &o.fps) < 6 || o.frames <= 0) continue;
        if (o.fps <= 0.5f || o.fps > 120.0f) o.fps = 15.0f;
        o.img = (uint32_t **)calloc(o.frames, sizeof(uint32_t *));
        o.iw = (int *)calloc(o.frames, sizeof(int)); o.ih = (int *)calloc(o.frames, sizeof(int));
        g_ov[g_ov_n++] = o;
    }
    fclose(f);
}
static void overlays_save(void) {
    char path[1024];
    overlays_path(path, sizeof(path), g_ov_room);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Silver Remaster: this room's overlays (environmental animations) -- tools/find_overlay_positions.py\n");
    fprintf(f, "# overlay <name> <frames> <x> <y> <width> <height> <picture it matched> <sure|guess|none|set> <fps>\n");
    fprintf(f, "# frames: assets/levels/<level>/<room>/<name>.<0..frames-1>.png, drawn with their top-left corner at x y of the room picture\n");
    for (int i = 0; i < g_ov_n; i++) {
        RoomOverlay *o = &g_ov[i];
        fprintf(f, "overlay %s %d %d %d %d %d %s %s %.2f\n", o->name, o->frames, o->x, o->y, o->w, o->h, o->bg[0] ? o->bg : "-", o->conf[0] ? o->conf : "set", o->fps);
    }
    fclose(f);
}
static int overlay_find(const char *name) {
    for (int i = 0; i < g_ov_n; i++) if (!strcmp(g_ov[i].name, name)) return i;
    return -1;
}
static const uint32_t *overlay_frame(RoomOverlay *o, int f, int *w, int *h) {
    if (f < 0) f = 0;
    if (f >= o->frames) f = o->frames - 1;
    if (!o->img[f]) {
        char level[64], room[64], path[1024];
        split_label(g_ov_room, level, sizeof(level), room, sizeof(room));
        root_path(path, sizeof(path), "assets/levels/%s/%s/%s.%d.png", level, room, o->name, f);
        o->img[f] = image_load(path, &o->iw[f], &o->ih[f]);
        if (!o->img[f]) { o->iw[f] = o->ih[f] = 0; }
    }
    *w = o->iw[f]; *h = o->ih[f];
    return o->img[f];
}
/* mode: the script action's OVM_* */
static void overlay_apply(RoomOverlay *o, int mode, int frame) {
    switch (mode) {
        case OVM_LOOP: o->state = OVS_LOOP; break;
        case OVM_ONCE: o->state = OVS_ONCE; break;
        case OVM_ONCE_HIDE: o->state = OVS_ONCE_HIDE; break;
        case OVM_FREEZE: o->state = OVS_FROZEN; break;
        case OVM_HIDE: o->state = OVS_HIDDEN; break;
        default: return;
    }
    o->t = 0.0f; o->finished = 0;
    o->frame = mode == OVM_FREEZE ? (frame < 0 ? 0 : frame >= o->frames ? o->frames - 1 : frame) : 0;
}
static void overlays_reset(void) { for (int i = 0; i < g_ov_n; i++) { g_ov[i].state = OVS_HIDDEN; g_ov[i].frame = 0; g_ov[i].finished = 0; } }
/* returns 1 if a frame shown changed */
static int overlays_tick(float dt) {
    int changed = 0;
    for (int i = 0; i < g_ov_n; i++) {
        RoomOverlay *o = &g_ov[i];
        if (o->state != OVS_LOOP && o->state != OVS_ONCE && o->state != OVS_ONCE_HIDE) continue;
        if (o->finished) continue;
        o->t += dt;
        int f = (int)(o->t * o->fps);
        if (o->state == OVS_LOOP) f %= o->frames;
        else if (f >= o->frames) {
            o->finished = 1;
            if (o->state == OVS_ONCE_HIDE) { o->state = OVS_HIDDEN; changed = 1; continue; }
            f = o->frames - 1; /* a "once" stays on its last frame */
        }
        if (f != o->frame) { o->frame = f; changed = 1; }
    }
    return changed;
}
/* into the room frame, right over the picture (under the characters) */
static void overlays_draw(void) {
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    for (int i = 0; i < g_ov_n; i++) {
        RoomOverlay *o = &g_ov[i];
        if (o->state == OVS_HIDDEN && i != g_ov_show) continue;
        int w, h;
        const uint32_t *px = overlay_frame(o, o->frame, &w, &h);
        if (!px) continue;
        for (int y = 0; y < h; y++) {
            int ry = o->y + y;
            if (ry < 0 || ry >= H) continue;
            const uint32_t *src = px + (size_t)y * w;
            uint32_t *dst = g_render_pixels + (size_t)ry * g_dib_stride;
            for (int x = 0; x < w; x++) {
                int rx = o->x + x;
                if (rx < 0 || rx >= W || (src[x] >> 24) < 128) continue;
                dst[rx] = src[x] & 0x00FFFFFFu;
            }
        }
    }
}

static int g_room_gen = 0;      /* bumped on every room load */
static int g_room_entered = 0;  /* a room was just loaded: game_tick starts its arrival / auto script */
static struct {
    int phase; float t;  /* room transition (see ROOM TRANSITION): 0 none, 1 fading out, 2 loading, 3 fading in */
    int door;            /* through this connector... */
    char room[128];      /* ...or (script "Change room") to this room, */
    int room_door;       /* coming in by its connector (id, 0 = where he'd spawn), */
    char script[64];     /* playing this script there */
} g_tr;
static void transition_start_room(const char *label, int door, const char *script);
static void room_travel(HWND hwnd, const char *label, int door, const char *script);
static char g_arrival_script[64] = ""; /* script of the connector David just came through */
static void room_scripts_load(const char *label); /* SCRIPTS */
static int g_sv_pick = 0;       /* a script action's point being picked in the room (SV_PICK_*) */
static void actors_clear_npcs(void);

static void unload_room(void) {
    free_room_mesh();
    free(g_dib_pixels); g_dib_pixels = NULL;
    free(g_dib_own); g_dib_own = NULL;
    free(g_render_pixels); g_render_pixels = NULL;
    free(g_depth_buffer); g_depth_buffer = NULL;
    free(g_depth_buffer_char); g_depth_buffer_char = NULL;
    overlays_free();
    g_loaded = 0;
}

/* Loads room "level/room": its picture, then camera/geometry/David. */
static int load_room(const char *label) {
    unload_room();
    char level[64], room[64], path[1024];
    split_label(label, level, sizeof(level), room, sizeof(room));
    root_path(path, sizeof(path), "assets/levels/%s/%s/%s.png", level, room, room);
    int w = 0, h = 0;
    uint32_t *px = image_load(path, &w, &h);
    if (!px) {
        snprintf(g_status, sizeof(g_status), "room picture not found: assets/levels/%s/%s/%s.png", level, room, room);
        return 0;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] &= 0x00FFFFFFu;
    g_hdr.width = (uint32_t)w; g_hdr.height = (uint32_t)h;
    g_dib_stride = (uint32_t)w;
    g_dib_pixels = px;
    g_render_pixels = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    g_depth_buffer = (float *)malloc((size_t)w * h * sizeof(float));
    g_depth_buffer_char = (float *)malloc((size_t)w * h * sizeof(float));
    snprintf(g_status, sizeof(g_status), "%s -- %dx%d", label, w, h);

    g_loaded = 1;
    g_player_x = g_player_target_x = g_hdr.width / 2.0;
    g_player_y = g_player_target_y = g_hdr.height / 2.0;
    g_player_moving = 0;
    g_cam_x = 0;
    g_cam_y = 0;

    g_has_3d_character = 0;
    actors_clear_npcs();
    audio_stop_kind(AUDIO_SOUND); audio_stop_kind(AUDIO_AMBIENCE); /* the music goes on */
    try_start_3d_character_for_room(label);
    room_scripts_load(label);
    overlays_load(label);
    g_room_gen++;
    g_room_entered = 1; /* game_tick: the room's arrival / auto script */
    resize_window_to_room();
    center_view_on_david();
    return 1;
}

/* Script action BACKGROUND: the room shows another picture (a path under
   assets/levels, stretched to the room's size if it differs), "" = its own
   picture again. Only the picture: geometry, depth, shapes stay the room's. */
static int background_set(const char *file) {
    if (!g_loaded) return 0;
    size_t npix = (size_t)g_hdr.width * g_hdr.height;
    if (!file || !file[0]) {
        if (g_dib_own) { memcpy(g_dib_pixels, g_dib_own, npix * sizeof(uint32_t)); free(g_dib_own); g_dib_own = NULL; }
        return 1;
    }
    char path[1024];
    root_path(path, sizeof(path), "assets/levels/%s", file);
    int w = 0, h = 0;
    uint32_t *px = image_load(path, &w, &h);
    if (!px) { snprintf(g_status, sizeof(g_status), "script: picture not found: assets/levels/%s", file); return 0; }
    if (!g_dib_own) {
        g_dib_own = (uint32_t *)malloc(npix * sizeof(uint32_t));
        if (g_dib_own) memcpy(g_dib_own, g_dib_pixels, npix * sizeof(uint32_t));
    }
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    for (int y = 0; y < H; y++) {
        const uint32_t *src = px + (size_t)(y * h / H) * w;
        for (int x = 0; x < W; x++) g_dib_pixels[(size_t)y * g_dib_stride + x] = src[x * w / W] & 0x00FFFFFFu;
    }
    free(px);
    return 1;
}

static char g_report_path[1024];

/* Room-to-room transition (TAB list, PgUp/PgDn, scene connectors). */
static void change_room(const char *label) {
    g_current_map_room = map_index_of_label(label);
    load_room(label);
}

/* Counts how many of David's skinned idle-pose vertices would actually
   draw on screen right now: on-screen through char_project AND passing
   the real depth test against g_depth_buffer (must already hold the
   room mesh's depth -- see render_room_mesh_depth_only). A faithful,
   non-drawing proxy for "would render_3d_character actually show him",
   used by the spawn-visibility batch check below instead of eyeballing
   277 screenshots by hand. */
static int count_character_visible_vertices(void) {
    if (!g_has_3d_character) return 0;
    return spawn_visibility_at(g_char_pos[0], g_char_pos[1], g_char_pos[2]);
}

/* Batch spawn-visibility check across every room the TAB browser knows
   about (see load_room_list) -- how many rooms end up with David's
   spawn point genuinely invisible (occluded or off-frame), the same
   failure mode found by hand on aftastep/libarea1, but measured instead
   of guessed at. Triggered by passing "VERIFY_SPAWNS" as the exe's
   first command-line token (see WinMain); writes a plain-text report
   and exits without opening the normal message loop. */
/* Batch pathfinding check ("VERIFY_PATHS"): in every room, clicks a
   5x4 grid of screen points (collision ON, same try_click_to_move as a
   real click), then plays the movement out frame by frame at 30 fps and
   counts, per frame: a wall crossed between consecutive positions, and
   feet more than 0.3 units off the floor beneath him ("flying"). Also
   counts clicks that reached their target and the slowest click. */
/* Debug: writes the current room's nav grid as a PPM (top view):
   black = no floor, gray = floor (brighter = more layers), green = the
   start node's reachable set (flood fill with the SAME edge rules as
   A*: step height + wall test), red = David. */
static void dump_nav_grid_ppm(const char *path) {
    /* Top view of the REAL nav data (same connected-area labels A* and
       the click fallbacks use): black = no standable floor, gray = floor
       in another area (brighter = more layers), green = David's area,
       red = David. */
    NavGrid *g = &g_nav_grid;
    if (!g->walkable || !g->comp) return;
    int W = g->cols, H = g->rows;
    int dn = navgrid_node_at(g, g_char_pos[0], g_char_pos[1], g_char_pos[2]);
    int area = dn >= 0 ? g->comp[dn] : -2;
    int sc = (int)((g_char_pos[0] - g->min_x) / g->cell_size), sr = (int)((g_char_pos[2] - g->min_z) / g->cell_size);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int r = 0; r < H; r++) for (int c2 = 0; c2 < W; c2++) {
        int cell = r * W + c2, k = g->walkable[cell], mine = 0;
        for (int l = 0; l < k; l++) if (g->comp[cell * NAV_MAX_LAYERS + l] == area) mine = 1;
        uint8_t px[3] = { 0, 0, 0 };
        if (k) { uint8_t v = (uint8_t)(70 + 45 * k); px[0] = px[1] = px[2] = v; }
        if (mine) { px[0] = 40; px[1] = 200; px[2] = 60; }
        if (c2 == sc && r == sr) { px[0] = 255; px[1] = 0; px[2] = 0; }
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static void run_path_verification(void) {
    FILE *out = fopen((root_path(g_report_path, sizeof(g_report_path), "build/path_verify_report.txt"), g_report_path), "w");
    long tot_clip = 0;
    long tot_clicks = 0, tot_accepted = 0, tot_arrived = 0, tot_frames = 0, tot_wall = 0, tot_float = 0;
    int rooms_bad = 0;
    double worst_ms = 0;
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    g_collision_enabled = 1;
    /* reference speeds, so runs stay comparable whatever the user tuned */
    g_david_walk_speed = 6.0f; g_david_run_speed = 13.0f; g_david_anim_speed = 1.0f; g_david_run_turn_pct = 50.0f;
    for (int i = 0; i < g_map_room_count; i++) {
        MapRoomEntry *e = &g_map_rooms[i];
        change_room(e->label);
        if (!g_loaded || !g_has_3d_character || g_room_mesh_tri_count <= 0) continue;
        {   const char *dn = getenv("SILVER_DUMP_NAV");
            if (dn && strstr(e->label, dn)) {
                char dp[600]; const char *slash = strrchr(e->label, '/');
                root_path(dp, sizeof(dp), "build/nav_%s.ppm", slash ? slash + 1 : e->label);
                dump_nav_grid_ppm(dp);
            } }
        int accepted = 0, arrived = 0, wall = 0, flo = 0, frames = 0, clicks = 0, why[16] = {0}, still_moving = 0, stopped_short = 0, clip = 0;
        double room_worst = 0;
        for (int gy = 1; gy <= 4; gy++) for (int gx = 1; gx <= 5; gx++) {
            float px = g_hdr.width * gx / 6.0f, py = g_hdr.height * gy / 5.0f;
            clicks++;
            LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
            int ok = try_click_to_move(px, py, 0);
            QueryPerformanceCounter(&t1);
            double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
            if (ms > room_worst) room_worst = ms;
            if (!ok) { why[g_click_fail < 16 ? g_click_fail : 15]++; continue; }
            accepted++;
            float goal[3] = { g_click_marker_world[0], g_click_marker_world[1], g_click_marker_world[2] };
            for (int f = 0; f < 30 * 30 && g_char_moving; f++) {
                float prev[3] = { g_char_pos[0], g_char_pos[1], g_char_pos[2] };
                advance_character(1.0f / 30.0f);
                frames++;
                if (segment_blocked_by_wall(prev, g_char_pos, g_room_mesh_tris, g_room_mesh_tri_count, 0.55f)) wall++;
                if (!body_clear_at(g_char_pos[0], g_char_pos[1], g_char_pos[2], 0.28f, g_room_mesh_tris, g_room_mesh_tri_count)) clip++;
                float fy;
                if (!floor_below(g_char_pos[0], g_char_pos[2], g_char_pos[1], 0.3f, g_room_mesh_tris, g_room_mesh_tri_count, &fy) ||
                    fabsf(fy - g_char_pos[1]) > 0.3f) flo++;
            }
            float ddx = g_char_pos[0] - goal[0], ddz = g_char_pos[2] - goal[2];
            if (!g_char_moving && ddx * ddx + ddz * ddz < 0.25f) arrived++;
            else if (g_char_moving) still_moving++;
            else stopped_short++;
            g_char_moving = 0; g_char_waypoint_count = 0; g_char_turn_clip = -1;
        }
        tot_clicks += clicks; tot_accepted += accepted; tot_arrived += arrived;
        tot_frames += frames; tot_wall += wall; tot_float += flo;
        if (room_worst > worst_ms) worst_ms = room_worst;
        tot_clip += clip;
        int bad = wall > 0 || clip > 0 || flo > frames / 20;
        if (bad) rooms_bad++;
        if (out) fprintf(out, "%-24s %s clicks=%d accepted=%d arrived=%d frames=%d wall_cross=%d body_clip=%d floating=%d worst_click=%.0fms timeout=%d short=%d refused[noFloorHit=%d noStart=%d noGoal=%d disconnected=%d]\n",
                         e->label, bad ? "BAD" : "ok ", clicks, accepted, arrived, frames, wall, clip, flo, room_worst, still_moving, stopped_short, why[1], why[11], why[12], why[13]);
        if (out) fflush(out);
    }
    if (out) {
        fprintf(out, "\n--- summary ---\nclicks %ld, accepted %ld, arrived %ld\nframes %ld, wall crossings %ld, body-in-geometry frames %ld, floating frames %ld\nrooms flagged: %d\nslowest click: %.0f ms\n",
                tot_clicks, tot_accepted, tot_arrived, tot_frames, tot_wall, tot_clip, tot_float, rooms_bad, worst_ms);
        fclose(out);
    }
}

static void run_spawn_verification(void) {
    FILE *out = fopen((root_path(g_report_path, sizeof(g_report_path), "build/spawn_verify_report.txt"), g_report_path), "w");
    int n_no_char = 0, n_bad = 0, n_ok = 0;
    for (int i = 0; i < g_map_room_count; i++) {
        MapRoomEntry *e = &g_map_rooms[i];
        change_room(e->label);
        if (!g_loaded) { if (out) fprintf(out, "%-24s LOAD_FAILED\n", e->label); continue; }
        if (!g_has_3d_character) { n_no_char++; if (out) fprintf(out, "%-24s NO_3D_CHARACTER\n", e->label); continue; }
        size_t npix = (size_t)g_dib_stride * g_hdr.height;
        for (size_t p = 0; p < npix; p++) g_depth_buffer_char[p] = 1e29f;
        render_room_mesh_depth_only_cam(&g_room_cam, g_depth_buffer_char);
        int vis = count_character_visible_vertices();
        int pct = DAVID_VERTEX_COUNT > 0 ? (vis * 100 / DAVID_VERTEX_COUNT) : 0;
        /* Projected on-screen height of David, as % of the 4:3 view height
           (width*3/4) -- catches the opposite failure: a spawn so close to
           the camera he fills the frame (fully "visible" but a giant). */
        float min_py = 1e30f, max_py = -1e30f;
        for (int vi = 0; vi < DAVID_VERTEX_COUNT; vi++) {
            float wp[3] = { g_david_idle_offsets[vi][0]+g_char_pos[0], g_david_idle_offsets[vi][1]+g_char_pos[1], g_david_idle_offsets[vi][2]+g_char_pos[2] };
            float px, py, pz;
            if (!char_project(g_char_pos, wp, &px, &py, &pz)) continue;
            if (py < min_py) min_py = py;
            if (py > max_py) max_py = py;
        }
        int hpct = max_py > min_py ? (int)((max_py - min_py) * 100.0f / (g_hdr.width * 0.75f)) : 0;
        const char *tag = pct < 5 ? "BAD " : (hpct > 75 ? "HUGE" : "ok  ");
        if (pct < 5 || hpct > 75) n_bad++; else n_ok++;
        if (out) fprintf(out, "%-24s %s visible=%d/%d (%d%%) height=%d%% pos=(%.2f,%.2f,%.2f)\n", e->label, tag, vis, DAVID_VERTEX_COUNT, pct, hpct, g_char_pos[0], g_char_pos[1], g_char_pos[2]);
    }
    if (out) {
        fprintf(out, "\n--- summary ---\ntotal rooms: %d\nok: %d\nbad (spawn <5%% visible): %d\nno 3D character: %d\n",
                g_map_room_count, n_ok, n_bad, n_no_char);
        fclose(out);
    }
}

/* ---------------- camera intrinsics calibration ----------------
   The blockouts' glTF cameras carry a yfov, but how the ORIGINAL renderer
   mapped it to pixels isn't something we can assume: measured against
   the game's own per-pixel depth masks (<room>.mask.png), the
   legacy "yfov spans the image height" convention was ~1.3x too zoomed
   on 640x480 rooms and badly wrong on tall/wide ones (see docs/PROGRESS.md).
   So each room's focal length + principal point are FITTED to its own
   depth mask: render the mesh's depth for a candidate (f, cx, cy) and
   score the Spearman rank correlation with the mask's depth code (rank-
   based, so the mask's unknown depth encoding curve doesn't matter).
   Same rule for every room; nothing hand-tuned. */

typedef struct { int w, h; uint16_t *code; } ZMask;

/* Rotates a camera about its own view axis by `deg` (R: view->world,
   columns = right, up, -forward; R' = R * Rz). */
static void camera_apply_roll(RoomCamera *cam, float deg) {
    float a = deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    float R[9];
    memcpy(R, cam->R, sizeof(R));
    for (int i = 0; i < 3; i++) {
        cam->R[i*3+0] = R[i*3+0] * c + R[i*3+1] * s;
        cam->R[i*3+1] = -R[i*3+0] * s + R[i*3+1] * c;
        cam->R[i*3+2] = R[i*3+2];
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) cam->Rinv[i*3+j] = cam->R[j*3+i];
}

/* The room's original depth mask (assets/levels/<level>/<room>/<room>.mask.png):
   a scalar in R, with G = 255-R and B = 127+R/2, growing with distance;
   other pixels (black, marker colours) carry no depth (0xFFFF). */
static int zmask_load(const char *label, ZMask *m) {
    char level[64], room[64], path[1024];
    split_label(label, level, sizeof(level), room, sizeof(room));
    root_path(path, sizeof(path), "assets/levels/%s/%s/%s.mask.png", level, room, room);
    int w = 0, h = 0;
    uint32_t *px = image_load(path, &w, &h);
    if (!px) return 0;
    m->w = w; m->h = h;
    m->code = (uint16_t *)malloc((size_t)w * h * 2);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        int a = (int)(px[i] >> 24), r = (int)((px[i] >> 16) & 255), g = (int)((px[i] >> 8) & 255), b = (int)(px[i] & 255);
        int valid = a > 0 && r + g + b > 0 && abs(g - (255 - r)) < 4 && abs(b - (127 + r / 2)) < 4;
        m->code[i] = valid ? (uint16_t)r : 0xFFFF;
    }
    free(px);
    return 1;
}

/* Depth-only raster into an explicit W*H buffer (row stride W), with
   the same two-pass front-face-preferred rule as the real renderer. */
static void calib_raster(const RoomCamera *cam, int W, int H, float *buf) {
    for (int i = 0; i < W * H; i++) buf[i] = 1e29f;
    for (int pass = 0; pass < 2; pass++)
    for (int t = 0; t < g_room_mesh_tri_count; t++) {
        const float *v[3] = { g_room_mesh_tris + (size_t)t * 9, g_room_mesh_tris + (size_t)t * 9 + 3, g_room_mesh_tris + (size_t)t * 9 + 6 };
        if (tri_faces_point(v[0], v[1], v[2], cam->translation) == pass) continue;
        float px[3], py[3], pz[3];
        int ok = 1;
        for (int k = 0; k < 3; k++) ok &= camera_world_to_pixel_z(cam, v[k], W, H, &px[k], &py[k], &pz[k]);
        if (!ok) continue;
        int minx = (int)floorf(fminf(px[0], fminf(px[1], px[2]))), maxx = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
        int miny = (int)floorf(fminf(py[0], fminf(py[1], py[2]))), maxy = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx >= W) maxx = W - 1;
        if (maxy >= H) maxy = H - 1;
        float den = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
        if (fabsf(den) < 1e-6f) continue;
        for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
            float fx = x + 0.5f, fy = y + 0.5f;
            float w0 = ((py[1] - py[2]) * (fx - px[2]) + (px[2] - px[1]) * (fy - py[2])) / den;
            float w1 = ((py[2] - py[0]) * (fx - px[2]) + (px[0] - px[2]) * (fy - py[2])) / den;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            depth_test_two_pass(&buf[y * W + x], 1.0f / (w0 / pz[0] + w1 / pz[1] + w2 / pz[2]), pass); /* perspective-correct */
        }
    }
    for (int i = 0; i < W * H; i++) if (buf[i] < 0) buf[i] = -buf[i];
}

/* Alignment score between the rendered mesh depth (candidate camera) and
   the room's original depth mask, in [-1, 1]: for every pair of
   neighbouring pixels, does depth go the same way (nearer/farther) in
   both? Measured this way because the mask's 8-bit depth code WRAPS
   AROUND in deep rooms (verified: gno/boilintr, a vertical shaft --
   the code climbs 62 -> 144 then drops back to ~10 for the far end),
   which broke the global rank correlation used before (it picked a 2x
   wrong focal there). Local differences only see a wrap as one big jump,
   skipped (|dcode| > 100). Also sharper: the right camera wins clearly
   on every room tested. Returns 2*agree/total - 1. */
static float calib_score(const RoomCamera *base, const ZMask *m, float f, float cx, float cy, int ds, float *coverage) {
    int W = (int)g_hdr.width / ds, H = (int)g_hdr.height / ds;
    RoomCamera c = *base;
    c.focal = f / ds; c.cx = cx / ds; c.cy = cy / ds;
    static float *buf = NULL, *code = NULL;
    static int cap = 0;
    if (W * H > cap) {
        cap = W * H;
        buf = realloc(buf, cap * sizeof(float)); code = realloc(code, cap * sizeof(float));
    }
    calib_raster(&c, W, H, buf);
    int n = 0, n_mask = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int mx = x * ds + ds / 2, my = y * ds + ds / 2;
        float cv = -1.0f;
        if (mx < m->w && my < m->h && m->code[my * m->w + mx] != 0xFFFF) cv = (float)m->code[my * m->w + mx];
        code[y * W + x] = cv;
        if (cv < 0) continue;
        n_mask++;
        if (buf[y * W + x] < 1e28f) n++;
    }
    if (coverage) *coverage = n_mask ? (float)n / n_mask : 0.0f;
    long agree = 0, total = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        if (code[i] < 0 || buf[i] >= 1e28f) continue;
        for (int k = 0; k < 2; k++) {
            int j = k == 0 ? (x + 1 < W ? i + 1 : -1) : (y + 1 < H ? i + W : -1);
            if (j < 0 || code[j] < 0 || buf[j] >= 1e28f) continue;
            float dm = code[j] - code[i], dz = buf[j] - buf[i];
            if (fabsf(dm) < 1.0f || fabsf(dm) > 100.0f || fabsf(dz) < 1e-6f) continue;
            total++;
            if ((dm > 0) == (dz > 0)) agree++;
        }
    }
    if (total < 200) return -1.0f;
    return 2.0f * (float)agree / (float)total - 1.0f;
}

/* Penalized objective: a candidate that leaves much of the mask's
   geometry with no rendered surface at all (zoomed so far out/away the
   mesh doesn't cover what the photo shows) must not win on rank
   correlation over a small patch. */
static float calib_objective(const RoomCamera *base, const ZMask *m, float f, float cx, float cy, int ds) {
    float cov;
    float r = calib_score(base, m, f, cx, cy, ds, &cov);
    if (cov < 0.85f) r -= (0.85f - cov);
    return r;
}

static void run_camera_calibration(void) {
    FILE *log = fopen((root_path(g_report_path, sizeof(g_report_path), "build/calibration_report.txt"), g_report_path), "w");
    int n_cal = 0, n_kept_legacy = 0, n_skip = 0;
    for (int i = 0; i < g_map_room_count; i++) {
        MapRoomEntry *e = &g_map_rooms[i];
        char out_path[1024];
        root_path(out_path, sizeof(out_path), "data/rooms/%s_intrinsics.cfg", e->label);
        /* remove any previous fit BEFORE loading the room: otherwise the
           room's camera comes up with the old fitted roll/intrinsics
           already applied and the new fit is measured on top of them */
        remove(out_path);
        change_room(e->label);
        ZMask m;
        if (!g_loaded || !g_has_3d_character || g_room_mesh_tri_count <= 0 || !zmask_load(e->label, &m)) {
            n_skip++;
            if (log) fprintf(log, "%-24s SKIP (no room/mesh/mask)\n", e->label);
            continue;
        }
        RoomCamera base = g_room_cam;
        base.focal = 0;
        float Wf = (float)g_hdr.width, Hf = (float)g_hdr.height;
        float f_leg = (Wf * 0.5f) / (tanf(base.yfov / 2.0f) * (16.0f / 9.0f)); /* the default rule (camera_intrinsics) */
        float leg = calib_score(&base, &m, f_leg, Wf * 0.5f, Hf * 0.5f, 2, NULL);
        /* coarse grid: focal x principal row x ROLL. Roll is fitted
           too: many exported cameras carry a large roll (97 rooms > 30
           deg) that the depth masks show is NOT the real framing
           (verified on rain/t_square). */
        float best_o = -1e9f, bf = f_leg, bcx = Wf * 0.5f, bcy = Hf * 0.5f, broll = 0.0f;
        for (int ri = -18; ri <= 18; ri++) {
            float roll = ri * 10.0f;
            RoomCamera rc = base; camera_apply_roll(&rc, roll);
            for (int fi = 0; fi < 9; fi++) {
                float f = f_leg * 0.6f * powf(1.6f / 0.6f, fi / 8.0f);
                for (int yi = -3; yi <= 3; yi++) {
                    float cy = Hf * 0.5f + yi * Hf * 0.1f;
                    float o = calib_objective(&rc, &m, f, Wf * 0.5f, cy, 8);
                    if (o > best_o) { best_o = o; bf = f; bcy = cy; broll = roll; }
                }
            }
        }
        /* coordinate refinement, shrinking steps, at 1/4 resolution */
        {
            RoomCamera rc = base; camera_apply_roll(&rc, broll);
            best_o = calib_objective(&rc, &m, bf, bcx, bcy, 4);
        }
        float sf = 0.12f, sp = 0.06f, sr = 6.0f;
        for (int it = 0; it < 6; it++) {
            int improved = 1;
            while (improved) {
                improved = 0;
                float cand[8][4] = { {bf * (1 + sf), bcx, bcy, broll}, {bf / (1 + sf), bcx, bcy, broll},
                                     {bf, bcx + sp * Wf, bcy, broll}, {bf, bcx - sp * Wf, bcy, broll},
                                     {bf, bcx, bcy + sp * Hf, broll}, {bf, bcx, bcy - sp * Hf, broll},
                                     {bf, bcx, bcy, broll + sr}, {bf, bcx, bcy, broll - sr} };
                for (int k = 0; k < 8; k++) {
                    RoomCamera rc = base; camera_apply_roll(&rc, cand[k][3]);
                    float o = calib_objective(&rc, &m, cand[k][0], cand[k][1], cand[k][2], 4);
                    if (o > best_o + 1e-4f) { best_o = o; bf = cand[k][0]; bcx = cand[k][1]; bcy = cand[k][2]; broll = cand[k][3]; improved = 1; }
                }
            }
            sf *= 0.5f; sp *= 0.5f; sr *= 0.5f;
        }
        float cov;
        RoomCamera fitted = base; camera_apply_roll(&fitted, broll);
        float fit = calib_score(&fitted, &m, bf, bcx, bcy, 2, &cov);
        /* Keep the fit only when it's a real, clear improvement over the
           default rule and a credible match in absolute terms --
           otherwise (mesh too approximate for this room, mask unusable)
           the default rule stays. */
        /* Per-room fits are only allowed to REFINE the common rule (focal
           +-10%, principal point +-5% of the image, roll +-5 deg). Bigger
           "corrections" mean something else is wrong with that room and
           must not be papered over with individual camera parameters. */
        int small = fabsf(bf / f_leg - 1.0f) <= 0.10f && fabsf(bcx - Wf * 0.5f) <= 0.05f * Wf &&
                    fabsf(bcy - Hf * 0.5f) <= 0.05f * Hf && fabsf(broll) <= 5.0f;
        int keep = small && fit > leg + 0.05f && fit > 0.25f && cov > 0.6f;
        if (keep) {
            ensure_parent_dir(out_path);
            FILE *o = fopen(out_path, "w");
            if (o) { fprintf(o, "focal %f\ncx %f\ncy %f\nroll %f\nrho %f\nrule_rho %f\n", bf, bcx, bcy, broll, fit, leg); fclose(o); }
            n_cal++;
        } else n_kept_legacy++;
        if (log) fprintf(log, "%-24s %4dx%-4d rule_rho=%6.3f fit_rho=%6.3f cov=%.2f f=%7.1f (rule %7.1f, x%.2f) cx=%6.1f cy=%6.1f roll=%6.1f %s\n",
                         e->label, (int)g_hdr.width, (int)g_hdr.height, leg, fit, cov, bf, f_leg, bf / f_leg, bcx, bcy, broll,
                         keep ? "FITTED" : "rule kept");
        if (log) fflush(log);
        free(m.code);
    }
    if (log) {
        fprintf(log, "\n--- summary ---\nfitted: %d\nrule kept: %d\nskipped: %d\n", n_cal, n_kept_legacy, n_skip);
        fclose(log);
    }
}

/* =====================================================================
   SOUND CATALOG: every sound of the game, by kind, for the choosers
     - the MUSIC: the soundtrack's tracks (assets/sound, named below);
     - the LINES: what the characters say, in the room folders
       (assets/levels/<level>/<room>/<name>.ogg) -- the room's own ones, and
       the other rooms';
     - the SOUND EFFECTS: the rest of assets/sound.
   A file name is relative to assets/sound, or "levels/<level>/<room>/x.ogg".
   ===================================================================== */
static const char *SOUND_MUSIC[] = { "apocpt1", "apocpt2", "boatgon", "chains", "credits", "deadin1", "deadpt1", "dfight", "dsting",
    "dvdsadp1", "fugeapt1", "fugeapt2", "fugebpt1", "fugebpt2", "gnbatpt1", "gnbatpt2", "gno", "gnocvp1", "gnocvp2", "grandad",
    "gudevlp1", "gudevlp2", "havnbpt1", "havnbpt2", "icetomb", "ievlbat1", "ievlbat2", "interpt1", "interpt2", "metalon", "mushpt1",
    "rainpt1", "rainpt2", "sewerpt1", "sting2", "sting3", "verdante", "wpalpt1", "wpalpt2" };
enum { SND_ROOM_LINES, SND_MUSIC, SND_SFX, SND_OTHER_LINES, SND_KINDS };
static const char *SND_KIND_NAME[SND_KINDS] = { "LINES OF THIS ROOM", "MUSIC", "SOUND EFFECTS", "LINES OF THE OTHER ROOMS" };
typedef struct { char name[96]; int kind; } SoundFile; /* kind: SND_MUSIC, SND_SFX, or a line (SND_OTHER_LINES) */
static SoundFile *g_snd = NULL;
static int g_snd_n = -1;
static int sound_file_cmp(const void *a, const void *b) { return strcmp(((const SoundFile *)a)->name, ((const SoundFile *)b)->name); }
static void sound_catalog_scan(void) {
    if (g_snd_n >= 0) return;
    g_snd_n = 0;
    int cap = 0;
    char pat[1024], nm[200];
    WIN32_FIND_DATAA fd;
    #define SND_ADD(n_, k_) do { if (g_snd_n == cap) { cap = cap ? cap * 2 : 2048; g_snd = (SoundFile *)realloc(g_snd, sizeof(SoundFile) * cap); } \
        snprintf(g_snd[g_snd_n].name, sizeof(g_snd[0].name), "%s", n_); \
        for (char *q_ = g_snd[g_snd_n].name; *q_; q_++) if (*q_ >= 'A' && *q_ <= 'Z') *q_ += 32; \
        g_snd[g_snd_n++].kind = k_; } while (0)
    root_path(pat, sizeof(pat), "assets/sound/*.ogg");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int music = 0;
            for (size_t i = 0; i < sizeof(SOUND_MUSIC) / sizeof(SOUND_MUSIC[0]); i++)
                if (!_strnicmp(fd.cFileName, SOUND_MUSIC[i], strlen(SOUND_MUSIC[i])) && !_stricmp(fd.cFileName + strlen(SOUND_MUSIC[i]), ".ogg")) music = 1;
            SND_ADD(fd.cFileName, music ? SND_MUSIC : SND_SFX);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* the lines: every folder of every level (not only the rooms that can be played: global/global, palace/happy... have lines too) */
    WIN32_FIND_DATAA lv, rm;
    root_path(pat, sizeof(pat), "assets/levels/*");
    HANDLE hl = FindFirstFileA(pat, &lv);
    if (hl != INVALID_HANDLE_VALUE) {
        do {
            if (!(lv.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || lv.cFileName[0] == '.') continue;
            root_path(pat, sizeof(pat), "assets/levels/%s/*", lv.cFileName);
            HANDLE hr = FindFirstFileA(pat, &rm);
            if (hr == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(rm.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || rm.cFileName[0] == '.') continue;
                root_path(pat, sizeof(pat), "assets/levels/%s/%s/*.ogg", lv.cFileName, rm.cFileName);
                h = FindFirstFileA(pat, &fd);
                if (h == INVALID_HANDLE_VALUE) continue;
                do { snprintf(nm, sizeof(nm), "levels/%s/%s/%s", lv.cFileName, rm.cFileName, fd.cFileName); SND_ADD(nm, SND_OTHER_LINES); } while (FindNextFileA(h, &fd));
                FindClose(h);
            } while (FindNextFileA(hr, &rm));
            FindClose(hr);
        } while (FindNextFileA(hl, &lv));
        FindClose(hl);
    }
    #undef SND_ADD
    qsort(g_snd, g_snd_n, sizeof(SoundFile), sound_file_cmp);
}
/* The kind of a file for the room `room_label` (its own lines apart). */
static int sound_kind(const SoundFile *f, const char *room_label) {
    if (f->kind != SND_OTHER_LINES) return f->kind;
    char pre[160];
    snprintf(pre, sizeof(pre), "levels/%s/", room_label);
    return !strncmp(f->name, pre, strlen(pre)) ? SND_ROOM_LINES : SND_OTHER_LINES;
}
/* Rows for a chooser: each kind of `order` in turn, a header (name = the
   kind's title, header = 1) then its files. Returns how many. */
typedef struct { char name[96]; int header; } SoundRow;
static int sound_rows(const int *order, int norder, const char *room_label, SoundRow **out) {
    sound_catalog_scan();
    *out = (SoundRow *)malloc(sizeof(SoundRow) * (g_snd_n + SND_KINDS + 1));
    int n = 0;
    for (int o = 0; o < norder; o++) {
        int count = 0;
        for (int i = 0; i < g_snd_n; i++) count += sound_kind(&g_snd[i], room_label) == order[o];
        if (!count) continue;
        snprintf((*out)[n].name, sizeof((*out)[n].name), "%s  (%d)", SND_KIND_NAME[order[o]], count);
        (*out)[n++].header = 1;
        for (int i = 0; i < g_snd_n; i++) if (sound_kind(&g_snd[i], room_label) == order[o]) {
            snprintf((*out)[n].name, sizeof((*out)[n].name), "%s", g_snd[i].name);
            (*out)[n++].header = 0;
        }
    }
    return n;
}
/* a sound as shown in a list: a line without its "levels/" */
static const char *sound_shown(const char *name) { return !strncmp(name, "levels/", 7) ? name + 7 : name; }

/* =====================================================================
   MOVESETS: the animations a character walks with
   ---------------------------------------------------------------------
   13 walking slots (MC_*): stand / walk / run cycles and the start / turn
   clips played when it sets off or changes direction (see move_clip_for),
   and 8 combat slots (attack mode, see ATTACK MODE) with their own preset,
   the 1-HANDED SWORD one. A moveset also says what the character holds in
   its right hand (an item of assets/chars/items; David: daveswrd).
   Each character model (folder of assets/chars) has one moveset:
   the HUMAN preset -- David's own clips -- or none, plus per-slot
   overrides (another clip, or "no animation") that never change the
   preset itself. Saved in data/movesets/<model>.cfg (only when it
   differs from "human preset, no override"):
     preset human|none
     combat sword1h|none
     right_hand <item>|-
     <slot key> <source>/<clip name>   (or  -  = no animation)
     sounds <slot key> <file> [<file>...]   (a combat slot's sounds)
   A clip only plays on a skeleton with the same bone count.
   ===================================================================== */
static const char *MOVE_SLOT_KEY[MC_COUNT] = {
    "run_turn_left", "run_turn_right", "run_back_left", "run_back_right",
    "walk_turn_left", "walk_turn_right", "start_walk", "start_run", "walk_back_left", "walk_back_right",
    "stand", "walk", "run",
    "hit", "dodge", "death", "down", "down_loop", "get_up",
    "attack_1", "attack_2", "attack_3", "swing_up", "swing_left", "swing_right", "swing_back_left", "swing_back_right" };
static const char *MOVE_SLOT_LABEL[MC_COUNT] = {
    "Run: turn left", "Run: turn right", "Run: back via left", "Run: back via right",
    "Walk: turn left", "Walk: turn right", "Start walking", "Start running", "Walk: back via left", "Walk: back via right",
    "Stand", "Walk", "Run",
    "Hit", "Dodge", "Death", "Knocked down", "Lying down", "Getting up",
    "Attack 1", "Attack 2", "Attack 3", "Swing up", "Swing left", "Swing right", "Swing down, via left", "Swing down, via right" };
static const char *MOVE_SLOT_DESC[MC_COUNT] = {
    "Running, the new direction is 45-135 degrees to its left: played while it turns, then the run cycle.",
    "Running, the new direction is 45-135 degrees to its right.",
    "Running, the new direction is behind it: turns around via its left.",
    "Running, the new direction is behind it: turns around via its right.",
    "Walking (or standing), the new direction is 45-135 degrees to its left.",
    "Walking (or standing), the new direction is 45-135 degrees to its right.",
    "Standing still, it sets off walking straight ahead.",
    "Standing still, it sets off running straight ahead.",
    "Walking, the new direction is behind it: turns around via its left.",
    "Walking, the new direction is behind it: turns around via its right.",
    "Standing still (loops).", "The walk cycle (loops).", "The run cycle (loops).",
    "A blow lands on it (it isn't dead yet): played over what it was doing.",
    "It avoids a blow (its AI's dodge chance; David: attack mode, right click).",
    "An ENEMY's health is gone: it dies (held on the last frame).",
    "An ALLY's health is gone: it's knocked down, until the enemies of the scene are beaten.",
    "Then, as long as it's down on the field (loops).",
    "The ally knocked down gets up again (its health back)." ,
    "Attack mode, a simple click: one of the three attacks, at random.", "Attack mode, a simple click: one of the three attacks, at random.",
    "Attack mode, a simple click: one of the three attacks, at random.",
    "Attack mode, button held and the mouse swung UP: a thrust ahead.",
    "Attack mode, button held and the mouse swung LEFT: a blow to its left.",
    "Attack mode, button held and the mouse swung RIGHT: a blow to its right.",
    "Attack mode, button held and the mouse swung DOWN (and a bit left): a blow turning around via its left.",
    "Attack mode, button held and the mouse swung DOWN (and a bit right): a blow turning around via its right." };
/* ---- PRESETS: named, reusable parts of a character's setup ----
     WALKING  the walking slots' clips               (built in: Human)
     COMBAT   the combat slots' clips and steps       (built in: Single Swords, Double Swords)
     SOUNDS   sound pools: weapon swings, weapon hits, grunts when hit
              (+ the chance a grunt plays)             (built in: David)
     MODEL    all of it at once: a walking, a combat and a sound preset,
              the equipped weapon and shield
   Saved from the moveset screen in data/presets/<name>.cfg:
     name <preset name>
     group walking|combat|sounds|model
     <slot key> <source>/<clip name>          (walking, combat)
     step <slot key> <hitbox diameters>       (combat: the step taken with the blow; < 0 = backward)
     swing|hit|grunt <sound file>             (sounds)   grunt_chance <percent>
     walk|combat|sound <preset name>          (model; combat "auto" = from the weapon)
     weapon|shield <item>|-                   (model)
   A character's combat preset can follow the weapon it holds: DOUBLE
   SWORDS for dualswrd, SINGLE SWORDS for any other. */
enum { PG_WALK, PG_COMBAT, PG_SOUND, PG_REACT, PG_AI, PG_STATS, PG_MODEL, PG_COUNT };
static const char *PG_NAME[PG_COUNT] = { "walking", "combat", "sounds", "reactions", "ai", "stats", "model" };
/* AI: what a character does on its own. MELEE: gets close enough to the
   nearest opponent to strike, strikes, pauses, strikes again; may dodge a blow. */
enum { AI_NONE, AI_MELEE };
typedef struct {
    int kind;                    /* AI_* */
    float range;                 /* it strikes from this far (hitbox diameters) */
    int run;                     /* it runs to its target (else walks) */
    float pause_min, pause_max;  /* between two attacks, seconds */
    int dodge;                   /* % of the blows aimed at it that it dodges */
} AiSettings;
/* STATS: its health, the damage of its blows, a boss (its health bar on the screen) */
typedef struct { int hp, damage, boss; } StatSettings;
static const StatSettings STATS_DEFAULT = { 100, 10, 0 };
enum { POOL_SWING, POOL_HIT, POOL_GRUNT, POOL_COUNT };
#define POOL_MAX 12
typedef struct {
    int n[POOL_COUNT];
    char file[POOL_COUNT][POOL_MAX][96];
    int grunt_chance;            /* % of the hits taken that make it grunt */
} SoundPools;
#define MOVE_PRESET_MAX 64
typedef struct {
    char name[48];
    int group;                   /* PG_* */
    int builtin;
    char clip[MC_COUNT][100];    /* walking / combat: per slot of its group, "source/name" or a clip name, "" = none */
    float step[MC_COUNT];        /* combat: hitbox diameters, forward (< 0 backward) */
    SoundPools snd;              /* sounds */
    CueList cues[COMBAT_SLOTS];  /* combat: each blow's sounds (not set: a swing of the pool) */
    AiSettings ai;               /* ai */
    StatSettings st;             /* stats */
    char walk[48], combat[48], sound[48], weapon[48], shield[48]; /* model ("combat" "" = from the weapon) */
    char react[48], aip[48], stats[48];                            /* model */
} MovePreset;
static MovePreset g_presets[MOVE_PRESET_MAX];
static int g_preset_n = -1;
static int slot_group(int slot) { return slot >= MC_FIRST_COMBAT ? PG_COMBAT : slot >= MC_FIRST_REACT ? PG_REACT : PG_WALK; }
static int group_has_slots(int g) { return g == PG_WALK || g == PG_REACT || g == PG_COMBAT; }
/* MAGIC WEAPONS: an orb is held as the weapon "orb:<element>" (no model: its sprite glows in the hand);
   the wands and staves are items held like weapons that cast their element */
static int weapon_is_orb(const char *w) { return w && !strncmp(w, "orb:", 4); }
static int weapon_is_wand(const char *w) {
    static const char *wands[] = { "icewand", "firewand", "lstaff", "wandbolt", "silvstaf", "moonstik", "cagstaff" };
    for (size_t i = 0; w && i < sizeof(wands) / sizeof(wands[0]); i++) if (!strcmp(w, wands[i])) return 1;
    return 0;
}
static const char *weapon_preset(const char *weapon) {
    if (weapon_is_orb(weapon)) return "Orb Magic";
    if (weapon_is_wand(weapon)) return "Wand Magic";
    return (weapon && !strcmp(weapon, "dualswrd")) ? "Double Swords" : "Single Swords";
}
static void preset_file(char *out, size_t n, const char *name) {
    char safe[64]; int k = 0;
    for (const char *c = name; *c && k < 60; c++) safe[k++] = ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-') ? *c : '_';
    safe[k] = 0;
    root_path(out, n, "data/presets/%s.cfg", safe);
}
static void pool_add(SoundPools *sp, int pool, const char *file) {
    if (sp->n[pool] < POOL_MAX) snprintf(sp->file[pool][sp->n[pool]++], 96, "%s", file);
}
static const char *pool_pick(const SoundPools *sp, int pool);
/* a blow's sounds: one step added / read from "sound <file>", "swing", "wait <seconds>" / written */
static void cue_add(CueList *l, int type, float sec, const char *file) {
    l->set = 1;
    if (l->n >= CUE_MAX) return;
    SoundCue *c = &l->c[l->n++];
    memset(c, 0, sizeof(*c));
    c->type = type; c->sec = sec;
    if (file) snprintf(c->file, sizeof(c->file), "%s", file);
}
static void cue_parse(CueList *l, const char *kind, const char *arg) {
    if (!strcmp(kind, "sound") && arg[0]) cue_add(l, CUE_SOUND, 0.0f, arg);
    else if (!strcmp(kind, "swing")) cue_add(l, CUE_SWING, 0.0f, NULL);
    else if (!strcmp(kind, "wait")) cue_add(l, CUE_WAIT, (float)atof(arg), NULL);
}
static void cue_write(FILE *f, const char *prefix, const char *slot, const CueList *l) {
    fprintf(f, "%scues %s\n", prefix, slot);
    for (int i = 0; i < l->n; i++) {
        const SoundCue *c = &l->c[i];
        if (c->type == CUE_SOUND) fprintf(f, "%scue %s sound %s\n", prefix, slot, c->file);
        else if (c->type == CUE_SWING) fprintf(f, "%scue %s swing\n", prefix, slot);
        else fprintf(f, "%scue %s wait %.3f\n", prefix, slot, c->sec);
    }
}
/* "cues <slot>" / "cue <slot> <kind> [arg]" (prefix: "own_" in a moveset): 1 if the line was one */
static int cue_line(const char *line, const char *prefix, CueList *lists, int *has) {
    char key[64], slot[64], kind[16], arg[128] = "";
    size_t pl = strlen(prefix);
    if (strncmp(line, prefix, pl)) return 0;
    int n = sscanf(line + pl, "%63s %63s %15s %127s", key, slot, kind, arg);
    if (n < 2 || (strcmp(key, "cues") && strcmp(key, "cue"))) return 0;
    for (int k = MC_FIRST_COMBAT; k < MC_COUNT; k++) if (!strcmp(slot, MOVE_SLOT_KEY[k])) {
        CueList *l = &lists[k - MC_FIRST_COMBAT];
        if (has) has[k - MC_FIRST_COMBAT] = 1;
        l->set = 1;
        if (!strcmp(key, "cue") && n >= 3) cue_parse(l, kind, n >= 4 ? arg : "");
    }
    return 1;
}
/* the time (s) a step of a list starts at */
static float cue_time(const CueList *l, int upto) {
    float t = 0.0f;
    for (int i = 0; i < upto && i < l->n; i++) if (l->c[i].type == CUE_WAIT) t += l->c[i].sec;
    return t;
}
/* Playing a list: start (NULL: the default, a swing of the pool), then
   each step of the game (dt in seconds of the animation). */
static void cue_start(CuePlayer *p, const CueList *l) {
    memset(p, 0, sizeof(*p));
    if (l) p->list = *l; else cue_add(&p->list, CUE_SWING, 0.0f, NULL);
    p->on = 1;
}
static void cue_run(CuePlayer *p, float dt, const SoundPools *sp) {
    if (!p->on) return;
    p->wait -= dt;
    while (p->on && p->wait <= 0.0f) {
        if (p->i >= p->list.n) { p->on = 0; break; }
        const SoundCue *c = &p->list.c[p->i++];
        if (c->type == CUE_WAIT) { p->wait += c->sec; continue; }
        const char *f = c->type == CUE_SWING ? pool_pick(sp, POOL_SWING) : c->file;
        if (f && f[0]) audio_play(f, AUDIO_SOUND, 0, 0);
    }
}
static void presets_load(void) {
    if (g_preset_n >= 0) return;
    g_preset_n = 0;
    static const char *human[MC_FIRST_REACT] = { "run90a", "run90c", "run180a", "run180c", "towalka", "towalkc", "towalk", "torun", "to180a", "to180c",
                                                 "stand", "walk", "run" };
    /* attack 1-3, swing up, left, right, down via left, down via right. lslice2 (the double swords' right
       swing) isn't in the blockouts: hedchop2, their other side blow, stands in for it */
    static const char *single[8] = { "rchop", "rchopp", "rchoppp", "lstab", "headchop", "revslice", "rchp180a", "rchp180c" };
    static const char *twin[8] = { "rchoptwo", "rchoptwo", "rchoptwo", "stabtwo", "rslice2", "hedchop2", "rlc2180a", "rlc2180c" };
    static const float steps[8] = { 1.0f, 1.0f, 1.0f, 4.0f, 1.25f, 1.5f, -1.5f, -1.5f };
    MovePreset *p = &g_presets[g_preset_n++];
    memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), "Human"); p->builtin = 1; p->group = PG_WALK;
    for (int k = 0; k < MC_FIRST_REACT; k++) snprintf(p->clip[k], sizeof(p->clip[k]), "%s", human[k]);
    for (int f = 0; f < 2; f++) { /* reactions: a human's; fuge dies his own way */
        static const char *react[6] = { "hitface", "dodgeb", "dieback", "unc_fall", "unc_pant", "unc_up" };
        p = &g_presets[g_preset_n++];
        memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), f ? "Fuge" : "Human"); p->group = PG_REACT; p->builtin = 1;
        for (int k = 0; k < 6; k++) snprintf(p->clip[MC_FIRST_REACT + k], 100, "%s", react[k]);
        if (f) snprintf(p->clip[MC_DEATH], 100, "fuge/fugedie");
    }
    p = &g_presets[g_preset_n++]; /* fuge and his two blades: closes in, strikes, sometimes dodges */
    memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), "Fuge - dual blades"); p->group = PG_AI; p->builtin = 1;
    p->ai.kind = AI_MELEE; p->ai.range = 1.6f; p->ai.run = 1; p->ai.pause_min = 0.6f; p->ai.pause_max = 1.4f; p->ai.dodge = 35;
    p = &g_presets[g_preset_n++]; /* a caster (an orb or a wand): casts from afar */
    memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), "Caster - keeps its distance"); p->group = PG_AI; p->builtin = 1;
    p->ai.kind = AI_MELEE; p->ai.range = 7.0f; p->ai.run = 0; p->ai.pause_min = 1.5f; p->ai.pause_max = 3.0f; p->ai.dodge = 15;
    p = &g_presets[g_preset_n++];
    memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), "Human"); p->group = PG_STATS; p->builtin = 1;
    p->st = STATS_DEFAULT;
    for (int w = 0; w < 2; w++) {
        p = &g_presets[g_preset_n++];
        memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), w ? "Double Swords" : "Single Swords"); p->group = PG_COMBAT; p->builtin = 1;
        for (int k = 0; k < 8; k++) { snprintf(p->clip[MC_FIRST_COMBAT + k], 100, "%s", w ? twin[k] : single[k]); p->step[MC_FIRST_COMBAT + k] = steps[k]; }
    }
    for (int w = 0; w < 2; w++) { /* magic: an orb in both hands (magkup / magkbob / magkaim...), a wand in the right hand (fcast...) */
        static const char *orbc[8] = { "magkaim", "magkaim", "mgkair", "mgkair", "mgk90a", "mgk90c", "mgk180a", "mgk180c" };
        static const char *wandc[8] = { "fcast", "conjure", "fcast", "fcast", "cast90a", "cast90c", "cast180a", "cast180c" };
        p = &g_presets[g_preset_n++];
        memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), w ? "Wand Magic" : "Orb Magic"); p->group = PG_COMBAT; p->builtin = 1;
        for (int k = 0; k < 8; k++) { snprintf(p->clip[MC_FIRST_COMBAT + k], 100, "%s", w ? wandc[k] : orbc[k]); p->cues[k].set = 1; } /* no swing whoosh: the spell has its sounds */
    }
    p = &g_presets[g_preset_n++]; /* David's sounds: his sword's swings and hits, his grunts */
    memset(p, 0, sizeof(*p)); snprintf(p->name, sizeof(p->name), "David"); p->group = PG_SOUND; p->builtin = 1;
    pool_add(&p->snd, POOL_SWING, "swrdatt1.ogg"); pool_add(&p->snd, POOL_SWING, "swrdatt2.ogg"); pool_add(&p->snd, POOL_SWING, "swrdatt3.ogg");
    pool_add(&p->snd, POOL_HIT, "swrdhit.ogg"); pool_add(&p->snd, POOL_HIT, "swrdhit1.ogg"); pool_add(&p->snd, POOL_HIT, "swrdhit2.ogg"); pool_add(&p->snd, POOL_HIT, "swrdhit3.ogg");
    for (int i = 1; i <= 5; i++) { char f[32]; snprintf(f, sizeof(f), "davehit%d.ogg", i); pool_add(&p->snd, POOL_GRUNT, f); }
    pool_add(&p->snd, POOL_GRUNT, "dvdgrnt1.ogg");
    for (int i = 1; i <= 3; i++) { char f[32]; snprintf(f, sizeof(f), "dvduh%d.ogg", i); pool_add(&p->snd, POOL_GRUNT, f); }
    p->snd.grunt_chance = 60;
    char pat[1024]; root_path(pat, sizeof(pat), "data/presets/*.cfg");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (g_preset_n >= MOVE_PRESET_MAX) break;
        char path[1024], line[256], key[64], val[128];
        root_path(path, sizeof(path), "data/presets/%s", fd.cFileName);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        p = &g_presets[g_preset_n];
        memset(p, 0, sizeof(*p));
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            const char *rest = strchr(line, ' ') ? strchr(line, ' ') + 1 : "";
            if (!strncmp(line, "name ", 5)) { snprintf(p->name, sizeof(p->name), "%s", rest); continue; }
            if (cue_line(line, "", p->cues, NULL)) continue;
            if (sscanf(line, "%63s %127s", key, val) != 2 || key[0] == '#') continue;
            if (!strcmp(key, "group")) { for (int g = 0; g < PG_COUNT; g++) if (!strcmp(val, PG_NAME[g])) p->group = g; }
            else if (!strcmp(key, "step")) { float v; if (sscanf(line + 5 + strlen(val), "%f", &v) == 1) for (int k = 0; k < MC_COUNT; k++) if (!strcmp(val, MOVE_SLOT_KEY[k])) p->step[k] = v; }
            else if (!strcmp(key, "swing")) pool_add(&p->snd, POOL_SWING, val);
            else if (!strcmp(key, "hit")) pool_add(&p->snd, POOL_HIT, val);
            else if (!strcmp(key, "grunt")) pool_add(&p->snd, POOL_GRUNT, val);
            else if (!strcmp(key, "grunt_chance")) p->snd.grunt_chance = atoi(val);
            else if (!strcmp(key, "ai_kind")) p->ai.kind = !strcmp(val, "melee") ? AI_MELEE : AI_NONE;
            else if (!strcmp(key, "ai_range")) p->ai.range = (float)atof(val);
            else if (!strcmp(key, "ai_run")) p->ai.run = atoi(val);
            else if (!strcmp(key, "ai_pause")) sscanf(rest, "%f %f", &p->ai.pause_min, &p->ai.pause_max);
            else if (!strcmp(key, "ai_dodge")) p->ai.dodge = atoi(val);
            else if (!strcmp(key, "hp")) p->st.hp = atoi(val);
            else if (!strcmp(key, "damage")) p->st.damage = atoi(val);
            else if (!strcmp(key, "boss")) p->st.boss = atoi(val);
            else if (!strcmp(key, "reactions")) snprintf(p->react, sizeof(p->react), "%s", rest);
            else if (!strcmp(key, "ai")) snprintf(p->aip, sizeof(p->aip), "%s", rest);
            else if (!strcmp(key, "stats")) snprintf(p->stats, sizeof(p->stats), "%s", rest);
            else if (!strcmp(key, "walk")) snprintf(p->walk, sizeof(p->walk), "%s", rest);
            else if (!strcmp(key, "combat")) snprintf(p->combat, sizeof(p->combat), "%s", strcmp(rest, "auto") ? rest : "");
            else if (!strcmp(key, "sound")) snprintf(p->sound, sizeof(p->sound), "%s", rest);
            else if (!strcmp(key, "weapon")) snprintf(p->weapon, sizeof(p->weapon), "%s", strcmp(val, "-") ? val : "");
            else if (!strcmp(key, "shield")) snprintf(p->shield, sizeof(p->shield), "%s", strcmp(val, "-") ? val : "");
            else for (int k = 0; k < MC_COUNT; k++) if (!strcmp(key, MOVE_SLOT_KEY[k])) snprintf(p->clip[k], sizeof(p->clip[k]), "%s", strcmp(val, "-") ? val : "");
        }
        fclose(f);
        if (p->name[0]) g_preset_n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
static MovePreset *preset_find(const char *name, int group) {
    presets_load();
    for (int i = 0; i < g_preset_n; i++) if (g_presets[i].group == group && !_stricmp(g_presets[i].name, name)) return &g_presets[i];
    return NULL;
}
static void preset_write(const MovePreset *p) {
    char path[1024];
    preset_file(path, sizeof(path), p->name);
    ensure_parent_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Silver Remaster: a %s preset (moveset screen)\nname %s\ngroup %s\n", PG_NAME[p->group], p->name, PG_NAME[p->group]);
    if (p->group == PG_AI)
        fprintf(f, "ai_kind %s\nai_range %.2f\nai_run %d\nai_pause %.2f %.2f\nai_dodge %d\n", p->ai.kind == AI_MELEE ? "melee" : "none",
                p->ai.range, p->ai.run, p->ai.pause_min, p->ai.pause_max, p->ai.dodge);
    if (p->group == PG_STATS) fprintf(f, "hp %d\ndamage %d\nboss %d\n", p->st.hp, p->st.damage, p->st.boss);
    if (group_has_slots(p->group))
        for (int k = 0; k < MC_COUNT; k++) {
            if (slot_group(k) != p->group) continue;
            fprintf(f, "%s %s\n", MOVE_SLOT_KEY[k], p->clip[k][0] ? p->clip[k] : "-");
            if (p->group == PG_COMBAT) fprintf(f, "step %s %.2f\n", MOVE_SLOT_KEY[k], p->step[k]);
            if (p->group == PG_COMBAT && p->cues[k - MC_FIRST_COMBAT].set) cue_write(f, "", MOVE_SLOT_KEY[k], &p->cues[k - MC_FIRST_COMBAT]);
        }
    if (p->group == PG_SOUND) {
        static const char *pk[POOL_COUNT] = { "swing", "hit", "grunt" };
        for (int q = 0; q < POOL_COUNT; q++) for (int i = 0; i < p->snd.n[q]; i++) fprintf(f, "%s %s\n", pk[q], p->snd.file[q][i]);
        fprintf(f, "grunt_chance %d\n", p->snd.grunt_chance);
    }
    if (p->group == PG_MODEL)
        fprintf(f, "walk %s\ncombat %s\nsound %s\nreactions %s\nai %s\nstats %s\nweapon %s\nshield %s\n", p->walk[0] ? p->walk : "-", p->combat[0] ? p->combat : "auto",
                p->sound[0] ? p->sound : "-", p->react[0] ? p->react : "-", p->aip[0] ? p->aip : "-", p->stats[0] ? p->stats : "-",
                p->weapon[0] ? p->weapon : "-", p->shield[0] ? p->shield : "-");
    fclose(f);
}

typedef struct {
    char model[48];
    char model_preset[48];       /* the model preset it was set from ("" = none) -- shown only */
    char walk_preset[48];        /* "Human" by default; "-" = none */
    char combat_preset[48];      /* "" = the weapon's (Single / Double Swords); "-" = none; else a preset */
    char sound_preset[48];       /* "David" for David; "-" = none */
    char react_preset[48];       /* "Human" ("Fuge" for fuge) */
    char ai_preset[48];          /* "-" = none ("Fuge - dual blades" for fuge) */
    char stats_preset[48];       /* "Human" */
    int own_ai, own_stats;       /* its own AI / stats (changed from its presets') */
    AiSettings ai;
    StatSettings st;
    char right_hand[48];         /* equipped weapon to begin with, "" = nothing */
    char left_hand[48];          /* equipped shield to begin with, "" = nothing */
    char slot[MC_COUNT][100];    /* "" = the preset's, "-" = no animation, else "source/name" */
    int has_step[MC_COUNT];      /* combat: its own step instead of the preset's */
    float step[MC_COUNT];
    int has_cues[COMBAT_SLOTS];  /* combat: its own sounds for a blow instead of the preset's */
    CueList cues[COMBAT_SLOTS];
    char combat_ovr_for[48];     /* the combat preset its combat changes were made on (they only apply to it) */
    int own_sounds;              /* its own sound pools (changed from its sound preset's) */
    SoundPools snd;
} Moveset;
static const char *default_right_hand(const char *model) {
    if (!strcmp(model, "david")) return "daveswrd";
    if (!strcmp(model, "fuge")) return "dualswrd"; /* his two blades, one in each hand */
    return "";
}
static const char *default_sound_preset(const char *model) { return !strcmp(model, "david") ? "David" : "-"; }
/* a character's preset of a group when nothing else is said */
static const char *default_group_preset(const char *model, int group, const char *weapon) {
    switch (group) {
        case PG_COMBAT: return weapon_preset(weapon);
        case PG_SOUND: return default_sound_preset(model);
        case PG_REACT: return !strcmp(model, "fuge") ? "Fuge" : "Human";
        case PG_AI: return !strcmp(model, "fuge") ? "Fuge - dual blades" : "-";
        default: return "Human";
    }
}
#define MAX_MOVESETS 64
static Moveset g_movesets[MAX_MOVESETS];
static int g_moveset_count = 0;
static int g_moveset_gen = 1;    /* bumped on every change: characters re-resolve their clips */

static void moveset_path(char *out, size_t n, const char *model) { root_path(out, n, "data/movesets/%s.cfg", model); }

static Moveset *moveset_get(const char *model) {
    for (int i = 0; i < g_moveset_count; i++) if (!strcmp(g_movesets[i].model, model)) return &g_movesets[i];
    if (g_moveset_count >= MAX_MOVESETS) return &g_movesets[0];
    Moveset *ms = &g_movesets[g_moveset_count++];
    memset(ms, 0, sizeof(*ms));
    snprintf(ms->model, sizeof(ms->model), "%s", model);
    snprintf(ms->walk_preset, sizeof(ms->walk_preset), "Human");
    snprintf(ms->sound_preset, sizeof(ms->sound_preset), "%s", default_sound_preset(model));
    snprintf(ms->react_preset, sizeof(ms->react_preset), "%s", default_group_preset(model, PG_REACT, ""));
    snprintf(ms->ai_preset, sizeof(ms->ai_preset), "%s", default_group_preset(model, PG_AI, ""));
    snprintf(ms->stats_preset, sizeof(ms->stats_preset), "%s", default_group_preset(model, PG_STATS, ""));
    snprintf(ms->right_hand, sizeof(ms->right_hand), "%s", default_right_hand(model));
    char path[1024], line[512], key[64], val[128];
    moveset_path(path, sizeof(path), model);
    FILE *f = fopen(path, "r");
    int legacy_combat = 0;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            const char *rest = strchr(line, ' ') ? strchr(line, ' ') + 1 : "";
            if (!strncmp(line, "walk_preset ", 12)) { snprintf(ms->walk_preset, sizeof(ms->walk_preset), "%s", rest); continue; }
            if (!strncmp(line, "combat_preset ", 14)) { snprintf(ms->combat_preset, sizeof(ms->combat_preset), "%s", strcmp(rest, "auto") ? rest : ""); continue; }
            if (!strncmp(line, "sound_preset ", 13)) { snprintf(ms->sound_preset, sizeof(ms->sound_preset), "%s", rest); continue; }
            if (!strncmp(line, "model_preset ", 13)) { snprintf(ms->model_preset, sizeof(ms->model_preset), "%s", rest); continue; }
            if (!strncmp(line, "react_preset ", 13)) { snprintf(ms->react_preset, sizeof(ms->react_preset), "%s", rest); continue; }
            if (!strncmp(line, "ai_preset ", 10)) { snprintf(ms->ai_preset, sizeof(ms->ai_preset), "%s", rest); continue; }
            if (!strncmp(line, "stats_preset ", 13)) { snprintf(ms->stats_preset, sizeof(ms->stats_preset), "%s", rest); continue; }
            if (!strncmp(line, "own_ai ", 7)) {
                char kind[16] = "";
                if (sscanf(rest, "%15s %f %d %f %f %d", kind, &ms->ai.range, &ms->ai.run, &ms->ai.pause_min, &ms->ai.pause_max, &ms->ai.dodge) >= 1) {
                    ms->ai.kind = !strcmp(kind, "melee") ? AI_MELEE : AI_NONE; ms->own_ai = 1;
                }
                continue;
            }
            if (!strncmp(line, "own_stats ", 10)) { if (sscanf(rest, "%d %d %d", &ms->st.hp, &ms->st.damage, &ms->st.boss) >= 1) ms->own_stats = 1; continue; }
            if (!strncmp(line, "combat_changes_for ", 19)) { snprintf(ms->combat_ovr_for, sizeof(ms->combat_ovr_for), "%s", rest); continue; }
            if (cue_line(line, "own_", ms->cues, ms->has_cues)) continue;
            if (sscanf(line, "%63s %127s", key, val) != 2 || key[0] == '#') continue;
            if (!strcmp(key, "preset")) { snprintf(ms->walk_preset, sizeof(ms->walk_preset), "%s", !strcmp(val, "human") ? "Human" : "-"); continue; } /* older files */
            if (!strcmp(key, "combat")) { snprintf(ms->combat_preset, sizeof(ms->combat_preset), "%s", !strcmp(val, "sword1h") ? "" : "-"); continue; }
            if (!strcmp(key, "right_hand")) { snprintf(ms->right_hand, sizeof(ms->right_hand), "%s", strcmp(val, "-") ? val : ""); continue; }
            if (!strcmp(key, "left_hand")) { snprintf(ms->left_hand, sizeof(ms->left_hand), "%s", strcmp(val, "-") ? val : ""); continue; }
            if (!strcmp(key, "step")) {
                float v;
                for (int k = 0; k < MC_COUNT; k++) if (!strcmp(val, MOVE_SLOT_KEY[k]) && sscanf(strstr(line, val) + strlen(val), "%f", &v) == 1) { ms->step[k] = v; ms->has_step[k] = 1; legacy_combat = 1; }
                continue;
            }
            if (!strcmp(key, "sounds")) continue; /* older per-attack sounds: replaced by the sound pools */
            if (!strcmp(key, "own_swing") || !strcmp(key, "own_hit") || !strcmp(key, "own_grunt")) {
                ms->own_sounds = 1;
                pool_add(&ms->snd, key[4] == 's' ? POOL_SWING : key[4] == 'h' ? POOL_HIT : POOL_GRUNT, val);
                continue;
            }
            if (!strcmp(key, "own_grunt_chance")) { ms->own_sounds = 1; ms->snd.grunt_chance = atoi(val); continue; }
            for (int k = 0; k < MC_COUNT; k++) if (!strcmp(key, MOVE_SLOT_KEY[k])) {
                snprintf(ms->slot[k], sizeof(ms->slot[k]), "%s", val);
                if (slot_group(k) == PG_COMBAT) legacy_combat = 1;
            }
        }
        fclose(f);
    }
    /* an older file's combat changes: made on the preset of its weapon */
    if (legacy_combat && !ms->combat_ovr_for[0])
        snprintf(ms->combat_ovr_for, sizeof(ms->combat_ovr_for), "%s", ms->combat_preset[0] && strcmp(ms->combat_preset, "-") ? ms->combat_preset : weapon_preset(ms->right_hand));
    return ms;
}

static void moveset_save(const Moveset *ms) {
    char path[1024];
    moveset_path(path, sizeof(path), ms->model);
    int custom = strcmp(ms->walk_preset, "Human") || ms->combat_preset[0] || strcmp(ms->sound_preset, default_sound_preset(ms->model)) ||
                 strcmp(ms->right_hand, default_right_hand(ms->model)) || ms->left_hand[0] || ms->own_sounds || ms->model_preset[0] ||
                 strcmp(ms->react_preset, default_group_preset(ms->model, PG_REACT, "")) || strcmp(ms->ai_preset, default_group_preset(ms->model, PG_AI, "")) ||
                 strcmp(ms->stats_preset, default_group_preset(ms->model, PG_STATS, "")) || ms->own_ai || ms->own_stats;
    for (int k = 0; k < MC_COUNT; k++) if (ms->slot[k][0] || ms->has_step[k]) custom = 1;
    for (int i = 0; i < COMBAT_SLOTS; i++) if (ms->has_cues[i]) custom = 1;
    g_moveset_gen++;
    if (!custom) { remove(path); return; }
    ensure_parent_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Silver Remaster: %s's moveset (walking, combat, sounds, equipment). Only what differs from its presets.\n", ms->model);
    if (ms->model_preset[0]) fprintf(f, "model_preset %s\n", ms->model_preset);
    fprintf(f, "walk_preset %s\n", ms->walk_preset[0] ? ms->walk_preset : "-");
    fprintf(f, "combat_preset %s\n", ms->combat_preset[0] ? ms->combat_preset : "auto");
    fprintf(f, "sound_preset %s\n", ms->sound_preset[0] ? ms->sound_preset : "-");
    fprintf(f, "react_preset %s\nai_preset %s\nstats_preset %s\n", ms->react_preset[0] ? ms->react_preset : "-", ms->ai_preset[0] ? ms->ai_preset : "-",
            ms->stats_preset[0] ? ms->stats_preset : "-");
    if (ms->own_ai) fprintf(f, "own_ai %s %.2f %d %.2f %.2f %d\n", ms->ai.kind == AI_MELEE ? "melee" : "none", ms->ai.range, ms->ai.run, ms->ai.pause_min, ms->ai.pause_max, ms->ai.dodge);
    if (ms->own_stats) fprintf(f, "own_stats %d %d %d\n", ms->st.hp, ms->st.damage, ms->st.boss);
    fprintf(f, "right_hand %s\n", ms->right_hand[0] ? ms->right_hand : "-");
    fprintf(f, "left_hand %s\n", ms->left_hand[0] ? ms->left_hand : "-");
    if (ms->combat_ovr_for[0]) fprintf(f, "combat_changes_for %s\n", ms->combat_ovr_for);
    for (int k = 0; k < MC_COUNT; k++) if (ms->slot[k][0]) fprintf(f, "%s %s\n", MOVE_SLOT_KEY[k], ms->slot[k]);
    for (int k = 0; k < MC_COUNT; k++) if (ms->has_step[k]) fprintf(f, "step %s %.2f\n", MOVE_SLOT_KEY[k], ms->step[k]);
    for (int k = MC_FIRST_COMBAT; k < MC_COUNT; k++) if (ms->has_cues[k - MC_FIRST_COMBAT]) cue_write(f, "own_", MOVE_SLOT_KEY[k], &ms->cues[k - MC_FIRST_COMBAT]);
    if (ms->own_sounds) {
        static const char *pk[POOL_COUNT] = { "own_swing", "own_hit", "own_grunt" };
        for (int q = 0; q < POOL_COUNT; q++) for (int i = 0; i < ms->snd.n[q]; i++) fprintf(f, "%s %s\n", pk[q], ms->snd.file[q][i]);
        fprintf(f, "own_grunt_chance %d\n", ms->snd.grunt_chance);
    }
    fclose(f);
}

/* "source/name" -> library clip */
static int anim_lib_find_ref(const char *ref) {
    const char *sl = strchr(ref, '/');
    if (!sl) return anim_lib_find(ref);
    for (int i = 0; i < anim_lib_count(); i++)
        if (!strcmp(anim_lib_name(i), sl + 1) && !strncmp(anim_lib_source(i), ref, (size_t)(sl - ref)) && anim_lib_source(i)[sl - ref] == 0) return i;
    return -1;
}

/* The preset a tab of a moveset uses (NULL: none), for a character holding
   `weapon`. A preset that no longer exists (deleted): the default one. */
/* the preset name a group of a moveset uses */
static char *ms_group_name(Moveset *ms, int group) {
    switch (group) {
        case PG_COMBAT: return ms->combat_preset;
        case PG_SOUND: return ms->sound_preset;
        case PG_REACT: return ms->react_preset;
        case PG_AI: return ms->ai_preset;
        case PG_STATS: return ms->stats_preset;
        default: return ms->walk_preset;
    }
}
static const MovePreset *moveset_preset_w(const Moveset *ms, int group, const char *weapon) {
    const char *n = ms_group_name((Moveset *)ms, group);
    if (!strcmp(n, "-")) return NULL;
    if (group == PG_COMBAT && !n[0]) n = weapon_preset(weapon);
    const MovePreset *p = preset_find(n, group);
    if (!p) { const char *d = default_group_preset(ms->model, group, weapon); if (strcmp(d, "-")) p = preset_find(d, group); }
    return p;
}
/* its AI (NULL: none) and its stats: its own, else its preset's */
static const AiSettings *moveset_ai(const Moveset *ms) {
    if (ms->own_ai) return &ms->ai;
    const MovePreset *p = moveset_preset_w(ms, PG_AI, ms->right_hand);
    return p ? &p->ai : NULL;
}
static const StatSettings *moveset_stats(const Moveset *ms) {
    if (ms->own_stats) return &ms->st;
    const MovePreset *p = moveset_preset_w(ms, PG_STATS, ms->right_hand);
    return p ? &p->st : &STATS_DEFAULT;
}
/* the name of the combat preset in use (for the changes made on it) */
static const char *moveset_combat_name(const Moveset *ms, const char *weapon) {
    const MovePreset *p = moveset_preset_w(ms, PG_COMBAT, weapon);
    return p ? p->name : "-";
}
/* a slot's own change applies: always for walking; for combat only on the preset it was made on */
static int moveset_change_on(const Moveset *ms, int slot, const char *weapon) {
    return slot_group(slot) != PG_COMBAT || !ms->combat_ovr_for[0] || !_stricmp(ms->combat_ovr_for, moveset_combat_name(ms, weapon));
}
/* The clip a slot plays (-1: none). *how: 0 preset, 1 custom, 2 no animation */
static int moveset_clip_w(const Moveset *ms, int slot, int *how, const char *weapon) {
    const char *v = ms->slot[slot];
    if (v[0] && moveset_change_on(ms, slot, weapon)) {
        if (!strcmp(v, "-")) { if (how) *how = 2; return -1; }
        if (how) *how = 1;
        return anim_lib_find_ref(v);
    }
    const MovePreset *p = moveset_preset_w(ms, slot_group(slot), weapon);
    if (!p || !p->clip[slot][0]) { if (how) *how = 2; return -1; }
    if (how) *how = 0;
    return anim_lib_find_ref(p->clip[slot]);
}
static int moveset_clip(const Moveset *ms, int slot, int *how) { return moveset_clip_w(ms, slot, how, ms->right_hand); }
/* the step (hitbox diameters) a combat slot takes */
static float moveset_step_w(const Moveset *ms, int slot, const char *weapon) {
    if (ms->has_step[slot] && moveset_change_on(ms, slot, weapon)) return ms->step[slot];
    const MovePreset *p = moveset_preset_w(ms, slot_group(slot), weapon);
    return p ? p->step[slot] : 0.0f;
}
/* a blow's sounds: its own, else its combat preset's (NULL: the default, a swing of the pool) */
static const CueList *moveset_cues_w(const Moveset *ms, int slot, const char *weapon) {
    if (slot < MC_FIRST_COMBAT) return NULL;
    int i = slot - MC_FIRST_COMBAT;
    if (ms->has_cues[i] && moveset_change_on(ms, slot, weapon)) return &ms->cues[i];
    const MovePreset *p = moveset_preset_w(ms, PG_COMBAT, weapon);
    return p && p->cues[i].set ? &p->cues[i] : NULL;
}
/* its sound pools: its own, else its sound preset's (NULL: none) */
static const SoundPools *moveset_sounds(const Moveset *ms) {
    if (ms->own_sounds) return &ms->snd;
    const MovePreset *p = moveset_preset_w(ms, PG_SOUND, ms->right_hand);
    return p ? &p->snd : NULL;
}
/* one sound of a pool at random (NULL: the pool is empty) */
static const char *pool_pick(const SoundPools *sp, int pool) {
    if (!sp || sp->n[pool] <= 0) return NULL;
    return sp->file[pool][rand() % sp->n[pool]];
}
/* A preset deleted: its file goes; characters that used it get the default back. */
static void preset_delete(MovePreset *p) {
    char path[1024];
    preset_file(path, sizeof(path), p->name);
    remove(path);
    int group = p->group;
    char name[48]; snprintf(name, sizeof(name), "%s", p->name);
    memmove(p, p + 1, sizeof(MovePreset) * (size_t)(g_presets + g_preset_n - p - 1));
    g_preset_n--;
    for (int i = 0; i < g_moveset_count; i++) {
        Moveset *ms = &g_movesets[i];
        int changed = 0;
        if (group == PG_WALK && !_stricmp(ms->walk_preset, name)) { snprintf(ms->walk_preset, 48, "Human"); changed = 1; }
        if (group == PG_COMBAT && !_stricmp(ms->combat_preset, name)) { ms->combat_preset[0] = 0; changed = 1; }
        if (group == PG_SOUND && !_stricmp(ms->sound_preset, name)) { snprintf(ms->sound_preset, 48, "%s", default_sound_preset(ms->model)); changed = 1; }
        if ((group == PG_REACT || group == PG_AI || group == PG_STATS) && !_stricmp(ms_group_name(ms, group), name)) {
            snprintf(ms_group_name(ms, group), 48, "%s", default_group_preset(ms->model, group, ms->right_hand)); changed = 1;
        }
        if (group == PG_MODEL && !_stricmp(ms->model_preset, name)) { ms->model_preset[0] = 0; changed = 1; }
        if (changed) moveset_save(ms);
    }
    g_moveset_gen++;
}

/* An item of assets/chars/items (weapons, shields...): a small model whose
   mesh is bound, with an identity bind, to its bone 2 -- its vertices are
   in the frame of the hand that holds it, the grip at the origin, a
   blade toward -Z. Loaded once. */
static CharModel *item_model(const char *name) {
    static CharModel *loaded[64];
    static char names[64][48];
    static int n = 0;
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], name)) return loaded[i];
    if (n >= 64) return NULL;
    char dir[1024];
    root_path(dir, sizeof(dir), "assets/chars/items");
    CharModel *m = (CharModel *)calloc(1, sizeof(CharModel));
    if (m && !char_model_load(m, dir, name)) { char_model_free(m); free(m); m = NULL; }
    if (m) { /* its own bones and binds baked in (bows, clubs, torches... aren't bound as simply as the swords) */
        NodeOverride ov[DAVID_MAX_NODES];
        memset(ov, 0, sizeof(ov));
        static Mat4 mats[DAVID_MAX_JOINTS];
        skeleton_skin_matrices_for(m, ov, mats);
        for (int v = 0; v < m->vertex_count; v++) {
            float acc[3] = { 0, 0, 0 }, sp[3], wsum = 0;
            for (int k = 0; k < 4; k++) {
                float w = m->weights[v][k];
                if (w <= 0) continue;
                mat4_vec3(&mats[m->joints_idx[v][k]], m->positions[v], sp);
                acc[0] += sp[0] * w; acc[1] += sp[1] * w; acc[2] += sp[2] * w; wsum += w;
            }
            if (wsum > 0) { m->positions[v][0] = acc[0] / wsum; m->positions[v][1] = acc[1] / wsum; m->positions[v][2] = acc[2] / wsum; }
        }
    }
    snprintf(names[n], sizeof(names[0]), "%s", name);
    loaded[n++] = m;
    return m;
}
/* The bone a hand holds things with: the grip bone when it really is in
   that hand, else the wrist's first child (the hand), else -1. */
static int grip_node(const CharModel *m, int left) {
    int grip = left ? LEFT_HAND_NODE : RIGHT_HAND_NODE, wrist = left ? LEFT_WRIST_NODE : RIGHT_WRIST_NODE;
    if (!m || m->node_count <= wrist) return -1;
    if (grip < m->node_count && m->node_parent[grip] == wrist) return grip;
    for (int i = wrist + 1; i < m->node_count; i++) if (m->node_parent[i] == wrist) return i;
    return -1;
}

/* A PAIR of weapons packed in one item (dualswrd: two swords crossed at
   the grip, one blade along -Z, the other along -X): split in two, the -Z
   one for the right hand, the other turned to -Z for the left hand. Each
   triangle goes to the blade it lies along. */
static struct { const CharModel *base; CharModel *right, *left; } g_pairs[16];
static int g_pair_count = 0;
static CharModel *item_part(const CharModel *base, int second) {
    CharModel *m = (CharModel *)malloc(sizeof(CharModel));
    if (!m) return NULL;
    *m = *base; /* texture, uvs, weights shared with the base */
    m->indices = (uint32_t *)malloc(sizeof(uint32_t) * base->index_count);
    m->index_count = 0;
    for (int t = 0; t < base->index_count / 3; t++) {
        float cx = 0, cz = 0;
        for (int k = 0; k < 3; k++) { cx += base->positions[base->indices[t * 3 + k]][0]; cz += base->positions[base->indices[t * 3 + k]][2]; }
        if ((fabsf(cx) > fabsf(cz)) != second) continue;
        for (int k = 0; k < 3; k++) m->indices[m->index_count++] = base->indices[t * 3 + k];
    }
    if (second) { /* -X -> -Z: a quarter turn about Y */
        m->positions = (float (*)[3])malloc(sizeof(float) * 3 * base->vertex_count);
        for (int v = 0; v < base->vertex_count; v++) {
            const float *p = base->positions[v];
            m->positions[v][0] = -p[2]; m->positions[v][1] = p[1]; m->positions[v][2] = p[0];
        }
    }
    return m;
}
static int item_is_pair(const CharModel *it) { /* two long blades, along -X and -Z, nothing far on the other side (an orb isn't) */
    float minx = 0, minz = 0, maxx = 0, maxz = 0;
    for (int v = 0; v < it->vertex_count; v++) {
        minx = fminf(minx, it->positions[v][0]); minz = fminf(minz, it->positions[v][2]);
        maxx = fmaxf(maxx, it->positions[v][0]); maxz = fmaxf(maxz, it->positions[v][2]);
    }
    return minx < -1.0f && minz < -1.0f && maxx < 1.0f && maxz < 1.0f;
}
/* A weapon's blade, in its hand's frame: from near the grip to the tip (-Z) -- for the trails. 0 if too short. */
static int item_blade(const CharModel *it, float base[3], float tip[3]) {
    float minz = 0;
    for (int v = 0; v < it->vertex_count; v++) minz = fminf(minz, it->positions[v][2]);
    if (minz > -0.4f) return 0;
    base[0] = base[1] = 0; base[2] = minz * 0.25f;
    tip[0] = tip[1] = 0; tip[2] = minz;
    return 1;
}
/* The bone a shield hangs on: 41 on the back of the left hand, else its grip. */
#define SHIELD_NODE 41
static int shield_node(const CharModel *m) {
    if (m && m->node_count > SHIELD_NODE && m->node_parent[SHIELD_NODE] == LEFT_WRIST_NODE) return SHIELD_NODE;
    return grip_node(m, 1);
}
/* what a hand holds when `name` is the item */
static CharModel *item_held(const char *name, int left) {
    CharModel *base = item_model(name);
    if (!base) return NULL;
    for (int i = 0; i < g_pair_count; i++) if (g_pairs[i].base == base) return left ? g_pairs[i].left : g_pairs[i].right;
    if (!item_is_pair(base) || g_pair_count >= 16) return left ? NULL : base;
    g_pairs[g_pair_count].base = base;
    g_pairs[g_pair_count].right = item_part(base, 0);
    g_pairs[g_pair_count].left = item_part(base, 1);
    g_pair_count++;
    return left ? g_pairs[g_pair_count - 1].left : g_pairs[g_pair_count - 1].right;
}
/* what a character model holds in its right / left hand */
static CharModel *model_right_hand(const CharModel *m) {
    if (grip_node(m, 0) < 0) return NULL;
    return item_held(moveset_get(m->name[0] ? m->name : "david")->right_hand, 0);
}
/* ...and on its left arm (its shield) */
static CharModel *model_left_shield(const CharModel *m) {
    if (shield_node(m) < 0) return NULL;
    return item_model(moveset_get(m->name[0] ? m->name : "david")->left_hand);
}


static void actor_resolve_clips(Actor *a) {
    if (a->clips_gen == g_moveset_gen) return;
    a->clips_gen = g_moveset_gen;
    const Moveset *ms = moveset_get(a->model->name[0] ? a->model->name : "david");
    for (int k = 0; k < MC_COUNT; k++) {
        int c = moveset_clip_w(ms, k, NULL, a->weapon);
        a->clips[k] = (c >= 0 && anim_lib_fits(c, a->model->node_count)) ? c : -1;
    }
    if (weapon_is_orb(a->weapon)) { int c = anim_lib_find("magkbob"); if (c >= 0 && anim_lib_fits(c, a->model->node_count)) a->clips[MC_STAND] = c; } /* holding its orb up */
    a->item = grip_node(a->model, 0) >= 0 && !weapon_is_orb(a->weapon) ? item_held(a->weapon, 0) : NULL;
    a->item_l = grip_node(a->model, 1) >= 0 && !a->shield[0] ? item_held(a->weapon, 1) : NULL;
    a->shield_m = shield_node(a->model) >= 0 && a->shield[0] ? item_model(a->shield) : NULL;
}

/* ---- the characters of the room ---- */
static void actor_reset(Actor *a, CharModel *m, int place_id) {
    memset(a, 0, sizeof(*a));
    a->used = 1; a->model = m; a->place_id = place_id;
    a->turn_clip = -1; a->pending_door = -1; a->play_clip = -1; a->play_next = -1;
    snprintf(a->weapon, sizeof(a->weapon), "%s", moveset_get(m && m->name[0] ? m->name : "david")->right_hand);
    snprintf(a->shield, sizeof(a->shield), "%s", moveset_get(m && m->name[0] ? m->name : "david")->left_hand);
    a->side = SIDE_ALLY;
    a->hp = a->hp_max = moveset_stats(moveset_get(m && m->name[0] ? m->name : "david"))->hp;
    a->ai_on = place_id != 0; /* a placed character's AI plays (a script can turn it off); never David's */
    a->ai_cool = 0.5f;
}
/* script characters leave with the room */
static void actors_clear_npcs(void) {
    for (int k = 1; k < MAX_ACTORS; k++) g_actors[k].used = 0;
}
static Actor *actor_by_place_id(int id) {
    if (id == 0) return DAVID_ACTOR;
    for (int k = 1; k < MAX_ACTORS; k++) if (g_actors[k].used && g_actors[k].place_id == id) return &g_actors[k];
    return NULL;
}

/* =====================================================================
   SCENE EDITOR + TOOL UI
   ---------------------------------------------------------------------
   Everything the user authors per room is a SHAPE drawn on the room
   image (rectangle / circle / polygon, in room-image pixels) carrying
   any combination of roles, set from its right-click menu (or the role
   buttons in the side panel):
     - RED zone        David may never stand/walk there
     - GREEN zone      forced walkable (steep/tight spots allowed)
     - FOREGROUND      that part of the pre-rendered picture is drawn
                       OVER the characters (pillars, railings, arches...)
     - SCENE CONNECTOR a door to another room: doorstep + arrival points
                       and the linked shape on the other side (wizard)
   Saved per room in data/rooms/<level>/<room>_shapes.cfg; the older
   <room>_nogo.cfg / <room>_doors.cfg are read (migrated) when a room has
   no shapes file yet.

   UI: tool buttons + help live in side panels OUTSIDE the game picture
   (black bars in fullscreen, a sidebar in windowed mode). CAPS LOCK ON =
   tools mode (cursor free to reach the panels); CAPS LOCK OFF = playing
   (cursor kept inside the game picture, the blockouts' game cursors, mouse at a
   picture edge scrolls the view). Every button has a keyboard shortcut.
   ===================================================================== */

#define SHAPE_MAX 96
#define SHAPE_MAX_PTS 64
enum { NAV_ROLE_NONE = 0, NAV_ROLE_RED = 1, NAV_ROLE_GREEN = 2 };
typedef struct {
    int id;
    int type, n;                 /* 0 rect (p0,p1), 1 circle (p0 centre, p1.x radius), 2 polygon */
    float p[SHAPE_MAX_PTS][2];
    int nav;                     /* NAV_ROLE_* */
    int fg;                      /* foreground mask */
    int door;                    /* scene connector */
    int has_step, has_arrival;
    float step[3], arrival[3];   /* world points on this room's floor */
    char target[128];            /* target room label ("level/room"), "" = none yet */
    int target_door;             /* id of the linked shape in the target room, -1 = none */
    char script[64];             /* script of the target room played when David arrives through it, "" = none */
} Shape;

static Shape g_shapes[SHAPE_MAX];
static int g_shape_count = 0;
static char g_shape_room[128] = "";
static int g_shape_gen = 0;      /* bumped on every change -> caches (fg mask) */

/* editor state */
static int g_show_walkable = 0;  /* P */
static int g_edit_mode = 0;      /* E */
enum { TOOL_SELECT, TOOL_RECT, TOOL_CIRCLE, TOOL_POLY };
static int g_tool = TOOL_SELECT;
static int g_sel = -1;           /* selected shape index */
enum { DRAG_NONE, DRAG_NEW, DRAG_MOVE, DRAG_HANDLE, DRAG_POINT };
static int g_drag = DRAG_NONE, g_drag_handle = -1, g_drag_changed = 0;
static float g_drag_start[2], g_drag_last[2], g_mouse_room[2];
static Shape g_poly;             /* polygon being drawn (n = vertices so far) */
static int g_edit_dragging = 0;  /* kept for the edge-scroll guard */

/* ---------------- shapes: geometry ---------------- */
static int shape_contains(const Shape *z, float x, float y) {
    if (z->type == 0) {
        float x0 = fminf(z->p[0][0], z->p[1][0]), x1 = fmaxf(z->p[0][0], z->p[1][0]);
        float y0 = fminf(z->p[0][1], z->p[1][1]), y1 = fmaxf(z->p[0][1], z->p[1][1]);
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    if (z->type == 1) {
        float dx = x - z->p[0][0], dy = y - z->p[0][1];
        return dx * dx + dy * dy <= z->p[1][0] * z->p[1][0];
    }
    int inside = 0;
    for (int i = 0, j = z->n - 1; i < z->n; j = i++) {
        float xi = z->p[i][0], yi = z->p[i][1], xj = z->p[j][0], yj = z->p[j][1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12f) + xi)) inside = !inside;
    }
    return inside;
}

static void shape_bbox(const Shape *z, float *x0, float *y0, float *x1, float *y1) {
    if (z->type == 1) { *x0 = z->p[0][0] - z->p[1][0]; *x1 = z->p[0][0] + z->p[1][0]; *y0 = z->p[0][1] - z->p[1][0]; *y1 = z->p[0][1] + z->p[1][0]; return; }
    *x0 = *y0 = 1e9f; *x1 = *y1 = -1e9f;
    for (int k = 0; k < z->n; k++) { *x0 = fminf(*x0, z->p[k][0]); *x1 = fmaxf(*x1, z->p[k][0]); *y0 = fminf(*y0, z->p[k][1]); *y1 = fmaxf(*y1, z->p[k][1]); }
}

/* Resize/reshape handles: rect = 4 corners, circle = 4 points on the
   circumference (any of them sets the radius), polygon = its vertices. */
static int shape_handles(const Shape *z, float hx[], float hy[]) {
    if (z->type == 0) {
        hx[0] = z->p[0][0]; hy[0] = z->p[0][1]; hx[1] = z->p[1][0]; hy[1] = z->p[0][1];
        hx[2] = z->p[1][0]; hy[2] = z->p[1][1]; hx[3] = z->p[0][0]; hy[3] = z->p[1][1];
        return 4;
    }
    if (z->type == 1) {
        float cx = z->p[0][0], cy = z->p[0][1], r = z->p[1][0];
        hx[0] = cx + r; hy[0] = cy; hx[1] = cx; hy[1] = cy + r; hx[2] = cx - r; hy[2] = cy; hx[3] = cx; hy[3] = cy - r;
        return 4;
    }
    for (int k = 0; k < z->n; k++) { hx[k] = z->p[k][0]; hy[k] = z->p[k][1]; }
    return z->n;
}

static void shape_move_handle(Shape *z, int h, float x, float y) {
    if (z->type == 0) {
        if (h == 0) { z->p[0][0] = x; z->p[0][1] = y; }
        else if (h == 1) { z->p[1][0] = x; z->p[0][1] = y; }
        else if (h == 2) { z->p[1][0] = x; z->p[1][1] = y; }
        else { z->p[0][0] = x; z->p[1][1] = y; }
    } else if (z->type == 1) {
        float dx = x - z->p[0][0], dy = y - z->p[0][1];
        z->p[1][0] = fmaxf(2.0f, sqrtf(dx * dx + dy * dy));
    } else if (h >= 0 && h < z->n) { z->p[h][0] = x; z->p[h][1] = y; }
}

static void shape_translate(Shape *z, float dx, float dy) {
    if (z->type == 1) { z->p[0][0] += dx; z->p[0][1] += dy; return; }
    for (int k = 0; k < z->n; k++) { z->p[k][0] += dx; z->p[k][1] += dy; }
}

static int shape_at(float x, float y) {
    if (g_sel >= 0 && g_sel < g_shape_count && shape_contains(&g_shapes[g_sel], x, y)) return g_sel;
    for (int i = g_shape_count - 1; i >= 0; i--) if (shape_contains(&g_shapes[i], x, y)) return i;
    return -1;
}

static int shape_index_by_id(int id) {
    for (int i = 0; i < g_shape_count; i++) if (g_shapes[i].id == id) return i;
    return -1;
}

static int shape_next_id(const Shape *s, int n) {
    int id = 1;
    for (int i = 0; i < n; i++) if (s[i].id >= id) id = s[i].id + 1;
    return id;
}

/* ---------------- shapes: files ---------------- */
static void shapes_file_path(char *out, size_t n, const char *room, const char *suffix) {
    root_path(out, n, "data/rooms/%s_%s.cfg", room, suffix);
}

static int parse_geom(const char *s, Shape *z) {
    if (sscanf(s, "rect %f %f %f %f", &z->p[0][0], &z->p[0][1], &z->p[1][0], &z->p[1][1]) == 4) { z->type = 0; z->n = 2; return 1; }
    if (sscanf(s, "circle %f %f %f", &z->p[0][0], &z->p[0][1], &z->p[1][0]) == 3) { z->type = 1; z->n = 2; return 1; }
    if (strncmp(s, "poly ", 5) == 0) {
        char *e; int n = (int)strtol(s + 5, &e, 10);
        if (n < 3 || n > SHAPE_MAX_PTS) return 0;
        for (int i = 0; i < n; i++) { z->p[i][0] = strtof(e, &e); z->p[i][1] = strtof(e, &e); }
        z->type = 2; z->n = n; return 1;
    }
    return 0;
}

static void write_geom(FILE *f, const Shape *z) {
    if (z->type == 0) fprintf(f, "rect %.1f %.1f %.1f %.1f\n", z->p[0][0], z->p[0][1], z->p[1][0], z->p[1][1]);
    else if (z->type == 1) fprintf(f, "circle %.1f %.1f %.1f\n", z->p[0][0], z->p[0][1], z->p[1][0]);
    else { fprintf(f, "poly %d", z->n); for (int k = 0; k < z->n; k++) fprintf(f, " %.1f %.1f", z->p[k][0], z->p[k][1]); fprintf(f, "\n"); }
}

static int shapes_read_file(const char *room, Shape *out, int max) {
    char path[600], line[4096];
    shapes_file_path(path, sizeof(path), room, "shapes");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0; Shape d; int in = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "shape ", 6) == 0) { memset(&d, 0, sizeof(d)); d.id = atoi(s + 6); d.target_door = -1; in = 1; }
        else if (!in) continue;
        else if (strncmp(s, "geom ", 5) == 0) parse_geom(s + 5, &d);
        else if (strncmp(s, "nav red", 7) == 0) d.nav = NAV_ROLE_RED;
        else if (strncmp(s, "nav green", 9) == 0) d.nav = NAV_ROLE_GREEN;
        else if (strncmp(s, "foreground", 10) == 0) d.fg = 1;
        else if (strncmp(s, "connector", 9) == 0) d.door = 1;
        else if (sscanf(s, "step %f %f %f", &d.step[0], &d.step[1], &d.step[2]) == 3) d.has_step = 1;
        else if (sscanf(s, "arrival %f %f %f", &d.arrival[0], &d.arrival[1], &d.arrival[2]) == 3) d.has_arrival = 1;
        else if (strncmp(s, "target ", 7) == 0) { sscanf(s + 7, "%127s %d", d.target, &d.target_door); if (strcmp(d.target, "-") == 0) d.target[0] = 0; }
        else if (strncmp(s, "script ", 7) == 0) {
            snprintf(d.script, sizeof(d.script), "%s", s + 7);
            size_t l = strlen(d.script);
            while (l > 0 && (d.script[l - 1] == '\n' || d.script[l - 1] == '\r')) d.script[--l] = 0;
        }
        else if (strncmp(s, "end", 3) == 0) { if (n < max) out[n++] = d; in = 0; }
    }
    fclose(f);
    return n;
}

static void shapes_write_file(const char *room, const Shape *s, int n) {
    char path[600]; shapes_file_path(path, sizeof(path), room, "shapes");
    ensure_parent_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Silver Remaster scene shapes (editor: E). Roles: nav red|green, foreground, connector (script = played on arrival).\n");
    for (int i = 0; i < n; i++) {
        const Shape *z = &s[i];
        fprintf(f, "shape %d\ngeom ", z->id); write_geom(f, z);
        if (z->nav == NAV_ROLE_RED) fprintf(f, "nav red\n");
        if (z->nav == NAV_ROLE_GREEN) fprintf(f, "nav green\n");
        if (z->fg) fprintf(f, "foreground\n");
        if (z->door) {
            fprintf(f, "connector\n");
            if (z->has_step) fprintf(f, "step %.4f %.4f %.4f\n", z->step[0], z->step[1], z->step[2]);
            if (z->has_arrival) fprintf(f, "arrival %.4f %.4f %.4f\n", z->arrival[0], z->arrival[1], z->arrival[2]);
            fprintf(f, "target %s %d\n", z->target[0] ? z->target : "-", z->target_door);
            if (z->script[0]) fprintf(f, "script %s\n", z->script);
        }
        fprintf(f, "end\n");
    }
    fclose(f);
}

static void rebuild_nav_after_zone_edit(void);
static void shapes_load(const char *room_lower) {
    snprintf(g_shape_room, sizeof(g_shape_room), "%s", room_lower);
    g_shape_count = shapes_read_file(room_lower, g_shapes, SHAPE_MAX);
    g_sel = -1; g_drag = DRAG_NONE; g_poly.n = 0;
    g_shape_gen++;
}

/* ---------------- undo (per room, local edits) ---------------- */
#define UNDO_MAX 40
typedef struct { Shape s[SHAPE_MAX]; int n; } ShapeSnapshot;
static ShapeSnapshot *g_undo = NULL;
static int g_undo_count = 0;
static char g_undo_room[128] = "";

static void undo_push(void) {
    if (!g_undo) g_undo = (ShapeSnapshot *)calloc(UNDO_MAX, sizeof(ShapeSnapshot));
    if (!g_undo) return;
    if (strcmp(g_undo_room, g_shape_room) != 0) { g_undo_count = 0; snprintf(g_undo_room, sizeof(g_undo_room), "%s", g_shape_room); }
    if (g_undo_count == UNDO_MAX) { memmove(&g_undo[0], &g_undo[1], sizeof(ShapeSnapshot) * (UNDO_MAX - 1)); g_undo_count--; }
    memcpy(g_undo[g_undo_count].s, g_shapes, sizeof(Shape) * (size_t)g_shape_count);
    g_undo[g_undo_count].n = g_shape_count;
    g_undo_count++;
}

/* Saves the room's shapes and refreshes whatever depends on them. */
static void shapes_changed(int nav_affected) {
    if (g_shape_room[0]) shapes_write_file(g_shape_room, g_shapes, g_shape_count);
    g_shape_gen++;
    if (nav_affected) rebuild_nav_after_zone_edit();
}

static int undo_pop(void) {
    if (!g_undo || g_undo_count == 0 || strcmp(g_undo_room, g_shape_room) != 0) return 0;
    g_undo_count--;
    memcpy(g_shapes, g_undo[g_undo_count].s, sizeof(Shape) * (size_t)g_undo[g_undo_count].n);
    g_shape_count = g_undo[g_undo_count].n;
    g_sel = -1;
    shapes_changed(1);
    return 1;
}

/* ---------------- navigation hook ---------------- */
static int shapes_pixel_zone(float x, float y) {
    int green = 0;
    for (int i = 0; i < g_shape_count; i++) {
        if (g_shapes[i].nav == NAV_ROLE_NONE || !shape_contains(&g_shapes[i], x, y)) continue;
        if (g_shapes[i].nav == NAV_ROLE_RED) return NAV_ZONE_BLOCK; /* red wins */
        green = 1;
    }
    return green ? NAV_ZONE_FORCE : NAV_ZONE_NONE;
}

/* g_nav_zone_at: a world point takes the zone it shows up in on the
   room's own photo (the shapes are drawn on that image). */
static int shapes_world_zone(float x, float y, float z) {
    if (g_shape_count == 0) return NAV_ZONE_NONE;
    float w[3] = { x, y, z }, px, py, pz;
    if (!camera_world_to_pixel_z(&g_room_cam, w, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) return NAV_ZONE_NONE;
    return shapes_pixel_zone(px, py);
}

static void rebuild_nav_after_zone_edit(void) {
    if (g_room_mesh_tri_count <= 0) return;
    navgrid_free(&g_nav_grid);
    navgrid_build(&g_nav_grid, g_room_mesh_tris, g_room_mesh_tri_count, 0.5f, g_room_floor_y - 12.0f);
    g_nav_gen++;
}

/* ---------------- tints: colour = colour * k + offset ---------------- */
typedef struct { float k, r, g, b; } Tint;
static const Tint TINT_GREEN = { 0.62f, 20.0f, 255 * 0.38f, 30.0f };
static const Tint TINT_RED   = { 0.55f, 255 * 0.45f, 15.0f, 15.0f };
static const Tint TINT_FORCE = { 0.45f, 60 * 0.55f, 255 * 0.55f, 90 * 0.55f };
static const Tint TINT_DOOR  = { 0.50f, 40.0f, 110.0f, 127.0f };
static const Tint TINT_FG    = { 0.60f, 90.0f, 30.0f, 100.0f };
static inline uint32_t tint_px(uint32_t c, const Tint *t) {
    float r = ((c >> 16) & 255) * t->k + t->r, g = ((c >> 8) & 255) * t->k + t->g, b = (c & 255) * t->k + t->b;
    uint32_t R = r > 255 ? 255 : (uint32_t)r, G = g > 255 ? 255 : (uint32_t)g, B = b > 255 ? 255 : (uint32_t)b;
    return (R << 16) | (G << 8) | B;
}

static void tint_shape(const Shape *z, const Tint *tint) {
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    float x0, y0, x1, y1; shape_bbox(z, &x0, &y0, &x1, &y1);
    int ix0 = (int)fmaxf(0, x0), ix1 = (int)fminf(W - 1, x1), iy0 = (int)fmaxf(0, y0), iy1 = (int)fminf(H - 1, y1);
    for (int y = iy0; y <= iy1; y++) {
        uint32_t *row = g_render_pixels + (size_t)y * g_dib_stride;
        for (int x = ix0; x <= ix1; x++) if (shape_contains(z, x + 0.5f, y + 0.5f)) row[x] = tint_px(row[x], tint);
    }
}

/* ---------------- P: reachable-area mask (cached) ---------------- */
static uint8_t *g_walk_mask = NULL;
static size_t g_walk_mask_size = 0;
static int g_walk_mask_area = -2, g_walk_mask_gen = -1;
static int g_walk_mask_all = 0;  /* 1 = every standable node (connector point picking) */
static const void *g_walk_mask_room = NULL;

static void walk_mask_tri(const float *px, const float *py, const float *pz, const float *depth, uint8_t *mask) {
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    int minx = (int)floorf(fminf(px[0], fminf(px[1], px[2]))), maxx = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
    int miny = (int)floorf(fminf(py[0], fminf(py[1], py[2]))), maxy = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= H) maxy = H - 1;
    float den = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2]);
    if (fabsf(den) < 1e-6f) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        float fx = x + 0.5f, fy = y + 0.5f;
        float w0 = ((py[1] - py[2]) * (fx - px[2]) + (px[2] - px[1]) * (fy - py[2])) / den;
        float w1 = ((py[2] - py[0]) * (fx - px[2]) + (px[0] - px[2]) * (fy - py[2])) / den;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float z = 1.0f / (w0 / pz[0] + w1 / pz[1] + w2 / pz[2]);
        size_t i = (size_t)y * g_dib_stride + x;
        if (z <= depth[i] * 1.02f + 0.05f) mask[i] = 1; /* visible (not behind a wall) */
    }
}

static void ensure_walk_mask(void) {
    int area = -1;
    int dn = navgrid_node_at(&g_nav_grid, g_char_pos[0], g_char_pos[1], g_char_pos[2]);
    if (dn >= 0 && g_nav_grid.comp) area = g_nav_grid.comp[dn];
    if (g_walk_mask_all) area = -3;
    size_t npix = (size_t)g_dib_stride * g_hdr.height;
    if (g_walk_mask && g_walk_mask_size == npix && area == g_walk_mask_area && g_nav_gen == g_walk_mask_gen &&
        g_walk_mask_room == (const void *)g_room_mesh_tris) return;
    if (g_walk_mask_size != npix) { free(g_walk_mask); g_walk_mask = (uint8_t *)malloc(npix); g_walk_mask_size = npix; }
    memset(g_walk_mask, 0, npix);
    g_walk_mask_area = area; g_walk_mask_gen = g_nav_gen; g_walk_mask_room = (const void *)g_room_mesh_tris;
    if ((area < 0 && area != -3) || !g_nav_grid.comp) return;
    float *depth = (float *)malloc(npix * sizeof(float));
    for (size_t i = 0; i < npix; i++) depth[i] = 1e29f;
    render_room_mesh_depth_only_cam(&g_room_cam, depth);
    NavGrid *g = &g_nav_grid;
    float h = g->cell_size * 0.5f;
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    for (int cell = 0; cell < g->cols * g->rows; cell++) {
        for (int l = 0; l < g->walkable[cell]; l++) {
            int node = cell * NAV_MAX_LAYERS + l;
            if (area != -3 && g->comp[node] != area) continue;
            float cx = g->min_x + (cell % g->cols + 0.5f) * g->cell_size, cz = g->min_z + (cell / g->cols + 0.5f) * g->cell_size;
            float y = g->floor_y[node];
            float corner[4][3] = { {cx - h, y, cz - h}, {cx + h, y, cz - h}, {cx + h, y, cz + h}, {cx - h, y, cz + h} };
            float px[4], py[4], pz[4]; int ok = 1;
            for (int k = 0; k < 4; k++) ok &= camera_world_to_pixel_z(&g_room_cam, corner[k], W, H, &px[k], &py[k], &pz[k]);
            if (!ok) continue;
            float ax[3] = { px[0], px[1], px[2] }, ay[3] = { py[0], py[1], py[2] }, az[3] = { pz[0], pz[1], pz[2] };
            float bx[3] = { px[0], px[2], px[3] }, by[3] = { py[0], py[2], py[3] }, bz[3] = { pz[0], pz[2], pz[3] };
            walk_mask_tri(ax, ay, az, depth, g_walk_mask);
            walk_mask_tri(bx, by, bz, depth, g_walk_mask);
        }
    }
    free(depth);
}

/* ---------------- connectors (doors) ---------------- */
enum { WIZ_NONE, WIZ_STEP, WIZ_ARRIVAL, WIZ_TARGET, WIZ_OTHER_SHAPE };
static int g_wiz = WIZ_NONE;
static int g_wiz_shape = -1;         /* index of the connector being configured */
static int g_wiz_other_side = 0;     /* configuring the second side of a link */
static int g_wiz_repick_only = 0;    /* "redo the two points": no target step after */
static char g_wiz_src_room[128] = ""; /* first side's room label (other-side step) */
static int g_wiz_src_id = -1;        /* first side's shape id */
static int g_map_pick_target = 0;    /* TAB list opened to choose a connector target */
static int g_hovered_door = -1;

static int wizard_picking_points(void) { return g_edit_mode && (g_wiz == WIZ_STEP || g_wiz == WIZ_ARRIVAL); }

static int map_index_of_label(const char *label) {
    for (int i = 0; i < g_map_room_count; i++) if (_stricmp(g_map_rooms[i].label, label) == 0) return i;
    return -1;
}
static const char *current_room_label(void) {
    return g_current_map_room >= 0 ? g_map_rooms[g_current_map_room].label : g_shape_room;
}
static const char *room_of_label(const char *label) { return label; } /* per-room files are keyed by "level/room" */

/* Number of connectors saved for a room (TAB list display). */
static int room_door_count(const char *label) {
    static Shape tmp[SHAPE_MAX];
    int n = shapes_read_file(room_of_label(label), tmp, SHAPE_MAX), c = 0;
    for (int i = 0; i < n; i++) if (tmp[i].door) c++;
    return c;
}

/* Edits a connector stored in ANOTHER room (link / unlink the far side). */
static void far_side_set_target(const char *room_label, int id, const char *target, int target_id) {
    static Shape tmp[SHAPE_MAX];
    int n = shapes_read_file(room_of_label(room_label), tmp, SHAPE_MAX);
    for (int i = 0; i < n; i++) if (tmp[i].id == id) {
        snprintf(tmp[i].target, sizeof(tmp[i].target), "%s", target ? target : "");
        tmp[i].target_door = target_id;
    }
    shapes_write_file(room_of_label(room_label), tmp, n);
}
/* Removes the connector role of a shape in another room (the shape stays). */
static void far_side_unlink(const char *room_label, int id) {
    static Shape tmp[SHAPE_MAX];
    int n = shapes_read_file(room_of_label(room_label), tmp, SHAPE_MAX);
    for (int i = 0; i < n; i++) if (tmp[i].id == id) { tmp[i].door = 0; tmp[i].target[0] = 0; tmp[i].target_door = -1; tmp[i].has_step = tmp[i].has_arrival = 0; tmp[i].script[0] = 0; }
    shapes_write_file(room_of_label(room_label), tmp, n);
}

static void connector_clear(int i) {
    Shape *s = &g_shapes[i];
    if (s->target[0] && s->target_door >= 0) far_side_unlink(s->target, s->target_door); /* both sides */
    s->door = 0; s->target[0] = 0; s->target_door = -1; s->has_step = s->has_arrival = 0; s->script[0] = 0;
}

/* data/rooms/<level>/<room>_scripts.cfg */
static void scripts_file_path(char *out, size_t n, const char *room_label) { root_path(out, n, "data/rooms/%s_scripts.cfg", room_label); }

/* Every room has a script "Default", first of its list: played
   automatically when David enters (auto), its first block the room's
   background music -- None (no music) until chosen. Rooms without one get
   it when their scripts are read (it's saved with the first edit). If the
   room already has another auto script, that one keeps playing (Default
   is then not auto). */
#define DEFAULT_SCRIPT "Default"
static int scripts_with_default(Script **sc, int n) {
    for (int i = 0; i < n; i++) if (!strcmp((*sc)[i].name, DEFAULT_SCRIPT)) return n;
    Script *ns = (Script *)realloc(*sc, sizeof(Script) * (n + 1));
    if (!ns) return n;
    memmove(&ns[1], &ns[0], sizeof(Script) * n);
    script_init(&ns[0], DEFAULT_SCRIPT);
    script_ensure_rows(&ns[0], 1);
    action_init(&ns[0], script_at(&ns[0], 0, 0), ACT_MUSIC);
    ns[0].auto_run = 1;
    for (int i = 1; i <= n; i++) if (ns[i].auto_run) ns[0].auto_run = 0;
    *sc = ns;
    return n + 1;
}
/* a room's scripts, with its Default */
static int room_scripts_read(const char *room_label, Script **out) {
    char path[1024];
    scripts_file_path(path, sizeof(path), room_label);
    *out = NULL;
    return scripts_with_default(out, scripts_load(path, out));
}

/* "Which script plays when David arrives in <room_label> through this
   connector?" -- a menu of that room's scripts. Returns 1 if one was
   picked (out = its name, "" = none), 0 if the menu was dismissed. */
static int ask_arrival_script(const char *room_label, char *out, size_t n) {
    Script *sc = NULL;
    int ns = room_scripts_read(room_label, &sc);
    HMENU m = CreatePopupMenu();
    char head[200];
    snprintf(head, sizeof(head), "When David arrives in %s through this connector, play:", room_label);
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, head);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | (!out[0] ? MF_CHECKED : 0), 1, "None");
    for (int i = 0; i < ns && i < 200; i++) AppendMenuA(m, MF_STRING | (!strcmp(out, sc[i].name) ? MF_CHECKED : 0), 2 + i, sc[i].name);
    if (ns == 0) AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "(that room has no script yet -- Scripts screen: S)");
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) out[0] = 0;
    else if (cmd >= 2 && cmd - 2 < ns) snprintf(out, n, "%s", sc[cmd - 2].name);
    for (int i = 0; i < ns; i++) script_free(&sc[i]);
    free(sc);
    return cmd >= 1;
}

static void wizard_cancel(const char *why) {
    if ((g_wiz == WIZ_STEP || g_wiz == WIZ_ARRIVAL) && !g_wiz_repick_only && g_wiz_shape >= 0 && g_wiz_shape < g_shape_count &&
        !g_shapes[g_wiz_shape].has_arrival && !g_wiz_other_side) {
        g_shapes[g_wiz_shape].door = 0; g_shapes[g_wiz_shape].has_step = 0; /* never finished: not a connector */
        shapes_changed(0);
    }
    g_wiz = WIZ_NONE; g_wiz_shape = -1; g_wiz_other_side = 0; g_wiz_repick_only = 0; g_map_pick_target = 0;
    snprintf(g_status, sizeof(g_status), "connector setup cancelled%s%s", why ? ": " : "", why ? why : "");
}

/* Make shape i a connector and start asking for its two points. */
static void wizard_start(int i, int other_side) {
    Shape *s = &g_shapes[i];
    s->door = 1; s->has_step = s->has_arrival = 0;
    if (other_side) { snprintf(s->target, sizeof(s->target), "%s", g_wiz_src_room); s->target_door = g_wiz_src_id; }
    else { s->target[0] = 0; s->target_door = -1; }
    shapes_changed(0);
    g_wiz_shape = i; g_wiz_other_side = other_side; g_wiz_repick_only = 0;
    g_wiz = WIZ_STEP; g_sel = i;
    g_status[0] = 0; /* the step box explains what to do */
}

/* Floor point designated by a click while picking connector points: the
   floor under the cursor, snapped to the nearest spot David's body fits
   (a nav grid node) -- he could otherwise be dropped where he can never
   walk out of. It may be in an area David can't currently reach. */
static int pick_floor_point(float px, float py, float out[3]) {
    float origin[3], dir[3], hit[3], nrm[3];
    camera_pixel_to_ray(view_cam(), px, py, (int)g_hdr.width, (int)g_hdr.height, origin, dir);
    if (!raycast_room_mesh_view(origin, dir, g_room_mesh_tris, g_room_mesh_tri_count, 1, 0.3f, hit, nrm) &&
        !raycast_room_mesh_view(origin, dir, g_room_mesh_tris, g_room_mesh_tri_count, 0, 0.0f, hit, nrm)) return 0;
    if (!stand_ok(hit[0], hit[1], hit[2], g_room_mesh_tris, g_room_mesh_tri_count)) {
        float sp[3];
        if (!navgrid_snap(&g_nav_grid, hit[0], hit[1], hit[2], sp)) return 0;
        hit[0] = sp[0]; hit[1] = sp[1]; hit[2] = sp[2];
    }
    out[0] = hit[0]; out[1] = hit[1]; out[2] = hit[2];
    return 1;
}

static void wizard_open_target_list(void) {
    g_wiz = WIZ_TARGET;
    g_map_pick_target = 1;
    g_map_mode = 1;
    g_map_filter[0] = 0; g_map_filter_len = 0;
    map_recompute_filter();
    map_select_current_room();
}

static void wizard_click(float px, float py) {
    if (g_wiz_shape < 0 || g_wiz_shape >= g_shape_count) { wizard_cancel(NULL); return; }
    Shape *d = &g_shapes[g_wiz_shape];
    float w[3];
    if (!pick_floor_point(px, py, w)) { snprintf(g_status, sizeof(g_status), "David can't stand there -- click on the GREEN floor"); return; }
    if (g_wiz == WIZ_STEP) {
        memcpy(d->step, w, sizeof(w)); d->has_step = 1;
        shapes_changed(0);
        g_wiz = WIZ_ARRIVAL;
        return;
    }
    if (g_wiz == WIZ_ARRIVAL) {
        memcpy(d->arrival, w, sizeof(w)); d->has_arrival = 1;
        shapes_changed(0);
        if (g_wiz_repick_only) { g_wiz = WIZ_NONE; g_wiz_shape = -1; g_wiz_repick_only = 0; snprintf(g_status, sizeof(g_status), "connector points updated"); return; }
        if (g_wiz_other_side) {
            ask_arrival_script(g_wiz_src_room, d->script, sizeof(d->script));
            shapes_changed(0);
            far_side_set_target(g_wiz_src_room, g_wiz_src_id, current_room_label(), d->id);
            snprintf(g_status, sizeof(g_status), "connector linked both ways: %s <-> %s", g_wiz_src_room, current_room_label());
            g_wiz = WIZ_NONE; g_wiz_shape = -1; g_wiz_other_side = 0;
            return;
        }
        wizard_open_target_list();
    }
}

static void wizard_target_chosen(HWND hwnd, int map_index) {
    g_map_pick_target = 0; g_map_mode = 0;
    if (g_wiz_shape < 0 || g_wiz_shape >= g_shape_count) { wizard_cancel(NULL); return; }
    Shape *d = &g_shapes[g_wiz_shape];
    MapRoomEntry *e = &g_map_rooms[map_index];
    if (d->target[0] && d->target_door >= 0) far_side_unlink(d->target, d->target_door); /* re-target: old far side released */
    snprintf(d->target, sizeof(d->target), "%s", e->label);
    d->target_door = -1;
    ask_arrival_script(e->label, d->script, sizeof(d->script));
    shapes_changed(0);
    snprintf(g_wiz_src_room, sizeof(g_wiz_src_room), "%s", current_room_label());
    g_wiz_src_id = d->id;
    char title[300];
    snprintf(title, sizeof(title), "Silver Remaster -- %s  [%d/%d]", e->label, map_index + 1, g_map_room_count);
    change_room(e->label);
    SetWindowTextA(hwnd, title);
    g_edit_mode = 1; g_tool = TOOL_RECT;
    g_wiz = WIZ_OTHER_SHAPE; g_wiz_shape = -1;
}

/* Game timer: David reached a connector's doorstep. */
static int arrive_through(int j, int run);
static void door_travel(HWND hwnd, int di) {
    if (di < 0 || di >= g_shape_count || !g_shapes[di].door) return;
    Shape d = g_shapes[di];
    if (!d.target[0]) { snprintf(g_status, sizeof(g_status), "this connector has no target room yet"); return; }
    int ti = map_index_of_label(d.target);
    if (ti < 0) { snprintf(g_status, sizeof(g_status), "connector target '%s' not found in the room list", d.target); return; }
    int run = g_door_travel_run; /* runs out if he ran in */
    char title[300];
    snprintf(title, sizeof(title), "Silver Remaster -- %s  [%d/%d]", g_map_rooms[ti].label, ti + 1, g_map_room_count);
    change_room(g_map_rooms[ti].label);
    SetWindowTextA(hwnd, title);
    snprintf(g_arrival_script, sizeof(g_arrival_script), "%s", d.script);
    int j = shape_index_by_id(d.target_door);
    if (!arrive_through(j, run)) snprintf(g_status, sizeof(g_status), "arrived in %s (the way back isn't set up yet)", g_map_rooms[ti].label);
}

/* David comes in by connector j of the room just loaded: on its doorstep,
   then walking (running) to its arrival point. 0 if it can't be done. */
static int arrive_through(int j, int run) {
    if (g_has_3d_character && j >= 0 && g_shapes[j].door && g_shapes[j].has_step) {
        /* appear on the other side's doorstep, then walk in */
        memcpy(g_char_pos, g_shapes[j].step, sizeof(float) * 3);
        g_char_moving = 0; g_char_waypoint_count = 0;
        if (g_room_mesh_tri_count > 0 && !stand_ok(g_char_pos[0], g_char_pos[1], g_char_pos[2], g_room_mesh_tris, g_room_mesh_tri_count)) {
            float sp[3];
            if (navgrid_snap(&g_nav_grid, g_char_pos[0], g_char_pos[1], g_char_pos[2], sp)) memcpy(g_char_pos, sp, sizeof(sp));
        }
        if (g_shapes[j].has_arrival) {
            g_char_facing = atan2f(g_shapes[j].arrival[0] - g_char_pos[0], g_shapes[j].arrival[2] - g_char_pos[2]);
            move_to_world_point(g_shapes[j].arrival, run);
            g_click_marker_active = 0;
        }
        center_view_on_david();
        return 1;
    }
    return 0;
}

/* Script "Change room": like going through a connector into `label`. */
static void room_travel(HWND hwnd, const char *label, int door, const char *script) {
    int ti = map_index_of_label(label);
    if (ti < 0) { snprintf(g_status, sizeof(g_status), "script: room '%s' not found", label); return; }
    char title[300];
    snprintf(title, sizeof(title), "Silver Remaster -- %s  [%d/%d]", g_map_rooms[ti].label, ti + 1, g_map_room_count);
    change_room(g_map_rooms[ti].label);
    SetWindowTextA(hwnd, title);
    snprintf(g_arrival_script, sizeof(g_arrival_script), "%s", script);
    if (door) arrive_through(shape_index_by_id(door), 0);
}

static int door_at_pixel(float x, float y) {
    for (int i = g_shape_count - 1; i >= 0; i--) if (g_shapes[i].door && shape_contains(&g_shapes[i], x, y)) return i;
    return -1;
}

static int door_click(int di, int run) {
    Shape *d = &g_shapes[di];
    if (!d->has_step) { snprintf(g_status, sizeof(g_status), "this connector isn't set up yet (editor: right-click it)"); return 0; }
    g_click_fail = 0;
    if (!move_to_world_point(d->step, run)) { report_click_refusal(); return 0; }
    g_pending_door = di;
    if (!g_char_moving) { g_door_travel_request = di; g_door_travel_run = run; g_pending_door = -1; }
    return 1;
}

/* ---------------- editor actions ---------------- */
static void shape_delete(int i) {
    if (i < 0 || i >= g_shape_count) return;
    undo_push();
    if (g_shapes[i].door) connector_clear(i);
    int nav = g_shapes[i].nav != NAV_ROLE_NONE;
    memmove(&g_shapes[i], &g_shapes[i + 1], sizeof(Shape) * (size_t)(g_shape_count - i - 1));
    g_shape_count--;
    g_sel = -1;
    shapes_changed(nav);
    snprintf(g_status, sizeof(g_status), "shape deleted");
}

static void shape_add(const Shape *geom) {
    if (g_shape_count >= SHAPE_MAX) { snprintf(g_status, sizeof(g_status), "max %d shapes per room", SHAPE_MAX); return; }
    undo_push();
    Shape s = *geom;
    s.id = shape_next_id(g_shapes, g_shape_count);
    s.nav = NAV_ROLE_NONE; s.fg = 0; s.door = 0; s.target[0] = 0; s.target_door = -1; s.has_step = s.has_arrival = 0;
    g_shapes[g_shape_count++] = s;
    g_sel = g_shape_count - 1;
    shapes_changed(0);
    g_tool = TOOL_SELECT; /* the new shape is selected: its handles work right away */
    if (g_wiz == WIZ_OTHER_SHAPE) { wizard_start(g_sel, 1); return; }
    snprintf(g_status, sizeof(g_status), "shape added -- give it a role (buttons, or right-click it)");
}

static void shape_set_nav(int i, int nav) {
    undo_push();
    g_shapes[i].nav = (g_shapes[i].nav == nav) ? NAV_ROLE_NONE : nav;
    shapes_changed(1);
}
static void shape_toggle_fg(int i) {
    undo_push();
    g_shapes[i].fg = !g_shapes[i].fg;
    shapes_changed(0);
}
static void shape_toggle_door(int i) {
    if (g_shapes[i].door) { undo_push(); connector_clear(i); shapes_changed(0); snprintf(g_status, sizeof(g_status), "no longer a connector (both sides)"); }
    else { g_edit_mode = 1; wizard_start(i, 0); }
}

/* ---------------- foreground ---------------- */
static uint8_t *g_fg_mask = NULL;
static uint32_t *g_fg_src = NULL;
static size_t g_fg_size = 0;
static int g_fg_gen = -1;
static int g_fg_any = 0;

static void ensure_fg_mask(void) {
    size_t npix = (size_t)g_dib_stride * g_hdr.height;
    if (g_fg_size != npix) {
        free(g_fg_mask); free(g_fg_src);
        g_fg_mask = (uint8_t *)malloc(npix); g_fg_src = (uint32_t *)malloc(npix * sizeof(uint32_t));
        g_fg_size = npix; g_fg_gen = -1;
    }
    if (g_fg_gen == g_shape_gen) return;
    g_fg_gen = g_shape_gen;
    memset(g_fg_mask, 0, npix);
    g_fg_any = 0;
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    for (int i = 0; i < g_shape_count; i++) {
        if (!g_shapes[i].fg) continue;
        g_fg_any = 1;
        float x0, y0, x1, y1; shape_bbox(&g_shapes[i], &x0, &y0, &x1, &y1);
        int ix0 = (int)fmaxf(0, x0), ix1 = (int)fminf(W - 1, x1), iy0 = (int)fmaxf(0, y0), iy1 = (int)fminf(H - 1, y1);
        for (int y = iy0; y <= iy1; y++) for (int x = ix0; x <= ix1; x++)
            if (shape_contains(&g_shapes[i], x + 0.5f, y + 0.5f)) g_fg_mask[(size_t)y * g_dib_stride + x] = 1;
    }
}

/* Right after the photo is composited (before shadow/David/tints). */
static void fg_capture(void) {
    ensure_fg_mask();
    if (g_fg_any) memcpy(g_fg_src, g_render_pixels, g_fg_size * sizeof(uint32_t));
}
/* After David is drawn: the foreground parts of the photo go back on top. */
static void fg_restore(void) {
    if (!g_fg_any) return;
    for (size_t i = 0; i < g_fg_size; i++) if (g_fg_mask[i]) g_render_pixels[i] = g_fg_src[i];
}

/* Tints into the room frame: P reachable floor (green), and in the
   editor the shapes by role. Called before David, so he's not tinted. */
static void apply_nav_overlays(void) {
    if (!g_has_3d_character || (!g_show_walkable && !g_edit_mode && !g_sv_pick)) return;
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    int picking = wizard_picking_points() || g_sv_pick;
    g_walk_mask_all = picking;
    if ((g_show_walkable || picking) && g_room_mesh_tri_count > 0) {
        ensure_walk_mask();
        for (int y = 0; y < H; y++) {
            uint32_t *row = g_render_pixels + (size_t)y * g_dib_stride;
            const uint8_t *m = g_walk_mask + (size_t)y * g_dib_stride;
            for (int x = 0; x < W; x++) if (m[x]) row[x] = tint_px(row[x], &TINT_GREEN);
        }
    }
    for (int i = 0; i < g_shape_count; i++) {
        const Shape *z = &g_shapes[i];
        if (z->nav == NAV_ROLE_RED) tint_shape(z, &TINT_RED);
        else if (z->nav == NAV_ROLE_GREEN) tint_shape(z, &TINT_FORCE);
        else if (z->door) tint_shape(z, &TINT_DOOR);
    }
}
/* Foreground shapes shown (editor only) AFTER the foreground restore. */
static void apply_fg_tint(void) {
    if (!g_edit_mode || !g_has_3d_character) return;
    for (int i = 0; i < g_shape_count; i++) if (g_shapes[i].fg && g_shapes[i].nav == NAV_ROLE_NONE && !g_shapes[i].door) tint_shape(&g_shapes[i], &TINT_FG);
}

/* =====================================================================
   SIDE PANELS: buttons, help, steps
   ===================================================================== */
enum {
    B_NONE = 0, B_PREV, B_NEXT, B_LIST,
    B_WALK, B_HITBOX, B_MESH, B_COLL, B_FULL, B_FREECAM,
    B_EDIT, B_T_SELECT, B_T_RECT, B_T_CIRCLE, B_T_POLY, B_UNDO, B_DELETE,
    B_R_RED, B_R_GREEN, B_R_FG, B_R_DOOR, B_R_REDO, B_R_RETARGET, B_R_SCRIPT, B_W_CANCEL, B_SETTINGS, B_SET_RESET, B_SCRIPTS,
    B_ANIMS, B_AV_MODEL, B_AV_MOVESET, B_AV_PREV, B_AV_PLAY, B_AV_NEXT, B_AV_STEPB, B_AV_STEPF, B_AV_SLOWER, B_AV_FASTER, B_AV_FOLLOW, B_AV_RESET, B_AV_CLOSE,
    B_SET_BASE = 100, /* + 2*i (-), + 2*i+1 (+) */
    B_SV_FIRST_ID = 280, B_SV_LAST_ID = 1499 /* the scripts screen's (sv_action), but 900-999: the moveset screen's */
};
typedef struct { int id; RECT r; const char *label; const char *key; const char *desc; int on; int enabled; int kind; } UiButton;
#define UI_MAX_BTN 256
static UiButton g_btn[UI_MAX_BTN];
static int g_btn_count = 0;
static char g_btn_text[UI_MAX_BTN][96]; /* labels built on the fly (ui_addf) */
static int g_hover_btn = B_NONE;
static RECT g_panel_left, g_panel_right;  /* g_panel_right empty in windowed mode */
/* info column (right bar in fullscreen, under the buttons in windowed):
   CAPS box at g_ui_caps_y, selected shape title at g_ui_sel_y (role
   buttons right under it), the rest (steps, help, status) from g_ui_next_y */
static int g_ui_caps_y = 0, g_ui_sel_y = 0, g_ui_next_y = 0;
#define SIDEBAR_W TOOL_SIDEBAR_W

static int g_show_settings = 0; /* N: David & navigation settings panel */
static int g_anim_view = 0;     /* A: animation viewer (full window) */
static int g_script_view = 0;   /* S: scripts screen (full window) */
static int g_ms_view = 0;       /* moveset screen (full window, over the others) */
static int g_pick_for_script = 0; /* the character picker chooses a script action's model */
static void script_view_toggle(void);
static int screen_view(void) { return g_anim_view || g_script_view || g_ms_view; }
static int attack_mode(void); /* ATTACK MODE */
static int g_radial; /* RADIAL MENU */
static int radial_tick(float dt);
static int g_pan_dir; /* cursor: the edge the view scrolls toward */
static void anim_view_layout(HWND hwnd);
static void anim_view_toggle(void);
static void anim_view_action(int id);
static int caps_on(void) { return (GetKeyState(VK_CAPITAL) & 1) != 0; }

static void ui_add(int id, int x, int y, int w, int h, const char *label, const char *key, const char *desc, int on, int enabled, int kind) {
    if (g_btn_count >= UI_MAX_BTN) return;
    UiButton *b = &g_btn[g_btn_count++];
    b->id = id; b->r.left = x; b->r.top = y; b->r.right = x + w; b->r.bottom = y + h;
    b->label = label; b->key = key; b->desc = desc; b->on = on; b->enabled = enabled; b->kind = kind;
}
/* same, with a label that doesn't have to outlive the call */
static void ui_addf(int id, int x, int y, int w, int h, const char *label, const char *key, const char *desc, int on, int enabled, int kind) {
    if (g_btn_count >= UI_MAX_BTN) return;
    snprintf(g_btn_text[g_btn_count], sizeof(g_btn_text[0]), "%s", label);
    ui_add(id, x, y, w, h, g_btn_text[g_btn_count], key, desc, on, enabled, kind);
}

/* Lays out every button for the current window/mode. Called before
   painting AND before hit-testing, so both always agree. */
static void sv_layout(HWND hwnd);
static void ms_layout(HWND hwnd);
static void sv_action(HWND hwnd, int id);
static void ui_layout(HWND hwnd) {
    if (g_ms_view) { ms_layout(hwnd); return; }
    if (g_anim_view) { anim_view_layout(hwnd); return; }
    if (g_script_view) { sv_layout(hwnd); return; }
    RECT rc; GetClientRect(hwnd, &rc);
    int win_w, win_h, ox, oy; float sc;
    get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    int game_r = ox + (int)(win_w * sc + 0.5f);
    if (g_is_fullscreen) {
        SetRect(&g_panel_left, 0, 0, ox, rc.bottom);
        SetRect(&g_panel_right, game_r, 0, rc.right, rc.bottom);
    } else {
        SetRect(&g_panel_left, 0, 0, SIDEBAR_W, rc.bottom);
        SetRect(&g_panel_right, 0, 0, 0, 0);
    }
    g_btn_count = 0;
    int pad = 10, x = g_panel_left.left + pad, w = (g_panel_left.right - g_panel_left.left) - 2 * pad;
    int half = (w - 6) / 2, h = 26, gap = 5;
    int y = 44;
    /* rooms */
    ui_add(B_PREV, x, y, half, h, "< Prev", "PgUp", "Load the previous room of the room list.", 0, 1, 0);
    ui_add(B_NEXT, x + half + 6, y, half, h, "Next >", "PgDn", "Load the next room of the room list.", 0, 1, 0);
    y += h + gap;
    ui_add(B_LIST, x, y, w, h, "Room list...", "TAB", "Open the list of all rooms (type to filter, Enter to go).", 0, 1, 0);
    y += h + gap + 18;
    /* view */
    int vy = y;
    ui_add(B_WALK, x, y, half, h, "Walkable", "P", "Show in green the floor David can reach from where he stands.", g_show_walkable, 1, 1);
    ui_add(B_HITBOX, x + half + 6, y, half, h, "Hitbox", "B", "Show David's collision volume (knee/chest rings) and his planned path.", g_show_collision, 1, 1);
    y += h + gap;
    ui_add(B_MESH, x, y, half, h, "3D mesh", "F3", "Show the raw 3D blockout instead of the picture, to check the alignment.", g_view_mode_3d, 1, 1);
    ui_add(B_COLL, x + half + 6, y, half, h, g_collision_enabled ? "Collisions" : "Fly mode", "C", "Collisions ON: David walks on floors only. OFF (fly mode): straight line through anything.", g_collision_enabled, 1, 1);
    y += h + gap;
    ui_add(B_ANIMS, x + half + 6, y, half, h, "Animations", "A", "Watch every animation of the chars folder played on David (list, play/pause, frame by frame, rotate, zoom).", 0, 1, 0);
    ui_add(B_FULL, x, y, half, h, g_is_fullscreen ? "Windowed" : "Fullscreen", "F11", "Switch between window and fullscreen (4:3 picture, tools in the side bars).", g_is_fullscreen, 1, 0);
    y += h + gap;
    ui_add(B_FREECAM, x, y, w, h, g_free_cam ? "Free camera: ON" : "Free camera", "F4",
           "Scene editor / placing a script's point: fly a camera anywhere in the room's 3D blockout (no picture), to put a connector's points (or a script's) where the room's camera doesn't look. W A S D / arrows, Q / E, Shift; right button dragged: look; wheel. F4 / Esc: back.",
           g_free_cam, g_loaded && (g_edit_mode || g_sv_pick), 1);
    y += h + gap;
    ui_add(B_SCRIPTS, x, y, w, h, "Scripts...", "S", "This room's scripts (cutscenes): a grid of actions -- waits, music, sounds, ambiences, pictures, characters placed and moved. Play them from there.", 0, g_has_3d_character, 0);
    y += h + gap + 18;
    (void)vy;
    /* editor */
    ui_add(B_EDIT, x, y, w, h, g_edit_mode ? "Scene editor: ON" : "Scene editor", "E", "Draw shapes on the picture and give them roles (red/green zone, foreground, scene connector).", g_edit_mode, g_has_3d_character, 1);
    y += h + gap;
    if (g_edit_mode) {
        ui_add(B_T_SELECT, x, y, half, h, "Select", "V", "Click a shape to select it; drag it to move it, drag its white squares to reshape it.", g_tool == TOOL_SELECT, 1, 2);
        ui_add(B_T_RECT, x + half + 6, y, half, h, "Rectangle", "1", "Drag on the picture to draw a rectangle.", g_tool == TOOL_RECT, 1, 2);
        y += h + gap;
        ui_add(B_T_CIRCLE, x, y, half, h, "Circle", "2", "Drag from the centre to draw a circle.", g_tool == TOOL_CIRCLE, 1, 2);
        ui_add(B_T_POLY, x + half + 6, y, half, h, "Polygon", "3", "Click each corner; click the first one (or Enter) to close the shape.", g_tool == TOOL_POLY, 1, 2);
        y += h + gap;
        ui_add(B_UNDO, x, y, half, h, "Undo", "Ctrl+Z", "Undo the last change to this room's shapes.", 0, g_undo_count > 0, 0);
        ui_add(B_DELETE, x + half + 6, y, half, h, "Delete", "Del", "Delete the selected shape (a connector is removed on both sides).", 0, g_sel >= 0, 0);
        y += h + gap;
        ui_add(B_SETTINGS, x, y, w, h, "David & navigation...", "N", "Hitbox radius, step height, slope, foot support, headroom... for THIS room. Opens in the info panel.", g_show_settings, 1, 1);
        y += h + gap;
    }
    g_ui_caps_y = g_is_fullscreen ? 12 : y + 10;
    g_ui_sel_y = g_ui_caps_y + 56;
    g_ui_next_y = g_ui_sel_y;
    if (g_edit_mode && g_show_settings) { /* settings panel takes the info column */
        int sx, sw;
        if (g_is_fullscreen) { sx = g_panel_right.left + pad; sw = (g_panel_right.right - g_panel_right.left) - 2 * pad; }
        else { sx = x; sw = w; }
        int sy = g_ui_caps_y + 22;
        for (int i = 0; i < SETTING_COUNT; i++) {
            TunableSetting *st = &g_settings[i];
            if (st->global && (i == 0 || !g_settings[i - 1].global)) sy += 44; /* room for "Reset" + the DAVID header */
            ui_add(B_SET_BASE + 2 * i, sx + sw - 58, sy, 27, 22, "-", "", st->desc, 0, *st->v > st->min + 1e-4f, 0);
            ui_add(B_SET_BASE + 2 * i + 1, sx + sw - 27, sy, 27, 22, "+", "", st->desc, 0, *st->v < st->max - 1e-4f, 0);
            sy += 27;
        }
        int reset_y = sy + 4;
        for (int i = 0; i < SETTING_COUNT; i++) if (g_settings[i].global) { reset_y = g_btn[g_btn_count - 2 * (SETTING_COUNT - i)].r.top - 44; break; }
        ui_add(B_SET_RESET, sx, reset_y, sw, 24, "Reset room to defaults", "", "Put every value of THIS room back to the defaults the game was tuned and verified with.", 0, 1, 0);
        g_ui_next_y = sy + 34;
        return; /* no role buttons while the settings are open */
    }
    /* role buttons for the selected shape: right panel (fullscreen) or below (windowed) */
    if (g_edit_mode && g_sel >= 0 && g_sel < g_shape_count && g_wiz == WIZ_NONE) {
        Shape *s = &g_shapes[g_sel];
        int rx, rw, ry;
        if (g_is_fullscreen) { rx = g_panel_right.left + pad; rw = (g_panel_right.right - g_panel_right.left) - 2 * pad; }
        else { rx = x; rw = w; }
        ry = g_ui_sel_y + 20;
        int rh = 24, rhalf = (rw - 6) / 2;
        ui_add(B_R_RED, rx, ry, rhalf, rh, "Red zone", "", "RED zone: David can never stand or walk here.", s->nav == NAV_ROLE_RED, 1, 3);
        ui_add(B_R_GREEN, rx + rhalf + 6, ry, rhalf, rh, "Green zone", "", "GREEN zone: David may walk here even if it's steep or tight.", s->nav == NAV_ROLE_GREEN, 1, 3);
        ry += rh + gap;
        ui_add(B_R_FG, rx, ry, rhalf, rh, "Foreground", "", "FOREGROUND: this part of the picture is drawn over the characters.", s->fg, 1, 3);
        ui_add(B_R_DOOR, rx + rhalf + 6, ry, rhalf, rh, "Connector", "", "SCENE CONNECTOR: a door to another room (you'll be guided step by step).", s->door, 1, 3);
        ry += rh + gap;
        if (s->door) {
            ui_add(B_R_REDO, rx, ry, rhalf, rh, "Redo points", "", "Pick again the doorstep (1) and the arrival point (2) of this connector.", 0, 1, 0);
            ui_add(B_R_RETARGET, rx + rhalf + 6, ry, rhalf, rh, "Change room", "", "Choose another target room for this connector.", 0, 1, 0);
            ry += rh + gap;
            ui_add(B_R_SCRIPT, rx, ry, rw, rh, "Arrival script...", "", "The script of the target room played when David arrives there through this connector (None = none).", s->script[0] != 0, s->target[0] != 0, 0);
            ry += rh + gap;
        }
        g_ui_next_y = ry + (s->door ? 44 : 4); /* + the connector's 'Leads to' / 'Plays' lines */
    }
    if (g_wiz != WIZ_NONE && g_wiz != WIZ_TARGET) {
        int rx, rw, ry;
        if (g_is_fullscreen) { rx = g_panel_right.left + pad; rw = (g_panel_right.right - g_panel_right.left) - 2 * pad; ry = rc.bottom - 60; }
        else { rx = x; rw = w; ry = rc.bottom - 40; }
        ui_add(B_W_CANCEL, rx, ry, rw, 26, "Cancel this step", "Esc", "Stop setting up this connector.", 0, 1, 0);
    }
}

static int ui_hit(int x, int y) {
    for (int i = 0; i < g_btn_count; i++) {
        RECT *r = &g_btn[i].r;
        if (x >= r->left && x < r->right && y >= r->top && y < r->bottom) return g_btn[i].enabled ? g_btn[i].id : B_NONE;
    }
    return B_NONE;
}
static const UiButton *ui_find(int id) {
    for (int i = 0; i < g_btn_count; i++) if (g_btn[i].id == id) return &g_btn[i];
    return NULL;
}

static void go_to_room_index(HWND hwnd, int i) {
    if (g_map_room_count <= 0) return;
    if (i >= g_map_room_count) i = 0;
    if (i < 0) i = g_map_room_count - 1;
    MapRoomEntry *e = &g_map_rooms[i];
    char title[300];
    snprintf(title, sizeof(title), "Silver Remaster -- %s  [%d/%d]", e->label, i + 1, g_map_room_count);
    change_room(e->label);
    SetWindowTextA(hwnd, title);
}

static void edit_set_tool(int tool) { g_tool = tool; g_poly.n = 0; g_drag = DRAG_NONE; }

/* A David/navigation setting changed: save, rebuild the walkable grid,
   and put David back on a spot that is valid under the new rules. */
static void settings_applied(int nav) {
    room_settings_save();
    david_settings_save();
    if (fabsf(g_view_zoom - g_room_zoom) > 1e-3f && !g_camslide.active) { float cx, cy; view_center(&cx, &cy); view_set(cx, cy, g_room_zoom); }
    if (!nav) return;
    nav_params_changed();
    rebuild_nav_after_zone_edit();
    if (g_has_3d_character && g_room_mesh_tri_count > 0) {
        g_char_moving = 0; g_char_waypoint_count = 0; g_pending_door = -1;
        if (!stand_ok(g_char_pos[0], g_char_pos[1], g_char_pos[2], g_room_mesh_tris, g_room_mesh_tri_count)) {
            float sp[3];
            if (navgrid_snap(&g_nav_grid, g_char_pos[0], g_char_pos[1], g_char_pos[2], sp)) memcpy(g_char_pos, sp, sizeof(sp));
        }
    }
}

/* One place for every action (buttons and keyboard shortcuts alike). */
static void ui_action(HWND hwnd, int id) {
    if (id >= B_SV_FIRST_ID && id <= B_SV_LAST_ID && !(id >= 900 && id < 1000)) { sv_action(hwnd, id); InvalidateRect(hwnd, NULL, FALSE); return; }
    switch (id) {
        case B_PREV: go_to_room_index(hwnd, g_current_map_room < 0 ? 0 : g_current_map_room - 1); break;
        case B_NEXT: go_to_room_index(hwnd, g_current_map_room + 1); break;
        case B_LIST:
            g_map_mode = 1; g_map_filter[0] = 0; g_map_filter_len = 0;
            map_recompute_filter(); map_select_current_room();
            break;
        case B_WALK: g_show_walkable = !g_show_walkable; break;
        case B_HITBOX: g_show_collision = !g_show_collision; break;
        case B_MESH: g_view_mode_3d = !g_view_mode_3d; break;
        case B_FREECAM: if (g_loaded && (g_edit_mode || g_sv_pick)) fcam_toggle(); break;
        case B_COLL: g_collision_enabled = !g_collision_enabled; break;
        case B_FULL: {
            int ww0, wh0, ox0, oy0; float sc0;
            get_view_window(&ww0, &wh0, &sc0, &ox0, &oy0);
            int keep_cx = g_cam_x + ww0 / 2, keep_cy = g_cam_y + wh0 / 2;
            toggle_fullscreen(hwnd);
            int ww1, wh1, ox1, oy1; float sc1;
            get_view_window(&ww1, &wh1, &sc1, &ox1, &oy1);
            g_cam_x = keep_cx - ww1 / 2; g_cam_y = keep_cy - wh1 / 2;
            clamp_camera();
            break;
        }
        case B_EDIT:
            if (!g_has_3d_character) break;
            if (g_edit_mode && g_wiz != WIZ_NONE) wizard_cancel(NULL);
            g_edit_mode = !g_edit_mode; g_sel = -1; edit_set_tool(TOOL_SELECT);
            snprintf(g_status, sizeof(g_status), g_edit_mode ? "scene editor ON" : "scene editor OFF");
            break;
        case B_T_SELECT: edit_set_tool(TOOL_SELECT); break;
        case B_T_RECT: edit_set_tool(TOOL_RECT); break;
        case B_T_CIRCLE: edit_set_tool(TOOL_CIRCLE); break;
        case B_T_POLY: edit_set_tool(TOOL_POLY); break;
        case B_UNDO: if (undo_pop()) snprintf(g_status, sizeof(g_status), "undone"); break;
        case B_DELETE: if (g_sel >= 0) shape_delete(g_sel); break;
        case B_R_RED: if (g_sel >= 0) shape_set_nav(g_sel, NAV_ROLE_RED); break;
        case B_R_GREEN: if (g_sel >= 0) shape_set_nav(g_sel, NAV_ROLE_GREEN); break;
        case B_R_FG: if (g_sel >= 0) shape_toggle_fg(g_sel); break;
        case B_R_DOOR: if (g_sel >= 0) shape_toggle_door(g_sel); break;
        case B_R_REDO: if (g_sel >= 0 && g_shapes[g_sel].door) { g_wiz = WIZ_STEP; g_wiz_shape = g_sel; g_wiz_repick_only = 1; g_wiz_other_side = 0; } break;
        case B_R_RETARGET: if (g_sel >= 0 && g_shapes[g_sel].door) { g_wiz_shape = g_sel; g_wiz_repick_only = 0; g_wiz_other_side = 0; wizard_open_target_list(); } break;
        case B_R_SCRIPT:
            if (g_sel >= 0 && g_shapes[g_sel].door && g_shapes[g_sel].target[0] &&
                ask_arrival_script(g_shapes[g_sel].target, g_shapes[g_sel].script, sizeof(g_shapes[g_sel].script))) {
                shapes_changed(0);
                snprintf(g_status, sizeof(g_status), "arrival script: %s", g_shapes[g_sel].script[0] ? g_shapes[g_sel].script : "none");
            }
            break;
        case B_SCRIPTS: script_view_toggle(); break;
        case B_SETTINGS: g_show_settings = !g_show_settings; break;
        case B_ANIMS: anim_view_toggle(); break;
        case B_AV_MODEL: case B_AV_MOVESET: case B_AV_PREV: case B_AV_PLAY: case B_AV_NEXT: case B_AV_STEPB: case B_AV_STEPF:
        case B_AV_SLOWER: case B_AV_FASTER: case B_AV_FOLLOW: case B_AV_RESET: case B_AV_CLOSE:
            anim_view_action(id); break;
        case B_SET_RESET: {
            for (int i = 0; i < SETTING_COUNT; i++) if (!g_settings[i].global) *g_settings[i].v = g_settings[i].def;
            settings_applied(1);
            snprintf(g_status, sizeof(g_status), "this room's David & navigation settings reset to defaults");
            break;
        }
        default:
            if (id >= B_SET_BASE && id < B_SET_BASE + 2 * SETTING_COUNT) {
                TunableSetting *st = &g_settings[(id - B_SET_BASE) / 2];
                float v = *st->v + (((id - B_SET_BASE) & 1) ? st->step : -st->step);
                if (v < st->min) v = st->min;
                if (v > st->max) v = st->max;
                *st->v = roundf(v / st->step) * st->step; /* no float drift */
                settings_applied(st->nav);
                char vb[32]; snprintf(vb, sizeof(vb), st->fmt, *st->v);
                snprintf(g_status, sizeof(g_status), "%s = %s (%s, saved)", st->label, vb, st->global ? "all rooms" : "this room");
            }
            break;
        case B_W_CANCEL: wizard_cancel(g_wiz == WIZ_OTHER_SHAPE ? "the connector stays one-way" : NULL); break;
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ---- panel drawing ---- */
/* Panel fonts, created once and kept (never DeleteObject them). */
static HFONT ui_font(int h, int bold) {
    static struct { int h, bold; HFONT f; } cache[16];
    static int n = 0;
    for (int i = 0; i < n; i++) if (cache[i].h == h && cache[i].bold == bold) return cache[i].f;
    HFONT f = CreateFontA(h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    if (n < 16) { cache[n].h = h; cache[n].bold = bold; cache[n].f = f; n++; }
    return f;
}
static void ui_fill(HDC hdc, const RECT *r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(hdc, r, b); DeleteObject(b); }
static void ui_frame(HDC hdc, const RECT *r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FrameRect(hdc, r, b); DeleteObject(b); }
static void ui_text(HDC hdc, int x, int y, int w, int h, const char *s, COLORREF c, UINT flags) {
    RECT r = { x, y, x + w, y + h }; SetTextColor(hdc, c); SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, s, -1, &r, flags | DT_NOPREFIX);
}
static int ui_para(HDC hdc, int x, int y, int w, const char *s, COLORREF c) { /* wrapped text, returns height */
    RECT r = { x, y, x + w, y + 400 }; SetTextColor(hdc, c); SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, s, -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    int hgt = r.bottom - r.top; r.right = x + w;
    DrawTextA(hdc, s, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
    return hgt;
}

static void ui_draw_buttons(HDC hdc) {
    HFONT f = ui_font(15, 0), fk = ui_font(12, 0);
    for (int i = 0; i < g_btn_count; i++) {
        UiButton *b = &g_btn[i];
        int hover = (b->id == g_hover_btn);
        COLORREF bg = !b->enabled ? RGB(28, 28, 32) : b->on ? (b->kind == 3 ? RGB(70, 60, 20) : RGB(38, 70, 110)) : hover ? RGB(58, 58, 66) : RGB(40, 40, 46);
        COLORREF border = b->on ? (b->kind == 3 ? RGB(230, 190, 70) : RGB(90, 160, 240)) : hover ? RGB(150, 150, 160) : RGB(70, 70, 80);
        ui_fill(hdc, &b->r, bg);
        ui_frame(hdc, &b->r, border);
        SelectObject(hdc, f);
        COLORREF tc = !b->enabled ? RGB(100, 100, 105) : RGB(235, 235, 240);
        int kw = 0;
        if (b->key && b->key[0]) {
            SIZE ks; SelectObject(hdc, fk); GetTextExtentPoint32A(hdc, b->key, (int)strlen(b->key), &ks);
            kw = ks.cx + 10;
            RECT kr = { b->r.right - kw - 4, b->r.top + 5, b->r.right - 4, b->r.bottom - 5 };
            ui_fill(hdc, &kr, RGB(22, 22, 26));
            ui_text(hdc, kr.left, kr.top, kr.right - kr.left, kr.bottom - kr.top, b->key, RGB(170, 170, 180), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, f);
        }
        ui_text(hdc, b->r.left + 8, b->r.top, (b->r.right - b->r.left) - 12 - kw, b->r.bottom - b->r.top, b->label, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static const char *script_step_text(char *buf, size_t n, const char **sub); /* SCRIPTS */
static const char *wizard_step_text(char *buf, size_t n, const char **sub) {
    const char *st = script_step_text(buf, n, sub);
    if (st) return st;
    *sub = "";
    switch (g_wiz) {
        case WIZ_STEP:
            *sub = g_free_cam ? "Click the floor where David walks to when this connector is clicked (green: where he can stand). Fly: W A S D, Q / E, Shift faster; look: right button dragged; F4: back to the picture."
                              : "Click the floor where David walks to when this connector is clicked. It's also where he appears when he comes in through it. Only the green floor is accepted. Out of the picture? F4: free camera.";
            return g_wiz_other_side ? "Other side -- step 1/2: the doorstep" : "Connector -- step 1/3: the doorstep";
        case WIZ_ARRIVAL:
            *sub = g_free_cam ? "Click where David walks to, right after coming in through this connector. Fly: W A S D, Q / E, Shift faster; look: right button dragged; F4: back."
                              : "Click where David walks to, right after coming in through this connector. Out of the picture? F4: free camera.";
            return g_wiz_other_side ? "Other side -- step 2/2: the arrival point" : "Connector -- step 2/3: the arrival point";
        case WIZ_TARGET:
            *sub = "Pick the room this connector leads to in the list (type to filter, Enter).";
            return "Connector -- step 3/3: the target room";
        case WIZ_OTHER_SHAPE:
            snprintf(buf, n, "Other side of the connector from %s", g_wiz_src_room);
            *sub = "Draw the connector on this side (Rectangle / Circle / Polygon), or click an existing shape. Then you'll place its two points. Esc: leave it one-way.";
            return buf;
    }
    return NULL;
}

static void ui_draw_panels(HWND hwnd, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    ui_fill(hdc, &g_panel_left, RGB(18, 18, 22));
    if (g_panel_right.right > g_panel_right.left) ui_fill(hdc, &g_panel_right, RGB(18, 18, 22));
    int pad = 10;
    int lx = g_panel_left.left + pad, lw = (g_panel_left.right - g_panel_left.left) - 2 * pad;
    HFONT fh = ui_font(20, 1), fs = ui_font(12, 1), fn = ui_font(14, 0);
    /* header: room */
    SelectObject(hdc, fh);
    char room[160];
    snprintf(room, sizeof(room), "%s", g_current_map_room >= 0 ? g_map_rooms[g_current_map_room].label : "(room)");
    ui_text(hdc, lx, 10, lw, 26, room, RGB(255, 225, 120), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, fs);
    const UiButton *bl = ui_find(B_WALK), *be = ui_find(B_EDIT);
    if (bl) ui_text(hdc, lx, bl->r.top - 17, lw, 15, "VIEW", RGB(120, 150, 190), DT_LEFT | DT_SINGLELINE);
    if (be) ui_text(hdc, lx, be->r.top - 17, lw, 15, "SCENE EDITOR", RGB(120, 150, 190), DT_LEFT | DT_SINGLELINE);

    ui_draw_buttons(hdc);

    /* info column: right bar (fullscreen) or below the buttons (windowed) */
    int ix, iw, iy;
    if (g_is_fullscreen) { ix = g_panel_right.left + pad; iw = (g_panel_right.right - g_panel_right.left) - 2 * pad; }
    else { ix = lx; iw = lw; }
    iy = g_ui_caps_y;
    if (g_edit_mode && g_show_settings) {
        SelectObject(hdc, fs);
        char sh[200];
        snprintf(sh, sizeof(sh), "DAVID & NAVIGATION -- THIS ROOM (%s)", g_settings_room);
        ui_text(hdc, ix, iy, iw, 16, sh, RGB(120, 150, 190), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, fn);
        for (int i = 0; i < SETTING_COUNT; i++) {
            const UiButton *mb = ui_find(B_SET_BASE + 2 * i);
            if (!mb) continue;
            TunableSetting *st = &g_settings[i];
            char vb[32]; snprintf(vb, sizeof(vb), st->fmt, *st->v);
            int changed = fabsf(*st->v - st->def) > 1e-4f;
            int hov = (g_hover_btn == B_SET_BASE + 2 * i || g_hover_btn == B_SET_BASE + 2 * i + 1);
            if (st->global && (i == 0 || !g_settings[i - 1].global))
                ui_text(hdc, ix, mb->r.top - 18, iw, 16, "DAVID  (all rooms)", RGB(120, 150, 190), DT_LEFT | DT_SINGLELINE);
            ui_text(hdc, ix, mb->r.top, mb->r.left - ix - 50, 22, st->label, hov ? RGB(255, 255, 255) : RGB(210, 210, 215), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ui_text(hdc, mb->r.left - 50, mb->r.top, 44, 22, vb, changed ? RGB(255, 210, 80) : RGB(150, 200, 255), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        iy = g_ui_next_y + 6;
        goto after_selection;
    }
    /* caps lock state */
    int caps = caps_on();
    RECT cr = { ix, iy, ix + iw, iy + 44 };
    ui_fill(hdc, &cr, caps ? RGB(20, 50, 30) : RGB(34, 30, 20));
    ui_frame(hdc, &cr, caps ? RGB(80, 200, 110) : RGB(150, 120, 60));
    SelectObject(hdc, fs);
    ui_text(hdc, ix + 8, iy + 4, iw - 16, 16, caps ? "CAPS LOCK ON -- TOOLS" : "CAPS LOCK OFF -- PLAYING", caps ? RGB(120, 240, 150) : RGB(240, 200, 110), DT_LEFT | DT_SINGLELINE);
    SelectObject(hdc, fn);
    ui_text(hdc, ix + 8, iy + 21, iw - 16, 18, caps ? "Mouse free: use the buttons." : "Mouse kept in the picture.", RGB(200, 200, 200), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    iy += 52;

    /* selected shape */
    if (g_edit_mode && g_sel >= 0 && g_sel < g_shape_count && g_wiz == WIZ_NONE) {
        Shape *s = &g_shapes[g_sel];
        SelectObject(hdc, fs);
        char t[160];
        snprintf(t, sizeof(t), "SELECTED SHAPE #%d (%s)", s->id, s->type == 0 ? "rectangle" : s->type == 1 ? "circle" : "polygon");
        ui_text(hdc, ix, g_ui_sel_y, iw, 16, t, RGB(120, 150, 190), DT_LEFT | DT_SINGLELINE);
        if (s->door) {
            const UiButton *redo = ui_find(B_R_REDO);
            int ty = redo ? redo->r.bottom + 4 : iy;
            SelectObject(hdc, fn);
            snprintf(t, sizeof(t), "Leads to: %s%s", s->target[0] ? s->target : "(not set)", s->target[0] && s->target_door < 0 ? " -- one-way" : "");
            ui_text(hdc, ix, ty, iw, 18, t, RGB(140, 200, 255), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            snprintf(t, sizeof(t), "Plays on arrival: %s", s->script[0] ? s->script : "no script");
            ui_text(hdc, ix, ty + 20, iw, 18, t, s->script[0] ? RGB(255, 210, 110) : RGB(140, 140, 150), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    iy = g_ui_next_y + 6;
after_selection:;

    /* connector wizard step (always visible while active) */
    char wbuf[200]; const char *sub;
    const char *step = wizard_step_text(wbuf, sizeof(wbuf), &sub);
    if (step) {
        SelectObject(hdc, fn);
        RECT meas = { 0, 0, iw - 20, 0 };
        DrawTextA(hdc, sub, -1, &meas, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        HFONT ft = ui_font(16, 1);
        SelectObject(hdc, ft);
        RECT tmeas = { 0, 0, iw - 20, 0 };
        DrawTextA(hdc, step, -1, &tmeas, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        int th = tmeas.bottom - tmeas.top;
        int bh = 8 + th + 6 + (meas.bottom - meas.top) + 10;
        RECT br = { ix, iy, ix + iw, iy + bh };
        ui_fill(hdc, &br, RGB(46, 38, 10));
        ui_frame(hdc, &br, RGB(255, 210, 60));
        ui_para(hdc, ix + 10, iy + 8, iw - 20, step, RGB(255, 220, 90));
        SelectObject(hdc, fn);
        ui_para(hdc, ix + 10, iy + 8 + th + 6, iw - 20, sub, RGB(235, 235, 235));
        iy += bh + 10;
    }

    /* help: hovered button, else what the current tool does */
    const char *help = NULL; char hbuf[300];
    const UiButton *hb = ui_find(g_hover_btn);
    if (hb) {
        const char *title = hb->label;
        if (hb->id >= B_SET_BASE && hb->id < B_SET_BASE + 2 * SETTING_COUNT) title = g_settings[(hb->id - B_SET_BASE) / 2].label;
        snprintf(hbuf, sizeof(hbuf), "%s%s%s%s\n%s", title, hb->key && hb->key[0] ? "   [" : "", hb->key ? hb->key : "", hb->key && hb->key[0] ? "]" : "", hb->desc);
        help = hbuf;
    } else if (g_edit_mode && (g_wiz == WIZ_STEP || g_wiz == WIZ_ARRIVAL)) {
        help = NULL; /* the step box says it all */
    } else if (g_edit_mode) {
        switch (g_tool) {
            case TOOL_SELECT: help = "SELECT: click a shape to select it. Drag it to move it; drag its white squares to reshape it. Right-click a shape to give it a role. Del deletes, Ctrl+Z undoes."; break;
            case TOOL_RECT: help = "RECTANGLE: drag on the picture. Then right-click the shape to give it a role."; break;
            case TOOL_CIRCLE: help = "CIRCLE: drag from the centre. Then right-click the shape to give it a role."; break;
            case TOOL_POLY: help = "POLYGON: click each corner, then click the first corner (or press Enter) to close it. Backspace removes the last corner, Esc cancels."; break;
        }
    } else {
        help = attack_mode()
            ? "ATTACK MODE (Ctrl held): click = an attack (one of three, toward the point clicked). Hold the button and swing the mouse: up = thrust, left / right = side blows, down = blow turning around. Right click = dodge back, right button held = shield up."
            : "Click to walk, double-click to run. Right click: the menu (weapons, shields, specials...). Hold Ctrl for attack mode. Mouse at an edge of the picture scrolls the view. Press CAPS LOCK to use the buttons.";
    }
    if (help) {
        SelectObject(hdc, fs);
        int top = g_is_fullscreen ? rc.bottom - 230 : iy;
        if (top < iy) top = iy;
        ui_text(hdc, ix, top, iw, 16, "HELP", RGB(120, 150, 190), DT_LEFT | DT_SINGLELINE);
        SelectObject(hdc, fn);
        int hh = ui_para(hdc, ix, top + 18, iw, help, RGB(210, 210, 215));
        iy = top + 18 + hh + 10;
    }
    /* status line */
    if (g_status[0]) {
        SelectObject(hdc, fn);
        int top = g_is_fullscreen ? rc.bottom - 100 : iy;
        if (top < iy) top = iy;
        ui_para(hdc, ix, top, iw, g_status, RGB(150, 150, 150));
    }
    SelectObject(hdc, GetStockObject(SYSTEM_FONT));
}

/* ---------------- composite <-> screen helpers ---------------- */
static void room_to_client(float x, float y, int *sx, int *sy) {
    int win_w, win_h, ox, oy; float sc;
    get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    *sx = ox + (int)((x - g_cam_x) * sc + 0.5f);
    *sy = oy + (int)((y - g_cam_y) * sc + 0.5f);
}
static float view_scale_now(void) { int a, b, c, d; float sc; get_view_window(&a, &b, &sc, &c, &d); return sc; }
static void game_rect_client(RECT *r) {
    int win_w, win_h, ox, oy; float sc;
    get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    SetRect(r, ox, oy, ox + (int)(win_w * sc + 0.5f), oy + (int)(win_h * sc + 0.5f));
}

/* Editor overlays drawn in SCREEN space on top of the scaled picture, so
   outlines, handles and labels stay crisp and constant-size. */
static void draw_shape_path(HDC hdc, const Shape *z) {
    int sx, sy, sx2, sy2;
    if (z->type == 0) {
        room_to_client(fminf(z->p[0][0], z->p[1][0]), fminf(z->p[0][1], z->p[1][1]), &sx, &sy);
        room_to_client(fmaxf(z->p[0][0], z->p[1][0]), fmaxf(z->p[0][1], z->p[1][1]), &sx2, &sy2);
        Rectangle(hdc, sx, sy, sx2 + 1, sy2 + 1);
    } else if (z->type == 1) {
        room_to_client(z->p[0][0] - z->p[1][0], z->p[0][1] - z->p[1][0], &sx, &sy);
        room_to_client(z->p[0][0] + z->p[1][0], z->p[0][1] + z->p[1][0], &sx2, &sy2);
        Ellipse(hdc, sx, sy, sx2, sy2);
    } else if (z->n > 0) {
        room_to_client(z->p[0][0], z->p[0][1], &sx, &sy); MoveToEx(hdc, sx, sy, NULL);
        for (int k = 1; k <= z->n; k++) { room_to_client(z->p[k % z->n][0], z->p[k % z->n][1], &sx, &sy); LineTo(hdc, sx, sy); }
    }
}

static COLORREF shape_color(const Shape *z) {
    if (z->door) return RGB(90, 170, 255);
    if (z->nav == NAV_ROLE_RED) return RGB(255, 70, 70);
    if (z->nav == NAV_ROLE_GREEN) return RGB(70, 255, 120);
    if (z->fg) return RGB(220, 110, 255);
    return RGB(230, 230, 230);
}

static void draw_world_point(HDC hdc, const float w[3], const char *label, COLORREF col) {
    float px, py, pz;
    if (!camera_world_to_pixel_z(view_cam(), w, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) return;
    int x, y; room_to_client(px, py, &x, &y);
    HPEN pen = CreatePen(PS_SOLID, 2, col); HPEN old = (HPEN)SelectObject(hdc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(BLACK_BRUSH));
    Ellipse(hdc, x - 8, y - 8, x + 9, y + 9);
    SelectObject(hdc, ob); SelectObject(hdc, old); DeleteObject(pen);
    HFONT f = ui_font(13, 1); HFONT of = (HFONT)SelectObject(hdc, f);
    ui_text(hdc, x - 8, y - 8, 17, 17, label[0] == '1' ? "1" : "2", col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ui_text(hdc, x + 12, y - 9, 160, 18, label, col, DT_LEFT | DT_SINGLELINE);
    SelectObject(hdc, of);
}

static int sv_pick_marker(float out[3]); /* SCRIPTS */
static void draw_editor_overlays(HDC hdc) {
    if (!g_has_3d_character) return;
    int show = g_edit_mode || g_show_walkable || g_sv_pick;
    if (!show) return;
    float pm[3];
    if (sv_pick_marker(pm)) draw_world_point(hdc, pm, "1 the character", RGB(255, 220, 60));
    if (g_free_cam) { /* free camera: David, and the connectors' points (the one being set up first) */
        for (int i = 0; i < g_shape_count; i++) {
            const Shape *z = &g_shapes[i];
            if (!z->door) continue;
            int cur = i == g_wiz_shape || i == g_sel;
            if (z->has_step) draw_world_point(hdc, z->step, cur ? "1 doorstep" : "1", cur ? RGB(255, 220, 60) : RGB(150, 130, 60));
            if (z->has_arrival) draw_world_point(hdc, z->arrival, cur ? "2 arrival" : "2", cur ? RGB(80, 230, 255) : RGB(60, 130, 150));
        }
        float dp[3] = { DAVID_ACTOR->pos[0], DAVID_ACTOR->pos[1], DAVID_ACTOR->pos[2] }, px, py, pz;
        if (camera_world_to_pixel_z(&g_fcam, dp, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) {
            int x, y; room_to_client(px, py, &x, &y);
            RECT r = { x - 5, y - 5, x + 6, y + 6 };
            ui_fill(hdc, &r, RGB(255, 120, 60));
            ui_text(hdc, x + 9, y - 9, 80, 18, "David", RGB(255, 150, 90), DT_LEFT | DT_SINGLELINE);
        }
        return;
    }
    if (g_ov_show >= 0 && g_ov_show < g_ov_n) { /* an overlay being placed */
        RoomOverlay *o = &g_ov[g_ov_show];
        int x0, y0, x1, y1;
        room_to_client((float)o->x, (float)o->y, &x0, &y0); room_to_client((float)(o->x + o->w), (float)(o->y + o->h), &x1, &y1);
        HPEN pen = CreatePen(PS_DASH, 1, RGB(255, 220, 60)); HPEN op = (HPEN)SelectObject(hdc, pen);
        Rectangle(hdc, x0, y0, x1, y1);
        SelectObject(hdc, op); DeleteObject(pen);
    }
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HFONT f = ui_font(13, 0); HFONT of = (HFONT)SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < g_shape_count; i++) {
        const Shape *z = &g_shapes[i];
        if (!g_edit_mode && !z->door && z->nav == NAV_ROLE_NONE) continue; /* P shows zones + connectors only */
        int sel = (i == g_sel && g_edit_mode);
        HPEN pen = CreatePen(z->fg && !z->door && z->nav == NAV_ROLE_NONE ? PS_DASH : PS_SOLID, sel ? 3 : 2, shape_color(z));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        if (z->fg && !sel) SetBkMode(hdc, TRANSPARENT);
        draw_shape_path(hdc, z);
        SelectObject(hdc, old); DeleteObject(pen);
        /* role label */
        char lab[160] = "";
        if (z->nav == NAV_ROLE_RED) strcat(lab, "no-go");
        if (z->nav == NAV_ROLE_GREEN) strcat(lab, "walkable");
        if (z->fg) { if (lab[0]) strcat(lab, " + "); strcat(lab, "foreground"); }
        if (z->door) { if (lab[0]) strcat(lab, " + "); strcat(lab, "-> "); strncat(lab, z->target[0] ? z->target : "(no target)", 60); }
        if (!lab[0] && g_edit_mode) strcpy(lab, "no role (right-click)");
        if (lab[0]) {
            float x0, y0, x1, y1; shape_bbox(z, &x0, &y0, &x1, &y1);
            int sx, sy; room_to_client(x0, y0, &sx, &sy);
            ui_text(hdc, sx + 1, sy - 17, 400, 16, lab, RGB(0, 0, 0), DT_LEFT | DT_SINGLELINE);
            ui_text(hdc, sx, sy - 18, 400, 16, lab, shape_color(z), DT_LEFT | DT_SINGLELINE);
        }
        if (sel) {
            float hx[SHAPE_MAX_PTS], hy[SHAPE_MAX_PTS];
            int nh = shape_handles(z, hx, hy);
            for (int k = 0; k < nh; k++) {
                int sx, sy; room_to_client(hx[k], hy[k], &sx, &sy);
                RECT hr = { sx - 5, sy - 5, sx + 6, sy + 6 };
                ui_fill(hdc, &hr, RGB(255, 255, 255));
                ui_frame(hdc, &hr, RGB(0, 0, 0));
            }
        }
        if (z->door && g_edit_mode && (sel || g_wiz_shape == i)) {
            if (z->has_step) draw_world_point(hdc, z->step, "1 doorstep", RGB(255, 220, 60));
            if (z->has_arrival) draw_world_point(hdc, z->arrival, "2 arrival", RGB(80, 230, 255));
        }
    }
    /* shape being drawn */
    if (g_edit_mode) {
        HPEN pp = CreatePen(PS_DASH, 1, RGB(255, 230, 90)); HPEN old = (HPEN)SelectObject(hdc, pp);
        SetBkMode(hdc, TRANSPARENT);
        if (g_drag == DRAG_NEW) {
            Shape t; memset(&t, 0, sizeof(t));
            if (g_tool == TOOL_RECT) { t.type = 0; t.n = 2; t.p[0][0] = g_drag_start[0]; t.p[0][1] = g_drag_start[1]; t.p[1][0] = g_mouse_room[0]; t.p[1][1] = g_mouse_room[1]; }
            else { float dx = g_mouse_room[0] - g_drag_start[0], dy = g_mouse_room[1] - g_drag_start[1];
                   t.type = 1; t.n = 2; t.p[0][0] = g_drag_start[0]; t.p[0][1] = g_drag_start[1]; t.p[1][0] = sqrtf(dx * dx + dy * dy); }
            draw_shape_path(hdc, &t);
        }
        if (g_tool == TOOL_POLY && g_poly.n > 0) {
            int sx, sy;
            room_to_client(g_poly.p[0][0], g_poly.p[0][1], &sx, &sy); MoveToEx(hdc, sx, sy, NULL);
            for (int k = 1; k < g_poly.n; k++) { room_to_client(g_poly.p[k][0], g_poly.p[k][1], &sx, &sy); LineTo(hdc, sx, sy); }
            room_to_client(g_mouse_room[0], g_mouse_room[1], &sx, &sy); LineTo(hdc, sx, sy);
            for (int k = 0; k < g_poly.n; k++) {
                room_to_client(g_poly.p[k][0], g_poly.p[k][1], &sx, &sy);
                int r = (k == 0 && g_poly.n >= 3) ? 7 : 4; /* first corner bigger once the shape can be closed */
                RECT hr = { sx - r, sy - r, sx + r + 1, sy + r + 1 };
                ui_fill(hdc, &hr, k == 0 && g_poly.n >= 3 ? RGB(255, 230, 90) : RGB(255, 255, 255));
                ui_frame(hdc, &hr, RGB(0, 0, 0));
            }
        }
        SelectObject(hdc, old); DeleteObject(pp);
    }
    SelectObject(hdc, of);
    SelectObject(hdc, ob);
}

/* ---------------- editor mouse ---------------- */
static int handle_hit(float rx, float ry) {
    if (g_sel < 0 || g_sel >= g_shape_count) return -1;
    float sc = view_scale_now(), tol = 7.0f / sc;
    float hx[SHAPE_MAX_PTS], hy[SHAPE_MAX_PTS];
    int nh = shape_handles(&g_shapes[g_sel], hx, hy);
    for (int k = 0; k < nh; k++) if (fabsf(hx[k] - rx) <= tol && fabsf(hy[k] - ry) <= tol) return k;
    Shape *s = &g_shapes[g_sel];
    if (s->door) { /* connector points: 100 doorstep, 101 arrival */
        for (int k = 0; k < 2; k++) {
            if (!(k == 0 ? s->has_step : s->has_arrival)) continue;
            float px, py, pz;
            if (!camera_world_to_pixel_z(&g_room_cam, k == 0 ? s->step : s->arrival, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) continue;
            if (fabsf(px - rx) <= 10.0f / sc && fabsf(py - ry) <= 10.0f / sc) return 100 + k;
        }
    }
    return -1;
}

static void edit_mouse_down(float x, float y) {
    if (g_wiz == WIZ_STEP || g_wiz == WIZ_ARRIVAL) { wizard_click(x, y); return; }
    if (g_free_cam) { snprintf(g_status, sizeof(g_status), "free camera: for placing points (a connector's, a script's) -- F4 / Esc: back to the picture to draw shapes"); return; }
    if (g_wiz == WIZ_OTHER_SHAPE && g_tool == TOOL_SELECT) {
        int i = shape_at(x, y);
        if (i >= 0) { wizard_start(i, 1); return; }
    }
    if (g_tool == TOOL_SELECT) {
        int h = handle_hit(x, y);
        if (h >= 0) {
            undo_push(); g_drag_changed = 0;
            g_drag = h >= 100 ? DRAG_POINT : DRAG_HANDLE; g_drag_handle = h;
            return;
        }
        int i = shape_at(x, y);
        g_sel = i;
        if (i >= 0) { undo_push(); g_drag = DRAG_MOVE; g_drag_changed = 0; g_drag_last[0] = x; g_drag_last[1] = y; }
        return;
    }
    if (g_tool == TOOL_POLY) {
        if (g_poly.n >= 3) {
            float sc = view_scale_now();
            float dx = x - g_poly.p[0][0], dy = y - g_poly.p[0][1];
            if (dx * dx + dy * dy <= (9.0f / sc) * (9.0f / sc)) { g_poly.type = 2; Shape z = g_poly; g_poly.n = 0; shape_add(&z); return; }
        }
        if (g_poly.n < SHAPE_MAX_PTS) { g_poly.p[g_poly.n][0] = x; g_poly.p[g_poly.n][1] = y; g_poly.n++; }
        return;
    }
    g_drag = DRAG_NEW;
    g_drag_start[0] = x; g_drag_start[1] = y;
}

static void edit_mouse_move(float x, float y) {
    g_mouse_room[0] = x; g_mouse_room[1] = y;
    if (g_sel < 0 || g_sel >= g_shape_count) return;
    if (g_drag == DRAG_MOVE) {
        float dx = x - g_drag_last[0], dy = y - g_drag_last[1];
        if (dx != 0 || dy != 0) { shape_translate(&g_shapes[g_sel], dx, dy); g_drag_changed = 1; g_shape_gen++; }
        g_drag_last[0] = x; g_drag_last[1] = y;
    } else if (g_drag == DRAG_HANDLE) {
        shape_move_handle(&g_shapes[g_sel], g_drag_handle, x, y); g_drag_changed = 1; g_shape_gen++;
    }
}

static void edit_mouse_up(float x, float y) {
    int d = g_drag; g_drag = DRAG_NONE;
    if (d == DRAG_NEW) {
        Shape z; memset(&z, 0, sizeof(z));
        float sx = g_drag_start[0], sy = g_drag_start[1];
        if (g_tool == TOOL_RECT) {
            if (fabsf(x - sx) < 3 || fabsf(y - sy) < 3) return;
            z.type = 0; z.n = 2; z.p[0][0] = fminf(sx, x); z.p[0][1] = fminf(sy, y); z.p[1][0] = fmaxf(sx, x); z.p[1][1] = fmaxf(sy, y);
        } else {
            float r = sqrtf((x - sx) * (x - sx) + (y - sy) * (y - sy));
            if (r < 3) return;
            z.type = 1; z.n = 2; z.p[0][0] = sx; z.p[0][1] = sy; z.p[1][0] = r;
        }
        shape_add(&z);
        return;
    }
    if (g_sel < 0 || g_sel >= g_shape_count) return;
    if (d == DRAG_POINT) {
        Shape *s = &g_shapes[g_sel];
        float w[3];
        if (pick_floor_point(x, y, w)) {
            if (g_drag_handle == 100) memcpy(s->step, w, sizeof(w)); else memcpy(s->arrival, w, sizeof(w));
            shapes_changed(0);
            snprintf(g_status, sizeof(g_status), "connector point moved");
        } else {
            g_undo_count > 0 ? g_undo_count-- : 0; /* nothing changed */
            snprintf(g_status, sizeof(g_status), "David can't stand there -- the point didn't move");
        }
        return;
    }
    if (d == DRAG_MOVE || d == DRAG_HANDLE) {
        Shape *s = &g_shapes[g_sel];
        if (s->type == 0) { /* keep p0 = top-left */
            float x0 = fminf(s->p[0][0], s->p[1][0]), x1 = fmaxf(s->p[0][0], s->p[1][0]), y0 = fminf(s->p[0][1], s->p[1][1]), y1 = fmaxf(s->p[0][1], s->p[1][1]);
            s->p[0][0] = x0; s->p[0][1] = y0; s->p[1][0] = x1; s->p[1][1] = y1;
        }
        if (g_drag_changed) shapes_changed(s->nav != NAV_ROLE_NONE);
        else if (g_undo_count > 0) g_undo_count--; /* a plain click: nothing to undo */
    }
}

/* Right-click on a shape: its roles. */
static void edit_context_menu(HWND hwnd, float rx, float ry, int client_x, int client_y) {
    int i = shape_at(rx, ry);
    if (i < 0) {
        if (g_tool == TOOL_POLY && g_poly.n > 0) g_poly.n = 0; /* right-click on nothing cancels the polygon */
        return;
    }
    g_sel = i;
    Shape *s = &g_shapes[i];
    int vertex = -1;
    if (s->type == 2 && s->n > 3) {
        float sc = view_scale_now();
        for (int k = 0; k < s->n; k++) if (fabsf(s->p[k][0] - rx) <= 7 / sc && fabsf(s->p[k][1] - ry) <= 7 / sc) vertex = k;
    }
    HMENU m = CreatePopupMenu();
    char head[120];
    snprintf(head, sizeof(head), "Shape #%d -- roles", s->id);
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, head);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | (s->nav == NAV_ROLE_RED ? MF_CHECKED : 0), 1, "Red zone\tDavid can't go here");
    AppendMenuA(m, MF_STRING | (s->nav == NAV_ROLE_GREEN ? MF_CHECKED : 0), 2, "Green zone\tDavid may go here (steep/tight ok)");
    AppendMenuA(m, MF_STRING | (s->fg ? MF_CHECKED : 0), 3, "Foreground\tpicture drawn over characters");
    AppendMenuA(m, MF_STRING | (s->door ? MF_CHECKED : 0), 4, s->door ? "Scene connector\t(untick: remove both sides)" : "Scene connector...\tdoor to another room");
    if (s->door) {
        AppendMenuA(m, MF_SEPARATOR, 0, NULL);
        AppendMenuA(m, MF_STRING, 11, "Redo the doorstep / arrival points");
        AppendMenuA(m, MF_STRING, 12, "Change target room...");
        AppendMenuA(m, MF_STRING | (s->target[0] ? 0 : MF_GRAYED), 13, "Arrival script...");
    }
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    if (vertex >= 0) AppendMenuA(m, MF_STRING, 6, "Delete this corner");
    AppendMenuA(m, MF_STRING, 5, "Delete shape\tDel");
    POINT pt = { client_x, client_y }; ClientToScreen(hwnd, &pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    switch (cmd) {
        case 1: shape_set_nav(i, NAV_ROLE_RED); break;
        case 2: shape_set_nav(i, NAV_ROLE_GREEN); break;
        case 3: shape_toggle_fg(i); break;
        case 4: shape_toggle_door(i); break;
        case 11: ui_action(hwnd, B_R_REDO); break;
        case 12: ui_action(hwnd, B_R_RETARGET); break;
        case 13: ui_action(hwnd, B_R_SCRIPT); break;
        case 5: shape_delete(i); break;
        case 6:
            undo_push();
            memmove(&s->p[vertex], &s->p[vertex + 1], sizeof(s->p[0]) * (size_t)(s->n - vertex - 1));
            s->n--;
            shapes_changed(s->nav != NAV_ROLE_NONE);
            break;
    }
}

/* =====================================================================
   ATTACK MODE: Ctrl held (the selected members of the team -- David for
   now). A simple click: one of the 3 attacks of the moveset at random,
   toward the point clicked. The button held and the mouse swung (on the
   screen): up = swing up (lstab), left = swing left (rchp90a, turns him
   90 degrees left), right = swing right (rchp90c), down = swing down via
   his left or right (rchp180a / rchp180c) depending on the angle.
   An attack asked for while one plays comes right after it.
   ===================================================================== */
#define ATK_GESTURE_PX 36
static int g_atk_press = 0, g_atk_x = 0, g_atk_y = 0;
static int attack_mode(void) {
    return (GetKeyState(VK_CONTROL) & 0x8000) && g_loaded && g_has_3d_character && !g_edit_mode && !script_playing() &&
           !screen_view() && !g_sv_pick && !g_tr.phase && !g_map_mode && !g_radial;
}
/* How far a clip turns the hips (radians, > 0 = left): the blows that turn
   the body leave it facing the new way. Under 45 degrees: not a turn. */
static float clip_turn(Actor *a, int clip) {
    float d = anim_lib_duration(clip);
    if (d <= 0.0f) return 0.0f;
    actor_begin(a);
    float t = wrap_angle(turn_clip_hip_yaw(clip, d - 1e-3f) - turn_clip_hip_yaw(clip, 0.0f));
    actor_end();
    return fabsf(t) > 0.785f ? t : 0.0f;
}
/* A step of `diameters` hitbox diameters (< 0 backward) with the clip
   about to play: as far as the floor allows (a wall, a ledge: shorter). */
static void actor_plan_step(Actor *a, float diameters) {
    a->stepping = 0;
    if (fabsf(diameters) < 1e-3f) return;
    float dist = diameters * 2.0f * g_nav.body_radius;
    float dx = sinf(a->facing) * dist, dz = cosf(a->facing) * dist;
    for (int tries = 0; tries < 4; tries++, dx *= 0.5f, dz *= 0.5f) {
        float to[3] = { a->pos[0] + dx, a->pos[1], a->pos[2] + dz };
        if (g_room_mesh_tri_count > 0 && g_collision_enabled) {
            float gy;
            if (!floor_below(to[0], to[2], a->pos[1], NAV_MAX_STEP, g_room_mesh_tris, g_room_mesh_tri_count, &gy)) continue;
            to[1] = gy;
            if (!path_is_walkable(a->pos, to, g_room_mesh_tris, g_room_mesh_tri_count, CHAR_RADIUS, NAV_MAX_STEP, g_room_floor_y - 12.0f)) continue;
        }
        memcpy(a->step_from, a->pos, sizeof(a->step_from));
        memcpy(a->step_to, to, sizeof(a->step_to));
        a->stepping = 1;
        return;
    }
}
/* Plays `clip` once over its pose, stopping where it stands; with a step
   and the turn the clip makes. */
static void actor_play_once(Actor *a, int clip, float step) {
    if (a->turn_clip >= 0) { a->facing = a->turn_to; a->turn_clip = -1; } /* it stops where it stands */
    a->moving = 0; a->walk_mode = 0; a->waypoint_count = 0; a->pending_door = -1;
    if (a == DAVID_ACTOR) g_click_marker_active = 0;
    a->play_clip = clip; a->play_t = 0.0f; a->play_left = 1; a->play_attack = 0; a->play_next = -1; a->trail = 0; a->play_hold = 0; a->play_speed = 0.0f;
    a->play_script = 0; a->play_ended = 0; a->play_freeze = 0;
    a->blow = 0; a->dodging = 0; a->hold_last = 0; a->play_rev = 0; a->cast_pending = 0;
    a->play_turn = clip_turn(a, clip);
    actor_plan_step(a, step);
}
/* a clip of the library by name, if it fits the character */
static int actor_clip(const Actor *a, const char *name) {
    int c = anim_lib_find(name);
    return (c >= 0 && anim_lib_fits(c, a->model->node_count)) ? c : -1;
}
static int actor_busy(const Actor *a) { return a->play_clip >= 0 || a->guard; }

static void actor_dodge_rolls(Actor *a);
static int actor_attack(Actor *a, int slot);
static int weapon_element(const char *w);
static float cast_moment(const char *clip);
static void david_attack(int slot) {
    if (DAVID_ACTOR->down || DAVID_ACTOR->dead) return;
    actor_attack(DAVID_ACTOR, slot);
}
/* a blow of any character (David: attack mode; the others: their AI). 1 if it started */
static int actor_attack(Actor *a, int slot) {
    actor_resolve_clips(a);
    if (a->guard) return 0; /* behind its shield */
    if (a->play_clip >= 0 && a->play_attack) { if (a == DAVID_ACTOR) g_atk_queued = slot; return 0; }
    if (a->play_clip >= 0) return 0; /* busy (dodging, sheathing...) */
    if (slot == MC_ATK1) { /* a click: one of the three attacks at random */
        int c[3], n = 0;
        for (int k = MC_ATK1; k <= MC_ATK3; k++) if (a->clips[k] >= 0) c[n++] = k;
        if (n) slot = c[rand() % n];
    }
    int clip = a->clips[slot];
    if (clip < 0) { if (a == DAVID_ACTOR) snprintf(g_status, sizeof(g_status), "attack: no animation for '%s' in %s's moveset", MOVE_SLOT_LABEL[slot], a->model->name); return 0; }
    const Moveset *ms = moveset_get(a->model->name[0] ? a->model->name : "david");
    actor_play_once(a, clip, moveset_step_w(ms, slot, a->weapon));
    a->play_attack = 1;
    a->blow = 1; a->hit_mask = 0;
    if (weapon_element(a->weapon)) { /* magic: no blade, a spell at the cast gesture */
        a->blow = 0;
        a->cast_pending = 1; a->cast_clip = clip; a->cast_elem = weapon_element(a->weapon);
        a->cast_at = anim_lib_duration(clip) * cast_moment(anim_lib_name(clip));
    }
    cue_start(&a->cue, moveset_cues_w(ms, slot, a->weapon)); /* its sounds (the ones at 0 s: now) */
    cue_run(&a->cue, 0.0f, moveset_sounds(ms));
    actor_dodge_rolls(a);
    return 1;
}
/* every step: the attack's sounds go on, in step with the animation */
static void actor_cue_tick(Actor *a, float dt) {
    if (!a->cue.on) return;
    cue_run(&a->cue, dt * g_david_anim_speed, moveset_sounds(moveset_get(a->model && a->model->name[0] ? a->model->name : "david")));
}
/* the button went down in the picture, in attack mode */
static void attack_press(HWND hwnd, int cx, int cy) { g_atk_press = 1; g_atk_x = cx; g_atk_y = cy; SetCapture(hwnd); }
/* the mouse moved with the button held: a swing once it's gone far enough */
static int attack_drag(int cx, int cy) {
    if (!g_atk_press) return 0;
    int dx = cx - g_atk_x, dy = cy - g_atk_y;
    if (dx * dx + dy * dy < ATK_GESTURE_PX * ATK_GESTURE_PX) return 1;
    g_atk_press = 0; ReleaseCapture();
    int slot;
    if (abs(dy) >= abs(dx)) slot = dy < 0 ? MC_SW_UP : (dx < 0 ? MC_SW_BACKL : MC_SW_BACKR);
    else slot = dx < 0 ? MC_SW_LEFT : MC_SW_RIGHT;
    david_attack(slot);
    return 1;
}
/* the button came up without a swing: a simple click, toward the point clicked */
static int attack_release(int cx, int cy) {
    if (!g_atk_press) return 0;
    g_atk_press = 0; ReleaseCapture();
    Actor *a = DAVID_ACTOR;
    int rx, ry; float w[3];
    client_to_room_point(cx, cy, &rx, &ry);
    if (!(a->play_clip >= 0 && a->play_attack) && !a->down && click_to_world((float)rx, (float)ry, w)) {
        float dx = w[0] - a->pos[0], dz = w[2] - a->pos[2];
        if (dx * dx + dz * dz > 0.04f) { a->facing = atan2f(dx, dz); if (a->turn_clip >= 0) a->turn_clip = -1; }
    }
    david_attack(MC_ATK1);
    return 1;
}

/* ---- combat mode, right button: a click dodges (dodgeb, 4 hitbox
   diameters back), held it raises the shield (shldup, then shldhold as long
   as it's held -- when it can counter hits --, shlddown when released) ---- */
#define GUARD_HOLD_S 0.22
#define DODGE_STEP -4.0f
static int g_guard_press = 0;   /* the right button is down, in combat mode */
static double g_guard_press_ms = 0;
static void guard_press(HWND hwnd) { g_guard_press = 1; g_guard_press_ms = perf_now_ms(); SetCapture(hwnd); }
static void david_dodge(void) {
    Actor *a = DAVID_ACTOR;
    actor_resolve_clips(a);
    int c = a->clips[MC_DODGE] >= 0 ? a->clips[MC_DODGE] : actor_clip(a, "dodgeb");
    if (c < 0 || actor_busy(a) || a->down) return;
    actor_play_once(a, c, DODGE_STEP);
    a->play_attack = 1;
    a->dodging = 1; /* blows miss him while it plays */
}
/* game_tick: held long enough -> the shield goes up */
static void guard_tick(void) {
    Actor *a = DAVID_ACTOR;
    if (!g_guard_press || a->guard || !a->shield[0] || perf_now_ms() - g_guard_press_ms < GUARD_HOLD_S * 1000.0) return;
    if (a->play_clip >= 0 && !a->play_attack) return; /* sheathing, equipping... */
    int up = actor_clip(a, "shldup"), hold = actor_clip(a, "shldhold");
    if (up < 0 || hold < 0) return;
    actor_play_once(a, up, 0.0f);
    a->play_next = hold; a->play_next_loop = 1;
    a->guard = 1;
}
static void guard_release(void) {
    if (!g_guard_press) return;
    g_guard_press = 0; ReleaseCapture();
    Actor *a = DAVID_ACTOR;
    if (a->guard) { /* the shield goes down */
        int down = actor_clip(a, "shlddown");
        a->guard = 0;
        a->play_next = -1;
        if (down >= 0) actor_play_once(a, down, 0.0f); else a->play_clip = -1;
        return;
    }
    david_dodge();
}

/* ---- equipment (the radial menu) ---- */
/* sheatmp: the weapon goes (or comes) back to its sheath; a new one comes out half way */
static void actor_equip_weapon(Actor *a, const char *name) {
    int c = actor_clip(a, "sheatmp");
    if (actor_busy(a)) { snprintf(g_status, sizeof(g_status), "busy"); return; }
    int away = !strcmp(a->weapon, name); /* the one it holds: put away */
    if (c < 0) { snprintf(a->weapon, sizeof(a->weapon), "%s", away ? "" : name); a->clips_gen = 0; return; }
    actor_play_once(a, c, 0.0f);
    a->play_event = EV_WEAPON; snprintf(a->event_arg, sizeof(a->event_arg), "%s", away ? "" : name);
    a->play_event_at = anim_lib_duration(c) * (away || !a->weapon[0] ? 0.9f : 0.5f);
    if (!away && !strcmp(name, "dualswrd") && a->shield[0]) a->shield[0] = 0, a->clips_gen = 0; /* two blades: no hand for a shield */
    snprintf(g_status, sizeof(g_status), away ? "weapon put away" : "weapon: %s", name);
}
/* shldquip: the shield comes out of (goes back into) the inventory */
static void actor_equip_shield(Actor *a, const char *name) {
    if (!strcmp(a->weapon, "dualswrd")) { snprintf(g_status, sizeof(g_status), "the double swords need both hands"); return; }
    if (actor_busy(a)) { snprintf(g_status, sizeof(g_status), "busy"); return; }
    int away = !strcmp(a->shield, name);
    int c = actor_clip(a, "shldquip");
    if (c < 0) { snprintf(a->shield, sizeof(a->shield), "%s", away ? "" : name); a->clips_gen = 0; return; }
    actor_play_once(a, c, 0.0f);
    a->play_event = EV_SHIELD; snprintf(a->event_arg, sizeof(a->event_arg), "%s", away ? "" : name);
    a->play_event_at = anim_lib_duration(c) * 0.6f;
    snprintf(g_status, sizeof(g_status), away ? "shield put away" : "shield: %s", name);
}
/* the specials: their animation, a blue trail behind the blades */
static void actor_special(Actor *a, const char *clip_name) {
    int c = actor_clip(a, clip_name);
    if (c < 0) { snprintf(g_status, sizeof(g_status), "'%s' isn't made for %s", clip_name, a->model->name); return; }
    if (actor_busy(a)) { snprintf(g_status, sizeof(g_status), "busy"); return; }
    actor_play_once(a, c, 0.0f);
    a->play_attack = 1;
    a->trail = 1;
    a->blow = 1; a->hit_mask = 0;
}

/* ---- weapon trails: the blades' segments over the last moments, fading ---- */
#define TRAIL_MAX 256
#define TRAIL_LIFE 0.35
typedef struct { float a[3], b[3]; double t; int who, hand, seq; } TrailSample;
static TrailSample g_trail[TRAIL_MAX];
static int g_trail_n = 0, g_trail_seq = 1;
static void trail_push(int who, int hand, const float a[3], const float b[3]) {
    if (g_trail_n == TRAIL_MAX) { memmove(g_trail, g_trail + 1, sizeof(TrailSample) * (TRAIL_MAX - 1)); g_trail_n--; }
    TrailSample *t = &g_trail[g_trail_n++];
    memcpy(t->a, a, sizeof(t->a)); memcpy(t->b, b, sizeof(t->b));
    t->t = perf_now_ms() / 1000.0; t->who = who; t->hand = hand; t->seq = g_trail_seq;
}
/* after actor_project: its blades in the world (the pose before kept, for the blows' sweep) */
static void blade_sample(Actor *a) {
    for (int hand = 0; hand < 2; hand++) {
        a->blade_prev_ok[hand] = a->blade_ok[hand];
        memcpy(a->blade_prev[hand], a->blade[hand], sizeof(a->blade[hand]));
        a->blade_ok[hand] = 0;
        const CharModel *it = hand ? a->item_l : a->item;
        float base[3], tip[3], h[3];
        if (!it || !(hand ? a->hand_l_ok : a->hand_ok) || !item_blade(it, base, tip)) continue;
        float cf = cosf(a->facing + 3.14159265f), sf = sinf(a->facing + 3.14159265f);
        const Mat4 *m = hand ? &a->hand_l : &a->hand;
        for (int e = 0; e < 2; e++) {
            mat4_vec3(m, e ? tip : base, h);
            a->blade[hand][e][0] = h[0] * cf + h[2] * sf + a->pos[0];
            a->blade[hand][e][1] = h[1] + a->pos[1];
            a->blade[hand][e][2] = -h[0] * sf + h[2] * cf + a->pos[2];
        }
        a->blade_ok[hand] = 1;
    }
}
/* after actor_project: the blades where they are now */
static void trail_sample(Actor *a) {
    for (int hand = 0; hand < 2; hand++) {
        const CharModel *it = hand ? a->item_l : a->item;
        if (!it || !(hand ? a->hand_l_ok : a->hand_ok)) continue;
        float base[3], tip[3], wb[3], wt[3], h[3];
        if (!item_blade(it, base, tip)) continue;
        float cf = cosf(a->facing + 3.14159265f), sf = sinf(a->facing + 3.14159265f);
        const Mat4 *m = hand ? &a->hand_l : &a->hand;
        mat4_vec3(m, base, h);
        wb[0] = h[0] * cf + h[2] * sf + a->pos[0]; wb[1] = h[1] + a->pos[1]; wb[2] = -h[0] * sf + h[2] * cf + a->pos[2];
        mat4_vec3(m, tip, h);
        wt[0] = h[0] * cf + h[2] * sf + a->pos[0]; wt[1] = h[1] + a->pos[1]; wt[2] = -h[0] * sf + h[2] * cf + a->pos[2];
        trail_push((int)(a - g_actors), hand, wb, wt);
    }
}
/* one screen triangle, blended (colour c, alpha 0..255) */
static void blend_tri(uint32_t *px, int W, int H, const float *x, const float *y, uint32_t c, uint32_t alpha) {
    int minx = (int)floorf(fminf(x[0], fminf(x[1], x[2]))), maxx = (int)ceilf(fmaxf(x[0], fmaxf(x[1], x[2])));
    int miny = (int)floorf(fminf(y[0], fminf(y[1], y[2]))), maxy = (int)ceilf(fmaxf(y[0], fmaxf(y[1], y[2])));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= H) maxy = H - 1;
    float den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
    if (fabsf(den) < 1e-6f) return;
    for (int yy = miny; yy <= maxy; yy++) for (int xx = minx; xx <= maxx; xx++) {
        float fx = xx + 0.5f, fy = yy + 0.5f;
        float w0 = ((y[1] - y[2]) * (fx - x[2]) + (x[2] - x[1]) * (fy - y[2])) / den;
        float w1 = ((y[2] - y[0]) * (fx - x[2]) + (x[0] - x[2]) * (fy - y[2])) / den;
        if (w0 < 0 || w1 < 0 || 1.0f - w0 - w1 < 0) continue;
        uint32_t *d = &px[(size_t)yy * W + xx];
        *d = lerp_rgb(*d, c, alpha);
    }
}
/* The trails (screen pixels of the view layer). */
static void trails_draw(uint32_t *px, int W, int H, float sc) {
    double now = perf_now_ms() / 1000.0;
    int keep = 0; /* drop the old ones */
    for (int i = 0; i < g_trail_n; i++) if (now - g_trail[i].t < TRAIL_LIFE) g_trail[keep++] = g_trail[i];
    g_trail_n = keep;
    for (int i = 1; i < g_trail_n; i++) {
        TrailSample *p0 = NULL, *p1 = &g_trail[i];
        for (int j = i - 1; j >= 0; j--) if (g_trail[j].who == p1->who && g_trail[j].hand == p1->hand) { p0 = &g_trail[j]; break; }
        if (!p0 || p0->seq != p1->seq) continue;
        Actor *a = &g_actors[p1->who];
        float qx[4], qy[4], qz;
        const float *pts[4] = { p0->a, p0->b, p1->b, p1->a };
        int ok = 1;
        for (int k = 0; k < 4; k++) {
            if (!char_project(a->pos, pts[k], &qx[k], &qy[k], &qz)) ok = 0;
            qx[k] = (qx[k] - g_cam_x) * sc; qy[k] = (qy[k] - g_cam_y) * sc;
        }
        if (!ok) continue;
        double age = now - p1->t;
        uint32_t alpha = (uint32_t)(150.0 * (1.0 - age / TRAIL_LIFE));
        float t1x[3] = { qx[0], qx[1], qx[2] }, t1y[3] = { qy[0], qy[1], qy[2] };
        float t2x[3] = { qx[0], qx[2], qx[3] }, t2y[3] = { qy[0], qy[2], qy[3] };
        blend_tri(px, W, H, t1x, t1y, 0x6EAAFF, alpha);
        blend_tri(px, W, H, t2x, t2y, 0x6EAAFF, alpha);
    }
}

/* ---- a shield breaking: it falls in pieces that shrink away, the
   shield is lowered and gone (lost for good -- the radial menu empties
   its slot). Nothing hits yet: Ctrl+K breaks it, to try. ---- */
#define DEBRIS_MAX 8
#define DEBRIS_LIFE 1.2f
typedef struct { CharModel *piece; float (*wpos)[3]; int nv; float cen[3], vel[3], off[3], age; } Debris;
static Debris g_debris[DEBRIS_MAX];
static char g_lost_items[16][48];
static int g_lost_count = 0;
static int item_lost(const char *name) { for (int i = 0; i < g_lost_count; i++) if (!strcmp(g_lost_items[i], name)) return 1; return 0; }
/* A hit taken: one of its grunts, maybe (its sound pools' chance). Nothing
   hits yet: Ctrl+H, to try. */
static void actor_hit(Actor *a) {
    const SoundPools *sp = moveset_sounds(moveset_get(a->model->name[0] ? a->model->name : "david"));
    const char *g = pool_pick(sp, POOL_GRUNT);
    if (g && rand() % 100 < sp->grunt_chance) audio_play(g, AUDIO_SOUND, 0, 0);
}
/* =====================================================================
   COMBAT. Two sides: David and his allies, the enemies. A blow (an attack
   or a special) lands when a blade touches the HITBOX of a character of
   the other side -- a cylinder around it, its body's radius, as tall as
   its model --, once per blow and target: the attacker's damage (stats),
   its hit sound, the target's grunt and its "hit" reaction. A raised
   shield blocks it. Health gone: an enemy dies (held on its last frame,
   for good), an ally is knocked down until no enemy is left, then gets up
   with its health back (David also after 10 s, so the player isn't stuck).
   ===================================================================== */
static float model_height(const CharModel *m) {
    static const CharModel *ms[64]; static float hs[64]; static int n = 0;
    for (int i = 0; i < n; i++) if (ms[i] == m) return hs[i];
    float h = 0.0f;
    for (int v = 0; v < m->vertex_count; v++) if (m->positions[v][1] > h) h = m->positions[v][1];
    if (h <= 0.0f) h = 1.8f;
    if (n < 64) { ms[n] = m; hs[n] = h; n++; }
    return h;
}
static void magic_tick(float dt);
static int actor_alive(const Actor *a) { return a->used && !a->dead && !a->down; }
static int actor_in_hitbox(const Actor *b, const float p[3]) {
    float r = g_nav.body_radius * 1.25f, dx = p[0] - b->pos[0], dz = p[2] - b->pos[2];
    return dx * dx + dz * dz < r * r && p[1] >= b->pos[1] - 0.1f && p[1] <= b->pos[1] + model_height(b->model) * 1.05f;
}
/* does a blade of a (now, or on its way since the pose before) go through b's hitbox? */
static int blade_touches(const Actor *a, const Actor *b) {
    for (int hand = 0; hand < 2; hand++) {
        if (!a->blade_ok[hand]) continue;
        int sweeps = a->blade_prev_ok[hand] ? 4 : 1;
        for (int s = 0; s < sweeps; s++) {
            float k = sweeps > 1 ? (float)s / (sweeps - 1) : 1.0f, seg[2][3];
            for (int e = 0; e < 2; e++) for (int j = 0; j < 3; j++)
                seg[e][j] = sweeps > 1 ? a->blade_prev[hand][e][j] + (a->blade[hand][e][j] - a->blade_prev[hand][e][j]) * k : a->blade[hand][e][j];
            for (int i = 0; i <= 6; i++) {
                float u = i / 6.0f, p[3] = { seg[0][0] + (seg[1][0] - seg[0][0]) * u, seg[0][1] + (seg[1][1] - seg[0][1]) * u, seg[0][2] + (seg[1][2] - seg[0][2]) * u };
                if (actor_in_hitbox(b, p)) return 1;
            }
        }
    }
    return 0;
}
static const Moveset *actor_moveset(const Actor *a) { return moveset_get(a->model && a->model->name[0] ? a->model->name : "david"); }
/* health gone: an enemy dies, an ally goes down (both held on their clip's last frame) */
static void actor_fall(Actor *b) {
    b->potion_m = NULL; b->pot_pending = 0; b->cast_pending = 0;
    actor_resolve_clips(b);
    int enemy = b->side == SIDE_ENEMY, c = b->clips[enemy ? MC_DEATH : MC_DOWN], loop = enemy ? -1 : b->clips[MC_DOWNLOOP];
    b->guard = 0; b->cue.on = 0; b->play_next = -1;
    if (c >= 0) {
        actor_play_once(b, c, 0.0f);
        if (loop >= 0) { b->play_next = loop; b->play_next_loop = 1; } /* down, then lying there */
        else b->hold_last = 1;
    }
    else if (loop >= 0) { actor_play_once(b, loop, 0.0f); b->play_left = -1; }
    else { b->moving = 0; b->waypoint_count = 0; b->play_clip = -1; }
    if (enemy) b->dead = 1; else { b->down = 1; b->down_t = 0.0f; }
    if (b == DAVID_ACTOR) g_click_marker_active = 0;
}
static void actor_get_up(Actor *b) {
    actor_resolve_clips(b);
    b->down = 0; b->hold_last = 0; b->play_ended = 0; b->play_clip = -1;
    b->hp = b->hp_max;
    if (b->clips[MC_GETUP] >= 0) actor_play_once(b, b->clips[MC_GETUP], 0.0f);
}
/* a's blow lands on b */
static void actor_take_damage(Actor *b, int dmg);
static void fx_spawn(const char *sheet, int first, int n, const float pos[3], float dur, float size, int add, const float *vel);
static void actor_take_blow(Actor *a, Actor *b) {
    if (b->guard) { audio_play("swrdblk1.ogg", AUDIO_SOUND, 0, 0); return; } /* on its shield */
    const char *hs = pool_pick(moveset_sounds(actor_moveset(a)), POOL_HIT);
    if (hs) audio_play(hs, AUDIO_SOUND, 0, 0);
    int dmg = moveset_stats(actor_moveset(a))->damage;
    if (a->str_t > 0.0f) dmg *= 2;
    if (!strcmp(a->weapon, "fireswrd")) { /* the fire sword burns */
        float c[3] = { b->pos[0], b->pos[1] + model_height(b->model) * 0.6f, b->pos[2] };
        fx_spawn("xplode", 0, 8, c, 0.45f, 1.0f, 0, NULL);
        audio_play("firehit.ogg", AUDIO_SOUND, 0, 0);
        dmg = dmg * 3 / 2;
    }
    actor_take_damage(b, dmg);
}
/* a starts a blow: the AI characters of the other side close to it may dodge it (their chance) */
static void actor_dodge_rolls(Actor *a) {
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *b = &g_actors[k];
        if (b == a || !actor_alive(b) || b->side == a->side || !b->ai_on || b == DAVID_ACTOR) continue;
        const AiSettings *ai = moveset_ai(actor_moveset(b));
        if (!ai || ai->kind != AI_MELEE || ai->dodge <= 0 || (b->play_clip >= 0 && !b->play_attack)) continue;
        float dx = b->pos[0] - a->pos[0], dz = b->pos[2] - a->pos[2], reach = 2.0f * g_nav.body_radius * 4.0f;
        if (dx * dx + dz * dz > reach * reach || rand() % 100 >= ai->dodge) continue;
        actor_resolve_clips(b);
        int c = b->clips[MC_DODGE];
        if (c < 0) continue;
        b->facing = atan2f(-dx, -dz); /* facing the blow, it jumps back */
        actor_play_once(b, c, -2.5f);
        b->dodging = 1;
    }
}
/* =====================================================================
   MAGIC. Elements (the orbs of the radial menu, the wands): their sprites
   (assets/sprites: <element>orb spinning in the hand, star.N the
   projectile's glow, twinkle / xplode / equipfx its trail, its blast, its
   burst when the orb comes out), a 3D projectile for some (acidbolt,
   iceshard), their sounds (assets/sound). A spell leaves the hands at the
   cast gesture of the clip (cast_moment), toward a foe in front if any.
   Health heals the caster, Time slows every foe around; ice slows the one
   it hits. Potions: drunk (drink, left hand) or thrown (throw: an arc,
   then a blast / a gas cloud).
   ===================================================================== */
enum { EL_NONE, EL_FIRE, EL_ICE, EL_HEALTH, EL_EARTH, EL_ACID, EL_LIGHTNING, EL_TIME, EL_LIGHT, EL_COUNT };
typedef struct {
    const char *name, *orb;               /* "orb:<name>", its spinning sprite */
    int star;                             /* star.N glow of the projectile, -1: none */
    const char *psheet; int pfirst, pn;   /* or an animated projectile sprite */
    const char *model;                    /* a 3D projectile, or NULL */
    const char *tsheet; int tfirst, tn, tadd; /* its trail */
    const char *bsheet; int bfirst, bn, badd; float bsize; /* its blast */
    int efirst, en;                       /* equipfx: the burst when the orb comes out */
    const char *snd_cast, *snd_hit;
    float speed, dmg, size;               /* units / s, x the caster's damage, the glow's size (units) */
} Element;
static const Element ELEM[EL_COUNT] = {
    [EL_FIRE] = { "fire", "fireorb", 17, NULL, 0, 0, NULL, "twinkle", 72, 8, 1, "xplode", 0, 8, 0, 1.3f, 0, 8, "fireball.ogg", "fireexp.ogg", 9.0f, 1.5f, 0.55f },
    [EL_ICE] = { "ice", "iceorb", 20, NULL, 0, 0, "iceshard", "twinkle", 16, 8, 1, "xplode", 19, 8, 0, 1.1f, 8, 8, "icea1.ogg", "iceplode.ogg", 10.0f, 1.1f, 0.45f },
    [EL_HEALTH] = { "health", "lifeorb", 23, NULL, 0, 0, NULL, "twinkle", 8, 8, 1, "twinkle", 80, 8, 1, 0.8f, 56, 4, "lifeheal.ogg", "lifeheal.ogg", 0.0f, 0.0f, 0.5f },
    [EL_EARTH] = { "earth", "earthorb", -1, "equipfx", 16, 8, NULL, "xplode", 29, 14, 0, "xplode", 29, 30, 0, 1.5f, 16, 8, "eartha1.ogg", "earthh1.ogg", 7.0f, 2.0f, 0.6f },
    [EL_ACID] = { "acid", "acidorb", 22, NULL, 0, 0, "acidbolt", "twinkle", 80, 4, 1, "equipfx", 60, 4, 0, 1.0f, 60, 4, "acida2.ogg", "acidhit1.ogg", 9.0f, 1.3f, 0.4f },
    [EL_LIGHTNING] = { "lightning", "lningorb", 18, NULL, 0, 0, NULL, "twinkle", 16, 8, 1, "xplode", 13, 2, 1, 1.2f, 32, 8, "lcast.ogg", "lighthit.ogg", 18.0f, 1.4f, 0.5f },
    [EL_TIME] = { "time", "timeorb", 19, NULL, 0, 0, NULL, "twinkle", 24, 8, 1, "twinkle", 24, 8, 1, 0.8f, 40, 8, "telemag.ogg", "telemag.ogg", 0.0f, 0.0f, 0.5f },
    [EL_LIGHT] = { "light", "lightorb", 16, NULL, 0, 0, NULL, "twinkle", 0, 8, 1, "equipfx", 48, 8, 1, 1.0f, 48, 8, "lightbeam.ogg", "lighthit.ogg", 14.0f, 1.2f, 0.5f },
};
static int weapon_element(const char *w) {
    if (!w || !w[0]) return EL_NONE;
    if (weapon_is_orb(w)) { for (int e = 1; e < EL_COUNT; e++) if (!strcmp(w + 4, ELEM[e].name)) return e; return EL_NONE; }
    if (!strcmp(w, "icewand")) return EL_ICE;
    if (!strcmp(w, "firewand")) return EL_FIRE;
    if (!strcmp(w, "lstaff") || !strcmp(w, "wandbolt")) return EL_LIGHTNING;
    if (!strcmp(w, "silvstaf") || !strcmp(w, "moonstik") || !strcmp(w, "cagstaff")) return EL_LIGHT;
    return EL_NONE;
}
/* when a clip's gesture sends the spell (from the hands' paths, measured: forward / up the most) */
static float cast_moment(const char *clip) {
    if (!strcmp(clip, "magkaim")) return 0.45f;
    if (!strcmp(clip, "mgkair")) return 0.5f;
    if (!strncmp(clip, "mgk", 3)) return 0.55f;
    if (!strcmp(clip, "fcast")) return 0.33f;
    if (!strcmp(clip, "conjure")) return 0.4f;
    if (!strncmp(clip, "cast", 4)) return 0.4f;
    return 0.45f;
}

/* ---- effects: sprites of assets/sprites in the world (a sheet's frames over their time) ---- */
#define FX_MAX 256
typedef struct { int used; const char *sheet; int first, n, add; float pos[3], vel[3], age, dur, size; } Fx;
static Fx g_fx[FX_MAX];
static void fx_spawn(const char *sheet, int first, int n, const float pos[3], float dur, float size, int add, const float *vel) {
    if (!sheet || n <= 0) return;
    Fx *f = NULL;
    for (int i = 0; i < FX_MAX && !f; i++) if (!g_fx[i].used) f = &g_fx[i];
    if (!f) { f = &g_fx[rand() % FX_MAX]; }
    memset(f, 0, sizeof(*f));
    f->used = 1; f->sheet = sheet; f->first = first; f->n = n; f->add = add; f->dur = dur; f->size = size;
    memcpy(f->pos, pos, sizeof(f->pos));
    if (vel) memcpy(f->vel, vel, sizeof(f->vel));
}
/* a few sprites around a point (heals, buffs...) */
static void fx_sparkle(const char *sheet, int first, int n, const float c[3], float r, int count, int add) {
    for (int i = 0; i < count; i++) {
        float p[3] = { c[0] + r * ((rand() % 200) / 100.0f - 1.0f), c[1] + r * ((rand() % 200) / 100.0f - 0.5f), c[2] + r * ((rand() % 200) / 100.0f - 1.0f) };
        float v[3] = { 0.0f, 0.6f, 0.0f };
        fx_spawn(sheet, first, n, p, 0.5f + (rand() % 40) / 100.0f, 0.3f, add, v);
    }
}
/* a sprite blended by adding its light (the glows: their dark parts vanish) */
static void blend_sprite_add(uint32_t *px, int W, int H, const uint32_t *spr, int sw, int sh, int x0, int y0, float sc, int alpha) {
    int dw = (int)(sw * sc), dh = (int)(sh * sc);
    for (int y = 0; y < dh; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= H) continue;
        const uint32_t *srow = spr + (size_t)(int)(y / sc) * sw;
        for (int x = 0; x < dw; x++) {
            int xx = x0 + x;
            if (xx < 0 || xx >= W) continue;
            uint32_t c = srow[(int)(x / sc)];
            uint32_t a = ((c >> 24) * (uint32_t)alpha) / 255;
            if (!a) continue;
            uint32_t *d = &px[(size_t)yy * W + xx];
            uint32_t r = ((*d >> 16) & 255) + (((c >> 16) & 255) * a) / 255, g = ((*d >> 8) & 255) + (((c >> 8) & 255) * a) / 255, b = (*d & 255) + ((c & 255) * a) / 255;
            *d = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
        }
    }
}
static const uint32_t *sprite_px(const char *name, int *w, int *h);
/* a sprite at a world point, `size` units tall */
static void sprite_at(const char *sheet, int frame, const float pos[3], float size, int add, int alpha, uint32_t *px, int W, int H, float sc) {
    char nm[48]; snprintf(nm, sizeof(nm), "%s.%d", sheet, frame);
    int sw, sh;
    const uint32_t *spr = sprite_px(nm, &sw, &sh);
    if (!spr || sw <= 0 || sh <= 0) return;
    float x0, y0, z0, x1, y1, z1, top[3] = { pos[0], pos[1] + size, pos[2] };
    if (!char_project(pos, pos, &x0, &y0, &z0) || !char_project(pos, top, &x1, &y1, &z1)) return;
    float hpx = fabsf(y1 - y0) * sc, k = hpx / (float)sh;
    if (k < 0.2f) k = 0.2f;
    int cx = (int)((x0 - g_cam_x) * sc), cy = (int)((y0 - g_cam_y) * sc);
    int x = cx - (int)(sw * k / 2), y = cy - (int)(sh * k / 2);
    if (add) blend_sprite_add(px, W, H, spr, sw, sh, x, y, k, alpha);
    else blend_sprite(px, W, H, spr, sw, sh, x, y, k);
}
static void fx_tick(float dt) {
    for (int i = 0; i < FX_MAX; i++) {
        Fx *f = &g_fx[i];
        if (!f->used) continue;
        f->age += dt;
        for (int k = 0; k < 3; k++) f->pos[k] += f->vel[k] * dt;
        if (f->age >= f->dur) f->used = 0;
    }
}

/* ---- projectiles ---- */
#define PROJ_MAX 48
typedef struct {
    int used, el, side, owner, potion; /* potion: 0 a spell, 1 exploding vial, 2 gas cloud vial */
    float pos[3], vel[3], age, life, grav, spin, trail_t;
    int dmg;
    CharModel *model;
} Proj;
static Proj g_proj[PROJ_MAX];
static int g_any_proj = 0;
static float actor_dir_now(const Actor *a) { /* where it faces now, in the middle of a clip that turns it */
    float d = a->play_clip >= 0 ? anim_lib_duration(a->play_clip) : 0.0f;
    float k = d > 0.0f ? fminf(1.0f, a->play_t / d) : 0.0f;
    return a->facing + a->play_turn * k;
}
static Actor *actor_nearest_foe(const Actor *a);
/* a foe within ~40 degrees in front of it (NULL: none): the spell goes to it */
static Actor *foe_in_front(const Actor *a, float dir) {
    Actor *best = NULL; float bd = 1e30f;
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *b = &g_actors[k];
        if (b == a || !b->used || b->dead || b->down || b->side == a->side) continue;
        float dx = b->pos[0] - a->pos[0], dz = b->pos[2] - a->pos[2], d = dx * dx + dz * dz;
        if (d > 900.0f || fabsf(wrap_angle(atan2f(dx, dz) - dir)) > 0.7f) continue;
        if (d < bd) { bd = d; best = b; }
    }
    return best;
}
static float model_height(const CharModel *m);
static const Moveset *actor_moveset(const Actor *a);
static int actor_damage_of(const Actor *a) { int d = moveset_stats(actor_moveset(a))->damage; return a->str_t > 0.0f ? d * 2 : d; }
static Proj *proj_new(void) {
    for (int i = 0; i < PROJ_MAX; i++) if (!g_proj[i].used) { memset(&g_proj[i], 0, sizeof(g_proj[i])); g_proj[i].used = 1; g_any_proj = 1; return &g_proj[i]; }
    return NULL;
}
/* a's spell now */
static void spell_cast(Actor *a, int el) {
    const Element *E = &ELEM[el];
    float h = model_height(a->model), dir = actor_dir_now(a);
    float chest[3] = { a->pos[0], a->pos[1] + h * 0.62f, a->pos[2] };
    audio_play(E->snd_cast, AUDIO_SOUND, 0, 0);
    if (el == EL_HEALTH) { /* the caster heals */
        a->hp += a->hp_max * 3 / 10; if (a->hp > a->hp_max) a->hp = a->hp_max;
        fx_sparkle("twinkle", 8, 8, chest, 0.5f, 10, 1);
        return;
    }
    if (el == EL_TIME) { /* every foe around goes slow */
        for (int k = 0; k < MAX_ACTORS; k++) {
            Actor *b = &g_actors[k];
            if (b == a || !b->used || b->dead || b->side == a->side) continue;
            float dx = b->pos[0] - a->pos[0], dz = b->pos[2] - a->pos[2];
            if (dx * dx + dz * dz > 100.0f) continue;
            b->slow_t = 6.0f;
            float c[3] = { b->pos[0], b->pos[1] + model_height(b->model) * 0.6f, b->pos[2] };
            fx_sparkle("twinkle", 24, 8, c, 0.5f, 8, 1);
        }
        fx_sparkle("twinkle", 24, 8, chest, 0.8f, 12, 1);
        return;
    }
    Actor *t = foe_in_front(a, dir);
    float dx = sinf(dir), dy = 0.0f, dz = cosf(dir);
    if (t) {
        float tc[3] = { t->pos[0], t->pos[1] + model_height(t->model) * 0.55f, t->pos[2] };
        float vx = tc[0] - chest[0], vy = tc[1] - chest[1], vz = tc[2] - chest[2], l = sqrtf(vx * vx + vy * vy + vz * vz);
        if (l > 1e-3f) { dx = vx / l; dy = vy / l; dz = vz / l; }
    }
    Proj *p = proj_new();
    if (!p) return;
    p->el = el; p->side = a->side; p->owner = (int)(a - g_actors);
    p->pos[0] = chest[0] + dx * 0.6f; p->pos[1] = chest[1]; p->pos[2] = chest[2] + dz * 0.6f;
    p->vel[0] = dx * E->speed; p->vel[1] = dy * E->speed; p->vel[2] = dz * E->speed;
    p->life = 3.0f;
    p->dmg = (int)(actor_damage_of(a) * E->dmg + 0.5f);
    p->model = E->model ? item_model(E->model) : NULL;
}
/* blows, spells and vials all come to this: health taken, reactions */
static void actor_fall(Actor *b);
static void actor_hit(Actor *a);
static void actor_take_damage(Actor *b, int dmg) {
    if (!b->used || b->dead || b->down) return;
    if (b->invuln_t > 0.0f) { float c[3] = { b->pos[0], b->pos[1] + model_height(b->model) * 0.6f, b->pos[2] }; fx_sparkle("twinkle", 24, 8, c, 0.4f, 4, 1); return; }
    if (b->armour_t > 0.0f) dmg = (dmg + 1) / 2;
    b->hp -= dmg;
    actor_hit(b); /* its grunt, maybe */
    if (b->hp <= 0) { b->hp = 0; actor_fall(b); return; }
    actor_resolve_clips(b);
    if (b->clips[MC_HIT] >= 0 && !b->guard) { actor_play_once(b, b->clips[MC_HIT], 0.0f); b->cue.on = 0; }
}
/* a projectile arrives (on b, or on the scenery: b NULL) */
static void proj_impact(Proj *p, Actor *b) {
    const Element *E = &ELEM[p->el];
    p->used = 0;
    if (p->potion == 1) { /* an exploding vial: a fire blast, everyone of the other side around */
        audio_play("explode.ogg", AUDIO_SOUND, 0, 0); audio_play("potbreak.ogg", AUDIO_SOUND, 0, 0);
        fx_spawn("xplode", 0, 8, p->pos, 0.6f, 2.2f, 0, NULL);
        for (int k = 0; k < MAX_ACTORS; k++) {
            Actor *a = &g_actors[k];
            if (!a->used || a->side == p->side) continue;
            float dx = a->pos[0] - p->pos[0], dz = a->pos[2] - p->pos[2];
            if (dx * dx + dz * dz < 6.0f) actor_take_damage(a, 25);
        }
        return;
    }
    if (p->potion == 2) { /* a gas cloud: poisons the ones of the other side around */
        audio_play("potbreak.ogg", AUDIO_SOUND, 0, 0); audio_play("acidpop.ogg", AUDIO_SOUND, 0, 0);
        for (int i = 0; i < 6; i++) {
            float c[3] = { p->pos[0] + ((rand() % 200) / 100.0f - 1.0f), p->pos[1] + 0.3f, p->pos[2] + ((rand() % 200) / 100.0f - 1.0f) };
            fx_spawn("xplode", 31, 28, c, 2.5f, 1.6f, 0, NULL);
            fx_spawn("twinkle", 56, 8, c, 1.5f, 0.6f, 1, NULL);
        }
        for (int k = 0; k < MAX_ACTORS; k++) {
            Actor *a = &g_actors[k];
            if (!a->used || a->side == p->side) continue;
            float dx = a->pos[0] - p->pos[0], dz = a->pos[2] - p->pos[2];
            if (dx * dx + dz * dz < 9.0f) a->poison_t = 5.0f;
        }
        return;
    }
    fx_spawn(E->bsheet, E->bfirst, E->bn, p->pos, 0.5f, E->bsize, E->badd, NULL);
    audio_play(b ? E->snd_hit : E->snd_hit, AUDIO_SOUND, 0, 0);
    if (!b) return;
    if (p->el == EL_ICE) b->slow_t = 3.0f; /* frozen a while */
    actor_take_damage(b, p->dmg);
}
static void proj_tick(float dt) {
    if (!g_any_proj) return;
    int any = 0;
    for (int i = 0; i < PROJ_MAX; i++) {
        Proj *p = &g_proj[i];
        if (!p->used) continue;
        any = 1;
        const Element *E = &ELEM[p->el];
        float prev[3] = { p->pos[0], p->pos[1], p->pos[2] };
        p->vel[1] -= p->grav * dt;
        for (int k = 0; k < 3; k++) p->pos[k] += p->vel[k] * dt;
        p->age += dt; p->spin += dt * 9.0f;
        p->trail_t += dt;
        if (p->trail_t > 0.035f && !p->potion) { p->trail_t = 0.0f; fx_spawn(E->tsheet, E->tfirst, E->tn, prev, 0.35f, E->size * 0.6f, E->tadd, NULL); }
        for (int k = 0; k < MAX_ACTORS && p->used; k++) { /* a character of the other side */
            Actor *b = &g_actors[k];
            if (!b->used || b->dead || b->down || b->side == p->side || b->dodging) continue;
            float r = g_nav.body_radius * 1.25f, dx = p->pos[0] - b->pos[0], dz = p->pos[2] - b->pos[2];
            if (dx * dx + dz * dz < r * r && p->pos[1] >= b->pos[1] - 0.2f && p->pos[1] <= b->pos[1] + model_height(b->model) * 1.05f) proj_impact(p, b);
        }
        if (!p->used) continue;
        float step[3] = { p->pos[0] - prev[0], p->pos[1] - prev[1], p->pos[2] - prev[2] };
        float l = sqrtf(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]), hit[3], nrm[3];
        if (l > 1e-4f && g_room_mesh_tri_count > 0) { /* the scenery */
            float dir[3] = { step[0] / l, step[1] / l, step[2] / l };
            if (raycast_room_mesh(prev, dir, g_room_mesh_tris, g_room_mesh_tri_count, 0, 0.0f, hit, nrm)) {
                float hx = hit[0] - prev[0], hy = hit[1] - prev[1], hz = hit[2] - prev[2];
                if (hx * hx + hy * hy + hz * hz <= l * l) { memcpy(p->pos, hit, sizeof(p->pos)); proj_impact(p, NULL); continue; }
            }
        }
        if (p->age > p->life || p->pos[1] < g_room_floor_y - 20.0f) p->used = 0;
    }
    g_any_proj = any;
}
static void proj_models_draw(uint32_t *px, int W, int H, float sc, float *zb, uint32_t *zst, uint32_t stamp) {
    static float vx[DAVID_MAX_VERTS], vy[DAVID_MAX_VERTS], vz[DAVID_MAX_VERTS];
    static int vis[DAVID_MAX_VERTS];
    for (int i = 0; i < PROJ_MAX; i++) {
        Proj *p = &g_proj[i];
        if (!p->used || !p->model) continue;
        CharModel *m = p->model;
        float yaw = atan2f(p->vel[0], p->vel[2]), cf = cosf(yaw + 3.14159265f), sf = sinf(yaw + 3.14159265f);
        float cs = cosf(p->spin), sn = sinf(p->spin);
        for (int v = 0; v < m->vertex_count && v < DAVID_MAX_VERTS; v++) {
            float q[3] = { m->positions[v][0], m->positions[v][1], m->positions[v][2] };
            if (p->potion) { float y = q[1] * cs - q[2] * sn, z = q[1] * sn + q[2] * cs; q[1] = y; q[2] = z; } /* a vial tumbles */
            else { float x = q[0] * cs - q[1] * sn, y = q[0] * sn + q[1] * cs; q[0] = x; q[1] = y; } /* a bolt spins on itself */
            float w[3] = { q[0] * cf + q[2] * sf + p->pos[0], q[1] + p->pos[1], -q[0] * sf + q[2] * cf + p->pos[2] };
            vis[v] = char_project(p->pos, w, &vx[v], &vy[v], &vz[v]);
        }
        render_mesh_hires(m, vx, vy, vz, vis, px, W, H, sc, zb, zst, stamp);
    }
}
static void fx_draw(uint32_t *px, int W, int H, float sc) {
    for (int i = 0; i < FX_MAX; i++) {
        Fx *f = &g_fx[i];
        if (!f->used) continue;
        int fr = f->first + (int)(f->age / f->dur * f->n);
        if (fr >= f->first + f->n) fr = f->first + f->n - 1;
        sprite_at(f->sheet, fr, f->pos, f->size, f->add, f->add ? (int)(255.0f * (1.0f - 0.5f * f->age / f->dur)) : 255, px, W, H, sc);
    }
    int frame = (int)(perf_now_ms() / 70.0);
    for (int i = 0; i < PROJ_MAX; i++) { /* the spells' glows */
        Proj *p = &g_proj[i];
        if (!p->used || p->potion) continue;
        const Element *E = &ELEM[p->el];
        if (E->star >= 0) sprite_at("star", E->star, p->pos, E->size, 1, 255, px, W, H, sc);
        if (E->psheet) sprite_at(E->psheet, E->pfirst + frame % E->pn, p->pos, E->size, 0, 255, px, W, H, sc);
    }
}
/* its orb, spinning in its hand */
static void orb_glow_draw(Actor *a, uint32_t *px, int W, int H, float sc) {
    int el = weapon_is_orb(a->weapon) ? weapon_element(a->weapon) : EL_NONE;
    if (!el || !a->hand_ok) return;
    float cf = cosf(a->facing + 3.14159265f), sf = sinf(a->facing + 3.14159265f), h[3] = { a->hand.m[12], a->hand.m[13], a->hand.m[14] };
    float w[3] = { h[0] * cf + h[2] * sf + a->pos[0], h[1] + a->pos[1], -h[0] * sf + h[2] * cf + a->pos[2] };
    sprite_at(ELEM[el].orb, (int)(perf_now_ms() / 80.0) % 16, w, 0.32f, 0, 255, px, W, H, sc);
}
static void orb_equip_fx(Actor *a) {
    int el = weapon_element(a->weapon);
    if (!el) return;
    float c[3] = { a->pos[0], a->pos[1] + model_height(a->model) * 0.75f, a->pos[2] };
    fx_spawn("equipfx", ELEM[el].efirst, ELEM[el].en, c, 0.6f, 1.2f, el == EL_LIGHTNING || el == EL_LIGHT, NULL);
}
/* an orb taken out (magkup: its hands go up, the orb appears), or put away (magkup backward) */
static void actor_equip_orb(Actor *a, const char *orb) {
    if (actor_busy(a) || a->down || a->dead) { snprintf(g_status, sizeof(g_status), "busy"); return; }
    int away = !strcmp(a->weapon, orb), c = actor_clip(a, "magkup");
    audio_play("magicup.ogg", AUDIO_SOUND, 0, 0);
    if (c < 0) { snprintf(a->weapon, sizeof(a->weapon), "%s", away ? "" : orb); a->clips_gen = 0; return; }
    actor_play_once(a, c, 0.0f);
    float d = anim_lib_duration(c);
    a->play_event = EV_WEAPON; snprintf(a->event_arg, sizeof(a->event_arg), "%s", away ? "" : orb);
    if (away) { a->play_rev = 1; a->play_event_at = d * 0.6f; } /* the hands go up, the orb goes */
    else a->play_event_at = d * 0.45f;                           /* the hands up: it's there */
    if (!away && a->shield[0]) { a->shield[0] = 0; a->clips_gen = 0; } /* both hands on the orb */
    snprintf(g_status, sizeof(g_status), away ? "orb put away" : "%s orb", orb + 4);
}
/* ---- potions: drunk, or thrown ---- */
enum { POT_HEALTH, POT_MAGIC, POT_STRENGTH, POT_ARMOUR, POT_EXPLODE, POT_GAS, POT_CHAOS, POT_PROTECT, POT_COUNT };
static const struct { const char *arg, *model; } POTION[POT_COUNT] = {
    { "health", "potred" }, { "magic", "potblue" }, { "strength", "potorang" }, { "armour", "potwhite" },
    { "explode", "potionr" }, { "gas", "potgreen" }, { "chaos", "potpurpl" }, { "protect", "potblack" } };
static void potion_effect(Actor *a, int kind) {
    float c[3] = { a->pos[0], a->pos[1] + model_height(a->model) * 0.6f, a->pos[2] };
    if (kind == POT_CHAOS) { static const int any[5] = { POT_HEALTH, POT_STRENGTH, POT_ARMOUR, POT_PROTECT, -1 }; kind = any[rand() % 5]; }
    switch (kind) {
        case POT_HEALTH: a->hp += a->hp_max / 2; if (a->hp > a->hp_max) a->hp = a->hp_max; fx_sparkle("twinkle", 8, 8, c, 0.5f, 12, 1); audio_play("lifeheal.ogg", AUDIO_SOUND, 0, 0); snprintf(g_status, sizeof(g_status), "health restored"); break;
        case POT_STRENGTH: a->str_t = 20.0f; fx_sparkle("twinkle", 32, 8, c, 0.5f, 12, 1); audio_play("pmagic.ogg", AUDIO_SOUND, 0, 0); snprintf(g_status, sizeof(g_status), "strength: double damage for 20 s"); break;
        case POT_ARMOUR: a->armour_t = 30.0f; fx_sparkle("twinkle", 0, 8, c, 0.5f, 12, 1); audio_play("pmagic.ogg", AUDIO_SOUND, 0, 0); snprintf(g_status, sizeof(g_status), "enchanted armour: half the damage for 30 s"); break;
        case POT_PROTECT: a->invuln_t = 10.0f; fx_sparkle("twinkle", 24, 8, c, 0.6f, 16, 1); audio_play("magicup.ogg", AUDIO_SOUND, 0, 0); snprintf(g_status, sizeof(g_status), "absolute protection for 10 s"); break;
        case POT_MAGIC: fx_sparkle("twinkle", 16, 8, c, 0.5f, 12, 1); audio_play("pmagic.ogg", AUDIO_SOUND, 0, 0); snprintf(g_status, sizeof(g_status), "magic potion: no magic meter yet"); break;
        default: a->slow_t = 5.0f; fx_sparkle("twinkle", 66, 1, c, 0.5f, 8, 1); snprintf(g_status, sizeof(g_status), "chaos: slowed down!"); break;
    }
}
static void actor_use_potion(Actor *a, const char *arg) {
    int kind = -1;
    for (int i = 0; i < POT_COUNT; i++) if (!strcmp(arg, POTION[i].arg)) kind = i;
    if (kind < 0) return;
    if (actor_busy(a) || a->down || a->dead) { snprintf(g_status, sizeof(g_status), "busy"); return; }
    int thrown = kind == POT_EXPLODE || kind == POT_GAS;
    int c = actor_clip(a, thrown ? "throw" : "drink");
    a->potion_m = item_model(POTION[kind].model);
    a->pot_right = thrown; a->pot_kind = kind;
    if (c < 0) { a->potion_m = NULL; if (!thrown) potion_effect(a, kind); return; }
    actor_play_once(a, c, 0.0f);
    a->pot_pending = 1; a->pot_clip = c; a->pot_at = anim_lib_duration(c) * (thrown ? 0.45f : 0.55f);
    if (thrown) { a->play_attack = 1; Actor *t = foe_in_front(a, a->facing); if (t) a->facing = atan2f(t->pos[0] - a->pos[0], t->pos[2] - a->pos[2]); }
}
static void potion_tick(Actor *a) {
    if (a->potion_m && a->play_clip != a->pot_clip) { a->potion_m = NULL; a->pot_pending = 0; } /* the clip is over: the vial is gone */
    if (!a->pot_pending || a->play_clip != a->pot_clip || a->play_t < a->pot_at) return;
    a->pot_pending = 0;
    if (!a->pot_right) { audio_play("drink1.ogg", AUDIO_SOUND, 0, 0); potion_effect(a, a->pot_kind); return; }
    Proj *p = proj_new(); /* thrown: an arc */
    CharModel *m = a->potion_m;
    a->potion_m = NULL;
    if (!p) return;
    float h = model_height(a->model), dir = a->facing;
    p->potion = a->pot_kind == POT_EXPLODE ? 1 : 2;
    p->side = a->side; p->owner = (int)(a - g_actors);
    p->pos[0] = a->pos[0] + sinf(dir) * 0.5f; p->pos[1] = a->pos[1] + h * 0.8f; p->pos[2] = a->pos[2] + cosf(dir) * 0.5f;
    Actor *t = foe_in_front(a, dir);
    float dist = t ? sqrtf((t->pos[0] - a->pos[0]) * (t->pos[0] - a->pos[0]) + (t->pos[2] - a->pos[2]) * (t->pos[2] - a->pos[2])) : 6.0f;
    if (dist > 12.0f) dist = 12.0f;
    float flight = 0.8f, speed = dist / flight;
    p->vel[0] = sinf(dir) * speed; p->vel[2] = cosf(dir) * speed; p->vel[1] = 4.9f * flight; p->grav = 9.8f;
    p->life = 3.0f; p->model = m;
}
/* buffs, poison, the spells on their way -- every step */
static void magic_tick(float dt) {
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *a = &g_actors[k];
        if (!a->used) continue;
        if (a->cast_pending) {
            if (a->play_clip != a->cast_clip) a->cast_pending = 0;
            else if (a->play_t >= a->cast_at) { a->cast_pending = 0; spell_cast(a, a->cast_elem); }
        }
        potion_tick(a);
        if (a->slow_t > 0.0f) a->slow_t -= dt;
        if (a->str_t > 0.0f) a->str_t -= dt;
        if (a->armour_t > 0.0f) a->armour_t -= dt;
        if (a->invuln_t > 0.0f) a->invuln_t -= dt;
        if (a->poison_t > 0.0f) {
            a->poison_t -= dt; a->poison_tick += dt;
            if (a->poison_tick >= 1.0f) { a->poison_tick = 0.0f; actor_take_damage(a, 3); }
        }
        a->buff_fx_t += dt;
        if (a->buff_fx_t > 0.35f && (a->str_t > 0.0f || a->armour_t > 0.0f || a->invuln_t > 0.0f || a->slow_t > 0.0f || a->poison_t > 0.0f)) {
            a->buff_fx_t = 0.0f;
            float c[3] = { a->pos[0], a->pos[1] + model_height(a->model) * 0.5f, a->pos[2] };
            int first = a->invuln_t > 0.0f ? 24 : a->str_t > 0.0f ? 32 : a->armour_t > 0.0f ? 0 : a->poison_t > 0.0f ? 56 : 16;
            fx_sparkle("twinkle", first, 8, c, 0.4f, 1, 1);
        }
    }
    proj_tick(dt);
    fx_tick(dt);
}
/* the nearest character of the other side still standing (NULL: none) */
static Actor *actor_nearest_foe(const Actor *a) {
    Actor *best = NULL; float bd = 1e30f;
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *b = &g_actors[k];
        if (b == a || !actor_alive(b) || b->side == a->side) continue;
        float dx = b->pos[0] - a->pos[0], dz = b->pos[2] - a->pos[2], d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = b; }
    }
    return best;
}
/* AI, "close in and strike": to the nearest foe until close enough, then its blows with pauses between them */
static void actor_ai_tick(Actor *a, float dt) {
    const AiSettings *ai = moveset_ai(actor_moveset(a));
    if (!ai || ai->kind != AI_MELEE || !actor_alive(a) || a->play_clip >= 0) return; /* nothing to do, or busy (a blow, a reaction, a script...) */
    a->ai_cool -= dt; a->ai_repath -= dt;
    Actor *t = actor_nearest_foe(a);
    if (!t) return;
    float dx = t->pos[0] - a->pos[0], dz = t->pos[2] - a->pos[2], dist = sqrtf(dx * dx + dz * dz);
    float reach = ai->range * 2.0f * g_nav.body_radius;
    if (dist > reach) {
        if (a->ai_repath <= 0.0f || !a->moving) {
            a->ai_repath = 0.35f;
            float k = dist > 1e-3f ? (dist - reach * 0.7f) / dist : 0.0f, goal[3] = { a->pos[0] + dx * k, t->pos[1], a->pos[2] + dz * k };
            actor_begin(a); move_to_world_point(goal, ai->run); actor_end();
        }
        return;
    }
    if (a->moving) { a->moving = 0; a->waypoint_count = 0; a->walk_mode = 0; a->turn_clip = -1; } /* there */
    a->facing = atan2f(dx, dz);
    if (a->ai_cool > 0.0f) return;
    actor_resolve_clips(a);
    int slots[8], n = 0;
    for (int s = MC_FIRST_COMBAT; s < MC_COUNT; s++) if (a->clips[s] >= 0 && s != MC_SW_BACKL && s != MC_SW_BACKR) slots[n++] = s; /* not the ones turning around */
    if (n && actor_attack(a, slots[rand() % n]))
        a->ai_cool = ai->pause_min + (ai->pause_max - ai->pause_min) * (float)rand() / (float)RAND_MAX;
}
/* every game step, once the characters have moved */
static void combat_tick(float dt) {
    int enemies = 0;
    for (int k = 0; k < MAX_ACTORS; k++) if (g_actors[k].used && g_actors[k].side == SIDE_ENEMY && actor_alive(&g_actors[k])) enemies++;
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *a = &g_actors[k];
        if (!a->used) continue;
        if (a->dodging && !(a->play_clip >= 0 && a->play_clip == a->clips[MC_DODGE]) && !(a == DAVID_ACTOR && a->play_attack)) a->dodging = 0;
        if (a->down) { /* up again once the scene's enemies are beaten (David: after 10 s at most) */
            a->down_t += dt;
            if (!enemies || (a == DAVID_ACTOR && a->down_t > 10.0f)) actor_get_up(a);
            continue;
        }
        if (a->blow && a->play_clip >= 0 && actor_alive(a))
            for (int j = 0; j < MAX_ACTORS; j++) {
                Actor *b = &g_actors[j];
                if (b == a || !actor_alive(b) || b->side == a->side || b->dodging || (a->hit_mask & (1u << j))) continue;
                if (blade_touches(a, b)) { a->hit_mask |= 1u << j; actor_take_blow(a, b); }
            }
        if (a->ai_on && k != 0) actor_ai_tick(a, a->slow_t > 0.0f ? dt * 0.35f : dt);
    }
    magic_tick(dt);
}
/* a boss enemy alive: its name and health bar at the top of the picture */
static void boss_bar_draw(HDC hdc) {
    if (screen_view() || !g_has_3d_character) return;
    Actor *boss = NULL;
    for (int k = 1; k < MAX_ACTORS; k++) {
        Actor *a = &g_actors[k];
        if (a->used && a->side == SIDE_ENEMY && !a->dead && moveset_stats(actor_moveset(a))->boss) { boss = a; break; }
    }
    if (!boss) return;
    RECT gr; game_rect_client(&gr);
    int w = (gr.right - gr.left) * 2 / 5, x = (gr.left + gr.right - w) / 2, y = gr.top + 34, h = 14;
    RECT frame = { x - 2, y - 2, x + w + 2, y + h + 2 }, bg = { x, y, x + w, y + h };
    ui_fill(hdc, &frame, RGB(200, 160, 64)); ui_fill(hdc, &bg, RGB(30, 10, 10));
    int fw = boss->hp_max > 0 ? w * boss->hp / boss->hp_max : 0;
    RECT fill = { x, y, x + fw, y + h };
    ui_fill(hdc, &fill, RGB(190, 30, 30));
    SelectObject(hdc, ui_font(16, 1));
    char t[80]; snprintf(t, sizeof(t), "%s", boss->model->name);
    if (t[0] >= 'a' && t[0] <= 'z') t[0] -= 32;
    ui_text(hdc, x, y - 24, w, 20, t, RGB(255, 225, 150), DT_CENTER | DT_SINGLELINE);
}
static void shield_break(Actor *a) {
    if (!a->shield[0] || !a->shield_m || !a->hand_s_ok) return;
    const CharModel *sh = a->shield_m;
    float cf = cosf(a->facing + 3.14159265f), sf = sinf(a->facing + 3.14159265f);
    for (int q = 0; q < 4; q++) { /* four pieces: the shield's quarters */
        Debris *d = NULL;
        for (int k = 0; k < DEBRIS_MAX; k++) if (!g_debris[k].piece) { d = &g_debris[k]; break; }
        if (!d) break;
        CharModel *pc = (CharModel *)malloc(sizeof(CharModel));
        *pc = *sh;
        pc->indices = (uint32_t *)malloc(sizeof(uint32_t) * sh->index_count);
        pc->index_count = 0;
        for (int t = 0; t < sh->index_count / 3; t++) {
            float cy = 0, cz = 0;
            for (int k = 0; k < 3; k++) { cy += sh->positions[sh->indices[t * 3 + k]][1]; cz += sh->positions[sh->indices[t * 3 + k]][2]; }
            if (((cy > 0) ? 1 : 0) + ((cz > 0) ? 2 : 0) != q) continue;
            for (int k = 0; k < 3; k++) pc->indices[pc->index_count++] = sh->indices[t * 3 + k];
        }
        d->piece = pc; d->nv = sh->vertex_count; d->age = 0.0f;
        d->wpos = (float (*)[3])malloc(sizeof(float) * 3 * sh->vertex_count);
        float c[3] = { 0, 0, 0 };
        for (int v = 0; v < sh->vertex_count; v++) {
            float h[3];
            mat4_vec3(&a->hand_s, sh->positions[v], h);
            d->wpos[v][0] = h[0] * cf + h[2] * sf + a->pos[0]; d->wpos[v][1] = h[1] + a->pos[1]; d->wpos[v][2] = -h[0] * sf + h[2] * cf + a->pos[2];
            c[0] += d->wpos[v][0]; c[1] += d->wpos[v][1]; c[2] += d->wpos[v][2];
        }
        for (int k = 0; k < 3; k++) d->cen[k] = c[k] / sh->vertex_count;
        float ang = q * 1.5707963f + 0.7f + (rand() % 100) * 0.004f;
        d->vel[0] = cosf(ang) * 1.2f; d->vel[1] = 1.5f; d->vel[2] = sinf(ang) * 1.2f;
        d->off[0] = d->off[1] = d->off[2] = 0;
    }
    if (g_lost_count < 16) snprintf(g_lost_items[g_lost_count++], 48, "%s", a->shield);
    a->shield[0] = 0; a->clips_gen = 0;
    if (a->guard) { a->guard = 0; a->play_next = -1; int down = actor_clip(a, "shlddown"); if (down >= 0) actor_play_once(a, down, 0.0f); }
    audio_play(!strncmp(g_lost_items[g_lost_count - 1], "wood", 4) ? "woodbrk.ogg" : "metalbrk.ogg", AUDIO_SOUND, 0, 0); /* the shield breaking */
}
static int debris_tick(float dt) {
    int any = 0;
    for (int k = 0; k < DEBRIS_MAX; k++) {
        Debris *d = &g_debris[k];
        if (!d->piece) continue;
        any = 1;
        d->age += dt;
        d->vel[1] -= 9.0f * dt;
        for (int i = 0; i < 3; i++) d->off[i] += d->vel[i] * dt;
        if (d->age >= DEBRIS_LIFE) { free(d->piece->indices); free(d->piece); free(d->wpos); d->piece = NULL; }
    }
    return any;
}
static void debris_draw(uint32_t *px, int W, int H, float sc, float *zb, uint32_t *zst, uint32_t stamp) {
    static float vx[DAVID_MAX_VERTS], vy[DAVID_MAX_VERTS], vz[DAVID_MAX_VERTS];
    static int vis[DAVID_MAX_VERTS];
    for (int k = 0; k < DEBRIS_MAX; k++) {
        Debris *d = &g_debris[k];
        if (!d->piece) continue;
        float s = 1.0f - d->age / DEBRIS_LIFE; /* shrinks away */
        for (int v = 0; v < d->nv && v < DAVID_MAX_VERTS; v++) {
            float w[3];
            for (int i = 0; i < 3; i++) w[i] = d->cen[i] + (d->wpos[v][i] - d->cen[i]) * s + d->off[i];
            vis[v] = char_project(d->cen, w, &vx[v], &vy[v], &vz[v]);
        }
        render_mesh_hires(d->piece, vx, vy, vz, vis, px, W, H, sc, zb, zst, stamp);
    }
}

/* =====================================================================
   RADIAL MENU (right button alone, in the game): 8 slots around the
   click, each a c_items.3 socket (c_items.4 under the mouse) with its
   ornament on the ring (c_cards.6 at the top, then clockwise to c_cards.13),
   the icon inside. The slots open submenus (the centre goes back) or use
   an item: weapons (sheathed / drawn: sheatmp), shields (shldquip), the
   specials (their animation, a blue trail behind the blades). The rest
   is there for later (no effect yet). Right button / a click outside:
   closed. The view doesn't scroll while it's open.
   ===================================================================== */
enum { RA_NONE, RA_SUB, RA_WEAPON, RA_SHIELD, RA_SPECIAL, RA_ITEM, RA_ORB, RA_POTION };
enum { RM_ROOT, RM_FOOD, RM_ORBS, RM_RANGED, RM_MAGIC, RM_BACKPACK, RM_POTIONS, RM_KEYS, RM_SHIELDS, RM_WEAPONS, RM_SPECIALS, RM_COUNT };
typedef struct { const char *icon, *name; int action; const char *arg; int sub; } RadialItem;
static const RadialItem RADIAL[RM_COUNT][8] = {
    [RM_ROOT] = { { "c_food.0", "Food", RA_SUB, NULL, RM_FOOD }, { "p_main.1", "Orbs", RA_SUB, NULL, RM_ORBS },
                  { "p_main.2", "Ranged", RA_SUB, NULL, RM_RANGED }, { "p_main.3", "Magical", RA_SUB, NULL, RM_MAGIC },
                  { "p_main.4", "Backpack", RA_SUB, NULL, RM_BACKPACK }, { "p_main.5", "Shields", RA_SUB, NULL, RM_SHIELDS },
                  { "p_main.6", "Weapons", RA_SUB, NULL, RM_WEAPONS }, { "p_main.7", "Specials", RA_SUB, NULL, RM_SPECIALS } },
    [RM_FOOD] = { { "p_pack.16", "Apple", RA_ITEM }, { "p_pack.17", "Carrot", RA_ITEM }, { "p_pack.18", "Bread", RA_ITEM },
                  { "p_pack.19", "Cheese", RA_ITEM }, { "p_pack.20", "Cake", RA_ITEM }, { "p_pack.21", "Roast rat", RA_ITEM },
                  { "p_pack.22", "Pie", RA_ITEM }, { "p_pack.23", "Chicken", RA_ITEM } },
    [RM_ORBS] = { { "p_main.8", "Fire", RA_ORB, "orb:fire" }, { "p_main.9", "Ice", RA_ORB, "orb:ice" }, { "p_main.10", "Health", RA_ORB, "orb:health" },
                  { "p_main.11", "Earth", RA_ORB, "orb:earth" }, { "p_main.12", "Acid", RA_ORB, "orb:acid" }, { "p_main.13", "Lightning", RA_ORB, "orb:lightning" },
                  { "p_main.14", "Time", RA_ORB, "orb:time" }, { "p_main.15", "Light", RA_ORB, "orb:light" } },
    [RM_RANGED] = { { "p_main.16", "Slingshot", RA_ITEM }, { "p_main.17", "Daggers", RA_ITEM }, { "p_main.18", "Bow", RA_ITEM },
                    { "p_main.19", "Large Bow", RA_ITEM }, { "p_main.20", "Axes", RA_ITEM }, { "p_main.21", "Bombs", RA_ITEM },
                    { "p_main.22", "Fire Bow", RA_ITEM }, { "p_main.23", "Shuriken", RA_ITEM } },
    [RM_MAGIC] = { { "p_main.24", "Ice wand", RA_WEAPON, "icewand" }, { "p_main.25", "Fire sword", RA_WEAPON, "fireswrd" }, { "p_main.26", "Lightning Staff", RA_WEAPON, "lstaff" },
                   { "p_main.27", "Ring of Invisibility", RA_ITEM }, { "p_main.28", "Amulet of Seeing", RA_ITEM },
                   { "p_main.29", "Ring of Resist Magic", RA_ITEM }, { "p_misc.1", "Summon Golem", RA_ITEM }, { NULL } },
    [RM_BACKPACK] = { { "p_main.0", "Inventory", RA_ITEM }, { "p_pack.0", "Potions", RA_SUB, NULL, RM_POTIONS },
                      { "p_pack.5", "Keys", RA_SUB, NULL, RM_KEYS }, { NULL }, { NULL }, { NULL }, { NULL }, { NULL } },
    [RM_POTIONS] = { { "p_pack.8", "Health Potion", RA_POTION, "health" }, { "p_pack.9", "Magic Potion", RA_POTION, "magic" }, { "p_pack.10", "Strength Potion", RA_POTION, "strength" },
                     { "p_pack.11", "Enchanted Armour", RA_POTION, "armour" }, { "p_pack.12", "Exploding Vials", RA_POTION, "explode" }, { "p_pack.13", "Gas Cloud Vials", RA_POTION, "gas" },
                     { "p_pack.14", "Chaos Potion", RA_POTION, "chaos" }, { "p_pack.15", "Absolute Protection", RA_POTION, "protect" } },
    [RM_KEYS] = { { NULL } },
    /* the shields' models: guessed from their names (woodshld, woodrivs, ironrim, stelshld, dragshld, enchshld, diamshld);
       the crested one is David's own (daveshld) */
    [RM_SHIELDS] = { { "p_pack.24", "Wooden shield", RA_SHIELD, "woodshld" }, { "p_pack.25", "Riveted shield", RA_SHIELD, "woodrivs" },
                     { "p_pack.26", "Iron rimmed shield", RA_SHIELD, "ironrim" }, { "p_pack.27", "Steel shield", RA_SHIELD, "stelshld" },
                     { "p_pack.28", "Crested shield", RA_SHIELD, "daveshld" }, { "p_pack.29", "Dragon shield", RA_SHIELD, "dragshld" },
                     { "p_pack.30", "Enchanted shield", RA_SHIELD, "enchshld" }, { "p_pack.31", "Diamond shield", RA_SHIELD, "diamshld" } },
    [RM_WEAPONS] = { { "p_main.40", "Short sword", RA_WEAPON, "shrtswrd" }, { "p_main.41", "Broad sword", RA_WEAPON, "daveswrd" },
                     { "p_main.42", "Battle axe", RA_WEAPON, "bipennis" }, { "p_main.43", "Long sword", RA_WEAPON, "longswrd" },
                     { "p_main.44", "War hammer", RA_WEAPON, "hammer" }, { "p_main.45", "Mace", RA_WEAPON, "spikmace" },
                     { "p_main.46", "Bastard sword", RA_WEAPON, "bastswrd" }, { "p_main.47", "Dual Knightly Swords", RA_WEAPON, "dualswrd" } },
    [RM_SPECIALS] = { { "p_main.48", "Web of Death", RA_SPECIAL, "flash" }, { "p_main.49", "Reaper", RA_SPECIAL, "fatal" },
                      { "p_main.50", "Cleaver", RA_SPECIAL, "butter" }, { "p_main.51", "Scythe", RA_SPECIAL, "dethspin" },
                      { "p_main.52", "Falcon", RA_SPECIAL, "tumble" }, { "p_main.53", "Hurricane", RA_SPECIAL, "2hurrica" },
                      { "p_main.54", "Berserker", RA_SPECIAL, "laters" }, { "p_main.55", "Armageddon", RA_SPECIAL, "armaged" } },
};
static int radial_parent(int menu) { return (menu == RM_POTIONS || menu == RM_KEYS) ? RM_BACKPACK : RM_ROOT; }
static int g_radial = 0;
static int g_radial_menu = RM_ROOT, g_radial_cx = 0, g_radial_cy = 0, g_radial_hover = -1; /* hover: 0-7 a slot */
static float g_radial_arc = 0.0f;    /* the arc pointing at the hovered slot (angle, turning toward it) */
static int g_radial_arc_on = 0;
static int g_radial_equipped[RM_COUNT]; /* the item chosen in Orbs / Ranged / Magical / Specials (+1; 0 = none) */
static void radial_sound(const char *f) { audio_play(f, AUDIO_SOUND, 0, 0); }

/* a sprite of assets/sprites (kept once loaded) */
static const uint32_t *sprite_px(const char *name, int *w, int *h) {
    static struct { char name[32]; uint32_t *px; int w, h; } cache[1024];
    static int n = 0;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) { *w = cache[i].w; *h = cache[i].h; return cache[i].px; }
    if (n >= 1024) return NULL;
    char path[1024];
    root_path(path, sizeof(path), "assets/sprites/%s.png", name);
    snprintf(cache[n].name, sizeof(cache[n].name), "%s", name);
    cache[n].px = image_load(path, &cache[n].w, &cache[n].h);
    *w = cache[n].w; *h = cache[n].h;
    return cache[n++].px;
}
/* sizes in client pixels: the sprites drawn bigger than 1:1, and the ring */
static float radial_scale(void) { float sc = view_scale_now() * 1.2f; return sc < 1.2f ? 1.2f : sc > 2.4f ? 2.4f : sc; }
static float radial_ring(void) { return 44.0f * radial_scale(); }
static void radial_slot_pos(int i, float *x, float *y) {
    float a = -1.5707963f + i * 0.7853982f; /* 0 at the top, clockwise */
    *x = g_radial_cx + cosf(a) * radial_ring(); *y = g_radial_cy + sinf(a) * radial_ring();
}
static void radial_open(int cx, int cy) {
    RECT r; game_rect_client(&r);
    int m = (int)(radial_ring() + 34.0f * radial_scale()); /* the whole ring stays in the picture */
    if (cx < r.left + m) cx = r.left + m;
    if (cx > r.right - m) cx = r.right - m;
    if (cy < r.top + m) cy = r.top + m;
    if (cy > r.bottom - m) cy = r.bottom - m;
    g_radial = 1; g_radial_menu = RM_ROOT; g_radial_cx = cx; g_radial_cy = cy; g_radial_hover = -1; g_radial_arc_on = 0;
    g_pan_dir = 0;
    radial_sound("pieopen.ogg");
}
/* closed (a click outside, Esc...) */
static void radial_close(void) { if (!g_radial) return; g_radial = 0; radial_sound("pieclse.ogg"); }
/* right button while it's open: back up one menu, or closed */
static void radial_back(void) {
    if (!g_radial) return;
    if (g_radial_menu == RM_ROOT) { radial_close(); return; }
    g_radial_menu = radial_parent(g_radial_menu); g_radial_hover = -1; g_radial_arc_on = 0;
    radial_sound("pieclse.ogg");
}
/* the arc turns toward the hovered slot; 1 while it moves */
static int radial_tick(float dt) {
    if (!g_radial || g_radial_hover < 0) return 0;
    float target = -1.5707963f + g_radial_hover * 0.7853982f;
    if (!g_radial_arc_on) { g_radial_arc = target; g_radial_arc_on = 1; return 1; }
    float d = wrap_angle(target - g_radial_arc);
    if (fabsf(d) < 1e-3f) return 0;
    float k = dt * 16.0f; if (k > 1.0f) k = 1.0f;
    g_radial_arc = wrap_angle(g_radial_arc + d * k);
    return 1;
}
/* the slot of the root menu shows what's equipped in its submenu */
static const char *radial_icon(int menu, int i) {
    const RadialItem *it = &RADIAL[menu][i];
    if (!it->icon || menu != RM_ROOT || it->action != RA_SUB) return it->icon;
    int sub = it->sub;
    for (int k = 0; k < 8; k++) {
        const RadialItem *s = &RADIAL[sub][k];
        if (!s->icon) continue;
        if (((s->action == RA_WEAPON || s->action == RA_ORB) && !strcmp(DAVID_ACTOR->weapon, s->arg)) || (s->action == RA_SHIELD && !strcmp(DAVID_ACTOR->shield, s->arg)) ||
            (s->action == RA_ITEM && g_radial_equipped[sub] == k + 1)) return s->icon;
    }
    return it->icon;
}
static int radial_at(int x, int y) {
    float half = 22.0f * radial_scale();
    for (int i = 0; i < 8; i++) {
        float sx, sy; radial_slot_pos(i, &sx, &sy);
        if ((x - sx) * (x - sx) + (y - sy) * (y - sy) <= half * half) return i;
    }
    return -1;
}
/* the menus whose items are equipped when chosen */
static int radial_equips(int menu) { return menu == RM_ORBS || menu == RM_RANGED || menu == RM_MAGIC || menu == RM_SPECIALS; }
static int radial_item_on(int menu, int k) {
    const RadialItem *it = &RADIAL[menu][k];
    Actor *a = DAVID_ACTOR;
    return ((it->action == RA_WEAPON || it->action == RA_ORB) && !strcmp(a->weapon, it->arg)) || (it->action == RA_SHIELD && !strcmp(a->shield, it->arg)) ||
           (it->action == RA_ITEM && radial_equips(menu) && g_radial_equipped[menu] == k + 1);
}
/* the mouse moved over the menu: a new slot hovered clicks */
static void radial_hover(int x, int y) {
    int h = radial_at(x, y);
    if (h == g_radial_hover) return;
    g_radial_hover = h;
    if (h >= 0 && RADIAL[g_radial_menu][h].icon) radial_sound("pieslice.ogg");
}
static void radial_click(int x, int y) {
    int k = radial_at(x, y);
    if (k < 0) { radial_close(); return; } /* outside: closed */
    const RadialItem *it = &RADIAL[g_radial_menu][k];
    if (!it->icon) return;
    if (it->action == RA_SUB) { g_radial_menu = it->sub; g_radial_hover = -1; g_radial_arc_on = 0; radial_sound("pieopen.ogg"); return; }
    if (it->action == RA_SHIELD && item_lost(it->arg)) return;
    int menu = g_radial_menu;
    radial_sound("pieselct.ogg"); /* a final choice: chosen, and the menu closes */
    radial_close();
    if (radial_equips(menu) && it->action == RA_ITEM) g_radial_equipped[menu] = k + 1;
    if (DAVID_ACTOR->down || DAVID_ACTOR->dead) return;
    switch (it->action) {
        case RA_ORB: actor_equip_orb(DAVID_ACTOR, it->arg); return;
        case RA_POTION: actor_use_potion(DAVID_ACTOR, it->arg); return;
        case RA_WEAPON: actor_equip_weapon(DAVID_ACTOR, it->arg); return;
        case RA_SHIELD: actor_equip_shield(DAVID_ACTOR, it->arg); return;
        case RA_SPECIAL: actor_special(DAVID_ACTOR, it->arg); return;
        default: snprintf(g_status, sizeof(g_status), radial_equips(menu) ? "%s: equipped (no effect yet)" : "%s: not usable yet", it->name); return;
    }
}
/* into the view layer (game picture, screen pixels; ox, oy: its client position) */
static void radial_draw(uint32_t *px, int W, int H, int ox, int oy) {
    if (!g_radial) return;
    float s = radial_scale();
    int w, h;
    if (g_radial_hover >= 0 && g_radial_arc_on) { /* the arc, pointing at the hovered slot */
        float ra = radial_ring() * 0.42f, th = 3.5f * s, span = 0.42f;
        for (int k = 0; k < 12; k++) {
            float a0 = g_radial_arc - span + 2.0f * span * k / 12, a1 = g_radial_arc - span + 2.0f * span * (k + 1) / 12;
            float x0i = g_radial_cx + cosf(a0) * (ra - th) - ox, y0i = g_radial_cy + sinf(a0) * (ra - th) - oy;
            float x0o = g_radial_cx + cosf(a0) * (ra + th) - ox, y0o = g_radial_cy + sinf(a0) * (ra + th) - oy;
            float x1i = g_radial_cx + cosf(a1) * (ra - th) - ox, y1i = g_radial_cy + sinf(a1) * (ra - th) - oy;
            float x1o = g_radial_cx + cosf(a1) * (ra + th) - ox, y1o = g_radial_cy + sinf(a1) * (ra + th) - oy;
            float ax[3] = { x0i, x0o, x1o }, ay[3] = { y0i, y0o, y1o }, bx[3] = { x0i, x1o, x1i }, by[3] = { y0i, y1o, y1i };
            uint32_t alpha = (uint32_t)(230.0f * (1.0f - fabsf(k - 5.5f) / 7.0f)); /* brighter in the middle */
            blend_tri(px, W, H, ax, ay, 0xFFD25A, alpha);
            blend_tri(px, W, H, bx, by, 0xFFD25A, alpha);
        }
    }
    for (int i = 0; i < 8; i++) {
        float sx, sy;
        radial_slot_pos(i, &sx, &sy);
        const RadialItem *it = &RADIAL[g_radial_menu][i];
        int lost = it->icon && it->action == RA_SHIELD && item_lost(it->arg);
        const uint32_t *sock = sprite_px(g_radial_hover == i || (it->icon && radial_item_on(g_radial_menu, i)) ? "c_items.4" : "c_items.3", &w, &h);
        if (sock) blend_sprite(px, W, H, sock, w, h, (int)(sx - ox - w * s / 2), (int)(sy - oy - h * s / 2), s);
        { /* the ornament on the ring, outside the socket */
            char orn[16]; snprintf(orn, sizeof(orn), "c_cards.%d", 6 + i);
            const uint32_t *op = sprite_px(orn, &w, &h);
            float a = -1.5707963f + i * 0.7853982f, d = radial_ring() + 25.0f * s;
            if (op) blend_sprite(px, W, H, op, w, h, (int)(g_radial_cx + cosf(a) * d - ox - w * s / 2), (int)(g_radial_cy + sinf(a) * d - oy - h * s / 2), s);
        }
        const char *icon = it->icon && !lost ? radial_icon(g_radial_menu, i) : NULL;
        const uint32_t *ip = icon ? sprite_px(icon, &w, &h) : NULL;
        if (ip) blend_sprite(px, W, H, ip, w, h, (int)(sx - ox - w * s / 2), (int)(sy - oy - h * s / 2), s);
    }
}
/* the hovered slot's name (screen text, after the picture) */
static void radial_draw_text(HDC hdc) {
    if (!g_radial || g_radial_hover < 0) return;
    const char *name;
    char t[96];
    {
        const RadialItem *it = &RADIAL[g_radial_menu][g_radial_hover];
        if (!it->icon) return;
        if (it->action == RA_SHIELD && item_lost(it->arg)) return;
        int held = (it->action == RA_WEAPON || it->action == RA_SHIELD) && radial_item_on(g_radial_menu, g_radial_hover);
        snprintf(t, sizeof(t), "%s%s%s", it->name, held ? "  (held: click to put it away)" : "",
                 !held && radial_item_on(g_radial_menu, g_radial_hover) ? "  (equipped)" : "");
        name = t;
    }
    int y = g_radial_cy + (int)(radial_ring() + 50.0f * radial_scale());
    SelectObject(hdc, ui_font(18, 1));
    ui_text(hdc, g_radial_cx - 199, y + 1, 400, 24, name, RGB(0, 0, 0), DT_CENTER | DT_SINGLELINE);
    ui_text(hdc, g_radial_cx - 200, y, 400, 24, name, RGB(255, 230, 140), DT_CENTER | DT_SINGLELINE);
}

/* ---------------- cursor: CAPS LOCK, clipping, edge panning ---------------- */
static HCURSOR g_cur_game = NULL, g_cur_door = NULL, g_cur_pan[9], g_cur_attack = NULL;
static int g_pan_dir = 0;        /* 0 none, 1 up 2 down 3 left 4 right 5 ul 6 ur 7 dl 8 dr */
static int g_clip_active = 0;
static RECT g_clip_rect;

/* A cursor from one of the blockouts' mouse sprites, scaled up (nearest) so it
   stays readable at 1080p, on a 64x64 canvas placed so the hotspot
   (the arrow's tip) can be anywhere: rule "tl"/"top"/"bottom"/"left"/
   "right"/"tr"/"bl"/"br", or an explicit tip (tip_x, tip_y) in sprite
   pixels, centred. */
static HCURSOR cursor_from_sprite(int index, float scale, const char *rule, int tip_x, int tip_y) {
    char path[1024];
    root_path(path, sizeof(path), "assets/sprites/mouse.%d.png", index);
    int sw = 0, sh = 0;
    uint32_t *spr = image_load(path, &sw, &sh);
    if (!spr) return NULL;
    const int C = 64;
    int w = (int)(sw * scale), h = (int)(sh * scale);
    if (w > C) w = C;
    if (h > C) h = C;
    int ox, oy, hx, hy;
    if (!rule) {
        ox = (C - w) / 2; oy = (C - h) / 2;
        hx = ox + (int)(tip_x * scale); hy = oy + (int)(tip_y * scale);
    } else {
        int left = !strcmp(rule, "tl") || !strcmp(rule, "bl") || !strcmp(rule, "left");
        int right = !strcmp(rule, "tr") || !strcmp(rule, "br") || !strcmp(rule, "right");
        int top = !strcmp(rule, "tl") || !strcmp(rule, "tr") || !strcmp(rule, "top");
        int bottom = !strcmp(rule, "bl") || !strcmp(rule, "br") || !strcmp(rule, "bottom");
        ox = left ? 0 : right ? C - w : (C - w) / 2;
        oy = top ? 0 : bottom ? C - h : (C - h) / 2;
        int tx, ty;
        if (!strcmp(rule, "tl")) { tx = 1; ty = 1; }
        else if (!strcmp(rule, "top")) { tx = w / 2; ty = 0; }
        else if (!strcmp(rule, "bottom")) { tx = w / 2; ty = h - 1; }
        else if (!strcmp(rule, "left")) { tx = 0; ty = h / 2; }
        else if (!strcmp(rule, "right")) { tx = w - 1; ty = h / 2; }
        else if (!strcmp(rule, "tr")) { tx = w - 2; ty = 1; }
        else if (!strcmp(rule, "bl")) { tx = 1; ty = h - 2; }
        else { tx = w - 2; ty = h - 2; }
        hx = ox + tx; hy = oy + ty;
    }
    BITMAPV5HEADER bh; memset(&bh, 0, sizeof(bh));
    bh.bV5Size = sizeof(bh); bh.bV5Width = C; bh.bV5Height = -C; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS; bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    HBITMAP mask = CreateBitmap(C, C, 1, 1, NULL);
    HCURSOR cur = NULL;
    if (color && bits && mask) {
        uint32_t *px = (uint32_t *)bits;
        memset(px, 0, (size_t)C * C * 4);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t s = spr[(size_t)((int)(y / scale)) * sw + (int)(x / scale)];
            uint32_t a = s >> 24;
            if (a < 128) continue; /* hard edges, like the original sprites */
            px[(size_t)(oy + y) * C + ox + x] = s | 0xFF000000u;
        }
        ICONINFO ii; ii.fIcon = FALSE; ii.xHotspot = (DWORD)hx; ii.yHotspot = (DWORD)hy; ii.hbmMask = mask; ii.hbmColor = color;
        cur = (HCURSOR)CreateIconIndirect(&ii);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    free(spr);
    return cur;
}

/* mouse.0 normal, mouse.1-8 edge scrolling (up, down, left, right, UL,
   UR, DL, DR), mouse.11 over a scene connector */
static void load_game_cursors(void) {
    static const char *rules[9] = { NULL, "top", "bottom", "left", "right", "tl", "tr", "bl", "br" };
    g_cur_game = cursor_from_sprite(0, 2.0f, "tl", 0, 0);
    for (int i = 1; i < 9; i++) g_cur_pan[i] = cursor_from_sprite(i, 2.0f, rules[i], 0, 0);
    g_cur_door = cursor_from_sprite(11, 1.5f, NULL, 21, 21);
    g_cur_attack = cursor_from_sprite(29, 2.0f, "tl", 0, 0); /* the sword: attack mode */
    if (!g_cur_game) g_cur_game = LoadCursorA(NULL, IDC_ARROW);
}

/* Playing (CAPS LOCK off): the cursor is kept inside the game picture so
   the tools never get in the way. Released whenever the window isn't in
   front, CAPS LOCK is on, or the room list is open. */
static void update_cursor_clip(HWND hwnd) {
    static int no_clip = -1; /* SILVER_NO_CLIP=1: automated test runs never trap the mouse */
    if (no_clip < 0) no_clip = getenv("SILVER_NO_CLIP") != NULL;
    int want = !no_clip && g_loaded && !caps_on() && !g_map_mode && !screen_view() && GetForegroundWindow() == hwnd && !IsIconic(hwnd);
    if (!want) {
        if (g_clip_active) { ClipCursor(NULL); g_clip_active = 0; }
        return;
    }
    RECT r; game_rect_client(&r);
    POINT a = { r.left, r.top }, b = { r.right, r.bottom };
    ClientToScreen(hwnd, &a); ClientToScreen(hwnd, &b);
    RECT sr = { a.x, a.y, b.x, b.y };
    if (!g_clip_active || !EqualRect(&sr, &g_clip_rect)) { ClipCursor(&sr); g_clip_rect = sr; g_clip_active = 1; }
}

static int mouse_in_game(int cx, int cy) { RECT r; game_rect_client(&r); return cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom; }

/* Which way the view would scroll with the mouse here (0 = none): only
   directions the view can actually still move in. */
static int pan_direction(int cx, int cy) {
    if (caps_on() || g_map_mode || screen_view() || script_playing() || g_radial || g_camslide.active || !g_loaded || g_drag != DRAG_NONE || g_free_cam) return 0;
    RECT r; game_rect_client(&r);
    if (!(cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom)) return 0;
    const int EDGE = 18;
    int win_w, win_h, ox, oy; float sc; get_view_window(&win_w, &win_h, &sc, &ox, &oy);
    int max_x = (int)g_hdr.width - win_w, max_y = (int)g_hdr.height - win_h;
    int up = cy < r.top + EDGE && g_cam_y > 0, down = cy >= r.bottom - EDGE && g_cam_y < max_y;
    int left = cx < r.left + EDGE && g_cam_x > 0, right = cx >= r.right - EDGE && g_cam_x < max_x;
    if (up && left) return 5;
    if (up && right) return 6;
    if (down && left) return 7;
    if (down && right) return 8;
    if (up) return 1;
    if (down) return 2;
    if (left) return 3;
    if (right) return 4;
    return 0;
}

static HCURSOR cursor_for(int cx, int cy) {
    if (screen_view()) return ui_hit(cx, cy) != B_NONE ? LoadCursorA(NULL, IDC_HAND) : g_cursor_arrow;
    if (g_map_mode) return g_cursor_arrow;
    if (g_sv_pick && mouse_in_game(cx, cy)) return LoadCursorA(NULL, IDC_CROSS);
    if (!mouse_in_game(cx, cy)) return ui_hit(cx, cy) != B_NONE ? LoadCursorA(NULL, IDC_HAND) : g_cursor_arrow;
    if (g_edit_mode) { /* editor: standard Windows cursors, never the game's */
        if (g_pan_dir) {
            static const LPCSTR pan_std[9] = { IDC_ARROW, IDC_SIZENS, IDC_SIZENS, IDC_SIZEWE, IDC_SIZEWE, IDC_SIZENWSE, IDC_SIZENESW, IDC_SIZENESW, IDC_SIZENWSE };
            return LoadCursorA(NULL, pan_std[g_pan_dir]);
        }
        if (g_tool != TOOL_SELECT || wizard_picking_points()) return LoadCursorA(NULL, IDC_CROSS);
        int rx, ry; client_to_room_point(cx, cy, &rx, &ry);
        if (handle_hit((float)rx, (float)ry) >= 0) return LoadCursorA(NULL, IDC_SIZEALL);
        return g_cursor_arrow;
    }
    if (attack_mode() && g_cur_attack) return g_cur_attack;
    if (g_pan_dir && g_cur_pan[g_pan_dir]) return g_cur_pan[g_pan_dir];
    if (g_hovered_door >= 0 && !g_show_walkable && g_cur_door) return g_cur_door;
    return g_cur_game;
}

/* =====================================================================
   ANIMATION VIEWER (A) + CHARACTER PICKER (Tab)
   A character of assets/chars (David by default) playing the clips made
   for its skeleton: its own folder's clips first, then the shared ones
   of chars/anims with the same skeleton. Big studio view: searchable
   list on the left, stage on the right (drag = rotate, wheel = zoom),
   playback buttons + shortcuts. The picker shows every character as a
   card with a 3D preview (grid, mouse or keyboard).
   ===================================================================== */
static int g_anim_sel = 0, g_anim_scroll = 0, g_anim_paused = 0, g_anim_follow = 1;
static float g_anim_t = 0.0f, g_anim_speed = 1.0f;
#define ANIM_DEFAULT_YAW 3.74f /* front three-quarter view */
static float g_anim_yaw = ANIM_DEFAULT_YAW, g_anim_pitch = 0.15f, g_anim_zoom = 1.0f;
static char g_anim_filter[32] = "";
static int g_anim_filter_len = 0;
static int g_anim_filtered[4096], g_anim_filtered_count = 0;
static int g_anim_drag = 0, g_anim_drag_x = 0, g_anim_drag_y = 0;
static int g_anim_skip_char = 0; /* the WM_CHAR of the key that opened the viewer */
static RECT g_anim_list_rc, g_anim_stage_rc;
#define ANIM_ROW_H 20
static CharModel *g_view_model = &g_david; /* the character shown */

/* ---- characters of assets/chars (scanned the first time the picker opens) ---- */
typedef struct {
    CharModel model;
    int own_clips;          /* .gltf files in its folder besides the model */
    int preview_clip;       /* clip used for its card (-1: rest pose) */
    uint32_t *preview;      /* cached card picture, PICK_PW x PICK_PH */
} PickEntry;
static PickEntry *g_pick = NULL;
static int g_pick_count = 0, g_pick_scanned = 0;
static int g_picker = 0;                     /* picker screen shown (inside the viewer) */
static int g_pick_sel = 0, g_pick_scroll = 0, g_pick_hover = -1;
static char g_pick_filter[32] = "";
static int g_pick_filter_len = 0;
static int g_pick_filtered[512], g_pick_filtered_count = 0;
static RECT g_pick_grid_rc;
static int g_mouse_x = 0, g_mouse_y = 0;
#define PICK_PW 176
#define PICK_PH 176
#define PICK_CARD_H (PICK_PH + 40)
#define PICK_GAP 12

static int g_anim_view_lib_n = 0;
static void anim_view_filter(void);
static void sv_model_chosen(const char *name); /* SCRIPTS */
static void ms_open(CharModel *m);            /* moveset screen */

/* A character's own clips join the animation library once (the picker
   and the scripts' characters both need them). Returns how many it has. */
static int char_clips_add(const char *name, const char *dir) {
    static char added[512][48];
    static int n_added = 0;
    int known = 0;
    for (int i = 0; i < n_added; i++) if (!strcmp(added[i], name)) known = 1;
    if (!known) {
        if (n_added < 512) snprintf(added[n_added++], sizeof(added[0]), "%s", name);
        return anim_lib_add_dir(dir, name);
    }
    int c = 0;
    for (int k = 0; k < anim_lib_count(); k++) if (!strcmp(anim_lib_source(k), name)) c++;
    return c;
}

static int pick_name_cmp(const void *a, const void *b) { return strcmp(((const PickEntry *)a)->model.name, ((const PickEntry *)b)->model.name); }

/* Every folder of assets/chars holding a skinned character <name>/<name>.gltf
   (not the shared clips, the items, nor the 4-6 bone props: weapons, keys...). */
static void pick_scan(void) {
    if (g_pick_scanned) return;
    g_pick_scanned = 1;
    char pat[1024];
    root_path(pat, sizeof(pat), "assets/chars/*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int cap = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (!_stricmp(fd.cFileName, "anims") || !_stricmp(fd.cFileName, "items")) continue;
        char dir[1024];
        root_path(dir, sizeof(dir), "assets/chars/%s", fd.cFileName);
        if (g_pick_count == cap) { cap = cap ? cap * 2 : 256; g_pick = (PickEntry *)realloc(g_pick, sizeof(PickEntry) * cap); }
        PickEntry *e = &g_pick[g_pick_count];
        memset(e, 0, sizeof(*e));
        char lower[64]; snprintf(lower, sizeof(lower), "%s", fd.cFileName);
        for (char *q = lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
        if (!char_model_load(&e->model, dir, lower) || e->model.node_count <= 6) { char_model_free(&e->model); continue; }
        /* its own clips join the library (David's are in it already) */
        e->own_clips = strcmp(lower, "david") ? char_clips_add(lower, dir) : 0;
        g_pick_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_pick, g_pick_count, sizeof(PickEntry), pick_name_cmp);
    for (int i = 0; i < g_pick_count; i++) {
        PickEntry *e = &g_pick[i];
        if (!strcmp(e->model.name, "david")) {
            for (int k = 0; k < anim_lib_count(); k++) if (!strcmp(anim_lib_source(k), "david")) e->own_clips++;
        }
        e->preview_clip = -2; /* chosen on first draw */
    }
    g_anim_view_lib_n = anim_lib_count();
}

/* The model of a character of assets/chars by its folder name (David's
   own, the picker's if it was opened, else loaded now -- once). */
static CharModel *model_by_name(const char *name) {
    if (!name || !name[0]) return NULL;
    if (!strcmp(name, "david")) return &g_david;
    for (int i = 0; i < g_pick_count; i++) if (!strcmp(g_pick[i].model.name, name)) return &g_pick[i].model;
    static CharModel *loaded[64];
    static int n_loaded = 0;
    for (int i = 0; i < n_loaded; i++) if (!strcmp(loaded[i]->name, name)) return loaded[i];
    if (n_loaded >= 64) return NULL;
    char dir[1024];
    root_path(dir, sizeof(dir), "assets/chars/%s", name);
    CharModel *m = (CharModel *)calloc(1, sizeof(CharModel));
    if (!m || !char_model_load(m, dir, name) || m->node_count <= 6) { if (m) { char_model_free(m); free(m); } return NULL; }
    char_clips_add(name, dir);
    loaded[n_loaded++] = m;
    return m;
}

/* A standing pose for the card: an own clip named like an idle, the shared
   "stand" when it fits, else the first own clip, else the rest pose. */
static int pick_preview_clip(const PickEntry *e) {
    const CharModel *m = &e->model;
    static const char *idle[] = { "stand", "idle", "still", "bob", "wait" };
    int first_own = -1;
    for (int k = 0; k < anim_lib_count(); k++) {
        if (strcmp(anim_lib_source(k), m->name)) continue;
        if (!anim_lib_fits(k, m->node_count)) continue;
        if (first_own < 0) first_own = k;
        for (int q = 0; q < 5; q++) if (strstr(anim_lib_name(k), idle[q])) return k;
    }
    int st = anim_lib_find("stand");
    if (anim_lib_fits(st, m->node_count)) return st;
    return first_own;
}

static void anim_view_rects(HWND hwnd);
#define ANIM_BUTTONS_W 1192 /* total width of the button row */
static void anim_view_rects(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int two_rows = (rc.right - 10 - 300) < ANIM_BUTTONS_W;
    SetRect(&g_anim_list_rc, 10, 64, 290, rc.bottom - 10);
    SetRect(&g_anim_stage_rc, 300, 44, rc.right - 10, rc.bottom - (two_rows ? 94 : 60));
    SetRect(&g_pick_grid_rc, 12, 72, rc.right - 12, rc.bottom - 12);
}
static int anim_rows_visible(void) { int n = (g_anim_list_rc.bottom - g_anim_list_rc.top) / ANIM_ROW_H; return n < 1 ? 1 : n; }
static int anim_current_clip(void) { return (g_anim_filtered_count > 0 && g_anim_sel < g_anim_filtered_count) ? g_anim_filtered[g_anim_sel] : -1; }

/* The clip list's scrollbar: drag the thumb, click (or hold) the track to
   page towards the mouse, wheel anywhere over the list. */
#define ANIM_SB_W 12
#define ANIM_SB_TIMER_ID 3
static int g_anim_sb_grab = 0;    /* thumb drag: mouse offset in the thumb */
static int g_anim_sb_page = 0;    /* -1/+1 while the track is held */
static int g_anim_sb_hover = 0;
static int anim_sb_geom(RECT *track, RECT *thumb) {
    int rows = anim_rows_visible();
    if (g_anim_filtered_count <= rows) return 0;
    *track = g_anim_list_rc; track->left = track->right - ANIM_SB_W;
    int th = track->bottom - track->top;
    int bh = th * rows / g_anim_filtered_count; if (bh < 24) bh = 24;
    int by = track->top + (th - bh) * g_anim_scroll / (g_anim_filtered_count - rows);
    *thumb = *track; thumb->top = by; thumb->bottom = by + bh;
    return 1;
}
static void anim_scroll_to(int s) {
    int maxs = g_anim_filtered_count - anim_rows_visible();
    if (s > maxs) s = maxs;
    if (s < 0) s = 0;
    g_anim_scroll = s;
}
/* one page towards the mouse, until the thumb reaches it */
static void anim_sb_page_step(void) {
    RECT tr, tb;
    if (!g_anim_sb_page || !anim_sb_geom(&tr, &tb)) return;
    if ((g_anim_sb_page < 0 && g_mouse_y >= tb.top) || (g_anim_sb_page > 0 && g_mouse_y < tb.bottom)) return;
    anim_scroll_to(g_anim_scroll + g_anim_sb_page * anim_rows_visible());
}

/* The clips shown for the current character: its own folder's first,
   then the shared ones of chars/anims, all with its skeleton. */
static void anim_view_filter(void) {
    int keep = anim_current_clip();
    const CharModel *m = g_view_model;
    g_anim_filtered_count = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < anim_lib_count() && g_anim_filtered_count < 4096; i++) {
            const char *src = anim_lib_source(i);
            if (pass == 0 ? strcmp(src, m->name) != 0 : pass == 1 ? strcmp(src, "anims") != 0 : (!strcmp(src, m->name) || !strcmp(src, "anims"))) continue;
            if (g_anim_filter[0] && !ci_strstr(anim_lib_name(i), g_anim_filter)) continue;
            if (anim_lib_fits(i, m->node_count)) g_anim_filtered[g_anim_filtered_count++] = i;
        }
    g_anim_sel = 0;
    for (int k = 0; k < g_anim_filtered_count; k++) if (g_anim_filtered[k] == keep) g_anim_sel = k;
    g_anim_scroll = 0;
}
static void anim_select(int k) {
    if (g_anim_filtered_count <= 0) return;
    if (k < 0) k = 0;
    if (k >= g_anim_filtered_count) k = g_anim_filtered_count - 1;
    if (k != g_anim_sel) g_anim_t = 0.0f;
    g_anim_sel = k;
    int rows = anim_rows_visible();
    if (g_anim_sel < g_anim_scroll) g_anim_scroll = g_anim_sel;
    if (g_anim_sel >= g_anim_scroll + rows) g_anim_scroll = g_anim_sel - rows + 1;
}
static void anim_view_toggle(void) {
    if (g_anim_view) { g_anim_view = 0; return; }
    if (anim_lib_count() <= 0) { snprintf(g_status, sizeof(g_status), "no animations found in assets/chars/anims"); return; }
    g_map_mode = 0;
    g_anim_view = 1;
    g_picker = 0;
    anim_view_filter();
    ClipCursor(NULL); g_clip_active = 0;
}

/* ---- rendering a character: skinned + lit + textured into 32-bit pixels ---- */
static void anim_px_line(uint32_t *px, int W, int H, float x0, float y0, float x1, float y1, uint32_t col) {
    int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0))) + 1;
    if (n > 4000) n = 4000;
    for (int i = 0; i <= n; i++) {
        int x = (int)(x0 + (x1 - x0) * i / n), y = (int)(y0 + (y1 - y0) * i / n);
        if (x >= 0 && y >= 0 && x < W && y < H) px[(size_t)y * W + x] = col;
    }
}
static void anim_raster_tri(const CharModel *m, uint32_t *px, float *zb, int W, int H, const float *sx, const float *sy, const float *sz,
                            const float *u, const float *v, float shade) {
    int minx = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), maxx = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    int miny = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2]))), maxy = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= W) maxx = W - 1;
    if (maxy >= H) maxy = H - 1;
    float den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
    if (fabsf(den) < 1e-6f) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        float fx = x + 0.5f, fy = y + 0.5f;
        float w0 = ((sy[1] - sy[2]) * (fx - sx[2]) + (sx[2] - sx[1]) * (fy - sy[2])) / den;
        float w1 = ((sy[2] - sy[0]) * (fx - sx[2]) + (sx[0] - sx[2]) * (fy - sy[2])) / den;
        float w2 = 1.0f - w0 - w1;
        if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
        float z = 1.0f / (w0 / sz[0] + w1 / sz[1] + w2 / sz[2]);
        size_t i = (size_t)y * W + x;
        if (z >= zb[i]) continue;
        zb[i] = z;
        float tu = (w0 * u[0] / sz[0] + w1 * u[1] / sz[1] + w2 * u[2] / sz[2]) * z;
        float tv = (w0 * v[0] / sz[0] + w1 * v[1] / sz[1] + w2 * v[2] / sz[2]) * z;
        int iu = (int)tu, iv = (int)tv;
        if (iu < 0) iu = 0;
        if (iu >= m->tex_w) iu = m->tex_w - 1;
        if (iv < 0) iv = 0;
        if (iv >= m->tex_h) iv = m->tex_h - 1;
        const uint8_t *c = m->tex_rgb + ((size_t)iv * m->tex_w + iu) * 3;
        int r = (int)(c[0] * shade), g = (int)(c[1] * shade), b = (int)(c[2] * shade);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        px[i] = (uint32_t)((r << 16) | (g << 8) | b);
    }
}

/* `m` posed by `clip` at time t (rest pose if clip < 0), orbit camera
   (yaw/pitch/zoom); follow = frame the whole body, grid = floor grid. */
static void render_char_view(const CharModel *m, const CharModel *item, const CharModel *shield, int clip, float t, uint32_t *px, int W, int H,
                             float yaw, float pitch, float zoom, int follow, int grid) {
    const CharModel *item_right = item;
    static float *zb = NULL; static size_t zbn = 0;
    size_t npx = (size_t)W * H;
    if (zbn < npx) { free(zb); zb = (float *)malloc(npx * sizeof(float)); zbn = npx; }
    for (int y = 0; y < H; y++) { /* studio backdrop */
        float k = (float)y / H;
        uint32_t col = (uint32_t)(((int)(46 - 26 * k) << 16) | ((int)(48 - 27 * k) << 8) | (int)(58 - 30 * k));
        for (int x = 0; x < W; x++) px[(size_t)y * W + x] = col;
    }
    for (size_t i = 0; i < npx; i++) zb[i] = 1e30f;
    if (m->vertex_count <= 0) return;

    NodeOverride ov[DAVID_MAX_NODES];
    if (clip >= 0 && anim_lib_fits(clip, m->node_count)) anim_lib_sample_for(clip, t, m, ov);
    else memset(ov, 0, sizeof(ov));
    static Mat4 mats[DAVID_MAX_JOINTS];
    skeleton_skin_matrices_for(m, ov, mats);
    static float sk[DAVID_MAX_VERTS][3];
    float cxs = 0, czs = 0, ymin = 1e9f, ymax = -1e9f, xmin = 1e9f, xmax = -1e9f, zmin = 1e9f, zmax = -1e9f;
    for (int vi = 0; vi < m->vertex_count; vi++) {
        float acc[3] = { 0, 0, 0 };
        for (int k = 0; k < 4; k++) {
            float w = m->weights[vi][k];
            if (w <= 0) continue;
            float sp[3]; mat4_vec3(&mats[m->joints_idx[vi][k]], m->positions[vi], sp);
            acc[0] += sp[0] * w; acc[1] += sp[1] * w; acc[2] += sp[2] * w;
        }
        memcpy(sk[vi], acc, sizeof(acc));
        cxs += acc[0]; czs += acc[2];
        ymin = fminf(ymin, acc[1]); ymax = fmaxf(ymax, acc[1]);
        xmin = fminf(xmin, acc[0]); xmax = fmaxf(xmax, acc[0]); zmin = fminf(zmin, acc[2]); zmax = fmaxf(zmax, acc[2]);
    }
    /* camera: orbits a target; "follow" keeps the character centred (a
       clip that moves it, big creatures), "fixed" shows motion over the grid */
    float tx = 0, tz = 0, ty = 0.95f, body_h = 1.93f;
    if (!(ymax - ymin < 1e6f)) { ymin = 0; ymax = 1.93f; cxs = czs = 0; } /* broken data guard */
    if (follow) {
        tx = cxs / m->vertex_count; tz = czs / m->vertex_count;
        ty = (ymin + ymax) * 0.5f;
        float span = fmaxf(ymax - ymin, 0.8f * fmaxf(xmax - xmin, zmax - zmin));
        body_h = fmaxf(0.5f, span);
    }
    float D = 2.3f * body_h / zoom, f = H * 1.15f;
    float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), spp = sinf(pitch);
    #define ANIM_PROJ(wx, wy, wz, ox, oy, oz) do { \
        float x_ = (wx) - tx, y_ = (wy) - ty, z_ = (wz) - tz; \
        float xr = x_ * cyw + z_ * syw, zr = -x_ * syw + z_ * cyw; \
        float yr = y_ * cp - zr * spp, zz = y_ * spp + zr * cp; \
        (oz) = D - zz; (ox) = W * 0.5f + f * xr / (oz); (oy) = H * 0.5f - f * yr / (oz); } while (0)
    if (grid) { /* ground grid (world-fixed, 0.5 units) + soft shadow */
        float g0x = floorf(tx) - 4.0f, g0z = floorf(tz) - 4.0f;
        for (int i = 0; i <= 16; i++) {
            float a = i * 0.5f;
            uint32_t col = (i % 2 == 0) ? 0x50525E : 0x3A3C46;
            float ax, ay, az, bx, by, bz;
            ANIM_PROJ(g0x + a, 0.0f, g0z, ax, ay, az); ANIM_PROJ(g0x + a, 0.0f, g0z + 8.0f, bx, by, bz);
            if (az > 0.2f && bz > 0.2f) anim_px_line(px, W, H, ax, ay, bx, by, col);
            ANIM_PROJ(g0x, 0.0f, g0z + a, ax, ay, az); ANIM_PROJ(g0x + 8.0f, 0.0f, g0z + a, bx, by, bz);
            if (az > 0.2f && bz > 0.2f) anim_px_line(px, W, H, ax, ay, bx, by, col);
        }
        float scx = cxs / m->vertex_count, scz = czs / m->vertex_count;
        for (int ring = 6; ring >= 1; ring--) {
            float rr = 0.08f * ring, lx = 0, ly = 0, lz = 0; int first = 1;
            for (int k = 0; k <= 24; k++) {
                float a = 6.2831853f * k / 24, sx2, sy2, sz2;
                ANIM_PROJ(scx + cosf(a) * rr, 0.0f, scz + sinf(a) * rr, sx2, sy2, sz2);
                if (!first && sz2 > 0.2f && lz > 0.2f) anim_px_line(px, W, H, lx, ly, sx2, sy2, 0x1C1D24);
                lx = sx2; ly = sy2; lz = sz2; first = 0;
            }
        }
    }
    static float vx[DAVID_MAX_VERTS], vy[DAVID_MAX_VERTS], vz[DAVID_MAX_VERTS];
    for (int vi = 0; vi < m->vertex_count; vi++) ANIM_PROJ(sk[vi][0], sk[vi][1], sk[vi][2], vx[vi], vy[vi], vz[vi]);
    const float L[3] = { -0.45f, 0.75f, 0.48f };
    for (int tr = 0; tr < m->index_count / 3; tr++) {
        int i0 = m->indices[tr * 3], i1 = m->indices[tr * 3 + 1], i2 = m->indices[tr * 3 + 2];
        if (vz[i0] < 0.1f || vz[i1] < 0.1f || vz[i2] < 0.1f) continue;
        float e1[3] = { sk[i1][0] - sk[i0][0], sk[i1][1] - sk[i0][1], sk[i1][2] - sk[i0][2] };
        float e2[3] = { sk[i2][0] - sk[i0][0], sk[i2][1] - sk[i0][1], sk[i2][2] - sk[i0][2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        float d = nl > 1e-9f ? fabsf((n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / nl) : 0.5f;
        float shade = 0.55f + 0.65f * d;
        float sx3[3] = { vx[i0], vx[i1], vx[i2] }, sy3[3] = { vy[i0], vy[i1], vy[i2] }, sz3[3] = { vz[i0], vz[i1], vz[i2] };
        float u3[3] = { m->uvs_px[i0][0], m->uvs_px[i1][0], m->uvs_px[i2][0] };
        float v3[3] = { m->uvs_px[i0][1], m->uvs_px[i1][1], m->uvs_px[i2][1] };
        anim_raster_tri(m, px, zb, W, H, sx3, sy3, sz3, u3, v3, shade);
    }
    for (int side = 0; side < 3; side++) { /* what it holds, in its hands (the left: a pair's second blade), its shield */
    Mat4 hand;
    if (side == 1) { item = NULL; if (!shield) for (int i = 0; i < g_pair_count; i++) if (g_pairs[i].right == item_right) item = g_pairs[i].left; }
    if (side == 2) item = shield;
    if (item && item->vertex_count > 0 && skeleton_node_global(side == 2 ? shield_node(m) : grip_node(m, side), &hand)) {
        static float iw3[DAVID_MAX_VERTS][3];
        for (int vi = 0; vi < item->vertex_count; vi++) {
            mat4_vec3(&hand, item->positions[vi], iw3[vi]);
            ANIM_PROJ(iw3[vi][0], iw3[vi][1], iw3[vi][2], vx[vi], vy[vi], vz[vi]);
        }
        for (int tr = 0; tr < item->index_count / 3; tr++) {
            int i0 = item->indices[tr * 3], i1 = item->indices[tr * 3 + 1], i2 = item->indices[tr * 3 + 2];
            if (vz[i0] < 0.1f || vz[i1] < 0.1f || vz[i2] < 0.1f) continue;
            float e1[3] = { iw3[i1][0] - iw3[i0][0], iw3[i1][1] - iw3[i0][1], iw3[i1][2] - iw3[i0][2] };
            float e2[3] = { iw3[i2][0] - iw3[i0][0], iw3[i2][1] - iw3[i0][1], iw3[i2][2] - iw3[i0][2] };
            float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            float d = nl > 1e-9f ? fabsf((n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / nl) : 0.5f;
            float sx3[3] = { vx[i0], vx[i1], vx[i2] }, sy3[3] = { vy[i0], vy[i1], vy[i2] }, sz3[3] = { vz[i0], vz[i1], vz[i2] };
            float u3[3] = { item->uvs_px[i0][0], item->uvs_px[i1][0], item->uvs_px[i2][0] };
            float v3[3] = { item->uvs_px[i0][1], item->uvs_px[i1][1], item->uvs_px[i2][1] };
            anim_raster_tri(item, px, zb, W, H, sx3, sy3, sz3, u3, v3, 0.55f + 0.65f * d);
        }
    }
    }
    #undef ANIM_PROJ
}

/* 32-bit top-down pixels -> screen */
static void blit_pixels(HDC hdc, int x, int y, int W, int H, const uint32_t *px) {
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, x, y, W, H, 0, 0, 0, H, px, &bi, DIB_RGB_COLORS);
}

static void anim_render_stage(HDC hdc, const RECT *r) {
    int W = r->right - r->left, H = r->bottom - r->top;
    if (W < 16 || H < 16) return;
    static uint32_t *px = NULL; static size_t cap = 0;
    if (cap < (size_t)W * H) { free(px); px = (uint32_t *)malloc((size_t)W * H * 4); cap = (size_t)W * H; }
    render_char_view(g_view_model, model_right_hand(g_view_model), model_left_shield(g_view_model), anim_current_clip(), g_anim_t, px, W, H, g_anim_yaw, g_anim_pitch, g_anim_zoom, g_anim_follow, 1);
    blit_pixels(hdc, r->left, r->top, W, H, px);
}

/* ---- picker ---- */
static int pick_cols(void) { int w = g_pick_grid_rc.right - g_pick_grid_rc.left; int c = (w + PICK_GAP) / (PICK_PW + PICK_GAP); return c < 1 ? 1 : c; }
static int pick_rows_visible(void) { int h = g_pick_grid_rc.bottom - g_pick_grid_rc.top; int r = (h + PICK_GAP) / (PICK_CARD_H + PICK_GAP); return r < 1 ? 1 : r; }
static void pick_filter(void) {
    int keep = (g_pick_filtered_count > 0 && g_pick_sel < g_pick_filtered_count) ? g_pick_filtered[g_pick_sel] : -1;
    g_pick_filtered_count = 0;
    for (int i = 0; i < g_pick_count && g_pick_filtered_count < 512; i++)
        if (!g_pick_filter[0] || ci_strstr(g_pick[i].model.name, g_pick_filter)) g_pick_filtered[g_pick_filtered_count++] = i;
    g_pick_sel = 0;
    for (int k = 0; k < g_pick_filtered_count; k++) if (g_pick_filtered[k] == keep) g_pick_sel = k;
    g_pick_scroll = 0;
}
static void pick_select(int k) {
    if (g_pick_filtered_count <= 0) return;
    if (k < 0) k = 0;
    if (k >= g_pick_filtered_count) k = g_pick_filtered_count - 1;
    g_pick_sel = k;
    int cols = pick_cols(), rows = pick_rows_visible(), row = k / cols;
    if (row < g_pick_scroll) g_pick_scroll = row;
    if (row >= g_pick_scroll + rows) g_pick_scroll = row - rows + 1;
}
static void picker_open(void) {
    pick_scan();
    g_picker = 1;
    g_pick_filter[0] = 0; g_pick_filter_len = 0;
    pick_filter();
    for (int k = 0; k < g_pick_filtered_count; k++) /* start on the character shown */
        if (!strcmp(g_pick[g_pick_filtered[k]].model.name, g_view_model->name)) { pick_select(k); break; }
}
/* Opens the animation menu with the chosen character. */
static void picker_choose(void) {
    if (g_pick_filtered_count <= 0) return;
    PickEntry *e = &g_pick[g_pick_filtered[g_pick_sel]];
    if (g_pick_for_script) { /* a script's "Place character": back to the scripts screen */
        g_pick_for_script = 0; g_picker = 0; g_anim_view = 0;
        sv_model_chosen(e->model.name);
        return;
    }
    g_view_model = !strcmp(e->model.name, "david") ? &g_david : &e->model;
    g_picker = 0;
    g_anim_filter[0] = 0; g_anim_filter_len = 0;
    g_anim_sel = 0; g_anim_t = 0.0f;
    g_anim_filtered_count = 0;
    anim_view_filter();
    int pv = e->preview_clip >= 0 ? e->preview_clip : -1; /* start on its standing clip */
    for (int k = 0; k < g_anim_filtered_count; k++) if (g_anim_filtered[k] == pv) { anim_select(k); break; }
    g_anim_yaw = ANIM_DEFAULT_YAW; g_anim_pitch = 0.15f; g_anim_zoom = 1.0f;
}
static int pick_card_at(int x, int y) {
    if (x < g_pick_grid_rc.left || y < g_pick_grid_rc.top || x >= g_pick_grid_rc.right || y >= g_pick_grid_rc.bottom) return -1;
    int cols = pick_cols();
    int cx = (x - g_pick_grid_rc.left) / (PICK_PW + PICK_GAP), cy = (y - g_pick_grid_rc.top) / (PICK_CARD_H + PICK_GAP);
    int ox = (x - g_pick_grid_rc.left) % (PICK_PW + PICK_GAP), oy = (y - g_pick_grid_rc.top) % (PICK_CARD_H + PICK_GAP);
    if (cx >= cols || ox >= PICK_PW || oy >= PICK_CARD_H) return -1;
    int k = (g_pick_scroll + cy) * cols + cx;
    return k < g_pick_filtered_count ? k : -1;
}

static void picker_paint(HWND hwnd, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    ui_fill(hdc, &rc, RGB(14, 14, 18));
    HFONT fh = ui_font(20, 1), fn = ui_font(14, 0), fs = ui_font(12, 1);
    char t[200];
    SelectObject(hdc, fh);
    snprintf(t, sizeof(t), "Choose a character  (%d / %d)", g_pick_filtered_count, g_pick_count);
    ui_text(hdc, 12, 10, 600, 26, t, RGB(255, 225, 120), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, fn);
    snprintf(t, sizeof(t), "filter: %s_", g_pick_filter);
    ui_text(hdc, 12, 40, 300, 18, t, RGB(200, 200, 200), DT_LEFT | DT_SINGLELINE);
    SelectObject(hdc, fs);
    ui_text(hdc, rc.right - 760, 14, 748, 16, "ARROWS / CLICK = SELECT   ENTER / DOUBLE-CLICK = OPEN   TYPE = FILTER   WHEEL = SCROLL   ESC = BACK",
            RGB(120, 150, 190), DT_RIGHT | DT_SINGLELINE);
    int cols = pick_cols(), rows = pick_rows_visible();
    static uint32_t live[PICK_PW * PICK_PH];
    for (int r = 0; r < rows + 1; r++) for (int cidx = 0; cidx < cols; cidx++) {
        int k = (g_pick_scroll + r) * cols + cidx;
        if (k >= g_pick_filtered_count) break;
        int x = g_pick_grid_rc.left + cidx * (PICK_PW + PICK_GAP), y = g_pick_grid_rc.top + r * (PICK_CARD_H + PICK_GAP);
        if (y + PICK_CARD_H > g_pick_grid_rc.bottom) break;
        PickEntry *e = &g_pick[g_pick_filtered[k]];
        if (e->preview_clip == -2) e->preview_clip = pick_preview_clip(e);
        const uint32_t *img;
        if (k == g_pick_sel) { /* selected: alive, turning slowly */
            float dur = anim_lib_duration(e->preview_clip);
            float tt = dur > 0 ? fmodf(g_anim_t, dur) : 0.0f;
            render_char_view(&e->model, NULL, NULL, e->preview_clip, tt, live, PICK_PW, PICK_PH, ANIM_DEFAULT_YAW + g_anim_t * 0.6f, 0.15f, 1.0f, 1, 0);
            img = live;
        } else {
            if (!e->preview) {
                e->preview = (uint32_t *)malloc(PICK_PW * PICK_PH * 4);
                render_char_view(&e->model, NULL, NULL, e->preview_clip, 0.0f, e->preview, PICK_PW, PICK_PH, ANIM_DEFAULT_YAW, 0.15f, 1.0f, 1, 0);
            }
            img = e->preview;
        }
        RECT card = { x - 2, y - 2, x + PICK_PW + 2, y + PICK_CARD_H + 2 };
        int cur = (e->model.name[0] && !strcmp(e->model.name, g_view_model->name));
        ui_fill(hdc, &card, k == g_pick_sel ? RGB(70, 58, 12) : k == g_pick_hover ? RGB(44, 44, 54) : RGB(26, 26, 32));
        blit_pixels(hdc, x, y, PICK_PW, PICK_PH, img);
        if (k == g_pick_sel) ui_frame(hdc, &card, RGB(255, 210, 60));
        else if (k == g_pick_hover) ui_frame(hdc, &card, RGB(150, 150, 165));
        SelectObject(hdc, ui_font(15, 1));
        ui_text(hdc, x + 6, y + PICK_PH + 2, PICK_PW - 12, 18, e->model.name, k == g_pick_sel ? RGB(255, 230, 90) : RGB(235, 235, 240), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, fn);
        snprintf(t, sizeof(t), "%d bones  |  %d own clips%s", e->model.node_count, e->own_clips, cur ? "  |  shown" : "");
        ui_text(hdc, x + 6, y + PICK_PH + 20, PICK_PW - 12, 16, t, RGB(140, 140, 150), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    int total_rows = (g_pick_filtered_count + cols - 1) / cols;
    if (total_rows > rows) { /* scrollbar */
        int th = g_pick_grid_rc.bottom - g_pick_grid_rc.top;
        int bh = th * rows / total_rows; if (bh < 24) bh = 24;
        int by = g_pick_grid_rc.top + (th - bh) * g_pick_scroll / (total_rows - rows);
        RECT sb = { rc.right - 7, by, rc.right - 3, by + bh };
        ui_fill(hdc, &sb, RGB(90, 90, 100));
    }
    SelectObject(hdc, GetStockObject(SYSTEM_FONT));
}

/* buttons under the stage (hover descriptions + shortcuts like the rest) */
static void anim_view_layout(HWND hwnd) {
    anim_view_rects(hwnd);
    g_btn_count = 0;
    if (g_picker) return;
    int x = g_anim_stage_rc.left, y = g_anim_stage_rc.bottom + 12, h = 28, gap = 6;
    static char model_lbl[64];
    snprintf(model_lbl, sizeof(model_lbl), "Model: %s", g_view_model->name);
    struct { int id, w; const char *label, *key, *desc; int on; } b[] = {
        { B_AV_MODEL, 130, model_lbl, "Tab", "Choose another character of the game (grid with previews).", 0 },
        { B_AV_MOVESET, 96, "Moveset...", "", "The animations this character walks with (stand, walk, run, starts and turns), as a graph -- change any of them.", 0 },
        { B_AV_PREV, 92, "< Prev", "Up", "Previous animation in the list.", 0 },
        { B_AV_PLAY, 104, g_anim_paused ? "Play" : "Pause", "Space", "Play / pause the animation (it loops).", g_anim_paused },
        { B_AV_NEXT, 92, "Next >", "Down", "Next animation in the list.", 0 },
        { B_AV_STEPB, 86, "Frame -", "Left", "Pause and step back 1/30 s.", 0 },
        { B_AV_STEPF, 86, "Frame +", "Right", "Pause and step forward 1/30 s.", 0 },
        { B_AV_SLOWER, 104, "Slower", "Num-", "Play slower (down to x0.125).", 0 },
        { B_AV_FASTER, 104, "Faster", "Num+", "Play faster (up to x4).", 0 },
        { B_AV_FOLLOW, 118, g_anim_follow ? "Camera: follow" : "Camera: fixed", "", "Follow: the camera stays on the character when a clip moves it. Fixed: see it travel over the grid.", !g_anim_follow },
        { B_AV_RESET, 96, "Reset view", "", "Default angle and zoom (drag the stage to rotate, mouse wheel to zoom).", 0 },
        { B_AV_CLOSE, 86, "Close", "Esc", "Back to the game.", 0 },
    };
    for (int i = 0; i < (int)(sizeof(b) / sizeof(b[0])); i++) {
        if (x + b[i].w > g_anim_stage_rc.right) { x = g_anim_stage_rc.left; y += h + gap; }
        ui_add(b[i].id, x, y, b[i].w, h, b[i].label, b[i].key, b[i].desc, b[i].on, 1, b[i].id == B_AV_PLAY || b[i].id == B_AV_FOLLOW ? 1 : 0);
        x += b[i].w + gap;
    }
}

static void anim_view_action(int id) {
    switch (id) {
        case B_AV_MODEL: picker_open(); break;
        case B_AV_MOVESET: ms_open(g_view_model); break;
        case B_AV_PREV: anim_select(g_anim_sel - 1); break;
        case B_AV_NEXT: anim_select(g_anim_sel + 1); break;
        case B_AV_PLAY: g_anim_paused = !g_anim_paused; break;
        case B_AV_STEPB: g_anim_paused = 1; g_anim_t -= 1.0f / 30.0f; if (g_anim_t < 0) g_anim_t += fmaxf(anim_lib_duration(anim_current_clip()), 1e-3f); break;
        case B_AV_STEPF: g_anim_paused = 1; g_anim_t += 1.0f / 30.0f; break;
        case B_AV_SLOWER: g_anim_speed = fmaxf(0.125f, g_anim_speed * 0.5f); break;
        case B_AV_FASTER: g_anim_speed = fminf(4.0f, g_anim_speed * 2.0f); break;
        case B_AV_FOLLOW: g_anim_follow = !g_anim_follow; break;
        case B_AV_RESET: g_anim_yaw = ANIM_DEFAULT_YAW; g_anim_pitch = 0.15f; g_anim_zoom = 1.0f; break;
        case B_AV_CLOSE: g_anim_view = 0; break;
    }
}

static void anim_view_paint(HWND hwnd, HDC hdc) {
    anim_view_layout(hwnd);
    if (g_picker) { picker_paint(hwnd, hdc); return; }
    RECT rc; GetClientRect(hwnd, &rc);
    ui_fill(hdc, &rc, RGB(14, 14, 18));
    HFONT fh = ui_font(20, 1), fs = ui_font(12, 1), fn = ui_font(14, 0), fb = ui_font(26, 1);
    int clip = anim_current_clip();
    const char *mname = g_view_model->name;
    /* header */
    SelectObject(hdc, fh);
    char t[200];
    snprintf(t, sizeof(t), "Animations on %s", mname);
    ui_text(hdc, 12, 10, 280, 26, t, RGB(255, 225, 120), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, fn);
    snprintf(t, sizeof(t), "filter: %s_   (%d clips)", g_anim_filter, g_anim_filtered_count);
    ui_text(hdc, 12, 40, 280, 18, t, RGB(200, 200, 200), DT_LEFT | DT_SINGLELINE);
    /* list */
    ui_fill(hdc, &g_anim_list_rc, RGB(22, 22, 28));
    int rows = anim_rows_visible();
    for (int r = 0; r < rows; r++) {
        int k = g_anim_scroll + r;
        if (k >= g_anim_filtered_count) break;
        int id = g_anim_filtered[k];
        int y = g_anim_list_rc.top + r * ANIM_ROW_H;
        if (k == g_anim_sel) { RECT hl = { g_anim_list_rc.left, y, g_anim_list_rc.right, y + ANIM_ROW_H }; ui_fill(hdc, &hl, RGB(60, 50, 10)); }
        int own = strcmp(anim_lib_source(id), mname) == 0;
        COLORREF col = k == g_anim_sel ? RGB(255, 230, 60) : own ? RGB(140, 200, 255) : RGB(210, 210, 210);
        snprintf(t, sizeof(t), "%s%s", anim_lib_name(id), own ? "  (own)" : "");
        ui_text(hdc, g_anim_list_rc.left + 8, y, 190, ANIM_ROW_H, t, col, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        snprintf(t, sizeof(t), "%.2fs", anim_lib_duration(id));
        ui_text(hdc, g_anim_list_rc.right - 62 - ANIM_SB_W, y, 54, ANIM_ROW_H, t, RGB(130, 130, 140), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (g_anim_filtered_count == 0) ui_text(hdc, g_anim_list_rc.left + 8, g_anim_list_rc.top + 6, 270, 40, "no clip for this skeleton", RGB(150, 150, 160), DT_LEFT | DT_WORDBREAK);
    RECT sb_track, sb_thumb;
    if (anim_sb_geom(&sb_track, &sb_thumb)) { /* scrollbar */
        ui_fill(hdc, &sb_track, RGB(30, 30, 38));
        InflateRect(&sb_thumb, -2, -1);
        int hot = g_anim_drag == 2 || g_anim_sb_hover;
        ui_fill(hdc, &sb_thumb, g_anim_drag == 2 ? RGB(200, 170, 60) : hot ? RGB(140, 140, 155) : RGB(90, 90, 100));
    }
    /* stage */
    anim_render_stage(hdc, &g_anim_stage_rc);
    ui_frame(hdc, &g_anim_stage_rc, RGB(60, 60, 70));
    float dur = anim_lib_duration(clip);
    SelectObject(hdc, fb);
    ui_text(hdc, g_anim_stage_rc.left + 14, g_anim_stage_rc.top + 10, 500, 32, clip >= 0 ? anim_lib_name(clip) : "(rest pose)", RGB(255, 255, 255), DT_LEFT | DT_SINGLELINE);
    SelectObject(hdc, fn);
    if (clip >= 0) {
        float tt = dur > 0 ? fmodf(g_anim_t, dur) : 0.0f;
        snprintf(t, sizeof(t), "%d / %d   |   %.2f s / %.2f s   |   %s   |   x%.3g%s   |   %s/%s.gltf",
                 g_anim_sel + 1, g_anim_filtered_count, tt, dur, g_anim_paused ? "paused" : "playing", g_anim_speed,
                 g_anim_follow ? "" : "   |   camera fixed", anim_lib_source(clip), anim_lib_name(clip));
        ui_text(hdc, g_anim_stage_rc.left + 14, g_anim_stage_rc.top + 44, g_anim_stage_rc.right - g_anim_stage_rc.left - 28, 18, t, RGB(190, 190, 200), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        /* timeline */
        RECT bar = { g_anim_stage_rc.left + 14, g_anim_stage_rc.bottom - 16, g_anim_stage_rc.right - 14, g_anim_stage_rc.bottom - 10 };
        ui_fill(hdc, &bar, RGB(50, 50, 60));
        RECT fill = bar; fill.right = bar.left + (int)((bar.right - bar.left) * (dur > 0 ? tt / dur : 0));
        ui_fill(hdc, &fill, RGB(255, 200, 60));
    }
    SelectObject(hdc, fs);
    ui_text(hdc, g_anim_stage_rc.right - 460, g_anim_stage_rc.top + 14, 446, 16, "TAB = CHARACTERS   DRAG = ROTATE   WHEEL = ZOOM   TYPE = FILTER", RGB(120, 150, 190), DT_RIGHT | DT_SINGLELINE);
    /* hovered button description */
    const UiButton *hb = ui_find(g_hover_btn);
    if (hb) {
        SelectObject(hdc, fn);
        ui_text(hdc, g_anim_stage_rc.left + 14, g_anim_stage_rc.bottom - 40, 700, 18, hb->desc, RGB(230, 230, 235), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    ui_draw_buttons(hdc);
    SelectObject(hdc, GetStockObject(SYSTEM_FONT));
}

/* input while the viewer is open; returns 1 if handled */
static int anim_view_key(int vk) {
    if (g_picker) {
        int cols = pick_cols(), page = cols * pick_rows_visible();
        switch (vk) {
            case VK_ESCAPE: case VK_TAB: g_picker = 0; if (g_pick_for_script) { g_pick_for_script = 0; g_anim_view = 0; } return 1;
            case VK_RETURN: picker_choose(); return 1;
            case VK_LEFT: pick_select(g_pick_sel - 1); return 1;
            case VK_RIGHT: pick_select(g_pick_sel + 1); return 1;
            case VK_UP: pick_select(g_pick_sel - cols); return 1;
            case VK_DOWN: pick_select(g_pick_sel + cols); return 1;
            case VK_PRIOR: pick_select(g_pick_sel - page); return 1;
            case VK_NEXT: pick_select(g_pick_sel + page); return 1;
            case VK_HOME: pick_select(0); return 1;
            case VK_END: pick_select(g_pick_filtered_count - 1); return 1;
            case VK_BACK:
                if (g_pick_filter_len > 0) { g_pick_filter[--g_pick_filter_len] = 0; pick_filter(); }
                return 1;
        }
        return 0;
    }
    switch (vk) {
        case VK_ESCAPE: g_anim_view = 0; return 1;
        case VK_TAB: picker_open(); return 1;
        case VK_UP: anim_select(g_anim_sel - 1); return 1;
        case VK_DOWN: anim_select(g_anim_sel + 1); return 1;
        case VK_PRIOR: anim_select(g_anim_sel - anim_rows_visible()); return 1;
        case VK_NEXT: anim_select(g_anim_sel + anim_rows_visible()); return 1;
        case VK_HOME: anim_select(0); return 1;
        case VK_END: anim_select(g_anim_filtered_count - 1); return 1;
        case VK_SPACE: anim_view_action(B_AV_PLAY); return 1;
        case VK_LEFT: anim_view_action(B_AV_STEPB); return 1;
        case VK_RIGHT: anim_view_action(B_AV_STEPF); return 1;
        case VK_SUBTRACT: anim_view_action(B_AV_SLOWER); return 1;
        case VK_ADD: anim_view_action(B_AV_FASTER); return 1;
        case VK_BACK:
            if (g_anim_filter_len > 0) { g_anim_filter[--g_anim_filter_len] = 0; anim_view_filter(); }
            return 1;
    }
    return 0;
}
static void anim_view_char(char c) {
    if (g_anim_skip_char) { g_anim_skip_char = 0; if (c == 'a' || c == 'A') return; }
    if (!(c > 32 && c < 127)) return;
    if (g_picker) {
        if (g_pick_filter_len < (int)sizeof(g_pick_filter) - 1) { g_pick_filter[g_pick_filter_len++] = c; g_pick_filter[g_pick_filter_len] = 0; pick_filter(); }
        return;
    }
    if (g_anim_filter_len < (int)sizeof(g_anim_filter) - 1) {
        g_anim_filter[g_anim_filter_len++] = c; g_anim_filter[g_anim_filter_len] = 0;
        anim_view_filter();
    }
}
static int anim_view_mouse_down(HWND hwnd, int x, int y, int dbl) {
    anim_view_layout(hwnd);
    if (g_picker) {
        int k = pick_card_at(x, y);
        if (k >= 0) {
            if (dbl || k == g_pick_sel) { pick_select(k); picker_choose(); }
            else pick_select(k);
        }
        return 1;
    }
    int b = ui_hit(x, y);
    if (b != B_NONE) { anim_view_action(b); return 1; }
    RECT tr, tb;
    if (anim_sb_geom(&tr, &tb) && x >= tr.left && x < tr.right && y >= tr.top && y < tr.bottom) {
        if (y >= tb.top && y < tb.bottom) { g_anim_drag = 2; g_anim_sb_grab = y - tb.top; }
        else {
            g_anim_sb_page = y < tb.top ? -1 : 1;
            anim_sb_page_step();
            SetTimer(hwnd, ANIM_SB_TIMER_ID, 350, NULL); /* then repeats while held */
        }
        SetCapture(hwnd);
        return 1;
    }
    if (x >= g_anim_list_rc.left && x < g_anim_list_rc.right && y >= g_anim_list_rc.top && y < g_anim_list_rc.bottom) {
        anim_select(g_anim_scroll + (y - g_anim_list_rc.top) / ANIM_ROW_H);
        return 1;
    }
    if (x >= g_anim_stage_rc.left && x < g_anim_stage_rc.right && y >= g_anim_stage_rc.top && y < g_anim_stage_rc.bottom) {
        g_anim_drag = 1; g_anim_drag_x = x; g_anim_drag_y = y; SetCapture(hwnd);
    }
    return 1;
}
static void anim_view_mouse_move(int x, int y) {
    g_mouse_x = x; g_mouse_y = y;
    if (g_picker) { g_pick_hover = pick_card_at(x, y); return; }
    RECT tr, tb;
    int has_sb = anim_sb_geom(&tr, &tb);
    g_anim_sb_hover = has_sb && x >= tr.left && x < tr.right && y >= tb.top && y < tb.bottom;
    if (g_anim_drag == 2) { /* thumb follows the mouse */
        if (!has_sb) return;
        int span = (tr.bottom - tr.top) - (tb.bottom - tb.top);
        int maxs = g_anim_filtered_count - anim_rows_visible();
        int pos = y - g_anim_sb_grab - tr.top;
        anim_scroll_to(span > 0 ? (pos * maxs + span / 2) / span : 0);
        return;
    }
    if (!g_anim_drag) return;
    g_anim_yaw += (x - g_anim_drag_x) * 0.01f;
    g_anim_pitch += (y - g_anim_drag_y) * 0.006f;
    if (g_anim_pitch < -0.35f) g_anim_pitch = -0.35f;
    if (g_anim_pitch > 1.2f) g_anim_pitch = 1.2f;
    g_anim_drag_x = x; g_anim_drag_y = y;
}
static void anim_view_wheel(int x, int y, int delta) {
    if (g_picker) {
        int cols = pick_cols(), rows = pick_rows_visible(), total = (g_pick_filtered_count + cols - 1) / cols;
        g_pick_scroll -= delta / 120;
        if (g_pick_scroll > total - rows) g_pick_scroll = total - rows;
        if (g_pick_scroll < 0) g_pick_scroll = 0;
        g_pick_hover = pick_card_at(x, y);
        return;
    }
    if (x >= g_anim_list_rc.left && x < g_anim_list_rc.right && y >= g_anim_list_rc.top && y < g_anim_list_rc.bottom) {
        anim_scroll_to(g_anim_scroll - (delta / 120) * 3);
        return;
    }
    g_anim_zoom *= powf(1.15f, delta / 120.0f);
    if (g_anim_zoom < 0.4f) g_anim_zoom = 0.4f;
    if (g_anim_zoom > 4.0f) g_anim_zoom = 4.0f;
}

/* =====================================================================
   SCRIPTS (S): the room's cutscenes
   ---------------------------------------------------------------------
   A script is a grid of actions (see script.h). Played row by row: every
   action of a row starts at once, the next row starts when all of them are
   finished. While a script plays the player can do nothing but left-click
   to skip the current row -- not a row that moves a character (it waits
   for the character to arrive). Esc stops it (testing).
   A script plays when David arrives through a connector that names it,
   or -- entering the room any other way -- the room's first script
   marked "auto". The Scripts screen (S) edits them and plays them.
   ===================================================================== */

/* ---- this room's scripts (data/rooms/<level>/<room>_scripts.cfg) ---- */
static Script *g_scripts = NULL;
static int g_script_count = 0;
static char g_scripts_room[128] = "";

static void room_scripts_free(void) {
    for (int i = 0; i < g_script_count; i++) script_free(&g_scripts[i]);
    free(g_scripts);
    g_scripts = NULL; g_script_count = 0;
}
static void room_scripts_load(const char *label) {
    room_scripts_free();
    snprintf(g_scripts_room, sizeof(g_scripts_room), "%s", label);
    g_script_count = room_scripts_read(label, &g_scripts);
}
static void room_scripts_save(void) {
    if (!g_scripts_room[0]) return;
    char path[1024];
    scripts_file_path(path, sizeof(path), g_scripts_room);
    ensure_parent_dir(path);
    scripts_save(path, g_scripts, g_script_count);
}
static int room_script_find(const char *name) {
    for (int i = 0; i < g_script_count; i++) if (!strcmp(g_scripts[i].name, name)) return i;
    return -1;
}

/* ---- portraits: each model speaks with one of the blockouts' portraits
   (sprites bigports.1-71), chosen once and kept for good, everywhere
   (data/portraits.cfg: "<model> <number>") ---- */
#define PORTRAIT_MAX 71
static char g_portrait_model[256][48];
static int g_portrait_num[256], g_portrait_n = -1;
static void portraits_load(void) {
    if (g_portrait_n >= 0) return;
    g_portrait_n = 0;
    char path[1024], line[256], m[48]; int k;
    root_path(path, sizeof(path), "data/portraits.cfg");
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f) && g_portrait_n < 256)
        if (line[0] != '#' && sscanf(line, "%47s %d", m, &k) == 2 && k >= 1 && k <= PORTRAIT_MAX) {
            snprintf(g_portrait_model[g_portrait_n], 48, "%s", m); g_portrait_num[g_portrait_n++] = k;
        }
    fclose(f);
}
static int portrait_of(const char *model) {
    portraits_load();
    for (int i = 0; i < g_portrait_n; i++) if (!strcmp(g_portrait_model[i], model)) return g_portrait_num[i];
    return 0;
}
static void portrait_set(const char *model, int num) {
    portraits_load();
    int i = 0;
    while (i < g_portrait_n && strcmp(g_portrait_model[i], model)) i++;
    if (i == g_portrait_n) { if (g_portrait_n >= 256) return; snprintf(g_portrait_model[g_portrait_n++], 48, "%s", model); }
    g_portrait_num[i] = num;
    char path[1024];
    root_path(path, sizeof(path), "data/portraits.cfg");
    ensure_parent_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Silver Remaster: the portrait each character speaks with (assets/sprites/bigports.<number>.png)\n");
    for (int j = 0; j < g_portrait_n; j++) fprintf(f, "%s %d\n", g_portrait_model[j], g_portrait_num[j]);
    fclose(f);
}
static const uint32_t *portrait_pixels(int num, int *w, int *h) {
    static uint32_t *img[PORTRAIT_MAX + 1];
    static int iw[PORTRAIT_MAX + 1], ih[PORTRAIT_MAX + 1], tried[PORTRAIT_MAX + 1];
    if (num < 1 || num > PORTRAIT_MAX) return NULL;
    if (!tried[num]) {
        tried[num] = 1;
        char path[1024];
        root_path(path, sizeof(path), "assets/sprites/bigports.%d.png", num);
        img[num] = image_load(path, &iw[num], &ih[num]);
    }
    *w = iw[num]; *h = ih[num];
    return img[num];
}

/* ---- how long an action lasts (advanced timelines) ----
   INSTANT: no time (a picture, a character placed...); KNOWN: its length
   is known beforehand (a wait, a sound, an animation played N times);
   DYNAMIC: only known once it has played (a character walking somewhere,
   an animation until the others end...) -- shown with its last measured
   length, else an estimate. */
enum { DUR_INSTANT, DUR_KNOWN, DUR_DYNAMIC };
static float sound_seconds(const char *file) { /* cached: the editor asks every frame */
    static struct { char f[160]; float s; } c[128];
    static int n = 0, next = 0;
    if (!file || !file[0]) return -1.0f;
    for (int i = 0; i < n; i++) if (!strcmp(c[i].f, file)) return c[i].s;
    float s = audio_file_seconds(file);
    int k = n < 128 ? n++ : (next++ % 128);
    snprintf(c[k].f, sizeof(c[k].f), "%s", file);
    c[k].s = s;
    return s;
}
static int action_duration_raw(const ScriptAction *a, float *sec);
/* its whole length when it can be trimmed (a sound, a line, an animation played N times): 1 */
static int action_media_length(const ScriptAction *a, float *full) {
    if (a->type != ACT_SOUND && a->type != ACT_SPEAK && !(a->type == ACT_ANIM && a->anim_mode == ANIM_TIMES)) return 0;
    return action_duration_raw(a, full) == DUR_KNOWN;
}
static int action_duration(const ScriptAction *a, float *sec) {
    int k = action_duration_raw(a, sec);
    float full;
    if (k == DUR_KNOWN && (a->trim_in > 0.0f || a->trim_out > 0.0f) && action_media_length(a, &full)) {
        *sec = full - a->trim_in - a->trim_out;
        if (*sec < 0.05f) *sec = 0.05f;
    }
    return k;
}
static int action_duration_raw(const ScriptAction *a, float *sec) {
    *sec = 0.0f;
    switch (a->type) {
        case ACT_WAIT: *sec = a->seconds; return a->seconds > 0.0f ? DUR_KNOWN : DUR_INSTANT;
        case ACT_SOUND: { float d = sound_seconds(a->file); if (d <= 0.0f) return DUR_INSTANT; *sec = d * (float)(a->repeat + 1); return DUR_KNOWN; }
        case ACT_SPEAK: { float d = sound_seconds(a->file); if (d <= 0.0f) return DUR_INSTANT; *sec = d; return DUR_KNOWN; }
        case ACT_CAMERA: *sec = a->seconds; return a->seconds > 0.0f ? DUR_KNOWN : DUR_INSTANT;
        case ACT_ANIM:
            if (a->anim_mode == ANIM_ROW) return DUR_DYNAMIC;
            if (a->anim_mode != ANIM_TIMES) return DUR_INSTANT;
            {
                int clip = a->file[0] ? anim_lib_find_ref(a->file) : -1;
                float sp = a->speed > 0.0f ? a->speed : g_david_anim_speed;
                if (clip < 0 || sp <= 0.0f) return DUR_INSTANT;
                *sec = anim_lib_duration(clip) * (float)(a->repeat < 1 ? 1 : a->repeat) / sp;
                return DUR_KNOWN;
            }
        case ACT_MOVE: return DUR_DYNAMIC;
        case ACT_CHAR: return (a->set_weapon || a->set_shield) ? DUR_DYNAMIC : DUR_INSTANT;
        case ACT_OVERLAY:
            for (int i = 0; i < a->nov; i++) if (a->ov_mode[i] == OVM_ONCE || a->ov_mode[i] == OVM_ONCE_HIDE) return DUR_DYNAMIC;
            return DUR_INSTANT;
        default: return DUR_INSTANT;
    }
}
/* the lengths the DYNAMIC actions really took, their last time played (script name + action id) */
static struct { char script[64]; int id; float sec; } g_measured[64];
static int g_measured_n = 0;
static void run_measure(const char *script, int id, float sec) {
    int k = -1;
    for (int i = 0; i < g_measured_n; i++) if (g_measured[i].id == id && !strcmp(g_measured[i].script, script)) k = i;
    if (k < 0) { k = g_measured_n < 64 ? g_measured_n++ : (id % 64); snprintf(g_measured[k].script, sizeof(g_measured[k].script), "%s", script); g_measured[k].id = id; }
    g_measured[k].sec = sec;
}
static int measured_duration(const char *script, int id, float *sec) {
    for (int i = 0; i < g_measured_n; i++) if (g_measured[i].id == id && !strcmp(g_measured[i].script, script)) { *sec = g_measured[i].sec; return 1; }
    return 0;
}

/* ---- playing ---- */
#define RUN_MAX_VOICES 64
#define ZONE_MAX_EV 256
typedef struct {
    int done;
    int voice;          /* SOUND / AMBIENCE */
    float timer;        /* WAIT */
    Actor *actor;       /* MOVE / ANIM / SPEAK */
    int door, room_gen; /* MOVE through a connector */
    int clip;           /* ANIM */
    int row_long;       /* ANIM "until the rest of the row is over" */
    int portrait;       /* SPEAK: shown until the line is over */
    int pend_weapon, pend_shield; /* CHAR: its weapon / shield change still to come (one after the other) */
    char pick[160];     /* ANIM / SOUND: the one picked in its pool */
    float limit;        /* in a timeline: it's cut after this long (its trimmed end), 0 = none */
} CellRun;
static struct {
    int active;
    Script s;           /* a copy: editing or leaving the room doesn't touch it */
    int row, row_started;
    CellRun cell[SCRIPT_MAX_COLS];
    int voice_action[RUN_MAX_VOICES], voice_id[RUN_MAX_VOICES], nvoices;
    int leave;          /* a "Change room" started: the script ends once its row has started */
    int step;           /* +1 each time the script goes on to its next row */
    /* an advanced timeline zone playing: its clock, its actions (cells) and how they're going */
    int zone_on, zone_n, zone_skip;
    float zone_t;
    int zev_row[ZONE_MAX_EV], zev_col[ZONE_MAX_EV], zev_started[ZONE_MAX_EV];
    float zev_t0[ZONE_MAX_EV];
    CellRun zcell[ZONE_MAX_EV];
} g_run;

static int script_playing(void) { return g_run.active; }

/* the script animations stop (the characters go back to their own pose) -- not the looped ones (ANIM_LOOP) */
static void run_stop_anims(void) {
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *a = &g_actors[k];
        if (a->play_hold && a->play_clip >= 0) continue;
        if (a->hold_last || a->dead || a->down) continue; /* dead / down */
        a->play_script = 0; a->play_ended = 0; a->play_freeze = 0;
        if (a->play_event) actor_play_end(a); /* an equipment change isn't lost */
        a->play_clip = -1; a->play_attack = 0; a->play_turn = 0.0f; a->stepping = 0; a->trail = 0; a->play_next = -1;
    }
}
static void script_stop(const char *why) {
    if (!g_run.active) return;
    run_stop_anims();
    snprintf(g_status, sizeof(g_status), "script \"%s\" %s", g_run.s.name, why ? why : "finished");
    g_run.active = 0;
    script_free(&g_run.s);
}

static void script_start(const Script *sc, int from_row) {
    script_stop("stopped");
    memset(&g_run, 0, sizeof(g_run));
    script_copy(&g_run.s, sc);
    g_run.active = 1;
    g_run.row = from_row < 0 ? 0 : from_row;
    DAVID_ACTOR->pending_door = -1; /* the player's last order is over */
    g_click_marker_active = 0;
    snprintf(g_status, sizeof(g_status), "playing script \"%s\"", sc->name);
}

/* A character comes in (or is put back) where the action says. */
static Actor *actor_place(const ScriptAction *a) {
    if (!g_has_3d_character) return NULL;
    if (!a->model[0] || !a->has_pos) { snprintf(g_status, sizeof(g_status), "script: a 'Place character' has no character or position"); return NULL; }
    CharModel *m = model_by_name(a->model);
    if (!m) { snprintf(g_status, sizeof(g_status), "script: character '%s' not found in assets/chars", a->model); return NULL; }
    Actor *ac = actor_by_place_id(a->id);
    if (!ac) for (int k = 1; k < MAX_ACTORS && !ac; k++) if (!g_actors[k].used) ac = &g_actors[k];
    if (!ac) { snprintf(g_status, sizeof(g_status), "script: too many characters in the room (max %d)", MAX_ACTORS - 1); return NULL; }
    actor_reset(ac, m, a->id);
    memcpy(ac->pos, a->pos, sizeof(ac->pos));
    memcpy(ac->target, a->pos, sizeof(ac->target));
    ac->facing = a->facing;
    ac->side = a->side ? SIDE_ENEMY : SIDE_ALLY;
    ac->ai_on = !a->ai_off;
    return ac;
}

/* CHARACTER SETTINGS: its side and AI at once; its weapon, then its shield, taken out / put away as with the radial menu */
static void run_char_equip(const ScriptAction *a, CellRun *c) {
    Actor *ac = c->actor;
    if (!ac || !ac->used || actor_busy(ac) || ac->dead || ac->down) { if (ac && (ac->dead || ac->down)) c->pend_weapon = c->pend_shield = 0; return; }
    if (c->pend_weapon) {
        c->pend_weapon = 0;
        if (weapon_is_orb(a->item)) actor_equip_orb(ac, a->item);
        else if (a->item[0]) actor_equip_weapon(ac, a->item);
        else if (weapon_is_orb(ac->weapon)) actor_equip_orb(ac, ac->weapon);
        else if (ac->weapon[0]) actor_equip_weapon(ac, ac->weapon); /* the one it holds: put away */
        return;
    }
    if (c->pend_shield) {
        c->pend_shield = 0;
        if (a->item2[0]) actor_equip_shield(ac, a->item2);
        else if (ac->shield[0]) actor_equip_shield(ac, ac->shield);
    }
}
static void run_start_char(const ScriptAction *a, CellRun *c) {
    Actor *ac = actor_by_place_id(a->actor);
    if (!ac) { char who[96]; script_actor_name(&g_run.s, a->actor, who, sizeof(who)); snprintf(g_status, sizeof(g_status), "script: %s isn't in the room", who); return; }
    if (a->set_side) ac->side = a->set_side == 2 ? SIDE_ENEMY : SIDE_ALLY;
    if (a->set_ai) ac->ai_on = a->set_ai == 1 && ac != DAVID_ACTOR;
    c->actor = ac;
    c->pend_weapon = a->set_weapon && strcmp(ac->weapon, a->item) != 0;
    c->pend_shield = a->set_shield && strcmp(ac->shield, a->item2) != 0;
    run_char_equip(a, c);
    c->done = !c->pend_weapon && !c->pend_shield && !actor_busy(ac);
}
static void run_start_move(const ScriptAction *a, CellRun *c) {
    char who[96];
    script_actor_name(&g_run.s, a->actor, who, sizeof(who));
    Actor *ac = actor_by_place_id(a->actor);
    if (!ac) { snprintf(g_status, sizeof(g_status), "script: %s isn't in the room", who); return; }
    c->actor = ac;
    int ok = 0, no_door = 0;
    actor_begin(ac);
    if (a->door) {
        int di = shape_index_by_id(a->door);
        if (di >= 0 && g_shapes[di].door && g_shapes[di].has_step) {
            g_click_fail = 0;
            ok = move_to_world_point(g_shapes[di].step, a->run);
            if (ok) {
                c->door = 1; c->room_gen = g_room_gen;
                g_pending_door = di;
                if (!g_char_moving) { /* already on the doorstep */
                    if (ac == DAVID_ACTOR) { g_door_travel_request = di; g_door_travel_run = a->run; } else ac->used = 0;
                    g_pending_door = -1;
                }
            }
        } else no_door = 1;
    } else if (a->has_pos) ok = move_to_world_point(a->pos, a->run);
    actor_end();
    if (no_door) { snprintf(g_status, sizeof(g_status), "script: connector #%d isn't in this room", a->door); return; }
    if (!ok) { snprintf(g_status, sizeof(g_status), "script: %s can't get there", who); return; }
    c->done = 0;
}

static int *run_voice_slot(int action_id) {
    for (int i = 0; i < g_run.nvoices; i++) if (g_run.voice_action[i] == action_id) return &g_run.voice_id[i];
    if (g_run.nvoices >= RUN_MAX_VOICES) return NULL;
    g_run.voice_action[g_run.nvoices] = action_id;
    return &g_run.voice_id[g_run.nvoices++];
}

static CharModel *run_actor_model(const Script *s, int actor);
/* ANIM / SOUND: its file, or one of its pool's, at random */
static const char *action_pick(const ScriptAction *a) {
    int n = 1 + a->npool, k = n > 1 ? rand() % n : 0;
    return k == 0 ? a->file : a->pool[k - 1];
}
static void run_start_anim(const ScriptAction *a, CellRun *c) {
    Actor *ac = actor_by_place_id(a->actor);
    snprintf(c->pick, sizeof(c->pick), "%s", action_pick(a));
    int clip = anim_lib_find_ref(c->pick);
    char who[96];
    script_actor_name(&g_run.s, a->actor, who, sizeof(who));
    if (!ac) { snprintf(g_status, sizeof(g_status), "script: %s isn't in the room", who); return; }
    if (a->anim_mode == ANIM_NORMAL) { /* back to its own behaviour */
        if (ac->play_clip >= 0 && !ac->play_attack) { ac->play_clip = -1; ac->play_next = -1; }
        ac->play_hold = 0;
        return;
    }
    if (clip < 0 || !anim_lib_fits(clip, ac->model->node_count)) { snprintf(g_status, sizeof(g_status), "script: %s can't play '%s'", who, c->pick); return; }
    ac->play_clip = clip; ac->play_t = 0.0f; ac->play_next = -1; ac->play_speed = a->speed;
    ac->play_left = (a->anim_mode == ANIM_ROW || a->anim_mode == ANIM_LOOP) ? -1 : (a->repeat < 1 ? 1 : a->repeat);
    ac->play_hold = a->anim_mode == ANIM_LOOP;
    ac->play_script = 1; ac->play_ended = 0; ac->hold_step = g_run.step;
    ac->play_freeze = a->freeze && a->anim_mode == ANIM_TIMES;
    if (a->anim_mode == ANIM_LOOP) return; /* the row goes on at once */
    c->actor = ac; c->clip = clip;
    c->row_long = a->anim_mode == ANIM_ROW;
    c->done = 0;
}
static void run_start(const ScriptAction *a, CellRun *c) {
    c->done = 1;
    switch (a->type) {
        case ACT_WAIT: c->timer = a->seconds; c->done = a->seconds <= 0.0f; break;
        case ACT_BACKGROUND: background_set(a->file); break;
        case ACT_MUSIC: {
            AudioTrack t[AUDIO_MAX_TRACKS];
            for (int i = 0; i < a->ntracks; i++) { snprintf(t[i].file, sizeof(t[i].file), "%s", a->track[i]); t[i].loop = a->track_loop[i]; }
            audio_music_play_fade(t, a->ntracks, a->fade);
            break;
        }
        case ACT_SOUND: case ACT_AMBIENCE: {
            int snd = a->type == ACT_SOUND;
            snprintf(c->pick, sizeof(c->pick), "%s", snd ? action_pick(a) : a->file);
            c->voice = audio_play(c->pick, snd ? AUDIO_SOUND : AUDIO_AMBIENCE, snd ? a->repeat : 0, !snd && a->loop);
            int *slot = run_voice_slot(a->id);
            if (slot) *slot = c->voice;
            if (snd && a->wait_end && c->voice) c->done = 0;
            break;
        }
        case ACT_STOP:
            if (a->stop_kind == STOP_MUSIC) audio_music_play_fade(NULL, 0, a->fade);
            else if (a->stop_kind == STOP_ALL_SOUNDS) { audio_fade_kind(AUDIO_SOUND, a->fade); audio_fade_kind(AUDIO_AMBIENCE, a->fade); }
            else for (int i = 0; i < g_run.nvoices; i++) if (g_run.voice_action[i] == a->stop_ref) audio_fade(g_run.voice_id[i], a->fade);
            break;
        case ACT_PLACE: actor_place(a); break;
        case ACT_MOVE: run_start_move(a, c); break;
        case ACT_ANIM: run_start_anim(a, c); break;
        case ACT_CHAR: run_start_char(a, c); break;
        case ACT_OVERLAY: {
            int wait = 0;
            for (int i = 0; i < a->nov; i++) {
                int k = overlay_find(a->ov_name[i]);
                if (k < 0) continue;
                overlay_apply(&g_ov[k], a->ov_mode[i], a->ov_frame[i]);
                if (a->ov_mode[i] == OVM_ONCE || a->ov_mode[i] == OVM_ONCE_HIDE) wait = 1;
            }
            c->done = !(a->wait_end && wait);
            break;
        }
        case ACT_CAMERA: {
            float cx, cy; view_center(&cx, &cy);
            if (a->cam_target == CAM_POINT && a->has_pos) { cx = a->pos[0]; cy = a->pos[1]; }
            else if (a->cam_target == CAM_DAVID) {
                float px, py, pz;
                if (camera_world_to_pixel_z(&g_room_cam, DAVID_ACTOR->pos, (int)g_hdr.width, (int)g_hdr.height, &px, &py, &pz)) { cx = px; cy = py - 40.0f; }
            }
            camera_slide(cx, cy, a->zoom > 0.0f ? a->zoom : g_room_zoom, a->seconds);
            c->done = !g_camslide.active;
            break;
        }
        case ACT_ROOM:
            if (!a->file[0]) break;
            transition_start_room(a->file, a->door, a->script);
            g_run.leave = 1;
            break;
        case ACT_SPEAK: {
            CharModel *m = run_actor_model(&g_run.s, a->actor);
            c->portrait = m ? portrait_of(m->name) : 0;
            c->voice = audio_play(a->file, AUDIO_SOUND, 0, 0);
            c->timer = 2.0f; /* no line (or unreadable): the portrait shows 2 s */
            c->done = 0;
            break;
        }
    }
}

static int run_cell_done(const ScriptAction *a, CellRun *c, float dt) {
    if (c->done) return 1;
    switch (a->type) {
        case ACT_WAIT: c->timer -= dt; if (c->timer <= 0.0f) c->done = 1; break;
        case ACT_SOUND: if (!audio_playing(c->voice)) c->done = 1; break;
        case ACT_SPEAK:
            if (c->voice) c->done = !audio_playing(c->voice);
            else { c->timer -= dt; c->done = c->timer <= 0.0f; }
            break;
        case ACT_CAMERA: c->done = !g_camslide.active; break;
        case ACT_OVERLAY: { /* its "once" overlays have played (or are gone with the room) */
            int all = 1;
            for (int i = 0; i < a->nov; i++) {
                if (a->ov_mode[i] != OVM_ONCE && a->ov_mode[i] != OVM_ONCE_HIDE) continue;
                int k = overlay_find(a->ov_name[i]);
                if (k >= 0 && !g_ov[k].finished) all = 0;
            }
            c->done = all;
            break;
        }
        case ACT_CHAR:
            run_char_equip(a, c);
            c->done = !c->actor || !c->actor->used || (!c->pend_weapon && !c->pend_shield && !actor_busy(c->actor));
            break;
        case ACT_ANIM:
            if (c->row_long) return 0; /* decided by the rest of the row (script_tick) */
            c->done = !c->actor->used || c->actor->play_clip != c->clip || c->actor->play_ended;
            break;
        case ACT_MOVE: {
            Actor *ac = c->actor;
            int gone = !ac->used || ac->place_id != a->actor;
            if (c->door && ac == DAVID_ACTOR)
                /* only once in the other room -- or if he stopped without going through (a
                   connector without target room: the row would otherwise never end) */
                c->done = g_room_gen != c->room_gen || (!ac->moving && ac->pending_door < 0 && g_door_travel_request < 0);
            else if (c->door) c->done = gone;
            else c->done = gone || !ac->moving;
            break;
        }
        default: c->done = 1;
    }
    return c->done;
}

/* the model of a script's character (David, or a PLACE action's) */
static CharModel *run_actor_model(const Script *s, int actor) {
    if (actor == 0) return &g_david;
    ScriptAction *p = script_find((Script *)s, actor, NULL, NULL);
    return (p && p->type == ACT_PLACE) ? model_by_name(p->model) : NULL;
}

/* SPEAK: the portraits of the characters speaking, top right of the picture
   (several at once: side by side, leftward) */
static void script_draw_portraits(uint32_t *px, int W, int H, float sc) {
    if (!g_run.active || !g_run.row_started || g_tr.phase) return;
    int right = W - (int)(12 * sc), y = (int)(12 * sc);
    int nitems = g_run.zone_on ? g_run.zone_n : g_run.s.cols;
    for (int c = 0; c < nitems; c++) {
        const ScriptAction *a = g_run.zone_on ? script_at(&g_run.s, g_run.zev_row[c], g_run.zev_col[c]) : script_at(&g_run.s, g_run.row, c);
        CellRun *cr = g_run.zone_on ? &g_run.zcell[c] : &g_run.cell[c];
        if (g_run.zone_on && !g_run.zev_started[c]) continue;
        if (!a || a->type != ACT_SPEAK || cr->done || !cr->portrait) continue;
        int pw, ph;
        const uint32_t *img = portrait_pixels(cr->portrait, &pw, &ph);
        if (!img) continue;
        float k = sc * 1.25f;
        int dw = (int)(pw * k), dh = (int)(ph * k), x = right - dw;
        for (int yy = y - 2; yy < y + dh + 2; yy++) for (int xx = x - 2; xx < x + dw + 2; xx++) /* gold frame */
            if (xx >= 0 && yy >= 0 && xx < W && yy < H) px[(size_t)yy * W + xx] = (yy < y - 1 || yy >= y + dh + 1 || xx < x - 1 || xx >= x + dw + 1) ? 0xC8A040u : 0x101010u;
        blend_sprite(px, W, H, img, pw, ph, x, y, k);
        right = x - (int)(10 * sc);
    }
}

static int script_row_has_move(const Script *s, int row) {
    for (int c = 0; c < s->cols; c++) { ScriptAction *a = script_at((Script *)s, row, c); if (a && a->type == ACT_MOVE) return 1; }
    return 0;
}

/* Left-click in a timeline zone: the rest of it is skipped -- the actions
   not started yet that change the scene (picture, music, characters
   placed...) still happen, the ones that take time don't. Not while a
   character moves in it. */
static void run_zone_skip(void) {
    for (int i = 0; i < g_run.zone_n; i++) {
        const ScriptAction *a = script_at(&g_run.s, g_run.zev_row[i], g_run.zev_col[i]);
        if (a->type == ACT_MOVE && !(g_run.zev_started[i] && g_run.zcell[i].done)) {
            snprintf(g_status, sizeof(g_status), "this timeline can't be skipped: a character moves in it");
            return;
        }
    }
    for (int i = 0; i < g_run.zone_n; i++) {
        const ScriptAction *a = script_at(&g_run.s, g_run.zev_row[i], g_run.zev_col[i]);
        CellRun *cr = &g_run.zcell[i];
        if (!g_run.zev_started[i]) {
            g_run.zev_started[i] = 1;
            int t = a->type;
            if (t == ACT_BACKGROUND || t == ACT_MUSIC || t == ACT_AMBIENCE || t == ACT_STOP || t == ACT_PLACE || t == ACT_OVERLAY || t == ACT_CAMERA || t == ACT_ROOM || t == ACT_CHAR)
                run_start(a, cr);
            cr->done = 1; cr->row_long = 0;
            continue;
        }
        if (a->type == ACT_SPEAK && !cr->done) audio_stop(cr->voice); /* the line is cut */
        if (a->type == ACT_ANIM && !cr->done && cr->actor && cr->actor->play_clip == cr->clip) cr->actor->play_clip = -1;
        cr->done = 1; cr->timer = 0.0f; cr->row_long = 0;
    }
}

/* Left-click while a script plays. */
static void script_skip_row(void) {
    if (!g_run.active) return;
    if (g_run.zone_on) { run_zone_skip(); return; }
    if (script_row_has_move(&g_run.s, g_run.row)) {
        snprintf(g_status, sizeof(g_status), "this row can't be skipped: a character is moving");
        return;
    }
    for (int c = 0; c < SCRIPT_MAX_COLS; c++) {
        CellRun *cr = &g_run.cell[c];
        const ScriptAction *a = c < g_run.s.cols ? script_at(&g_run.s, g_run.row, c) : NULL;
        if (a && a->type == ACT_SPEAK && !cr->done) audio_stop(cr->voice); /* the line is cut */
        if (a && a->type == ACT_ANIM && !cr->done && cr->actor && cr->actor->play_clip == cr->clip) cr->actor->play_clip = -1;
        cr->done = 1; cr->timer = 0.0f;
    }
    if (!g_run.row_started) { g_run.row++; }
}

/* The script clips that are over (held on their last frame) go back to the
   character's own pose -- once the next row has started, so a clip it
   starts follows with no idle in between. A frozen one ("Freeze on the
   last frame") waits for the end of its row. */
static void run_release_holds(void) {
    for (int k = 0; k < MAX_ACTORS; k++) {
        Actor *a = &g_actors[k];
        if (!a->play_ended) continue;
        if (a->play_clip < 0) { a->play_ended = a->play_script = a->play_freeze = 0; continue; }
        if (a->play_freeze && g_run.active && a->hold_step == g_run.step) continue;
        if (a->hold_last || a->dead || a->down) continue; /* dead / down: not a script's */
        actor_play_end(a);
    }
}

/* a timeline action just started: from its trimmed start, cut at its trimmed end */
static void zone_trim_start(const ScriptAction *a, CellRun *c) {
    float full;
    c->limit = 0.0f;
    if (!action_media_length(a, &full) || (a->trim_in <= 0.0f && a->trim_out <= 0.0f)) return;
    c->limit = full - a->trim_in - a->trim_out;
    if (c->limit < 0.05f) c->limit = 0.05f;
    if (a->trim_in <= 0.0f) return;
    if ((a->type == ACT_SOUND || a->type == ACT_SPEAK) && c->voice) {
        audio_stop(c->voice);
        c->voice = audio_play_at(c->pick[0] ? c->pick : a->file, AUDIO_SOUND, a->type == ACT_SOUND ? a->repeat : 0, 0, a->trim_in);
        if (a->type == ACT_SOUND) { int *slot = run_voice_slot(a->id); if (slot) *slot = c->voice; }
        if (!c->voice) c->done = 1;
    }
    if (a->type == ACT_ANIM && c->actor && c->actor->play_clip == c->clip) {
        Actor *ac = c->actor;
        float d = anim_lib_duration(c->clip), sp = a->speed > 0.0f ? a->speed : g_david_anim_speed;
        float t = a->trim_in * sp;
        while (d > 0.0f && t >= d && ac->play_left > 1) { t -= d; ac->play_left--; }
        ac->play_t = d > 0.001f ? fminf(t, d - 0.001f) : 0.0f;
    }
}
/* ...and its trimmed end reached */
static void zone_trim_cut(const ScriptAction *a, CellRun *c) {
    if ((a->type == ACT_SOUND || a->type == ACT_SPEAK) && c->voice) audio_fade(c->voice, 0.03f);
    if (a->type == ACT_ANIM && c->actor && c->actor->play_clip == c->clip) c->actor->play_ended = 1; /* held on that frame */
    c->done = 1;
}

/* An advanced timeline zone, every game step: its clock goes on, each
   action starts at its time, the zone is over (returns 1) once they all
   are -- however many rows it was given. A DYNAMIC action's real length
   is kept for the editor. */
static int run_zone_tick(Script *s, int zi, float dt) {
    const ScriptZone *z = &s->zone[zi];
    if (!g_run.row_started) {
        g_run.row_started = 1;
        g_run.zone_on = 1; g_run.zone_t = 0.0f; g_run.zone_n = 0; g_run.zone_skip = 0;
        for (int r = z->row0; r < z->row0 + z->rows && r < s->rows; r++)
            for (int c = 0; c < s->cols && g_run.zone_n < ZONE_MAX_EV; c++) {
                if (script_at(s, r, c)->type == ACT_NONE) continue;
                int k = g_run.zone_n++;
                g_run.zev_row[k] = r; g_run.zev_col[k] = c; g_run.zev_started[k] = 0; g_run.zev_t0[k] = 0.0f;
                memset(&g_run.zcell[k], 0, sizeof(CellRun));
            }
    } else g_run.zone_t += dt;
    int all = 1;
    for (int k = 0; k < g_run.zone_n; k++) {
        const ScriptAction *a = script_at(s, g_run.zev_row[k], g_run.zev_col[k]);
        CellRun *c = &g_run.zcell[k];
        if (!g_run.zev_started[k]) {
            if (a->start > g_run.zone_t + 1e-4f) { all = 0; continue; }
            g_run.zev_started[k] = 1; g_run.zev_t0[k] = g_run.zone_t;
            run_start(a, c);
            if (a->type == ACT_SOUND && c->voice) c->done = 0; /* in a timeline a sound lasts its length */
            zone_trim_start(a, c);
            if (g_run.leave) return 1;
        }
        if (c->row_long) continue;
        if (c->limit > 0.0f && !c->done && g_run.zone_t - g_run.zev_t0[k] >= c->limit - 1e-4f) zone_trim_cut(a, c);
        int was = c->done;
        if (!run_cell_done(a, c, dt)) { all = 0; continue; }
        float d;
        if (!was && action_duration(a, &d) == DUR_DYNAMIC) run_measure(s->name, a->id, g_run.zone_t - g_run.zev_t0[k]);
    }
    if (!all) return 0;
    for (int k = 0; k < g_run.zone_n; k++) { /* the "until the others are over" animations end */
        CellRun *c = &g_run.zcell[k];
        if (!c->row_long || c->done) continue;
        if (c->actor && c->actor->play_clip == c->clip) c->actor->play_clip = -1;
        const ScriptAction *a = script_at(s, g_run.zev_row[k], g_run.zev_col[k]);
        run_measure(s->name, a->id, g_run.zone_t - g_run.zev_t0[k]);
        c->done = 1;
    }
    return 1;
}

/* Every game step: a room just entered starts its script, then the one
   playing goes on (several rows in one step if they finish at once). */
static void script_tick(HWND hwnd, float dt) {
    (void)hwnd;
    if (g_room_entered) {
        g_room_entered = 0;
        if (!g_run.active && !g_edit_mode && !g_script_view && g_has_3d_character) {
            int i = -1;
            if (g_arrival_script[0]) i = room_script_find(g_arrival_script);
            else for (int k = 0; k < g_script_count && i < 0; k++) if (g_scripts[k].auto_run) i = k;
            if (i >= 0) script_start(&g_scripts[i], 0);
        }
        g_arrival_script[0] = 0;
    }
    for (int guard = 0; g_run.active && guard < 100000; guard++) {
        Script *s = &g_run.s;
        if (g_run.row >= s->rows) { script_stop(NULL); break; }
        int zi = script_zone_at(s, g_run.row);
        if (zi >= 0) { /* an advanced timeline: its actions at their own times */
            int over = run_zone_tick(s, zi, dt);
            if (g_run.leave) { script_stop("ended: going to another room"); break; }
            if (!over) break;
            g_run.row = s->zone[zi].row0 + s->zone[zi].rows; g_run.row_started = 0; g_run.step++; g_run.zone_on = 0;
            continue;
        }
        if (!g_run.row_started) {
            memset(g_run.cell, 0, sizeof(g_run.cell));
            g_run.row_started = 1;
            for (int c = 0; c < s->cols; c++) {
                const ScriptAction *a = script_at(s, g_run.row, c);
                if (a->type == ACT_NONE) g_run.cell[c].done = 1;
                else run_start(a, &g_run.cell[c]);
            }
            if (g_run.leave) { script_stop("ended: going to another room"); break; }
        }
        int all = 1;
        for (int c = 0; c < s->cols; c++) {
            const ScriptAction *a = script_at(s, g_run.row, c);
            if (a->type != ACT_NONE && !g_run.cell[c].row_long && !run_cell_done(a, &g_run.cell[c], dt)) all = 0;
        }
        if (!all) break;
        /* the rest of the row is over: the "until the row is over" animations end */
        for (int c = 0; c < s->cols; c++) {
            CellRun *cr = &g_run.cell[c];
            if (!cr->row_long || cr->done) continue;
            if (cr->actor && cr->actor->play_clip == cr->clip) cr->actor->play_clip = -1;
            cr->done = 1;
        }
        g_run.row++; g_run.row_started = 0; g_run.step++;
    }
    run_release_holds();
}

enum { SV_PICK_NONE, SV_PICK_PLACE_POS, SV_PICK_PLACE_FACE, SV_PICK_MOVE, SV_PICK_OVERLAY, SV_PICK_CAMERA };

/* The yellow step box of the side panel while a script plays or a point
   is being picked (see wizard_step_text). */
static const char *script_step_text(char *buf, size_t n, const char **sub) {
    static char sb[400];
    *sub = sb;
    if (g_sv_pick == SV_PICK_CAMERA) {
        snprintf(sb, sizeof(sb), "Click the point of the room the view slides to (it becomes the centre of the view). Esc: cancel.");
        return "Camera -- its target";
    }
    if (g_sv_pick == SV_PICK_OVERLAY) {
        snprintf(sb, sizeof(sb), "Click where the overlay's CENTRE goes (it's shown at its place now). Its position is the room's: every script uses it. Esc: cancel.");
        return "Overlays -- place an animation";
    }
    if (g_sv_pick == SV_PICK_PLACE_POS) {
        snprintf(sb, sizeof(sb), "Click the floor where the character appears (the green floor). Esc: cancel.");
        return "Script -- place a character: its position";
    }
    if (g_sv_pick == SV_PICK_PLACE_FACE) {
        snprintf(sb, sizeof(sb), "Click the point it looks toward. Right-click or Esc: keep its current direction.");
        return "Script -- place a character: where it faces";
    }
    if (g_sv_pick == SV_PICK_MOVE) {
        snprintf(sb, sizeof(sb), "Click the floor where the character goes, or click a CONNECTOR (blue) to make it leave the room through it. Esc: cancel.");
        return "Script -- move a character: its destination";
    }
    if (g_run.active && g_run.zone_on) {
        snprintf(buf, n, "Script \"%s\" -- timeline, %.1f s", g_run.s.name, g_run.zone_t);
        snprintf(sb, sizeof(sb), "Left-click: skip the rest of this timeline (not while a character moves in it).\nEsc: stop the script (testing).");
        return buf;
    }
    if (g_run.active) {
        snprintf(buf, n, "Script \"%s\" -- row %d / %d", g_run.s.name, g_run.row + 1, g_run.s.rows);
        snprintf(sb, sizeof(sb), "%s\nEsc: stop the script (testing).",
                 script_row_has_move(&g_run.s, g_run.row) ? "This row can't be skipped: a character is moving." : "Left-click: skip this row.");
        return buf;
    }
    return NULL;
}

/* =====================================================================
   SCRIPTS SCREEN (S)
   Left: the room's scripts. Middle: the grid (one action per cell).
   Right: the selected cell's action and its settings.
   ===================================================================== */
enum {
    B_SV_AN_TIMES = 280, B_SV_AN_ROW, B_SV_AN_CLIP, B_SV_PORTRAIT, B_SV_AN_LOOP, B_SV_AN_NORMAL, B_SV_AN_FOLLOW, B_SV_AN_SPD_M, B_SV_AN_SPD_P, B_SV_FADE_M, B_SV_FADE_P, B_SV_AN_FREEZE,
    B_SV_T_M1, B_SV_T_M01, B_SV_T_P01, B_SV_T_P1, B_SV_ZONE, B_SV_UNZONE, B_SV_ZSCALE_M, B_SV_ZSCALE_P, /* (below B_SV_FIRST_ID: routed like the others) */
    B_SV_FIRST = 300,
    B_SV_NEW = B_SV_FIRST, B_SV_RENAME, B_SV_DUP, B_SV_DELETE, B_SV_AUTO, B_SV_CLOSE,
    B_SV_PLAY, B_SV_PLAY_ROW, B_SV_ROW_INS, B_SV_ROW_DEL, B_SV_COL_ADD, B_SV_COL_DEL, B_SV_COPY, B_SV_PASTE, B_SV_CLEAR,
    B_SV_TYPE, B_SV_W_M1, B_SV_W_M01, B_SV_W_P01, B_SV_W_P1, B_SV_BG_CHOOSE, B_SV_BG_OWN, B_SV_MU_ADD,
    B_SV_FILE, B_SV_LISTEN, B_SV_REP_M, B_SV_REP_P, B_SV_WAITEND, B_SV_LOOP, B_SV_ST_ALL, B_SV_ST_MUSIC,
    B_SV_PL_MODEL, B_SV_PL_POS, B_SV_FACE_L, B_SV_FACE_R, B_SV_MV_DEST, B_SV_MV_WALK, B_SV_MV_RUN, B_SV_MOVESET,
    B_CH_OK, B_CH_CANCEL, B_CH_LISTEN, B_CH_SCOPE0, B_CH_SCOPE1, B_CH_SCOPE2,
    B_SV_OV_LEFT, B_SV_OV_RIGHT, B_SV_OV_UP, B_SV_OV_DOWN, B_SV_OV_PICK, B_SV_OV_WAIT, B_SV_OV_RELOAD, B_SV_RM_ROOM,
    B_SV_CAM_KEEP, B_SV_CAM_POINT, B_SV_CAM_DAVID, B_SV_CAM_PICK, B_SV_CAM_ZM, B_SV_CAM_ZP, B_SV_CAM_ZROOM, B_SV_CAM_SM, B_SV_CAM_SP,
    B_SV_ADD = 400,     /* + action type */
    B_SV_TRACK = 500,   /* + track * 8 + op (0 listen, 1 loop, 2 up, 3 down, 4 remove) */
    B_SV_STOPREF = 600, /* + index in the script's sounds / ambiences */
    B_SV_ACTOR = 700,   /* + index: 0 David, then the script's placed characters */
    B_SV_OVROW = 1000,  /* + entry * 8 + op (0 select, 1 mode, 2 frame -, 3 frame +) */
    B_SV_RM_DOOR = 1200, /* + index: 0 = where he'd spawn, then the target room's connectors */
    B_SV_RM_SCRIPT = 1300, /* + index: 0 = none, then the target room's scripts */
    B_SV_ZROWS_M = 1380, B_SV_ZROWS_P, /* the selected timeline: a row less / more */
    B_SV_TRIM_IN_M, B_SV_TRIM_IN_P, B_SV_TRIM_OUT_M, B_SV_TRIM_OUT_P, B_SV_TRIM_RESET, /* a timeline action's trims */
    B_SV_POOL = 1400,    /* + i: take the pool's entry i out */
    B_SV_POOL_ADD = 1410,
    B_SV_CS_SIDE = 1420, /* + 0 unchanged, 1 ally, 2 enemy (Character settings) */
    B_SV_CS_AI = 1425,   /* + 0 unchanged, 1 on, 2 off */
    B_SV_CS_WPN = 1430,  /* + 0 unchanged, 1 choose */
    B_SV_CS_SHD = 1435,  /* + 0 unchanged, 1 choose */
    B_SV_PL_SIDE = 1440, /* + 0 ally, 1 enemy (Place character) */
    B_SV_PL_AI = 1445,
    B_SV_LAST = 899
};
#define SV_ROW_H 46
#define SV_HDR_H 24
#define SV_ROWHDR_W 44
#define SV_LIST_ROW_H 26
#define CH_ROW_H 22

static int g_sv_script = 0, g_sv_row = 0, g_sv_col = 0, g_sv_scroll = 0, g_sv_list_scroll = 0;
static int g_sv_hover = -1;                 /* hovered cell: row * SCRIPT_MAX_COLS + col */
static int g_sv_anchor = 0;                 /* several rows selected: from this row to g_sv_row (Shift) */
static float g_sv_pending_start = -1.0f;    /* the empty cell selected in a timeline: a new action there starts at this time */
static int g_sv_drag = 0, g_sv_drag_y0 = 0, g_sv_drag_moved = 0; /* an action of a timeline dragged */
static int g_sv_zdrag = 0;                  /* a timeline's bottom edge dragged: its index + 1 */
/* A GROUP of actions selected (their ids): all in plain rows, or all in one
   timeline -- never both. Ctrl+click adds / removes one, Shift+click takes
   every action of the rows from the anchor, a rectangle drawn from an
   empty spot takes the ones it touches; dragging one moves them all. */
#define SV_MAX_SEL 256
static int g_sv_sel_ids[SV_MAX_SEL], g_sv_sel_n = 0, g_sv_sel_script = -1;
static int g_sv_band = 0, g_sv_band_zone = -1, g_sv_band_x0, g_sv_band_y0, g_sv_band_x1, g_sv_band_y1; /* 1 pressed, 2 drawing */
static int g_sv_cdrag = 0, g_sv_cdrag_r0, g_sv_cdrag_c0, g_sv_cdrag_dr, g_sv_cdrag_dc; /* plain rows: the group dragged by (dr, dc) cells */
static float g_sv_gt0[SV_MAX_SEL];          /* a timeline group dragged: each one's start, lane, when it began */
static int g_sv_glane0[SV_MAX_SEL], g_sv_gdc = 0, g_sv_drag_c0 = 0, g_sv_drag_id = 0;
static float g_sv_last_dt = 0.0f, g_sv_rsz_last = 0.0f; /* the last position a drag / an edge had with nothing overlapping */
static int g_sv_drag_free = 0;              /* the dragged ones already overlapped something when the drag began: not held back */
static int sv_sel_has(int id);
static int g_sv_rsz = 0;                    /* an action of a timeline resized: 1 its top edge, 2 its bottom edge */
static float g_sv_rsz_start, g_sv_rsz_in, g_sv_rsz_out, g_sv_rsz_sec, g_sv_rsz_len;
static int g_sv_edge_hover = -1;            /* the action edge under the mouse: (row * SCRIPT_MAX_COLS + col) * 4 + 1 top / 2 bottom */
static int g_sv_zedge_hover = -1;           /* the timeline whose bottom edge is under the mouse */
static ScriptAction g_sv_clip;              /* copy / paste */
static int g_sv_has_clip = 0;
static int g_sv_text = 0;                   /* typing a script name: 1 new, 2 rename */
static char g_sv_text_buf[64];
static RECT g_sv_list_rc, g_sv_grid_rc, g_sv_insp_rc, g_sv_thumb_rc, g_sv_port_rc, g_sv_ovprev_rc;
static int g_sv_port_num = 0; /* the portrait shown in the inspector (SPEAK) */
static int g_sv_ov_sel = 0;     /* the overlay selected in an Overlays action */
static struct { char label[128]; int nd, door_id[24]; char door_to[24][128]; int ns; char script[24][64]; } g_rinfo;

/* labels of the screen, built with its buttons (sv_layout) */
typedef struct { RECT r; char text[220]; COLORREF col; int font; UINT flags; } SvLabel;
static SvLabel g_svl[160];
static int g_svl_n = 0;
static void svl(int x, int y, int w, int h, int font, COLORREF col, UINT flags, const char *fmt, ...) {
    if (g_svl_n >= (int)(sizeof(g_svl) / sizeof(g_svl[0]))) return;
    SvLabel *l = &g_svl[g_svl_n++];
    SetRect(&l->r, x, y, x + w, y + h);
    va_list ap; va_start(ap, fmt); vsnprintf(l->text, sizeof(l->text), fmt, ap); va_end(ap);
    l->col = col; l->font = font; l->flags = flags;
}
static HFONT svl_font(int f) {
    switch (f) { case 1: return ui_font(12, 1); case 2: return ui_font(20, 1); case 3: return ui_font(16, 1); default: return ui_font(14, 0); }
}
static void svl_draw(HDC hdc) {
    for (int i = 0; i < g_svl_n; i++) {
        SvLabel *l = &g_svl[i];
        SelectObject(hdc, svl_font(l->font));
        ui_text(hdc, l->r.left, l->r.top, l->r.right - l->r.left, l->r.bottom - l->r.top, l->text, l->col, l->flags | DT_NOPREFIX);
    }
}
#define SVL_SECTION RGB(120, 150, 190)
#define SVL_TEXT RGB(215, 215, 220)
#define SVL_VALUE RGB(150, 200, 255)
#define SVL_DIM RGB(140, 140, 150)
#define SV_ONE (DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)

static COLORREF action_color(int type) {
    static const COLORREF c[ACT_COUNT] = { RGB(90, 90, 100), RGB(160, 160, 170), RGB(190, 130, 255), RGB(90, 160, 255), RGB(255, 160, 60),
                                           RGB(60, 205, 190), RGB(255, 90, 90), RGB(240, 210, 80), RGB(110, 220, 110),
                                           RGB(255, 130, 200), RGB(235, 215, 170), RGB(170, 230, 90), RGB(190, 190, 255), RGB(120, 220, 230), RGB(255, 190, 120) };
    return (type >= 0 && type < ACT_COUNT) ? c[type] : RGB(200, 200, 200);
}

static Script *sv_cur(void) { return (g_sv_script >= 0 && g_sv_script < g_script_count) ? &g_scripts[g_sv_script] : NULL; }
/* the selected cell (NULL: no script, or past the last row and !make) */
static ScriptAction *sv_cell(int make) {
    Script *s = sv_cur();
    if (!s) return NULL;
    if (make) script_ensure_rows(s, g_sv_row + 1);
    return script_at(s, g_sv_row, g_sv_col);
}
static void sv_zones_unoverlap(Script *s);
static void sv_changed(void) { Script *s = sv_cur(); if (s) sv_zones_unoverlap(s); room_scripts_save(); }

/* every SOUND / AMBIENCE action (STOP targets) or PLACE action (MOVE characters) of a script */
static int sv_actions_of(Script *s, int t1, int t2, ScriptAction **out, int *rows, int max) {
    int n = 0;
    for (int r = 0; r < s->rows; r++)
        for (int c = 0; c < s->cols; c++) {
            ScriptAction *a = script_at(s, r, c);
            if ((a->type == t1 || a->type == t2) && n < max) { rows[n] = r; out[n++] = a; }
        }
    return n;
}

/* ---- choosers: sound files, pictures ---- */
enum { CH_NONE, CH_SOUND, CH_TRACK, CH_PICTURE, CH_CLIP, CH_PORTRAIT, CH_ROOM };
static CharModel *g_ch_model = NULL;   /* CH_CLIP: the character the clips are for; CH_PORTRAIT: the one getting a portrait */
static float g_ch_t = 0.0f;            /* CH_CLIP: preview time */
#define PORT_CELL_W 100
#define PORT_CELL_H 148
static int ch_grid(void);
static int ch_cols(void);
static int g_ch = CH_NONE, g_ch_sel = 0, g_ch_scroll = 0, g_ch_scope = 0;
static char g_ch_filter[48] = "";
static int g_ch_filter_len = 0;
static RECT g_ch_rc, g_ch_list_rc, g_ch_prev_rc;
typedef struct { char name[160]; int w, h; float secs; int header; } ChEntry; /* w = -1 / secs = -2: not read yet; header: a sound category's title */
static ChEntry *g_ch_all = NULL;
static int g_ch_all_n = 0, g_ch_all_kind = -1, g_ch_all_scope = -1;
static int *g_ch_list = NULL, g_ch_n = 0;

static void ch_add(const char *name, int *cap) {
    if (g_ch_all_n == *cap) { *cap = *cap ? *cap * 2 : 1024; g_ch_all = (ChEntry *)realloc(g_ch_all, sizeof(ChEntry) * *cap); }
    ChEntry *e = &g_ch_all[g_ch_all_n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->w = -1; e->h = 0; e->secs = -2.0f; e->header = 0;
}
static int ch_name_cmp(const void *a, const void *b) { return strcmp(((const ChEntry *)a)->name, ((const ChEntry *)b)->name); }
/* the pictures of one room folder (level/room), main one first */
static void ch_scan_room_pictures(const char *label, int *cap, int main_only) {
    char level[64], room[64], pat[1024], name[200];
    split_label(label, level, sizeof(level), room, sizeof(room));
    if (main_only) {
        snprintf(name, sizeof(name), "%s/%s/%s.png", level, room, room);
        ch_add(name, cap);
        return;
    }
    root_path(pat, sizeof(pat), "assets/levels/%s/%s/*.png", level, room);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int first = g_ch_all_n;
    do {
        if (strstr(fd.cFileName, ".mask.")) continue;
        char lower[128]; snprintf(lower, sizeof(lower), "%s", fd.cFileName);
        for (char *q = lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
        snprintf(name, sizeof(name), "%s/%s/%s", level, room, lower);
        ch_add(name, cap);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_ch_all + first, g_ch_all_n - first, sizeof(ChEntry), ch_name_cmp);
}
static void ch_build(void) {
    static char room[128] = "";
    if (g_ch == CH_CLIP || g_ch == CH_PORTRAIT || g_ch == CH_ROOM) { /* rebuilt every time: they depend on the character */
        g_ch_all_n = 0;
        int cap = 0;
        free(g_ch_all); g_ch_all = NULL;
        g_ch_all_kind = g_ch == CH_CLIP ? 2 : 3;
        if (g_ch == CH_ROOM) { for (int i = 0; i < g_map_room_count; i++) ch_add(g_map_rooms[i].label, &cap); return; }
        if (g_ch == CH_PORTRAIT) {
            char nm[32];
            for (int i = 1; i <= PORTRAIT_MAX; i++) { snprintf(nm, sizeof(nm), "bigports.%d", i); ch_add(nm, &cap); }
            return;
        }
        const CharModel *m = g_ch_model;
        if (!m) return;
        char ref[160];
        for (int pass = 0; pass < 3; pass++) /* its own clips first, then the shared ones */
            for (int i = 0; i < anim_lib_count(); i++) {
                const char *src = anim_lib_source(i);
                if (pass == 0 ? strcmp(src, m->name) != 0 : pass == 1 ? strcmp(src, "anims") != 0 : (!strcmp(src, m->name) || !strcmp(src, "anims"))) continue;
                if (!anim_lib_fits(i, m->node_count)) continue;
                snprintf(ref, sizeof(ref), "%s/%s", src, anim_lib_name(i));
                ch_add(ref, &cap);
                g_ch_all[g_ch_all_n - 1].secs = anim_lib_duration(i);
            }
        return;
    }
    if (g_ch == CH_SOUND || g_ch == CH_TRACK) { /* by kind, the likeliest first */
        static const int for_track[SND_KINDS] = { SND_MUSIC, SND_ROOM_LINES, SND_SFX, SND_OTHER_LINES };
        static const int for_speak[SND_KINDS] = { SND_ROOM_LINES, SND_OTHER_LINES, SND_SFX, SND_MUSIC };
        static const int for_sound[SND_KINDS] = { SND_ROOM_LINES, SND_SFX, SND_MUSIC, SND_OTHER_LINES };
        ScriptAction *a = sv_cell(0);
        const int *order = g_ch == CH_TRACK ? for_track : (a && a->type == ACT_SPEAK) ? for_speak : for_sound;
        SoundRow *rows;
        int n = sound_rows(order, SND_KINDS, current_room_label(), &rows), cap = 0;
        g_ch_all_n = 0;
        free(g_ch_all); g_ch_all = NULL;
        g_ch_all_kind = 4;
        for (int i = 0; i < n; i++) { ch_add(rows[i].name, &cap); g_ch_all[g_ch_all_n - 1].header = rows[i].header; }
        free(rows);
        return;
    }
    if (g_ch_all_kind >= 2) g_ch_all_kind = -1;
    int kind = g_ch == CH_PICTURE ? 1 : 0, scope = kind ? g_ch_scope : 0;
    if (g_ch_all_kind == kind && g_ch_all_scope == scope && ((kind == 1 && scope == 1) || !strcmp(room, current_room_label()))) return; /* scanned already */
    snprintf(room, sizeof(room), "%s", current_room_label());
    g_ch_all_n = 0;
    int cap = 0;
    free(g_ch_all); g_ch_all = NULL;
    g_ch_all_kind = kind; g_ch_all_scope = scope;
    if (!kind) {
        /* the room's own sounds first (its characters' lines), then assets/sound */
        char level[64], rm[64], pat[1024], nm[200];
        split_label(current_room_label(), level, sizeof(level), rm, sizeof(rm));
        root_path(pat, sizeof(pat), "assets/levels/%s/%s/*.ogg", level, rm);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                snprintf(nm, sizeof(nm), "levels/%s/%s/%s", level, rm, fd.cFileName);
                for (char *q = nm; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
                ch_add(nm, &cap);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        qsort(g_ch_all, g_ch_all_n, sizeof(ChEntry), ch_name_cmp);
        int first_shared = g_ch_all_n;
        root_path(pat, sizeof(pat), "assets/sound/*.ogg");
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char lower[128]; snprintf(lower, sizeof(lower), "%s", fd.cFileName);
                for (char *q = lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
                ch_add(lower, &cap);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        qsort(g_ch_all + first_shared, g_ch_all_n - first_shared, sizeof(ChEntry), ch_name_cmp);
    } else if (scope == 0) ch_scan_room_pictures(current_room_label(), &cap, 0);
    else if (scope == 1) { for (int i = 0; i < g_map_room_count; i++) ch_scan_room_pictures(g_map_rooms[i].label, &cap, 1); }
    else {
        char level[64], room[64];
        split_label(current_room_label(), level, sizeof(level), room, sizeof(room));
        for (int i = 0; i < g_map_room_count; i++) {
            char l2[64], r2[64];
            split_label(g_map_rooms[i].label, l2, sizeof(l2), r2, sizeof(r2));
            if (!strcmp(l2, level)) ch_scan_room_pictures(g_map_rooms[i].label, &cap, 0);
        }
    }
}
static int ch_rows_visible(void) { int n = (g_ch_list_rc.bottom - g_ch_list_rc.top) / CH_ROW_H; return n < 1 ? 1 : n; }
static void ch_filter(void) {
    ch_build();
    g_ch_list = (int *)realloc(g_ch_list, sizeof(int) * (g_ch_all_n + 1));
    g_ch_n = 0;
    int header = -1; /* a category's title shows when one of its files does */
    for (int i = 0; i < g_ch_all_n; i++) {
        if (g_ch_all[i].header) { header = i; continue; }
        if (!ci_strstr(g_ch_all[i].name, g_ch_filter)) continue;
        if (header >= 0) { g_ch_list[g_ch_n++] = header; header = -1; }
        g_ch_list[g_ch_n++] = i;
    }
    g_ch_sel = 0;
    while (g_ch_sel < g_ch_n - 1 && g_ch_all[g_ch_list[g_ch_sel]].header) g_ch_sel++;
    g_ch_scroll = 0;
}
static void ch_select(int k) {
    if (g_ch_n <= 0) return;
    if (k < 0) k = 0;
    if (k >= g_ch_n) k = g_ch_n - 1;
    int dir = k < g_ch_sel ? -1 : 1; /* the category titles are skipped */
    while (g_ch_all[g_ch_list[k]].header && k + dir >= 0 && k + dir < g_ch_n) k += dir;
    if (g_ch_all[g_ch_list[k]].header) { dir = -dir; while (g_ch_all[g_ch_list[k]].header && k + dir >= 0 && k + dir < g_ch_n) k += dir; }
    if (k != g_ch_sel) g_ch_t = 0.0f;
    g_ch_sel = k;
    if (ch_grid()) { /* g_ch_scroll counts grid rows */
        int cols = ch_cols(), row = k / cols, rows = (g_ch_list_rc.bottom - g_ch_list_rc.top) / PORT_CELL_H;
        if (rows < 1) rows = 1;
        if (row < g_ch_scroll) g_ch_scroll = row;
        if (row >= g_ch_scroll + rows) g_ch_scroll = row - rows + 1;
        return;
    }
    int rows = ch_rows_visible();
    if (g_ch_sel < g_ch_scroll) g_ch_scroll = g_ch_sel;
    if (g_ch_sel >= g_ch_scroll + rows) g_ch_scroll = g_ch_sel - rows + 1;
}
static int ch_grid(void) { return g_ch == CH_PORTRAIT; }
static int ch_cols(void) { int c = (g_ch_list_rc.right - g_ch_list_rc.left) / PORT_CELL_W; return c < 1 ? 1 : c; }
static int ch_grid_at(int x, int y) {
    if (x < g_ch_list_rc.left || y < g_ch_list_rc.top || x >= g_ch_list_rc.right || y >= g_ch_list_rc.bottom) return -1;
    int c = (x - g_ch_list_rc.left) / PORT_CELL_W, r = (y - g_ch_list_rc.top) / PORT_CELL_H;
    if (c >= ch_cols()) return -1;
    int k = (g_ch_scroll + r) * ch_cols() + c;
    return k < g_ch_n ? k : -1;
}
static const char *ch_current(void) { return (g_ch_n > 0 && g_ch_sel < g_ch_n && !g_ch_all[g_ch_list[g_ch_sel]].header) ? g_ch_all[g_ch_list[g_ch_sel]].name : NULL; }
static void ch_open(int kind, const char *current) {
    g_ch = kind;
    g_ch_filter[0] = 0; g_ch_filter_len = 0;
    if (kind == CH_PICTURE) g_ch_scope = 0;
    g_ch_sel = 0;
    ch_filter();
    if (current && current[0])
        for (int k = 0; k < g_ch_n; k++) if (!strcmp(g_ch_all[g_ch_list[k]].name, current)) { g_ch_sel = k; break; }
    g_ch_scroll = ch_grid() ? 0 : g_ch_sel - 8; if (g_ch_scroll < 0) g_ch_scroll = 0;
    g_ch_t = 0.0f;
}
static int g_ch_to_pool = 0; /* the chooser adds to the action's random pool (not its main file) */
static void ch_close(void) { g_ch = CH_NONE; audio_preview(NULL); g_ch_to_pool = 0; }
static void ch_choose(void) {
    const char *name = ch_current();
    ScriptAction *a = sv_cell(1);
    if (!name || !a) { ch_close(); return; }
    if (g_ch == CH_PORTRAIT) { /* kept for this character for good */
        if (g_ch_model) portrait_set(g_ch_model->name, atoi(name + 9));
        ch_close();
        return;
    }
    if (g_ch_to_pool && ((g_ch == CH_CLIP && a->type == ACT_ANIM) || (g_ch == CH_SOUND && a->type == ACT_SOUND))) {
        if (a->npool < SCRIPT_POOL_MAX) snprintf(a->pool[a->npool++], sizeof(a->pool[0]), "%s", name);
    }
    else if (g_ch == CH_PICTURE && a->type == ACT_BACKGROUND) snprintf(a->file, sizeof(a->file), "%s", name);
    else if (g_ch == CH_CLIP && a->type == ACT_ANIM) snprintf(a->file, sizeof(a->file), "%s", name);
    else if (g_ch == CH_ROOM && a->type == ACT_ROOM) { snprintf(a->file, sizeof(a->file), "%s", name); a->door = 0; a->script[0] = 0; }
    else if (g_ch == CH_SOUND && (a->type == ACT_SOUND || a->type == ACT_AMBIENCE || a->type == ACT_SPEAK)) snprintf(a->file, sizeof(a->file), "%s", name);
    else if (g_ch == CH_TRACK && a->type == ACT_MUSIC && a->ntracks < AUDIO_MAX_TRACKS) {
        snprintf(a->track[a->ntracks], sizeof(a->track[0]), "%s", name);
        a->track_loop[a->ntracks] = 0;
        a->ntracks++;
    }
    sv_changed();
    ch_close();
}

/* picture preview (the chooser, the inspector): one picture kept */
static void blit_picture(HDC hdc, const RECT *r, const char *file) {
    static char cached[160] = "";
    static uint32_t *px = NULL;
    static int pw = 0, ph = 0;
    if (strcmp(cached, file) != 0) {
        free(px); px = NULL; pw = ph = 0;
        snprintf(cached, sizeof(cached), "%s", file);
        char path[1024];
        if (file[0]) root_path(path, sizeof(path), "assets/levels/%s", file);
        else { char l[64], rm[64]; split_label(current_room_label(), l, sizeof(l), rm, sizeof(rm)); root_path(path, sizeof(path), "assets/levels/%s/%s/%s.png", l, rm, rm); }
        px = image_load(path, &pw, &ph);
    }
    ui_fill(hdc, r, RGB(10, 10, 14));
    if (!px || pw <= 0 || ph <= 0) { ui_text(hdc, r->left, r->top, r->right - r->left, r->bottom - r->top, "(no picture)", SVL_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE); return; }
    int W = r->right - r->left, H = r->bottom - r->top;
    float k = fminf((float)W / pw, (float)H / ph);
    int dw = (int)(pw * k), dh = (int)(ph * k);
    if (dw < 1 || dh < 1) return;
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc, r->left + (W - dw) / 2, r->top + (H - dh) / 2, dw, dh, 0, 0, pw, ph, px, &bi, DIB_RGB_COLORS, SRCCOPY);
}
static void png_size(const char *file, int *w, int *h) {
    char path[1024];
    root_path(path, sizeof(path), "assets/levels/%s", file);
    *w = 0; *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char b[24];
    if (fread(b, 1, 24, f) == 24) { *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19]; *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23]; }
    fclose(f);
}

/* ---- the screen ---- */
static void sv_open_view(void) {
    g_script_view = 1;
    g_rinfo.label[0] = 0;
    g_map_mode = 0; g_anim_view = 0;
    ClipCursor(NULL); g_clip_active = 0;
    if (g_sv_script >= g_script_count) g_sv_script = g_script_count - 1;
    if (g_sv_script < 0) g_sv_script = 0;
}
static void script_view_toggle(void) {
    if (g_script_view) { g_script_view = 0; ch_close(); g_sv_text = 0; return; }
    if (!g_has_3d_character || script_playing()) return;
    if (g_edit_mode && g_wiz != WIZ_NONE) wizard_cancel(NULL);
    sv_open_view();
}

static int sv_grid_rows_visible(void) { int n = (g_sv_grid_rc.bottom - g_sv_grid_rc.top - SV_HDR_H) / SV_ROW_H; return n < 1 ? 1 : n; }
static int sv_grid_rows_total(void) {
    Script *s = sv_cur();
    int n = s ? script_used_rows(s) + 1 : 1;
    if (n < g_sv_row + 1) n = g_sv_row + 1;
    for (int z = 0; s && z < s->nzones; z++) if (n < s->zone[z].row0 + s->zone[z].rows + 1) n = s->zone[z].row0 + s->zone[z].rows + 1;
    return n;
}
static void sv_select(int row, int col) {
    Script *s = sv_cur();
    if (!s) return;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col >= s->cols) col = s->cols - 1;
    g_sv_row = row; g_sv_col = col;
    if (!(GetKeyState(VK_SHIFT) & 0x8000)) g_sv_anchor = row;
    g_sv_pending_start = -1.0f;
    int vis = sv_grid_rows_visible();
    if (g_sv_row < g_sv_scroll) g_sv_scroll = g_sv_row;
    if (g_sv_row >= g_sv_scroll + vis) g_sv_scroll = g_sv_row - vis + 1;
}
static void sv_cell_rect(int row, int col, RECT *r) {
    Script *s = sv_cur();
    int cols = s ? s->cols : SCRIPT_DEFAULT_COLS;
    int cw = (g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W) / cols;
    int x = g_sv_grid_rc.left + SV_ROWHDR_W + col * cw, y = g_sv_grid_rc.top + SV_HDR_H + (row - g_sv_scroll) * SV_ROW_H;
    SetRect(r, x + 2, y + 2, x + cw - 2, y + SV_ROW_H - 2);
}
static int sv_cell_at(int x, int y, int *row, int *col) {
    Script *s = sv_cur();
    if (!s || x < g_sv_grid_rc.left + SV_ROWHDR_W || x >= g_sv_grid_rc.right || y < g_sv_grid_rc.top + SV_HDR_H || y >= g_sv_grid_rc.bottom) return 0;
    int cw = (g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W) / s->cols;
    int c = (x - g_sv_grid_rc.left - SV_ROWHDR_W) / cw, r = g_sv_scroll + (y - g_sv_grid_rc.top - SV_HDR_H) / SV_ROW_H;
    if (c >= s->cols) return 0;
    *row = r; *col = c;
    return 1;
}

/* ---- ADVANCED TIMELINE zones in the grid: each action a block from its
   start, as tall as it lasts (an instant one: a thin line); the columns
   are lanes, overlapping blocks of a lane are indented. ---- */
static void sv_sel_range(int *lo, int *hi) { *lo = g_sv_anchor < g_sv_row ? g_sv_anchor : g_sv_row; *hi = g_sv_anchor > g_sv_row ? g_sv_anchor : g_sv_row; }
static int sv_range_free(const Script *s, int lo, int hi) {
    for (int z = 0; z < s->nzones; z++) if (lo < s->zone[z].row0 + s->zone[z].rows && s->zone[z].row0 <= hi) return 0;
    return 1;
}
static int sv_zone_y(const ScriptZone *z) { return g_sv_grid_rc.top + SV_HDR_H + (z->row0 - g_sv_scroll) * SV_ROW_H; }
/* where the zone's actions are over (the "until the others end" animations don't count) */
static float sv_zone_end(Script *s, int zi) {
    const ScriptZone *z = &s->zone[zi];
    float e = 0.0f;
    for (int r = z->row0; r < z->row0 + z->rows && r < s->rows; r++)
        for (int c = 0; c < s->cols; c++) {
            const ScriptAction *a = script_at(s, r, c);
            if (a->type == ACT_NONE || (a->type == ACT_ANIM && a->anim_mode == ANIM_ROW)) continue;
            float d;
            if (action_duration(a, &d) == DUR_DYNAMIC && !measured_duration(s->name, a->id, &d)) d = a->type == ACT_MOVE ? 2.0f : 1.0f;
            if (a->start + d > e) e = a->start + d;
        }
    return e;
}
/* its length as drawn: a DYNAMIC one its last measured length, else an estimate */
static float sv_event_len(Script *s, int zi, const ScriptAction *a, int *kind) {
    float d;
    *kind = action_duration(a, &d);
    if (*kind != DUR_DYNAMIC) return d;
    if (measured_duration(s->name, a->id, &d)) return d;
    if (a->type == ACT_ANIM) { float e = sv_zone_end(s, zi) - a->start; return e > 0.5f ? e : 0.5f; }
    return a->type == ACT_MOVE ? 2.0f : 1.0f;
}
static void sv_event_geom(Script *s, int zi, int r, int c, RECT *out, int *kind, float *len) {
    const ScriptZone *z = &s->zone[zi];
    const ScriptAction *a = script_at(s, r, c);
    *len = sv_event_len(s, zi, a, kind);
    float pps = SV_ROW_H / z->sec_per_row;
    int depth = 0;
    if (*kind != DUR_INSTANT)
        for (int rr = z->row0; rr < z->row0 + z->rows && rr < s->rows; rr++) {
            if (rr == r) continue;
            const ScriptAction *b = script_at(s, rr, c);
            if (b->type == ACT_NONE) continue;
            int k2; float l2 = sv_event_len(s, zi, b, &k2);
            if (k2 == DUR_INSTANT) continue;
            if ((b->start < a->start || (b->start == a->start && rr < r)) && b->start + l2 > a->start + 1e-4f) depth++;
        }
    int cw = (g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W) / s->cols;
    int lx = g_sv_grid_rc.left + SV_ROWHDR_W + c * cw;
    int x0 = lx + 4 + depth * 16, x1 = lx + cw - 4;
    if (x0 > x1 - 40) x0 = x1 - 40;
    int y = sv_zone_y(z) + (int)(a->start * pps);
    if (*kind == DUR_INSTANT) SetRect(out, x0, y - 1, x1, y + 2);
    else { int h = (int)(*len * pps); if (h < 10) h = 10; SetRect(out, x0, y, x1, y + h); }
}
/* the point (x, y) is in a timeline zone: which one, its lane, the time there (snapped), the action under it (*row -1: none) */
static int sv_zone_hit(int x, int y, int *zi, int *row, int *col, float *t) {
    Script *s = sv_cur();
    if (!s || x < g_sv_grid_rc.left + SV_ROWHDR_W || x >= g_sv_grid_rc.right || y < g_sv_grid_rc.top + SV_HDR_H || y >= g_sv_grid_rc.bottom) return 0;
    int cw = (g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W) / s->cols;
    int c = (x - g_sv_grid_rc.left - SV_ROWHDR_W) / cw;
    if (c >= s->cols) return 0;
    for (int z = 0; z < s->nzones; z++) {
        int y0 = sv_zone_y(&s->zone[z]), y1 = y0 + s->zone[z].rows * SV_ROW_H;
        if (y < y0 || y >= y1) continue;
        *zi = z; *col = c; *row = -1;
        float tt = (float)(y - y0) * s->zone[z].sec_per_row / SV_ROW_H;
        *t = roundf(tt / 0.05f) * 0.05f;
        int best = -1, bc = 0, bscore = -1;
        POINT p = { x, y };
        for (int r = s->zone[z].row0; r < s->zone[z].row0 + s->zone[z].rows && r < s->rows; r++)
            for (int cc = 0; cc < s->cols; cc++) {
                if (script_at(s, r, cc)->type == ACT_NONE) continue;
                RECT er; int k; float l;
                sv_event_geom(s, z, r, cc, &er, &k, &l);
                if (k == DUR_INSTANT) InflateRect(&er, 0, 5);
                if (!PtInRect(&er, p)) continue;
                int score = (k == DUR_INSTANT ? 100000 : 0) + er.left; /* thin lines first, then the most indented */
                if (score > bscore) { bscore = score; best = r; bc = cc; }
            }
        if (best >= 0) { *row = best; *col = bc; }
        return 1;
    }
    return 0;
}
static void sv_paint_zones(HDC hdc, Script *s) {
    char t[300];
    int cw = (g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W) / s->cols;
    int gx = g_sv_grid_rc.left + SV_ROWHDR_W, body_top = g_sv_grid_rc.top + SV_HDR_H, body_bot = g_sv_grid_rc.bottom;
    for (int zi = 0; zi < s->nzones; zi++) {
        const ScriptZone *z = &s->zone[zi];
        int y0 = sv_zone_y(z), y1 = y0 + z->rows * SV_ROW_H;
        if (y1 < body_top || y0 > body_bot) continue;
        int saved = SaveDC(hdc);
        IntersectClipRect(hdc, g_sv_grid_rc.left, body_top, g_sv_grid_rc.right, body_bot);
        RECT zr = { gx, y0, gx + cw * s->cols, y1 };
        ui_fill(hdc, &zr, RGB(16, 30, 34));
        for (int k = 1; k < z->rows; k++) { RECT ln = { gx, y0 + k * SV_ROW_H, zr.right, y0 + k * SV_ROW_H + 1 }; ui_fill(hdc, &ln, RGB(30, 54, 60)); }
        for (int c = 1; c < s->cols; c++) { RECT ln = { gx + c * cw, y0, gx + c * cw + 1, y1 }; ui_fill(hdc, &ln, RGB(36, 60, 66)); }
        ui_frame(hdc, &zr, RGB(70, 170, 170));
        SelectObject(hdc, ui_font(11, 1));
        snprintf(t, sizeof(t), "ADVANCED TIMELINE  -  %.2g s PER ROW", z->sec_per_row);
        ui_text(hdc, zr.right - 300, y0 + 2, 294, 14, t, RGB(80, 160, 160), DT_RIGHT | DT_SINGLELINE);
        IntersectClipRect(hdc, zr.left, zr.top, zr.right, zr.bottom + 1);
        for (int pass = 0; pass < 2; pass++) /* the blocks, then the thin lines over them */
            for (int r = z->row0; r < z->row0 + z->rows && r < s->rows; r++)
                for (int c = 0; c < s->cols; c++) {
                    const ScriptAction *a = script_at(s, r, c);
                    if (a->type == ACT_NONE) continue;
                    RECT er; int kind; float len;
                    sv_event_geom(s, zi, r, c, &er, &kind, &len);
                    if ((kind == DUR_INSTANT) != (pass == 1)) continue;
                    int grp = sv_sel_has(a->id) && g_sv_sel_n > 1;
                    int sel = (r == g_sv_row && c == g_sv_col) || grp, hov = g_sv_hover == r * SCRIPT_MAX_COLS + c;
                    COLORREF selc = grp ? RGB(255, 160, 60) : RGB(255, 210, 60);
                    COLORREF col = action_color(a->type);
                    if (kind == DUR_INSTANT) {
                        ui_fill(hdc, &er, col);
                        RECT dot = { er.left, er.top - 3, er.left + 8, er.bottom + 3 };
                        ui_fill(hdc, &dot, col);
                        if (sel) { RECT fr = er; InflateRect(&fr, 2, 4); ui_frame(hdc, &fr, selc); }
                        char sm[200]; action_summary(s, a, sm, sizeof(sm));
                        snprintf(t, sizeof(t), "%s: %s", action_type_name(a->type), sm);
                        SelectObject(hdc, ui_font(11, sel || hov));
                        ui_text(hdc, er.left + 12, er.top - 14, er.right - er.left - 16, 13, t, sel || hov ? RGB(245, 245, 250) : RGB(150, 150, 160), SV_ONE);
                        continue;
                    }
                    ui_fill(hdc, &er, hov ? RGB(54, 54, 66) : RGB(40, 40, 50));
                    RECT st = { er.left, er.top, er.left + 5, er.bottom };
                    ui_fill(hdc, &st, col);
                    if (kind == DUR_DYNAMIC) /* no known end: it fades out */
                        for (int yy = er.bottom - 14; yy < er.bottom; yy += 3) { RECT ln = { er.left + 5, yy, er.right, yy + 1 }; ui_fill(hdc, &ln, col); }
                    ui_frame(hdc, &er, sel ? selc : RGB(72, 72, 88));
                    if (sel) { RECT in = er; InflateRect(&in, -1, -1); ui_frame(hdc, &in, selc); }
                    if (a->trim_in > 0.0f) for (int x = er.left + 6; x < er.right - 2; x += 6) { RECT d = { x, er.top, x + 3, er.top + 3 }; ui_fill(hdc, &d, RGB(255, 170, 90)); }
                    if (a->trim_out > 0.0f) for (int x = er.left + 6; x < er.right - 2; x += 6) { RECT d = { x, er.bottom - 3, x + 3, er.bottom }; ui_fill(hdc, &d, RGB(255, 170, 90)); }
                    if (g_sv_edge_hover >= 0 && g_sv_edge_hover / 4 == r * SCRIPT_MAX_COLS + c) {
                        int top = g_sv_edge_hover % 4 == 1;
                        RECT hl = { er.left, top ? er.top : er.bottom - 3, er.right, top ? er.top + 3 : er.bottom };
                        ui_fill(hdc, &hl, RGB(140, 240, 240));
                    }
                    int h = er.bottom - er.top, w = er.right - er.left - 14;
                    float m;
                    if (kind == DUR_KNOWN) snprintf(t, sizeof(t), "%.2f s%s", len, a->trim_in > 0.0f || a->trim_out > 0.0f ? " (cut)" : "");
                    else snprintf(t, sizeof(t), measured_duration(s->name, a->id, &m) ? "~%.1f s (last run)" : "? (~%.1f s)", len);
                    if (h >= 14) {
                        SelectObject(hdc, ui_font(12, 1));
                        ui_text(hdc, er.left + 10, er.top + 1, w - 70, 14, action_type_name(a->type), col, SV_ONE);
                        SelectObject(hdc, ui_font(11, 0));
                        ui_text(hdc, er.right - 76, er.top + 1, 70, 14, t, RGB(160, 200, 200), DT_RIGHT | DT_SINGLELINE);
                    }
                    if (h >= 30) {
                        char sm[200]; action_summary(s, a, sm, sizeof(sm));
                        SelectObject(hdc, ui_font(12, 0));
                        ui_text(hdc, er.left + 10, er.top + 16, w, 14, sm, RGB(220, 220, 228), SV_ONE);
                    }
                }
        /* where it's really over: the rows after the zone start there */
        float end = sv_zone_end(s, zi);
        int ye = y0 + (int)(end * SV_ROW_H / z->sec_per_row);
        if (ye < y1 - 2) {
            for (int x = zr.left; x < zr.right; x += 10) { RECT d = { x, ye, x + 5 < zr.right ? x + 5 : zr.right, ye + 2 }; ui_fill(hdc, &d, RGB(90, 210, 210)); }
            SelectObject(hdc, ui_font(11, 1));
            snprintf(t, sizeof(t), "ENDS AT %.2f s -- THE NEXT ROWS START HERE", end);
            ui_text(hdc, zr.left + 6, ye + 3, 400, 13, t, RGB(90, 210, 210), DT_LEFT | DT_SINGLELINE);
        } else if (end > z->rows * z->sec_per_row + 0.01f) {
            SelectObject(hdc, ui_font(11, 1));
            snprintf(t, sizeof(t), "v  GOES ON TO %.2f s: insert rows or zoom out", end);
            ui_text(hdc, zr.right - 330, y1 - 15, 324, 13, t, RGB(255, 170, 90), DT_RIGHT | DT_SINGLELINE);
        }
        RestoreDC(hdc, saved);
        /* the bottom edge: a handle to drag */
        if (y1 >= body_top && y1 <= body_bot) {
            int hot = g_sv_zedge_hover == zi || g_sv_zdrag == zi + 1;
            int mx = (zr.left + zr.right) / 2;
            RECT hb = { mx - 40, y1 - 3, mx + 40, y1 + 2 };
            ui_fill(hdc, &hb, hot ? RGB(140, 240, 240) : RGB(70, 170, 170));
            if (hot) {
                SelectObject(hdc, ui_font(11, 1));
                snprintf(t, sizeof(t), "DRAG: TALLER / SHORTER  (%d ROW%s = %.2g s)", z->rows, z->rows > 1 ? "S" : "", z->rows * z->sec_per_row);
                ui_text(hdc, mx + 48, y1 - 14, 360, 13, t, RGB(140, 240, 240), DT_LEFT | DT_SINGLELINE);
            }
        }
    }
}
/* the hovered action of a timeline, in full */
static void sv_paint_zone_tip(HDC hdc) {
    Script *s = sv_cur();
    if (!s || g_sv_hover < 0 || g_sv_drag || g_sv_rsz) return;
    int r = g_sv_hover / SCRIPT_MAX_COLS, c = g_sv_hover % SCRIPT_MAX_COLS;
    int zi = script_zone_at(s, r);
    const ScriptAction *a = script_at(s, r, c);
    if (zi < 0 || !a || a->type == ACT_NONE) return;
    char l1[200], l2[200], l3[200];
    int kind; float len = sv_event_len(s, zi, a, &kind), m;
    snprintf(l1, sizeof(l1), "%s  (lane %d)", action_type_name(a->type), c + 1);
    action_summary(s, a, l2, sizeof(l2));
    if (kind == DUR_INSTANT) snprintf(l3, sizeof(l3), "At %.2f s -- instant", a->start);
    else if (kind == DUR_KNOWN && (a->trim_in > 0.0f || a->trim_out > 0.0f))
        snprintf(l3, sizeof(l3), "%.2f s -> %.2f s  (lasts %.2f s: from %.2f s into it, %.2f s cut at its end)", a->start, a->start + len, len, a->trim_in, a->trim_out);
    else if (kind == DUR_KNOWN) snprintf(l3, sizeof(l3), "%.2f s -> %.2f s  (lasts %.2f s)", a->start, a->start + len, len);
    else snprintf(l3, sizeof(l3), "From %.2f s, until it's over (%s %.1f s)", a->start, measured_duration(s->name, a->id, &m) ? "last run:" : "estimate:", len);
    int w = 360, h = 62, x = g_mouse_client_x + 16, y = g_mouse_client_y + 18;
    if (x + w > g_sv_grid_rc.right) x = g_sv_grid_rc.right - w;
    if (y + h > g_sv_grid_rc.bottom) y = g_mouse_client_y - h - 8;
    RECT b = { x, y, x + w, y + h };
    ui_fill(hdc, &b, RGB(30, 30, 40)); ui_frame(hdc, &b, action_color(a->type));
    SelectObject(hdc, ui_font(13, 1));
    ui_text(hdc, x + 8, y + 4, w - 16, 16, l1, action_color(a->type), SV_ONE);
    SelectObject(hdc, ui_font(12, 0));
    ui_text(hdc, x + 8, y + 22, w - 16, 16, l2, RGB(225, 225, 230), SV_ONE);
    ui_text(hdc, x + 8, y + 40, w - 16, 16, l3, RGB(140, 210, 210), SV_ONE);
}
static int sv_sel_has(int id) { for (int i = 0; i < g_sv_sel_n; i++) if (g_sv_sel_ids[i] == id) return 1; return 0; }
/* ---- NO OVERLAP IN A LANE: two blocks of a lane never go into each other;
   an instant one may be on a block's edge, not inside it; two instant ones
   stay 0.05 s apart. (Other lanes: anything goes, that's what they're for.) ---- */
static int sv_ev_collide(float s1, float l1, int i1, float s2, float l2, int i2) {
    const float e = 1e-4f;
    if (i1 && i2) return fabsf(s1 - s2) < 0.05f - e;
    if (i1) return s2 + e < s1 && s1 < s2 + l2 - e;
    if (i2) return s1 + e < s2 && s2 < s1 + l1 - e;
    return s1 < s2 + l2 - e && s2 < s1 + l1 - e;
}
/* would one of these actions (ids) overlap another action of its lane (not one of them)? */
static int sv_ids_collide(Script *s, int zi, const int *ids, int n) {
    const ScriptZone *z = &s->zone[zi];
    for (int i = 0; i < n; i++) {
        int r, c;
        const ScriptAction *a = script_find(s, ids[i], &r, &c);
        if (!a || script_zone_at(s, r) != zi) continue;
        int k1; float l1 = sv_event_len(s, zi, a, &k1);
        for (int rr = z->row0; rr < z->row0 + z->rows && rr < s->rows; rr++) {
            const ScriptAction *b = script_at(s, rr, c);
            if (b->type == ACT_NONE || b == a) continue;
            int mine = 0;
            for (int j = 0; j < n; j++) if (ids[j] == b->id) mine = 1;
            if (mine) continue;
            int k2; float l2 = sv_event_len(s, zi, b, &k2);
            if (sv_ev_collide(a->start, l1, k1 == DUR_INSTANT, b->start, l2, k2 == DUR_INSTANT)) return 1;
        }
    }
    return 0;
}
/* After any change (a longer sound, another clip, a new action dropped in a
   block...): in each lane, in the order they start, an action that would go
   into the one before it starts at its end instead (and so on down the lane). */
static void sv_zones_unoverlap(Script *s) {
    for (int zi = 0; zi < s->nzones; zi++) {
        const ScriptZone *z = &s->zone[zi];
        for (int c = 0; c < s->cols; c++) {
            ScriptAction *ev[256]; int n = 0;
            for (int r = z->row0; r < z->row0 + z->rows && r < s->rows && n < 256; r++) {
                ScriptAction *a = script_at(s, r, c);
                if (a->type != ACT_NONE) ev[n++] = a;
            }
            int inst[256]; float len[256];
            for (int i = 0; i < n; i++) { int k; len[i] = sv_event_len(s, zi, ev[i], &k); inst[i] = k == DUR_INSTANT; }
            for (int i = 1; i < n; i++) /* by start, the instant ones first when equal */
                for (int j = i; j > 0 && (ev[j]->start < ev[j - 1]->start - 1e-5f || (fabsf(ev[j]->start - ev[j - 1]->start) <= 1e-5f && inst[j] && !inst[j - 1])); j--) {
                    ScriptAction *ta = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = ta;
                    int ti = inst[j]; inst[j] = inst[j - 1]; inst[j - 1] = ti;
                    float tl = len[j]; len[j] = len[j - 1]; len[j - 1] = tl;
                }
            for (int i = 1; i < n; i++) {
                float min = (inst[i] && inst[i - 1]) ? ev[i - 1]->start + 0.05f : ev[i - 1]->start + (inst[i - 1] ? 0.0f : len[i - 1]);
                if (ev[i]->start < min - 1e-4f) ev[i]->start = roundf(min * 1000.0f) / 1000.0f;
            }
        }
    }
}
/* where the group is: -1 plain rows, else its timeline; -2 empty */
static int sv_sel_kind(Script *s) {
    int r;
    if (!g_sv_sel_n || !script_find(s, g_sv_sel_ids[0], &r, NULL)) return -2;
    return script_zone_at(s, r);
}
static void sv_sel_one(Script *s, int r, int c) {
    const ScriptAction *a = script_at(s, r, c);
    g_sv_sel_n = 0;
    if (a && a->type != ACT_NONE) g_sv_sel_ids[g_sv_sel_n++] = a->id;
}
static void sv_sel_add(Script *s, int r, int c) {
    const ScriptAction *a = script_at(s, r, c);
    if (!a || a->type == ACT_NONE || sv_sel_has(a->id)) return;
    if (g_sv_sel_n && sv_sel_kind(s) != script_zone_at(s, r)) g_sv_sel_n = 0; /* never rows and a timeline together */
    if (g_sv_sel_n < SV_MAX_SEL) g_sv_sel_ids[g_sv_sel_n++] = a->id;
}
static void sv_sel_toggle(Script *s, int r, int c) {
    const ScriptAction *a = script_at(s, r, c);
    if (!a || a->type == ACT_NONE) return;
    for (int i = 0; i < g_sv_sel_n; i++) if (g_sv_sel_ids[i] == a->id) { g_sv_sel_ids[i] = g_sv_sel_ids[--g_sv_sel_n]; return; }
    sv_sel_add(s, r, c);
}
/* every action of the plain rows lo..hi */
static void sv_sel_rows(Script *s, int lo, int hi) {
    g_sv_sel_n = 0;
    for (int r = lo; r <= hi && r < s->rows; r++) {
        if (script_zone_at(s, r) >= 0) continue;
        for (int c = 0; c < s->cols; c++) sv_sel_add(s, r, c);
    }
}
/* the keyboard moved the selection: the group follows (Shift: the rows) */
static void sv_sel_after_nav(void) {
    Script *s = sv_cur();
    if (!s) return;
    if (GetKeyState(VK_SHIFT) & 0x8000) { int lo, hi; sv_sel_range(&lo, &hi); sv_sel_rows(s, lo, hi); }
    else sv_sel_one(s, g_sv_row, g_sv_col);
}
/* plain rows: can the group move by (dr, dc) cells -- inside the grid, not into a timeline,
   onto empty cells (or its own)? apply: do it */
static int sv_group_move_cells(Script *s, int dr, int dc, int apply) {
    static ScriptAction buf[SV_MAX_SEL];
    int nr[SV_MAX_SEL], nc[SV_MAX_SEL], n = 0, maxr = 0;
    for (int i = 0; i < g_sv_sel_n; i++) {
        int r, c;
        if (!script_find(s, g_sv_sel_ids[i], &r, &c)) continue;
        int tr = r + dr, tc = c + dc;
        if (tr < 0 || tc < 0 || tc >= s->cols || script_zone_at(s, tr) >= 0) return 0;
        const ScriptAction *d = script_at(s, tr, tc);
        if (d && d->type != ACT_NONE && !sv_sel_has(d->id)) return 0;
        nr[n] = tr; nc[n] = tc; n++;
        if (tr > maxr) maxr = tr;
    }
    if (!apply || n == 0) return n > 0;
    for (int i = 0, k = 0; i < g_sv_sel_n; i++) { /* lifted, then put down (the grid may grow) */
        ScriptAction *a = script_find(s, g_sv_sel_ids[i], NULL, NULL);
        if (!a) continue;
        buf[k++] = *a; memset(a, 0, sizeof(*a));
    }
    script_ensure_rows(s, maxr + 1);
    for (int k = 0; k < n; k++) *script_at(s, nr[k], nc[k]) = buf[k];
    g_sv_row += dr; g_sv_col += dc; g_sv_anchor = g_sv_row;
    if (g_sv_row < 0) g_sv_row = 0;
    if (g_sv_col < 0) g_sv_col = 0;
    if (g_sv_col >= s->cols) g_sv_col = s->cols - 1;
    return 1;
}
/* a timeline group: each one to its lane when the drag began + dc (a free cell there); 0 (nothing moved) if one can't */
static int sv_zone_group_lanes(Script *s, int zi, int dc) {
    static ScriptAction buf[SV_MAX_SEL];
    int fr[SV_MAX_SEL], fc[SV_MAX_SEL], pr[SV_MAX_SEL], n = g_sv_sel_n;
    const ScriptZone *z = &s->zone[zi];
    for (int i = 0; i < n; i++) {
        int L = g_sv_glane0[i] + dc;
        if (L < 0 || L >= s->cols || !script_find(s, g_sv_sel_ids[i], NULL, NULL)) return 0;
    }
    for (int i = 0; i < n; i++) { ScriptAction *a = script_find(s, g_sv_sel_ids[i], &fr[i], &fc[i]); buf[i] = *a; memset(a, 0, sizeof(*a)); }
    int ok = 1, placed = 0;
    for (int i = 0; i < n && ok; i++) {
        int L = g_sv_glane0[i] + dc, row = -1;
        for (int r = z->row0; r < z->row0 + z->rows && r < s->rows && row < 0; r++) if (script_at(s, r, L)->type == ACT_NONE) row = r;
        if (row < 0) { ok = 0; break; }
        *script_at(s, row, L) = buf[i]; pr[i] = row; placed++;
    }
    if (!ok) { /* back where they were */
        for (int i = 0; i < placed; i++) memset(script_at(s, pr[i], g_sv_glane0[i] + dc), 0, sizeof(ScriptAction));
        for (int i = 0; i < n; i++) *script_at(s, fr[i], fc[i]) = buf[i];
        return 0;
    }
    return 1;
}
/* the group's starts: each one's when the drag began + dt */
static void sv_group_shift(Script *s, float dt) {
    for (int i = 0; i < g_sv_sel_n; i++) {
        ScriptAction *b = script_find(s, g_sv_sel_ids[i], NULL, NULL);
        if (!b) continue;
        float t = roundf((g_sv_gt0[i] + dt) * 1000.0f) / 1000.0f;
        if (fabsf(t - b->start) > 1e-4f) { b->start = t; g_sv_drag_moved = 1; }
    }
}
/* dragging a timeline group: up / down = their starts (0.05 s steps, Shift: 0.01; none before 0), sideways = other lanes */
static void sv_drag_move(int x, int y) {
    Script *s = sv_cur();
    int r, c;
    ScriptAction *a = s ? script_find(s, g_sv_drag_id, &r, NULL) : NULL;
    int zi = a ? script_zone_at(s, r) : -1;
    if (zi < 0) { g_sv_drag = 0; return; }
    float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.01f : 0.05f;
    float dt = (float)(y - g_sv_drag_y0) * s->zone[zi].sec_per_row / SV_ROW_H;
    dt = roundf(dt / step) * step;
    float mn = 1e9f;
    for (int i = 0; i < g_sv_sel_n; i++) if (g_sv_gt0[i] < mn) mn = g_sv_gt0[i];
    if (mn + dt < 0.0f) dt = -mn;
    sv_group_shift(s, dt);
    if (!g_sv_drag_free && sv_ids_collide(s, zi, g_sv_sel_ids, g_sv_sel_n)) { /* as far as it goes toward there without going into a neighbour */
        float best = g_sv_last_dt, dir = dt > g_sv_last_dt ? step : -step;
        for (float tt = g_sv_last_dt + dir; dir > 0 ? tt <= dt + 1e-4f : tt >= dt - 1e-4f; tt += dir) {
            if (mn + tt < -1e-4f) break;
            sv_group_shift(s, tt);
            if (sv_ids_collide(s, zi, g_sv_sel_ids, g_sv_sel_n)) break;
            best = tt;
        }
        dt = best;
        sv_group_shift(s, dt);
    }
    g_sv_last_dt = dt;
    int cw = (g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W) / s->cols;
    c = (x - g_sv_grid_rc.left - SV_ROWHDR_W) / cw;
    if (c < 0) c = 0;
    if (c >= s->cols) c = s->cols - 1;
    int dc = c - g_sv_drag_c0;
    if (dc != g_sv_gdc && sv_zone_group_lanes(s, zi, dc)) {
        if (!g_sv_drag_free && sv_ids_collide(s, zi, g_sv_sel_ids, g_sv_sel_n)) sv_zone_group_lanes(s, zi, g_sv_gdc); /* no room there: back */
        else { g_sv_gdc = dc; g_sv_drag_moved = 1; }
    }
    if (script_find(s, g_sv_drag_id, &r, &c)) { g_sv_row = r; g_sv_col = c; g_sv_anchor = r; }
}
/* a rectangle drawn from an empty spot: the actions it touches (of plain rows, or of that timeline) */
static void sv_band_select(Script *s) {
    RECT band = { g_sv_band_x0 < g_sv_band_x1 ? g_sv_band_x0 : g_sv_band_x1, g_sv_band_y0 < g_sv_band_y1 ? g_sv_band_y0 : g_sv_band_y1,
                  g_sv_band_x0 > g_sv_band_x1 ? g_sv_band_x0 : g_sv_band_x1, g_sv_band_y0 > g_sv_band_y1 ? g_sv_band_y0 : g_sv_band_y1 };
    RECT tmp;
    g_sv_sel_n = 0;
    if (g_sv_band_zone >= 0) {
        const ScriptZone *z = &s->zone[g_sv_band_zone];
        for (int r = z->row0; r < z->row0 + z->rows && r < s->rows; r++)
            for (int c = 0; c < s->cols; c++) {
                if (script_at(s, r, c)->type == ACT_NONE) continue;
                RECT er; int k; float l;
                sv_event_geom(s, g_sv_band_zone, r, c, &er, &k, &l);
                if (k == DUR_INSTANT) InflateRect(&er, 0, 3);
                if (IntersectRect(&tmp, &er, &band)) sv_sel_add(s, r, c);
            }
        return;
    }
    int vis = sv_grid_rows_visible();
    for (int r = g_sv_scroll; r < g_sv_scroll + vis && r < s->rows; r++) {
        if (script_zone_at(s, r) >= 0) continue;
        for (int c = 0; c < s->cols; c++) {
            RECT cr; sv_cell_rect(r, c, &cr);
            if (IntersectRect(&tmp, &cr, &band)) sv_sel_add(s, r, c);
        }
    }
}

/* can a timeline action be made shorter / longer by its edges? (a sound, a line, an
   animation played N times: trimmed; a wait, a camera slide: their length) */
static int sv_resizable(const ScriptAction *a) {
    float f;
    return a->type == ACT_WAIT || a->type == ACT_CAMERA || action_media_length(a, &f);
}
/* the top / bottom edge of a timeline action under (x, y): 1 / 2 (*row, *col: the action), else 0 */
static int sv_event_edge_at(int x, int y, int *row, int *col) {
    Script *s = sv_cur();
    if (!s) return 0;
    POINT p = { x, y };
    for (int z = 0; z < s->nzones; z++)
        for (int r = s->zone[z].row0; r < s->zone[z].row0 + s->zone[z].rows && r < s->rows; r++)
            for (int c = 0; c < s->cols; c++) {
                const ScriptAction *a = script_at(s, r, c);
                if (a->type == ACT_NONE || !sv_resizable(a)) continue;
                RECT er; int k; float l;
                sv_event_geom(s, z, r, c, &er, &k, &l);
                if (k != DUR_KNOWN || x < er.left || x >= er.right) continue;
                (void)p;
                if (er.bottom - er.top >= 16 && y >= er.top - 2 && y <= er.top + 3) { *row = r; *col = c; return 1; }
                if (y >= er.bottom - 4 && y <= er.bottom + 2) { *row = r; *col = c; return 2; }
            }
    return 0;
}
/* an edge dragged: the time under the mouse becomes its start (top) or its end (bottom) */
static void sv_resize_apply(ScriptAction *a, float t);
static void sv_resize_move(int y) {
    Script *s = sv_cur();
    ScriptAction *a = sv_cell(0);
    int zi = s ? script_zone_at(s, g_sv_row) : -1;
    if (!a || a->type == ACT_NONE || zi < 0) { g_sv_rsz = 0; return; }
    float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 0.01f : 0.05f;
    float t = (float)(y - sv_zone_y(&s->zone[zi])) * s->zone[zi].sec_per_row / SV_ROW_H;
    t = roundf(t / step) * step;
    if (t < 0.0f) t = 0.0f;
    sv_resize_apply(a, t);
    if (!g_sv_drag_free && sv_ids_collide(s, zi, &a->id, 1)) { /* up to the neighbour, not into it */
        float best = g_sv_rsz_last, dir = t > g_sv_rsz_last ? step : -step;
        for (float tt = g_sv_rsz_last + dir; dir > 0 ? tt <= t + 1e-4f : tt >= t - 1e-4f; tt += dir) {
            sv_resize_apply(a, tt);
            if (sv_ids_collide(s, zi, &a->id, 1)) break;
            best = tt;
        }
        t = best;
        sv_resize_apply(a, t);
    }
    g_sv_rsz_last = t;
    g_sv_drag_moved = 1;
}
/* the edge being dragged at time t: the action's start (top) or end (bottom) there */
static void sv_resize_apply(ScriptAction *a, float t) {
    float full, end = g_sv_rsz_start + g_sv_rsz_len;
    int media = action_media_length(a, &full);
    if (g_sv_rsz == 2) { /* its end */
        float len = t - a->start;
        if (len < 0.05f) len = 0.05f;
        if (media) {
            float out = full - a->trim_in - len;
            if (out < 0.0f) out = 0.0f;
            a->trim_out = out;
        } else a->seconds = len;
    } else { /* its start: a sound / animation starts further in it (or less), its end stays */
        if (t > end - 0.05f) t = end - 0.05f;
        if (media) {
            float in = g_sv_rsz_in + (t - g_sv_rsz_start);
            if (in < 0.0f) in = 0.0f;
            if (in > full - g_sv_rsz_out - 0.05f) in = full - g_sv_rsz_out - 0.05f;
            a->trim_in = in;
            a->start = g_sv_rsz_start + (in - g_sv_rsz_in);
        } else {
            a->start = t;
            a->seconds = end - t;
        }
    }
}

/* the bottom edge of a timeline under (x, y): its index, else -1 */
static int sv_zone_edge_at(int x, int y) {
    Script *s = sv_cur();
    if (!s || x < g_sv_grid_rc.left + SV_ROWHDR_W || x >= g_sv_grid_rc.right || y < g_sv_grid_rc.top + SV_HDR_H || y >= g_sv_grid_rc.bottom) return -1;
    for (int z = 0; z < s->nzones; z++) {
        int y1 = sv_zone_y(&s->zone[z]) + s->zone[z].rows * SV_ROW_H;
        if (y >= y1 - 5 && y <= y1 + 4) return z;
    }
    return -1;
}
/* the edge dragged: the zone as many rows tall as the mouse says (never less than its actions need) */
static void sv_zone_edge_move(int y) {
    Script *s = sv_cur();
    int z = g_sv_zdrag - 1;
    if (!s || z < 0 || z >= s->nzones) { g_sv_zdrag = 0; return; }
    int in_zone = script_zone_at(s, g_sv_row) == z;
    int want = (y - sv_zone_y(&s->zone[z]) + SV_ROW_H / 2) / SV_ROW_H;
    if (want < 1) want = 1;
    if (want != s->zone[z].rows) {
        script_zone_resize(s, z, want);
        if (in_zone && script_zone_at(s, g_sv_row) != z) { g_sv_row = s->zone[z].row0; g_sv_anchor = g_sv_row; }
        g_sv_drag_moved = 1;
    }
}
/* - Row / + Row, Ins / Delete row in a timeline */
static void sv_zone_rows(int delta) {
    Script *s = sv_cur();
    int z = s ? script_zone_at(s, g_sv_row) : -1;
    if (z < 0) return;
    int before = s->zone[z].rows, now = script_zone_resize(s, z, before + delta);
    if (now == before && delta < 0) { snprintf(g_status, sizeof(g_status), "this timeline can't be shorter: a lane needs all its rows for its actions"); return; }
    if (script_zone_at(s, g_sv_row) != z) { g_sv_row = s->zone[z].row0 + s->zone[z].rows - 1; g_sv_anchor = g_sv_row; }
    sv_changed();
}
static void sv_drag_end(void) {
    if (g_sv_cdrag) {
        g_sv_cdrag = 0; ReleaseCapture();
        Script *s = sv_cur();
        if (s && (g_sv_cdrag_dr || g_sv_cdrag_dc)) {
            if (sv_group_move_cells(s, g_sv_cdrag_dr, g_sv_cdrag_dc, 1)) sv_changed();
            else snprintf(g_status, sizeof(g_status), "can't move there: a cell is taken, outside the grid or in a timeline");
        }
        return;
    }
    if (g_sv_band) {
        Script *s = sv_cur();
        if (g_sv_band == 2 && s) sv_band_select(s);
        g_sv_band = 0; ReleaseCapture();
        return;
    }
    if (g_sv_zdrag) { g_sv_zdrag = 0; ReleaseCapture(); if (g_sv_drag_moved) sv_changed(); return; }
    if (g_sv_rsz) { g_sv_rsz = 0; ReleaseCapture(); if (g_sv_drag_moved) sv_changed(); return; }
    if (!g_sv_drag) return;
    g_sv_drag = 0;
    ReleaseCapture();
    if (g_sv_drag_moved) sv_changed();
}

static void sv_new_script(void) {
    Script *ns = (Script *)realloc(g_scripts, sizeof(Script) * (g_script_count + 1));
    if (!ns) return;
    g_scripts = ns;
    char name[64];
    for (int k = g_script_count + 1;; k++) { snprintf(name, sizeof(name), "Script %d", k); if (room_script_find(name) < 0) break; }
    script_init(&g_scripts[g_script_count], name);
    script_ensure_rows(&g_scripts[g_script_count], 1); /* the first block: the room's music, None until chosen */
    action_init(&g_scripts[g_script_count], script_at(&g_scripts[g_script_count], 0, 0), ACT_MUSIC);
    g_sv_script = g_script_count++;
    g_sv_row = g_sv_col = g_sv_scroll = 0;
    sv_changed();
    g_sv_text = 2; g_sv_text_buf[0] = 0; /* type its name right away */
}
static void sv_text_commit(void) {
    Script *s = sv_cur();
    int mode = g_sv_text;
    g_sv_text = 0;
    if (!s || mode == 0 || !g_sv_text_buf[0]) return;
    char name[64];
    snprintf(name, sizeof(name), "%s", g_sv_text_buf);
    int i = room_script_find(name);
    if (i >= 0 && i != g_sv_script) { snprintf(g_status, sizeof(g_status), "a script is already called \"%s\"", name); return; }
    snprintf(s->name, sizeof(s->name), "%s", name);
    sv_changed();
}
/* SPEAK for a character that has no portrait yet: choose it now */
static void sv_ask_portrait(ScriptAction *a) {
    CharModel *m = run_actor_model(sv_cur(), a->actor);
    if (m && !portrait_of(m->name)) { g_ch_model = m; ch_open(CH_PORTRAIT, NULL); }
}
static void sv_set_type(int type) {
    ScriptAction *a = sv_cell(1);
    if (!a) return;
    Script *s = sv_cur();
    float keep = a->start;
    action_init(s, a, type);
    if (script_zone_at(s, g_sv_row) >= 0) a->start = g_sv_pending_start >= 0.0f ? g_sv_pending_start : keep;
    g_sv_pending_start = -1.0f;
    if (type == ACT_STOP) { /* the last sound / ambience before it, if any */
        ScriptAction *list[64]; int rows[64];
        int n = sv_actions_of(s, ACT_SOUND, ACT_AMBIENCE, list, rows, 64);
        for (int k = 0; k < n; k++) if (rows[k] < g_sv_row) { a->stop_kind = STOP_ACTION; a->stop_ref = list[k]->id; }
    }
    if (type == ACT_OVERLAY) { /* every overlay of the room, already at its place: the ones of its own picture loop */
        char level[64], own[64];
        split_label(current_room_label(), level, sizeof(level), own, sizeof(own));
        for (int k = 0; k < g_ov_n && a->nov < SCRIPT_MAX_OVERLAYS; k++) {
            snprintf(a->ov_name[a->nov], sizeof(a->ov_name[0]), "%s", g_ov[k].name);
            a->ov_mode[a->nov] = (g_ov[k].frames > 1 && !strcmp(g_ov[k].bg, own)) ? OVM_LOOP : OVM_KEEP;
            a->nov++;
        }
        g_sv_ov_sel = 0;
    }
    sv_changed();
    if (type == ACT_SPEAK) sv_ask_portrait(a);
}

/* right-click / double-click on a cell */
static void sv_cell_menu(HWND hwnd, int client_x, int client_y) {
    Script *s = sv_cur();
    if (!s) return;
    ScriptAction *a = sv_cell(0);
    int empty = !a || a->type == ACT_NONE;
    HMENU m = CreatePopupMenu();
    char head[80];
    snprintf(head, sizeof(head), "Row %d, column %d", g_sv_row + 1, g_sv_col + 1);
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, head);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    for (int t = ACT_WAIT; t < ACT_COUNT; t++) {
        char lab[64]; snprintf(lab, sizeof(lab), "%s%s", empty ? "" : "Replace with: ", action_type_name(t));
        AppendMenuA(m, MF_STRING | (!empty && a->type == t ? MF_CHECKED : 0), 100 + t, lab);
    }
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | (empty ? MF_GRAYED : 0), 1, "Copy\tCtrl+C");
    AppendMenuA(m, MF_STRING | (g_sv_has_clip ? 0 : MF_GRAYED), 2, "Paste\tCtrl+V");
    AppendMenuA(m, MF_STRING | (empty ? MF_GRAYED : 0), 3, "Clear the cell\tDel");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, 4, "Insert a row here\tIns");
    AppendMenuA(m, MF_STRING | (g_sv_row < s->rows ? 0 : MF_GRAYED), 5, "Delete this row\tShift+Del");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    {
        int lo, hi; sv_sel_range(&lo, &hi);
        char zl[96];
        if (lo == hi) snprintf(zl, sizeof(zl), "Make row %d an advanced timeline", lo + 1);
        else snprintf(zl, sizeof(zl), "Make rows %d-%d an advanced timeline", lo + 1, hi + 1);
        AppendMenuA(m, MF_STRING | (sv_range_free(s, lo, hi) ? 0 : MF_GRAYED), 6, zl);
        AppendMenuA(m, MF_STRING | (script_zone_at(s, g_sv_row) >= 0 ? 0 : MF_GRAYED), 7, "Back to rows (end this timeline)");
    }
    POINT pt = { client_x, client_y }; ClientToScreen(hwnd, &pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd >= 100 + ACT_WAIT && cmd < 100 + ACT_COUNT) sv_set_type(cmd - 100);
    else if (cmd == 1) ui_action(hwnd, B_SV_COPY);
    else if (cmd == 2) ui_action(hwnd, B_SV_PASTE);
    else if (cmd == 3) ui_action(hwnd, B_SV_CLEAR);
    else if (cmd == 4) ui_action(hwnd, B_SV_ROW_INS);
    else if (cmd == 5) ui_action(hwnd, B_SV_ROW_DEL);
    else if (cmd == 6) ui_action(hwnd, B_SV_ZONE);
    else if (cmd == 7) ui_action(hwnd, B_SV_UNZONE);
}

/* Point picking in the room for PLACE / MOVE: the scripts screen hides
   until the click (step box in the side panel, green floor shown). */
static void sv_pick_start(int mode) {
    g_sv_pick = mode;
    g_script_view = 0;
    g_status[0] = 0;
}
static void sv_pick_end(void) { g_sv_pick = SV_PICK_NONE; g_script_view = 1; }
static void sv_pick_click(float rx, float ry, int right) {
    ScriptAction *a = sv_cell(1);
    if (!a) { sv_pick_end(); return; }
    if (g_sv_pick == SV_PICK_CAMERA) {
        if (!right) { a->pos[0] = rx; a->pos[1] = ry; a->has_pos = 1; a->cam_target = CAM_POINT; sv_changed(); }
        sv_pick_end();
        return;
    }
    if (g_sv_pick == SV_PICK_OVERLAY) {
        if (!right && g_ov_show >= 0 && g_ov_show < g_ov_n) {
            RoomOverlay *o = &g_ov[g_ov_show];
            o->x = (int)rx - o->w / 2; o->y = (int)ry - o->h / 2;
            snprintf(o->conf, sizeof(o->conf), "set");
            overlays_save();
        }
        g_ov_show = -1;
        sv_pick_end();
        return;
    }
    if (g_sv_pick == SV_PICK_PLACE_FACE) {
        float w[3];
        if (!right && pick_floor_point(rx, ry, w)) a->facing = atan2f(w[0] - a->pos[0], w[2] - a->pos[2]);
        sv_changed();
        sv_pick_end();
        return;
    }
    if (right) { sv_pick_end(); return; }
    if (g_sv_pick == SV_PICK_MOVE) {
        int di = door_at_pixel(rx, ry);
        if (di >= 0) {
            if (!g_shapes[di].has_step) { snprintf(g_status, sizeof(g_status), "that connector isn't set up (no doorstep)"); return; }
            a->door = g_shapes[di].id;
            sv_changed(); sv_pick_end();
            return;
        }
    }
    float w[3];
    if (!pick_floor_point(rx, ry, w)) { snprintf(g_status, sizeof(g_status), "no floor a character can stand on there -- click on the GREEN floor"); return; }
    memcpy(a->pos, w, sizeof(w));
    a->has_pos = 1;
    if (g_sv_pick == SV_PICK_MOVE) { a->door = 0; sv_changed(); sv_pick_end(); return; }
    sv_changed();
    g_sv_pick = SV_PICK_PLACE_FACE; /* then where it looks */
}

static void sv_model_chosen(const char *name) {
    ScriptAction *a = sv_cell(1);
    if (a && a->type == ACT_PLACE) { snprintf(a->model, sizeof(a->model), "%s", name); sv_changed(); }
    g_script_view = 1;
}

static void sv_play(int from_row) {
    Script *s = sv_cur();
    if (!s) return;
    if (script_used_rows(s) == 0) { snprintf(g_status, sizeof(g_status), "this script is empty"); return; }
    /* a clean start: characters, picture and sounds of a previous test are gone */
    actors_clear_npcs();
    background_set("");
    audio_stop_kind(AUDIO_SOUND); audio_stop_kind(AUDIO_AMBIENCE); audio_music_stop();
    overlays_reset();
    ch_close();
    g_script_view = 0; g_sv_text = 0;
    script_start(s, from_row);
}

/* the characters a MOVE can pick: David + the script's PLACE actions */
static int sv_actor_list(Script *s, int *ids, int *rows, int max) {
    ScriptAction *pl[64]; int pr[64];
    int n = sv_actions_of(s, ACT_PLACE, ACT_PLACE, pl, pr, 64), k = 0;
    ids[k] = 0; rows[k] = -1; k++;
    for (int i = 0; i < n && k < max; i++) { ids[k] = pl[i]->id; rows[k] = pr[i]; k++; }
    return k;
}

/* an action of a timeline, just changed: does it now go into another one of its lane? */
static int sv_changed_would_overlap(Script *s, ScriptAction *a) {
    int r, zi;
    if (!s || !script_find(s, a->id, &r, NULL) || (zi = script_zone_at(s, r)) < 0) return 0;
    return sv_ids_collide(s, zi, &a->id, 1);
}
static int sv_item_menu(HWND hwnd, int shield, char *out, int n);
static void sv_action(HWND hwnd, int id) {
    Script *s = sv_cur();
    ScriptAction *a = sv_cell(0);
    int has = a && a->type != ACT_NONE;
    switch (id) {
        case B_SV_CLOSE: script_view_toggle(); return;
        case B_SV_NEW: sv_new_script(); return;
        case B_SV_RENAME: if (s && strcmp(s->name, DEFAULT_SCRIPT)) { g_sv_text = 2; snprintf(g_sv_text_buf, sizeof(g_sv_text_buf), "%s", s->name); } return;
        case B_SV_DUP:
            if (s) {
                Script *ns = (Script *)realloc(g_scripts, sizeof(Script) * (g_script_count + 1));
                if (!ns) return;
                g_scripts = ns; s = &g_scripts[g_sv_script];
                script_copy(&g_scripts[g_script_count], s);
                char name[64];
                for (int k = 1;; k++) { snprintf(name, sizeof(name), k == 1 ? "%.50s copy" : "%.50s copy %d", s->name, k); if (room_script_find(name) < 0) break; }
                snprintf(g_scripts[g_script_count].name, sizeof(name), "%s", name);
                g_scripts[g_script_count].auto_run = 0;
                g_sv_script = g_script_count++;
                sv_changed();
            }
            return;
        case B_SV_DELETE:
            if (s && strcmp(s->name, DEFAULT_SCRIPT)) {
                char q[200]; snprintf(q, sizeof(q), "Delete the script \"%s\"?", s->name);
                if (MessageBoxA(hwnd, q, "Silver Remaster -- scripts", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
                script_free(s);
                memmove(&g_scripts[g_sv_script], &g_scripts[g_sv_script + 1], sizeof(Script) * (g_script_count - g_sv_script - 1));
                g_script_count--;
                if (g_sv_script >= g_script_count) g_sv_script = g_script_count - 1;
                if (g_sv_script < 0) g_sv_script = 0;
                g_sv_row = g_sv_col = g_sv_scroll = 0;
                sv_changed();
            }
            return;
        case B_SV_AUTO: if (s) { s->auto_run = !s->auto_run; sv_changed(); } return;
        case B_SV_PLAY: sv_play(0); return;
        case B_SV_PLAY_ROW: sv_play(g_sv_row); return;
        case B_SV_ZONE:
            if (s) {
                int lo, hi; sv_sel_range(&lo, &hi);
                if (script_zone_make(s, lo, hi - lo + 1) < 0) { snprintf(g_status, sizeof(g_status), "those rows overlap a timeline already"); return; }
                g_sv_anchor = g_sv_row;
                snprintf(g_status, sizeof(g_status), "rows %d-%d are an advanced timeline: drag its actions to set when they start", lo + 1, hi + 1);
                sv_changed();
            }
            return;
        case B_SV_ZROWS_M: sv_zone_rows(-1); return;
        case B_SV_ZROWS_P: sv_zone_rows(1); return;
        case B_SV_UNZONE: if (s && script_zone_at(s, g_sv_row) >= 0) { script_zone_remove(s, script_zone_at(s, g_sv_row)); sv_changed(); } return;
        case B_SV_ZSCALE_M: case B_SV_ZSCALE_P: {
            int zi = s ? script_zone_at(s, g_sv_row) : -1;
            if (zi < 0) return;
            static const float steps[6] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
            int k = 0;
            while (k < 5 && steps[k] < s->zone[zi].sec_per_row - 1e-4f) k++;
            k += id == B_SV_ZSCALE_P ? 1 : -1;
            if (k < 0) k = 0;
            if (k > 5) k = 5;
            s->zone[zi].sec_per_row = steps[k];
            sv_changed();
            return;
        }
        case B_SV_ROW_INS:
            if (s && script_zone_at(s, g_sv_row) >= 0) { sv_zone_rows(1); return; } /* in a timeline: it grows */
            if (s) { script_insert_row(s, g_sv_row); sv_changed(); }
            return;
        case B_SV_ROW_DEL:
            if (s && script_zone_at(s, g_sv_row) >= 0) { sv_zone_rows(-1); return; }
            if (s && g_sv_row < s->rows) { script_delete_row(s, g_sv_row); sv_changed(); }
            return;
        case B_SV_COL_ADD: if (s) { script_add_col(s); sv_changed(); } return;
        case B_SV_COL_DEL:
            if (s && s->cols > 1) {
                int any = 0;
                for (int r = 0; r < s->rows; r++) if (script_at(s, r, g_sv_col)->type != ACT_NONE) any = 1;
                if (any && MessageBoxA(hwnd, "Delete this column and the actions in it?", "Silver Remaster -- scripts", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
                script_delete_col(s, g_sv_col);
                if (g_sv_col >= s->cols) g_sv_col = s->cols - 1;
                sv_changed();
            }
            return;
        case B_SV_COPY: if (has) { g_sv_clip = *a; g_sv_has_clip = 1; snprintf(g_status, sizeof(g_status), "action copied"); } return;
        case B_SV_PASTE:
            if (g_sv_has_clip && s) { a = sv_cell(1); int nid = s->next_id++; *a = g_sv_clip; a->id = nid; sv_changed(); }
            return;
        case B_SV_CLEAR:
            if (s && g_sv_sel_n > 1) {
                char q[120]; snprintf(q, sizeof(q), "Clear the %d selected actions?", g_sv_sel_n);
                if (MessageBoxA(hwnd, q, "Silver Remaster -- scripts", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
                for (int i = 0; i < g_sv_sel_n; i++) { ScriptAction *b = script_find(s, g_sv_sel_ids[i], NULL, NULL); if (b) memset(b, 0, sizeof(*b)); }
                g_sv_sel_n = 0;
                sv_changed();
                return;
            }
            if (has) { memset(a, 0, sizeof(*a)); sv_changed(); }
            return;
        case B_SV_TYPE: { RECT r; sv_cell_rect(g_sv_row, g_sv_col, &r); sv_cell_menu(hwnd, r.left + 10, r.bottom); return; }
        case B_CH_OK: ch_choose(); return;
        case B_CH_CANCEL: ch_close(); return;
        case B_CH_LISTEN: if (audio_preview_playing()) audio_preview(NULL); else audio_preview(ch_current()); return;
        case B_CH_SCOPE0: case B_CH_SCOPE1: case B_CH_SCOPE2: g_ch_scope = id - B_CH_SCOPE0; g_ch_sel = 0; ch_filter(); return;
    }
    if (id >= B_SV_ADD && id < B_SV_ADD + ACT_COUNT) { sv_set_type(id - B_SV_ADD); return; }
    if (!has) return;
    switch (id) {
        case B_SV_W_M1: a->seconds -= 1.0f; break;
        case B_SV_W_M01: a->seconds -= 0.1f; break;
        case B_SV_W_P01: a->seconds += 0.1f; break;
        case B_SV_W_P1: a->seconds += 1.0f; break;
        case B_SV_BG_CHOOSE: ch_open(CH_PICTURE, a->file); return;
        case B_SV_BG_OWN: a->file[0] = 0; break;
        case B_SV_MU_ADD: ch_open(CH_TRACK, NULL); return;
        case B_SV_FILE: ch_open(CH_SOUND, a->file); return;
        case B_SV_LISTEN: if (audio_preview_playing()) audio_preview(NULL); else audio_preview(a->file); return;
        case B_SV_REP_M: if (a->repeat > (a->type == ACT_ANIM ? 1 : 0)) a->repeat--; break;
        case B_SV_AN_TIMES: a->anim_mode = ANIM_TIMES; if (a->repeat < 1) a->repeat = 1; break;
        case B_SV_AN_LOOP: a->anim_mode = ANIM_LOOP; break;
        case B_SV_AN_FOLLOW: a->speed = a->speed > 0.0f ? 0.0f : 1.0f; break;
        case B_SV_AN_FREEZE: a->freeze = !a->freeze; break;
        case B_SV_TRIM_IN_M: case B_SV_TRIM_IN_P: case B_SV_TRIM_OUT_M: case B_SV_TRIM_OUT_P: {
            float full;
            if (!action_media_length(a, &full)) return;
            float *v = (id == B_SV_TRIM_IN_M || id == B_SV_TRIM_IN_P) ? &a->trim_in : &a->trim_out;
            float d = (id == B_SV_TRIM_IN_P || id == B_SV_TRIM_OUT_P) ? 0.05f : -0.05f;
            float other = v == &a->trim_in ? a->trim_out : a->trim_in;
            float old = *v;
            *v += d;
            if (*v < 0.0f) *v = 0.0f;
            if (*v > full - other - 0.05f) *v = full - other - 0.05f;
            *v = roundf(*v * 100.0f) / 100.0f;
            if (d > 0.0f || !sv_changed_would_overlap(s, a)) break;
            *v = old; /* longer: it would go into the next action of its lane */
            snprintf(g_status, sizeof(g_status), "no room: it would go into another action of its lane");
            return;
        }
        case B_SV_TRIM_RESET: a->trim_in = a->trim_out = 0.0f; break;
        case B_SV_POOL_ADD:
            if (a->type == ACT_ANIM) { g_ch_model = run_actor_model(s, a->actor); if (!g_ch_model) return; ch_open(CH_CLIP, NULL); }
            else ch_open(CH_SOUND, NULL);
            g_ch_to_pool = 1;
            return;
        case B_SV_CS_WPN + 1: case B_SV_CS_SHD + 1: {
            char pick[48];
            if (!sv_item_menu(hwnd, id == B_SV_CS_SHD + 1, pick, sizeof(pick))) return;
            if (id == B_SV_CS_WPN + 1) { a->set_weapon = 1; snprintf(a->item, sizeof(a->item), "%s", pick); }
            else { a->set_shield = 1; snprintf(a->item2, sizeof(a->item2), "%s", pick); }
            break;
        }
        case B_SV_CS_WPN: a->set_weapon = 0; break;
        case B_SV_CS_SHD: a->set_shield = 0; break;
        case B_SV_CS_SIDE: case B_SV_CS_SIDE + 1: case B_SV_CS_SIDE + 2: a->set_side = id - B_SV_CS_SIDE; break;
        case B_SV_CS_AI: case B_SV_CS_AI + 1: case B_SV_CS_AI + 2: a->set_ai = id - B_SV_CS_AI; break;
        case B_SV_PL_SIDE: case B_SV_PL_SIDE + 1: a->side = id - B_SV_PL_SIDE; break;
        case B_SV_PL_AI: a->ai_off = !a->ai_off; break;
        case B_SV_T_M1: case B_SV_T_M01: case B_SV_T_P01: case B_SV_T_P1: {
            static const float d[4] = { -0.1f, -0.01f, 0.01f, 0.1f };
            float old = a->start;
            a->start += d[id - B_SV_T_M1];
            if (a->start < 0.0f) a->start = 0.0f;
            a->start = roundf(a->start * 100.0f) / 100.0f;
            if (sv_changed_would_overlap(s, a)) {
                a->start = old;
                snprintf(g_status, sizeof(g_status), "no room: it would go into another action of its lane");
                return;
            }
            break;
        }
        case B_SV_FADE_M: case B_SV_FADE_P:
            a->fade += id == B_SV_FADE_P ? 0.5f : -0.5f;
            if (a->fade < 0.01f) a->fade = 0.0f;
            if (a->fade > 10.0f) a->fade = 10.0f;
            break;
        case B_SV_AN_SPD_M: case B_SV_AN_SPD_P:
            a->speed += id == B_SV_AN_SPD_P ? 0.1f : -0.1f;
            if (a->speed < 0.1f) a->speed = 0.1f;
            if (a->speed > 4.0f) a->speed = 4.0f;
            break;
        case B_SV_AN_NORMAL: a->anim_mode = ANIM_NORMAL; break;
        case B_SV_RM_ROOM: ch_open(CH_ROOM, a->file); return;
        case B_SV_CAM_KEEP: a->cam_target = CAM_KEEP; break;
        case B_SV_CAM_DAVID: a->cam_target = CAM_DAVID; break;
        case B_SV_CAM_POINT: if (!a->has_pos) { sv_pick_start(SV_PICK_CAMERA); return; } a->cam_target = CAM_POINT; break;
        case B_SV_CAM_PICK: sv_pick_start(SV_PICK_CAMERA); return;
        case B_SV_CAM_ZM: a->zoom = a->zoom - 0.1f < 1.0f ? 0.0f : roundf((a->zoom - 0.1f) * 10.0f) / 10.0f; break;
        case B_SV_CAM_ZP: a->zoom = a->zoom <= 0.0f ? 1.1f : roundf((a->zoom + 0.1f) * 10.0f) / 10.0f; if (a->zoom > 3.0f) a->zoom = 3.0f; break;
        case B_SV_CAM_ZROOM: a->zoom = 0.0f; break;
        case B_SV_CAM_SM: a->seconds = a->seconds - 0.5f < 0.0f ? 0.0f : a->seconds - 0.5f; break;
        case B_SV_CAM_SP: a->seconds = a->seconds + 0.5f > 10.0f ? 10.0f : a->seconds + 0.5f; break;
        case B_SV_OV_WAIT: a->wait_end = !a->wait_end; break;
        case B_SV_OV_RELOAD:
            for (int k = 0; k < g_ov_n && a->nov < SCRIPT_MAX_OVERLAYS; k++) {
                int listed = 0;
                for (int i = 0; i < a->nov; i++) if (!strcmp(a->ov_name[i], g_ov[k].name)) listed = 1;
                if (listed) continue;
                snprintf(a->ov_name[a->nov], sizeof(a->ov_name[0]), "%s", g_ov[k].name);
                a->ov_mode[a->nov] = OVM_KEEP; a->ov_frame[a->nov] = 0;
                a->nov++;
            }
            break;
        case B_SV_OV_LEFT: case B_SV_OV_RIGHT: case B_SV_OV_UP: case B_SV_OV_DOWN: case B_SV_OV_PICK: {
            int k = g_sv_ov_sel < a->nov ? overlay_find(a->ov_name[g_sv_ov_sel]) : -1;
            if (k < 0) return;
            if (id == B_SV_OV_PICK) { g_ov_show = k; sv_pick_start(SV_PICK_OVERLAY); return; }
            int st = (GetKeyState(VK_SHIFT) & 0x8000) ? 8 : 1;
            RoomOverlay *o = &g_ov[k];
            if (id == B_SV_OV_LEFT) o->x -= st;
            if (id == B_SV_OV_RIGHT) o->x += st;
            if (id == B_SV_OV_UP) o->y -= st;
            if (id == B_SV_OV_DOWN) o->y += st;
            snprintf(o->conf, sizeof(o->conf), "set");
            overlays_save();
            return;
        }
        case B_SV_AN_ROW: a->anim_mode = ANIM_ROW; break;
        case B_SV_AN_CLIP: g_ch_model = run_actor_model(s, a->actor); if (g_ch_model) ch_open(CH_CLIP, a->file); return;
        case B_SV_PORTRAIT: g_ch_model = run_actor_model(s, a->actor); if (g_ch_model) ch_open(CH_PORTRAIT, NULL); return;
        case B_SV_REP_P: if (a->repeat < 99) a->repeat++; break;
        case B_SV_WAITEND: a->wait_end = !a->wait_end; break;
        case B_SV_LOOP: a->loop = !a->loop; break;
        case B_SV_ST_ALL: a->stop_kind = STOP_ALL_SOUNDS; break;
        case B_SV_ST_MUSIC: a->stop_kind = STOP_MUSIC; break;
        case B_SV_PL_MODEL: g_pick_for_script = 1; g_anim_view = 1; picker_open(); return;
        case B_SV_PL_POS: sv_pick_start(SV_PICK_PLACE_POS); return;
        case B_SV_FACE_L: a->facing = wrap_angle(a->facing + 0.785398f); break;
        case B_SV_FACE_R: a->facing = wrap_angle(a->facing - 0.785398f); break;
        case B_SV_MV_DEST: sv_pick_start(SV_PICK_MOVE); return;
        case B_SV_MV_WALK: a->run = 0; break;
        case B_SV_MV_RUN: a->run = 1; break;
        case B_SV_MOVESET: {
            const char *model = "david";
            if (a->type == ACT_PLACE) model = a->model;
            else if (a->actor) { ScriptAction *p = script_find(s, a->actor, NULL, NULL); if (p) model = p->model; }
            CharModel *m = model_by_name(model);
            if (m) ms_open(m); else snprintf(g_status, sizeof(g_status), "choose the character first");
            return;
        }
        default:
            if (id >= B_SV_TRACK && id < B_SV_TRACK + AUDIO_MAX_TRACKS * 8 && a->type == ACT_MUSIC) {
                int t = (id - B_SV_TRACK) / 8, op = (id - B_SV_TRACK) % 8;
                if (t >= a->ntracks) return;
                if (op == 0) { if (audio_preview_playing()) audio_preview(NULL); else audio_preview(a->track[t]); return; }
                if (op == 1) a->track_loop[t] = !a->track_loop[t];
                if ((op == 2 && t > 0) || (op == 3 && t < a->ntracks - 1)) {
                    int u = op == 2 ? t - 1 : t + 1;
                    char tmp[64]; int tl = a->track_loop[t];
                    memcpy(tmp, a->track[t], 64); memcpy(a->track[t], a->track[u], 64); memcpy(a->track[u], tmp, 64);
                    a->track_loop[t] = a->track_loop[u]; a->track_loop[u] = tl;
                }
                if (op == 4) {
                    memmove(&a->track[t], &a->track[t + 1], sizeof(a->track[0]) * (a->ntracks - t - 1));
                    memmove(&a->track_loop[t], &a->track_loop[t + 1], sizeof(int) * (a->ntracks - t - 1));
                    a->ntracks--;
                }
            } else if (id >= B_SV_OVROW && id < B_SV_OVROW + SCRIPT_MAX_OVERLAYS * 8 && a->type == ACT_OVERLAY) {
                int i = (id - B_SV_OVROW) / 8, op = (id - B_SV_OVROW) % 8;
                if (i >= a->nov) return;
                g_sv_ov_sel = i;
                int k = overlay_find(a->ov_name[i]), frames = k >= 0 ? g_ov[k].frames : 1;
                int st = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
                if (op == 1) {
                    HMENU m = CreatePopupMenu();
                    static const int order[OVM_COUNT] = { OVM_LOOP, OVM_ONCE, OVM_ONCE_HIDE, OVM_FREEZE, OVM_HIDE, OVM_KEEP };
                    static const char *label[OVM_COUNT] = { "Loop\tplays over and over", "Once\tplays once, stays on its last frame",
                        "Once, then hidden\tplays once, then disappears", "Frozen on a frame\tshows one frame (choose it below)",
                        "Hidden\tnot shown", "Unchanged\tstays as it is (not touched by this action)" };
                    for (int q = 0; q < OVM_COUNT; q++) AppendMenuA(m, MF_STRING | (a->ov_mode[i] == order[q] ? MF_CHECKED : 0), 1 + order[q], label[q]);
                    POINT pt; GetCursorPos(&pt);
                    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
                    DestroyMenu(m);
                    if (cmd <= 0) return;
                    a->ov_mode[i] = cmd - 1;
                }
                if (op == 2) a->ov_frame[i] = a->ov_frame[i] - st < 0 ? 0 : a->ov_frame[i] - st;
                if (op == 3) a->ov_frame[i] = a->ov_frame[i] + st > frames - 1 ? frames - 1 : a->ov_frame[i] + st;
                if (op == 0) return;
            } else if (id >= B_SV_RM_DOOR && id < B_SV_RM_DOOR + 25 && a->type == ACT_ROOM) {
                int k = id - B_SV_RM_DOOR;
                a->door = (k == 0 || k - 1 >= g_rinfo.nd) ? 0 : g_rinfo.door_id[k - 1];
            } else if (id >= B_SV_RM_SCRIPT && id < B_SV_RM_SCRIPT + 25 && a->type == ACT_ROOM) {
                int k = id - B_SV_RM_SCRIPT;
                if (k == 0 || k - 1 >= g_rinfo.ns) a->script[0] = 0; else snprintf(a->script, sizeof(a->script), "%s", g_rinfo.script[k - 1]);
            } else if (id >= B_SV_STOPREF && id < B_SV_STOPREF + 64 && a->type == ACT_STOP) {
                ScriptAction *list[64]; int rows[64];
                int n = sv_actions_of(s, ACT_SOUND, ACT_AMBIENCE, list, rows, 64);
                int k = id - B_SV_STOPREF;
                if (k < n) { a->stop_kind = STOP_ACTION; a->stop_ref = list[k]->id; }
            } else if (id >= B_SV_POOL && id < B_SV_POOL + SCRIPT_POOL_MAX && (a->type == ACT_ANIM || a->type == ACT_SOUND)) {
                int i = id - B_SV_POOL;
                if (i < a->npool) { memmove(a->pool[i], a->pool[i + 1], sizeof(a->pool[0]) * (size_t)(a->npool - i - 1)); a->npool--; }
            } else if (id >= B_SV_ACTOR && id < B_SV_ACTOR + 64 && (a->type == ACT_MOVE || a->type == ACT_ANIM || a->type == ACT_SPEAK || a->type == ACT_CHAR)) {
                int ids[64], rows[64];
                int n = sv_actor_list(s, ids, rows, 64), k = id - B_SV_ACTOR;
                if (k < n && a->actor != ids[k]) {
                    a->actor = ids[k];
                    if (a->type == ACT_ANIM) a->file[0] = 0; /* another skeleton */
                    sv_changed();
                    if (a->type == ACT_SPEAK) sv_ask_portrait(a);
                    return;
                }
            } else return;
    }
    if (a->type == ACT_WAIT && 0) {}
    if (a->type == ACT_WAIT) { a->seconds = roundf(a->seconds * 10.0f) / 10.0f; if (a->seconds < 0.0f) a->seconds = 0.0f; if (a->seconds > 3600.0f) a->seconds = 3600.0f; }
    sv_changed();
}

static void sv_layout_main(HWND hwnd);
/* The screen's buttons and labels; with a chooser open, only its buttons. */
static void sv_layout(HWND hwnd) {
    sv_layout_main(hwnd);
    if (g_ch == CH_NONE) return;
    RECT rc; GetClientRect(hwnd, &rc);
    g_btn_count = 0;
    SetRect(&g_ch_rc, g_sv_grid_rc.left, 44, rc.right - 10, rc.bottom - 10);
    int pic = g_ch == CH_PICTURE, wide = pic || g_ch == CH_CLIP;
    int lw = wide ? (g_ch_rc.right - g_ch_rc.left) * 45 / 100 : (g_ch_rc.right - g_ch_rc.left) - 20;
    if (!wide && !ch_grid() && lw > 700) lw = 700;
    SetRect(&g_ch_list_rc, g_ch_rc.left + 10, g_ch_rc.top + 70, g_ch_rc.left + 10 + lw, g_ch_rc.bottom - 50);
    SetRect(&g_ch_prev_rc, g_ch_list_rc.right + 16, g_ch_list_rc.top, g_ch_rc.right - 10, g_ch_list_rc.bottom);
    int bx = g_ch_rc.left + 10, by = g_ch_rc.bottom - 40;
    ui_add(B_CH_OK, bx, by, 120, 28, "Choose", "Enter", "Use the selected file.", 0, g_ch_n > 0, 0); bx += 126;
    ui_add(B_CH_CANCEL, bx, by, 100, 28, "Cancel", "Esc", "Back without changing anything.", 0, 1, 0); bx += 106;
    if (g_ch == CH_SOUND || g_ch == CH_TRACK) ui_add(B_CH_LISTEN, bx, by, 150, 28, audio_preview_playing() ? "Stop listening" : "Listen", "Space", "Hear the selected file (again: stop).", audio_preview_playing(), g_ch_n > 0, 0);
    else if (pic) {
        ui_add(B_CH_SCOPE0, bx, by, 150, 28, "This room's folder", "", "The pictures of this room's folder (its picture, its animation frames).", g_ch_scope == 0, 1, 3); bx += 156;
        ui_add(B_CH_SCOPE1, bx, by, 150, 28, "Every room's picture", "", "The main picture of every room of the game.", g_ch_scope == 1, 1, 3); bx += 156;
        ui_add(B_CH_SCOPE2, bx, by, 150, 28, "This whole level", "", "Every picture of this room's level (many files).", g_ch_scope == 2, 1, 3);
    }
}
/* Change room: the target room's connectors and scripts (read once per room) */
static void room_info(const char *label) {
    if (!strcmp(g_rinfo.label, label)) return;
    memset(&g_rinfo, 0, sizeof(g_rinfo));
    snprintf(g_rinfo.label, sizeof(g_rinfo.label), "%s", label);
    static Shape tmp[SHAPE_MAX];
    int n = shapes_read_file(label, tmp, SHAPE_MAX);
    for (int i = 0; i < n && g_rinfo.nd < 24; i++) if (tmp[i].door && tmp[i].has_step) {
        g_rinfo.door_id[g_rinfo.nd] = tmp[i].id;
        snprintf(g_rinfo.door_to[g_rinfo.nd], sizeof(g_rinfo.door_to[0]), "%s", tmp[i].target[0] ? tmp[i].target : "no target");
        g_rinfo.nd++;
    }
    Script *sc = NULL;
    int ns = room_scripts_read(label, &sc);
    for (int i = 0; i < ns; i++) { if (g_rinfo.ns < 24) snprintf(g_rinfo.script[g_rinfo.ns++], 64, "%s", sc[i].name); script_free(&sc[i]); }
    free(sc);
}

/* Overlays: the room's picture with the overlays of the action on it, animated (inspector) */
static void sv_overlay_preview(HDC hdc, const RECT *r, const ScriptAction *a) {
    if (!g_loaded || !g_dib_pixels) return;
    int W = (int)g_hdr.width, H = (int)g_hdr.height;
    static uint32_t *buf = NULL; static size_t cap = 0;
    if (cap < (size_t)W * H) { free(buf); buf = (uint32_t *)malloc((size_t)W * H * 4); cap = buf ? (size_t)W * H : 0; if (!buf) return; }
    const uint32_t *pic = g_dib_own ? g_dib_own : g_dib_pixels;
    for (int y = 0; y < H; y++) memcpy(buf + (size_t)y * W, pic + (size_t)y * g_dib_stride, W * 4);
    int selx = -1, sely = 0, selw = 0, selh = 0;
    for (int i = 0; i < a->nov; i++) {
        int k = overlay_find(a->ov_name[i]);
        if (k < 0 || a->ov_mode[i] == OVM_HIDE) continue;
        RoomOverlay *o = &g_ov[k];
        int f = a->ov_mode[i] == OVM_FREEZE ? a->ov_frame[i] : (int)(g_ch_t * o->fps) % o->frames;
        int w, h;
        const uint32_t *px = overlay_frame(o, f, &w, &h);
        if (px) blend_sprite(buf, W, H, px, w, h, o->x, o->y, 1.0f);
        if (i == g_sv_ov_sel) { selx = o->x; sely = o->y; selw = o->w; selh = o->h; }
    }
    int RW = r->right - r->left, RH = r->bottom - r->top;
    float k = fminf((float)RW / W, (float)RH / H);
    int dw = (int)(W * k), dh = (int)(H * k), ox = r->left + (RW - dw) / 2, oy = r->top + (RH - dh) / 2;
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc, ox, oy, dw, dh, 0, 0, W, H, buf, &bi, DIB_RGB_COLORS, SRCCOPY);
    if (selx >= 0) {
        RECT sr = { ox + (int)(selx * k) - 1, oy + (int)(sely * k) - 1, ox + (int)((selx + selw) * k) + 2, oy + (int)((sely + selh) * k) + 2 };
        ui_frame(hdc, &sr, RGB(255, 210, 60));
    }
}

/* the "CHARACTER" choice of MOVE / ANIM / SPEAK: David + the script's placed characters */
static int sv_actor_buttons(Script *s, ScriptAction *a, int ix, int iy, int iw, int rh) {
    char t[160];
    svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "CHARACTER");
    iy += 22;
    int ids[64], rows[64];
    int n = sv_actor_list(s, ids, rows, 64);
    for (int k = 0; k < n && k < 8; k++) {
        if (ids[k] == 0) snprintf(t, sizeof(t), "David");
        else { ScriptAction *p = script_find(s, ids[k], NULL, NULL); snprintf(t, sizeof(t), "%s  (placed row %d)", p && p->model[0] ? p->model : "?", rows[k] + 1); }
        ui_addf(B_SV_ACTOR + k, ix, iy, iw, rh - 2, t, "", "The character of this action. Placed characters must be brought in by an earlier row.", a->actor == ids[k], 1, 3);
        iy += rh + 3;
    }
    return iy + 10;
}
/* ANIM / SOUND: its random pool */
static int sv_pool_buttons(ScriptAction *a, int ix, int iy, int iw, int rh, int anim) {
    svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "RANDOM POOL (ONE OF THEM, EACH TIME)");
    iy += 20;
    if (a->npool == 0) {
        svl(ix, iy, iw, 34, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, anim ? "Only the animation above. Add others: one of them is picked at random each time it plays."
                                                                   : "Only the sound above. Add others: one of them is picked at random each time it plays.");
        iy += 38;
    }
    for (int i = 0; i < a->npool; i++) {
        svl(ix, iy, iw - 42, rh, 0, SVL_TEXT, SV_ONE, "%d.  %s", i + 2, a->pool[i]);
        ui_add(B_SV_POOL + i, ix + iw - 34, iy, 34, rh, "x", "", "Take it out of the pool.", 0, 1, 0);
        iy += rh + 4;
    }
    ui_add(B_SV_POOL_ADD, ix, iy, iw, rh, anim ? "+ Add another animation..." : "+ Add another sound...", "",
           "One more for the pool: each time the action plays, one of them (the first above included) is picked at random.", 0, a->npool < SCRIPT_POOL_MAX && a->file[0], 0);
    return iy + rh + 14;
}
/* "FADE OUT": - 2.0 s + (MUSIC, STOP) */
static int sv_fade_buttons(ScriptAction *a, int ix, int iy, int iw, int rh, const char *title, const char *desc) {
    svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "%s", title);
    iy += 20;
    ui_add(B_SV_FADE_M, ix, iy, 34, rh, "-", "", "A shorter fade (0.5 s).", 0, a->fade > 0.0f, 0);
    if (a->fade > 0.0f) svl(ix + 42, iy, iw - 84, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "%.1f s", a->fade);
    else svl(ix + 42, iy, iw - 84, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "none (at once)");
    ui_add(B_SV_FADE_P, ix + iw - 34, iy, 34, rh, "+", "", "A longer fade (0.5 s).", 0, a->fade < 10.0f, 0);
    iy += rh + 4;
    svl(ix, iy, iw, 34, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "%s", desc);
    return iy + 40;
}

static void sv_layout_main(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    if (g_sv_sel_script != g_sv_script) { g_sv_sel_n = 0; g_sv_sel_script = g_sv_script; } /* a group is of one script */
    SetRect(&g_sv_port_rc, 0, 0, 0, 0);
    SetRect(&g_sv_ovprev_rc, 0, 0, 0, 0);
    int W = rc.right, H = rc.bottom;
    g_btn_count = 0; g_svl_n = 0;
    int list_w = W / 6; if (list_w < 180) list_w = 180; if (list_w > 240) list_w = 240;
    int insp_w = W / 4; if (insp_w < 300) insp_w = 300; if (insp_w > 400) insp_w = 400;
    SetRect(&g_sv_list_rc, 10, 66, 10 + list_w, H - 118);
    SetRect(&g_sv_insp_rc, W - insp_w - 10, 44, W - 10, H - 10);
    SetRect(&g_sv_grid_rc, g_sv_list_rc.right + 12, 112, g_sv_insp_rc.left - 12, H - 10);
    Script *s = sv_cur();

    /* header */
    svl(12, 8, list_w + 400, 28, 2, RGB(255, 225, 120), SV_ONE, "Scripts -- %s", current_room_label());
    ui_add(B_SV_CLOSE, W - 96, 8, 86, 26, "Close", "Esc", "Back to the game.", 0, 1, 0);

    /* left: the room's scripts */
    svl(10, 46, list_w, 16, 1, SVL_SECTION, SV_ONE, "SCRIPTS OF THIS ROOM (%d)", g_script_count);
    int bx = 10, by = g_sv_list_rc.bottom + 8, bh = 26, bw = (list_w - 6) / 2;
    ui_add(B_SV_NEW, bx, by, bw, bh, "New", "", "A new empty script for this room (then type its name, Enter).", 0, 1, 0);
    int is_default = s && !strcmp(s->name, DEFAULT_SCRIPT);
    ui_add(B_SV_RENAME, bx + bw + 6, by, bw, bh, "Rename", "F2", "Rename the selected script (type, Enter). A connector playing it must be pointed at the new name again. 'Default' keeps its name.", 0, s != NULL && !is_default, 0);
    by += bh + 5;
    ui_add(B_SV_DUP, bx, by, bw, bh, "Duplicate", "", "A copy of the selected script.", 0, s != NULL, 0);
    ui_add(B_SV_DELETE, bx + bw + 6, by, bw, bh, "Delete", "", "Delete the selected script (asks first). Every room keeps its 'Default' script.", 0, s != NULL && !is_default, 0);
    by += bh + 5;
    ui_add(B_SV_AUTO, bx, by, list_w, bh, s && s->auto_run ? "Auto on entry: ON" : "Auto on entry: off", "",
           "ON: played whenever David enters this room without a connector script of its own (the first 'auto' script of the list). Not while the scene editor is on.",
           s && s->auto_run, s != NULL, 1);

    /* middle: toolbar + grid */
    int tx = g_sv_grid_rc.left, ty = 44, th = 28, gap = 6;
    int zsel = s ? script_zone_at(s, g_sv_row) : -1, zlo, zhi;
    sv_sel_range(&zlo, &zhi);
    int zfree = s && sv_range_free(s, zlo, zhi);
    struct { int id, w; const char *label, *key, *desc; int enabled; } tb[] = {
        { B_SV_PLAY, 100, "Play", "F5", "Play this script from its first row, in the game (a clean start: script characters, picture, sounds and music of a previous test are removed).", s != NULL },
        { B_SV_PLAY_ROW, 150, "Play from this row", "F6", "Play it from the selected row (characters placed by the rows above won't be there).", s != NULL },
        { B_SV_ROW_INS, 100, "Insert row", "Ins", "Insert an empty row at the selected one (the rows below move down).", s != NULL },
        { B_SV_ROW_DEL, 100, "Delete row", "", "Delete the selected row and its actions (the rows below move up).", s && g_sv_row < s->rows },
        { B_SV_COL_ADD, 96, "+ Column", "", "Add a column: more actions played at the same time in a row.", s && s->cols < SCRIPT_MAX_COLS },
        { B_SV_COL_DEL, 96, "- Column", "", "Delete the selected column (asks first if it holds actions).", s && s->cols > 1 },
        { B_SV_COPY, 70, "Copy", "Ctrl+C", "Copy the selected action.", s != NULL },
        { B_SV_PASTE, 70, "Paste", "Ctrl+V", "Paste the copied action into the selected cell.", s && g_sv_has_clip },
        { B_SV_CLEAR, 70, "Clear", "Del", "Empty the selected cell.", s != NULL },
        { B_SV_ZONE, 124, "Make timeline", "", "Turn the selected rows (Shift+click or Shift+arrows: several rows) into an ADVANCED TIMELINE: its actions start at their own time, as tall as they last, and can overlap.", s && zfree },
        { B_SV_UNZONE, 110, "Back to rows", "", "Turn this timeline back into plain rows (its actions in the order they start).", zsel >= 0 },
        { B_SV_ZROWS_M, 64, "- Row", "", "This timeline one row shorter (not below what its actions need). Or drag its bottom edge.", zsel >= 0 && s->zone[zsel].rows > 1 },
        { B_SV_ZROWS_P, 64, "+ Row", "", "This timeline one row taller: more time for its actions. Or drag its bottom edge.", zsel >= 0 },
        { B_SV_ZSCALE_M, 76, "Zoom in", "", "The timeline shows fewer seconds per row: more room to place its actions precisely.", zsel >= 0 && s->zone[zsel].sec_per_row > 0.26f },
        { B_SV_ZSCALE_P, 76, "Zoom out", "", "More seconds per row: a long timeline in fewer rows.", zsel >= 0 && s->zone[zsel].sec_per_row < 7.9f },
    };
    for (int i = 0; i < (int)(sizeof(tb) / sizeof(tb[0])); i++) {
        if (tx + tb[i].w > g_sv_grid_rc.right) { tx = g_sv_grid_rc.left; ty += th + gap; }
        ui_add(tb[i].id, tx, ty, tb[i].w, th, tb[i].label, tb[i].key, tb[i].desc, 0, tb[i].enabled, 0);
        tx += tb[i].w + gap;
    }
    g_sv_grid_rc.top = ty + th + 12;

    /* right: the selected cell */
    int ix = g_sv_insp_rc.left + 12, iw = g_sv_insp_rc.right - g_sv_insp_rc.left - 24, iy = g_sv_insp_rc.top + 10;
    int rh = 26, half = (iw - 6) / 2;
    SetRect(&g_sv_thumb_rc, 0, 0, 0, 0);
    if (!s) {
        svl(ix, iy, iw, 60, 0, SVL_TEXT, DT_LEFT | DT_WORDBREAK, "This room has no script yet. Click New (left) to make one.");
        return;
    }
    ScriptAction *a = sv_cell(0);
    int type = a ? a->type : ACT_NONE;
    int izone = script_zone_at(s, g_sv_row);
    if (izone >= 0) {
        const ScriptZone *z = &s->zone[izone];
        svl(ix, iy, iw, 16, 1, RGB(90, 190, 190), SV_ONE, "TIMELINE (ROWS %d-%d) -- LANE %d", z->row0 + 1, z->row0 + z->rows, g_sv_col + 1);
        iy += 20;
        if (type != ACT_NONE) {
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "STARTS AT");
            iy += 20;
            ui_add(B_SV_T_M1, ix, iy, 44, rh, "-0.1", "", "Sooner (0.1 s). Or drag it up in the timeline.", 0, a->start > 0.0f, 0);
            ui_add(B_SV_T_M01, ix + 48, iy, 44, rh, "-.01", "", "Sooner (0.01 s).", 0, a->start > 0.0f, 0);
            svl(ix + 96, iy, iw - 192, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "%.2f s", a->start);
            ui_add(B_SV_T_P01, ix + iw - 92, iy, 44, rh, "+.01", "", "Later (0.01 s).", 0, 1, 0);
            ui_add(B_SV_T_P1, ix + iw - 44, iy, 44, rh, "+0.1", "", "Later (0.1 s). Or drag it down in the timeline.", 0, 1, 0);
            iy += rh + 4;
            int kind; float len = sv_event_len(s, izone, a, &kind), m;
            char dl[160];
            if (kind == DUR_INSTANT) snprintf(dl, sizeof(dl), "Instant: drawn as a thin line.");
            else if (kind == DUR_KNOWN) snprintf(dl, sizeof(dl), "Lasts %.2f s (ends at %.2f s).", len, a->start + len);
            else snprintf(dl, sizeof(dl), "Lasts until it's over -- %s %.1f s.", measured_duration(s->name, a->id, &m) ? "last run:" : "estimate:", len);
            svl(ix, iy, iw, 20, 0, SVL_DIM, SV_ONE, "%s", dl);
            iy += 28;
            float full;
            if (action_media_length(a, &full)) { /* its part played */
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "PART PLAYED (OF %.2f s) -- OR DRAG ITS TOP / BOTTOM EDGE", full);
                iy += 20;
                ui_add(B_SV_TRIM_IN_M, ix, iy, 34, rh, "-", "", "Start less far into it (0.05 s).", 0, a->trim_in > 0.0f, 0);
                svl(ix + 40, iy, iw - 80, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, a->trim_in > 0.0f ? "starts %.2f s into it" : "from its start", a->trim_in);
                ui_add(B_SV_TRIM_IN_P, ix + iw - 34, iy, 34, rh, "+", "", "Start further into it (0.05 s): the beginning is skipped.", 0, full - a->trim_in - a->trim_out > 0.1f, 0);
                iy += rh + 4;
                ui_add(B_SV_TRIM_OUT_M, ix, iy, 34, rh, "-", "", "Cut less of its end (0.05 s).", 0, a->trim_out > 0.0f, 0);
                svl(ix + 40, iy, iw - 80, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, a->trim_out > 0.0f ? "ends %.2f s early" : "to its end", a->trim_out);
                ui_add(B_SV_TRIM_OUT_P, ix + iw - 34, iy, 34, rh, "+", "", "End it earlier (0.05 s).", 0, full - a->trim_in - a->trim_out > 0.1f, 0);
                iy += rh + 4;
                ui_add(B_SV_TRIM_RESET, ix, iy, iw, rh, "Play all of it", "", "No cut: from its start to its end.", 0, a->trim_in > 0.0f || a->trim_out > 0.0f, 0);
                iy += rh + 12;
            }
        }
    } else {
        svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "ROW %d, COLUMN %d", g_sv_row + 1, g_sv_col + 1);
        iy += 20;
    }
    if (type == ACT_NONE) {
        if (izone >= 0) svl(ix, iy, iw, 22, 3, SVL_TEXT, SV_ONE, "Empty -- add an action at %.2f s:", g_sv_pending_start >= 0.0f ? g_sv_pending_start : 0.0f);
        else svl(ix, iy, iw, 22, 3, SVL_TEXT, SV_ONE, "Empty cell -- add an action:");
        iy += 28;
        static const char *add_desc[ACT_COUNT] = { "",
            "Wait a given time. The game goes on meanwhile.",
            "Show another picture as the room's background (the geometry and masks stay the room's).",
            "Change the music: a playlist of tracks, each looped or not. Replaces the music playing (the same track goes on).",
            "Play a sound (repeated or not). The music isn't touched.",
            "Play an atmosphere sound, looped or not. The music isn't touched.",
            "Stop a sound or ambience started by this script (or all of them, or the music).",
            "Bring a character into the room, at a point you click, facing a direction.",
            "Make a character (David or one placed by this script) walk / run to a point you click, or through a connector.",
            "Make a character play an animation: a number of times, or over and over until the rest of the row is over (e.g. talking while its line plays).",
            "A character says a line (a sound): its portrait is shown until the line is over. The row waits for it.",
            "The room's environmental animations, already at their place: loop some, play some once, freeze some on a frame, hide some.",
            "Go to another room, as through a connector: David comes in by one of its connectors, then one of its scripts plays. This script ends there.",
            "Move the view: slide it smoothly to a point of the room (or to David) and zoom in or out. The row waits for the slide.",
            "Change a character: its side (ally / enemy), its AI on or off, its weapon or shield (taken out as with the radial menu)." };
        for (int t = ACT_WAIT; t < ACT_COUNT; t++) {
            ui_add(B_SV_ADD + t, ix + ((t - 1) % 2) * (half + 6), iy, half, rh, action_type_name(t), "", add_desc[t], 0, 1, 0);
            if ((t - 1) % 2 == 1) iy += rh + 6;
        }
        iy += rh + 12;
        svl(ix, iy, iw, 80, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK,
            "Every action of a row starts at the same time; the next row starts when they are all finished. Right-click a cell for more.");
        return;
    }
    svl(ix, iy, iw - 110, 26, 2, action_color(type), SV_ONE, "%s", action_type_name(type));
    ui_add(B_SV_TYPE, ix + iw - 104, iy, 104, 24, "Change...", "", "Replace this action by another kind (or copy / clear / rows).", 0, 1, 0);
    iy += 36;
    char t[256];
    switch (type) {
        case ACT_WAIT:
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "DURATION");
            iy += 20;
            svl(ix, iy, iw, 30, 2, SVL_VALUE, SV_ONE, "%.1f seconds", a->seconds);
            iy += 36;
            { int q = (iw - 18) / 4;
              ui_add(B_SV_W_M1, ix, iy, q, rh, "-1 s", "", "One second less.", 0, a->seconds > 0, 0);
              ui_add(B_SV_W_M01, ix + (q + 6), iy, q, rh, "-0.1 s", "", "A tenth of a second less.", 0, a->seconds > 0, 0);
              ui_add(B_SV_W_P01, ix + 2 * (q + 6), iy, q, rh, "+0.1 s", "", "A tenth of a second more.", 0, 1, 0);
              ui_add(B_SV_W_P1, ix + 3 * (q + 6), iy, q, rh, "+1 s", "", "One second more.", 0, 1, 0); }
            iy += rh + 12;
            svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "Nothing happens meanwhile: the game goes on normally. A left-click skips it (the whole row).");
            break;
        case ACT_BACKGROUND:
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "PICTURE");
            iy += 20;
            svl(ix, iy, iw, 20, 0, SVL_VALUE, SV_ONE, "%s", a->file[0] ? a->file : "the room's own picture");
            iy += 26;
            ui_add(B_SV_BG_CHOOSE, ix, iy, half, rh, "Choose picture...", "", "Pick a picture of the blockouts (this room's, any room's, or its whole level). Stretched to the room's size if it differs.", 0, 1, 0);
            ui_add(B_SV_BG_OWN, ix + half + 6, iy, half, rh, "Room's own picture", "", "Put the room's own picture back.", !a->file[0], 1, 0);
            iy += rh + 10;
            SetRect(&g_sv_thumb_rc, ix, iy, ix + iw, iy + iw * 3 / 4);
            iy += iw * 3 / 4 + 8;
            svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "Only the picture changes: walls, floors, depth and foreground shapes stay the room's. It lasts until the room is left.");
            break;
        case ACT_MUSIC:
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "PLAYLIST (played in order)");
            iy += 22;
            for (int i = 0; i < a->ntracks; i++) {
                int bw = 26, lw = 58;
                svl(ix, iy, iw - (bw * 4 + lw + 30), rh, 0, SVL_TEXT, SV_ONE, "%d. %s", i + 1, a->track[i]);
                int x = ix + iw - (bw * 4 + lw + 24);
                ui_add(B_SV_TRACK + i * 8 + 0, x, iy, bw, rh, ">", "", "Listen to this track (again: stop).", 0, 1, 0); x += bw + 6;
                ui_add(B_SV_TRACK + i * 8 + 1, x, iy, lw, rh, a->track_loop[i] ? "Loop" : "Once", "", "Loop: this track plays forever (the ones after it never come). Once: plays once, then the next track.", a->track_loop[i], 1, 1); x += lw + 6;
                ui_add(B_SV_TRACK + i * 8 + 2, x, iy, bw, rh, "^", "", "Move it up.", 0, i > 0, 0); x += bw + 3;
                ui_add(B_SV_TRACK + i * 8 + 3, x, iy, bw, rh, "v", "", "Move it down.", 0, i < a->ntracks - 1, 0); x += bw + 3;
                ui_add(B_SV_TRACK + i * 8 + 4, x, iy, bw, rh, "x", "", "Remove it from the playlist.", 0, 1, 0);
                iy += rh + 5;
            }
            if (a->ntracks == 0) { svl(ix, iy, iw, 20, 0, RGB(255, 150, 120), SV_ONE, "None: no music here (stops the music playing)."); iy += 26; }
            ui_add(B_SV_MU_ADD, ix, iy, iw, rh, "Add a track...", "", "Choose a music file (assets/sound) to add at the end of the playlist.", 0, a->ntracks < AUDIO_MAX_TRACKS, 0);
            iy += rh + 14;
            iy = sv_fade_buttons(a, ix, iy, iw, rh, "FADE OUT (the music playing before)",
                                 a->ntracks ? "The music playing fades out over this time, then the new playlist starts." : "The music fades out over this time instead of stopping at once.");
            svl(ix, iy, iw, 90, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK,
                "It replaces the music playing and its playlist -- but if the music playing is already this playlist's first track (the same music as the room before), it goes on without restarting. The row doesn't wait for it.");
            break;
        case ACT_SOUND: case ACT_AMBIENCE:
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "FILE");
            iy += 20;
            svl(ix, iy, iw, 20, 0, SVL_VALUE, SV_ONE, "%s", a->file[0] ? a->file : "(none yet)");
            iy += 26;
            ui_add(B_SV_FILE, ix, iy, half, rh, "Choose file...", "", "Choose a sound of assets/sound (type to filter, Space to listen).", 0, 1, 0);
            ui_add(B_SV_LISTEN, ix + half + 6, iy, half, rh, audio_preview_playing() ? "Stop listening" : "Listen", "", "Hear it (again: stop).", audio_preview_playing(), a->file[0] != 0, 0);
            iy += rh + 14;
            if (type == ACT_SOUND) iy = sv_pool_buttons(a, ix, iy, iw, rh, 0);
            if (type == ACT_SOUND) {
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "REPEAT");
                iy += 20;
                ui_add(B_SV_REP_M, ix, iy, 34, rh, "-", "", "One repeat less.", 0, a->repeat > 0, 0);
                snprintf(t, sizeof(t), "%d  (plays %d time%s)", a->repeat, a->repeat + 1, a->repeat ? "s" : "");
                svl(ix + 42, iy, iw - 84, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "%s", t);
                ui_add(B_SV_REP_P, ix + iw - 34, iy, 34, rh, "+", "", "One repeat more.", 0, a->repeat < 99, 0);
                iy += rh + 14;
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "WAIT UNTIL IT'S OVER");
                iy += 20;
                ui_add(B_SV_WAITEND, ix, iy, iw, rh, a->wait_end ? "Yes: the next row waits for it" : "No: the next row doesn't wait", "",
                       "Yes: the row isn't finished until the sound (and its repeats) is over. No: it plays on while the next rows start.", a->wait_end, 1, 1);
                iy += rh + 12;
            } else {
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "LOOP");
                iy += 20;
                ui_add(B_SV_LOOP, ix, iy, iw, rh, a->loop ? "Loops until stopped" : "Plays once", "", "Loop: the ambience plays until a 'Stop sound' action stops it.", a->loop, 1, 1);
                iy += rh + 12;
            }
            svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "The music isn't touched. A 'Stop sound' action can stop it while it plays.");
            break;
        case ACT_STOP: {
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "STOP WHAT?");
            iy += 22;
            ui_add(B_SV_ST_ALL, ix, iy, iw, rh, "All sounds & ambiences", "", "Every sound and ambience playing (not the music).", a->stop_kind == STOP_ALL_SOUNDS, 1, 3);
            iy += rh + 5;
            ui_add(B_SV_ST_MUSIC, ix, iy, iw, rh, "The music", "", "Stop the music and its playlist.", a->stop_kind == STOP_MUSIC, 1, 3);
            iy += rh + 12;
            iy = sv_fade_buttons(a, ix, iy, iw, rh, "FADE OUT", "What's stopped fades out over this time instead of stopping at once.");
            ScriptAction *list[64]; int rows[64];
            int n = sv_actions_of(s, ACT_SOUND, ACT_AMBIENCE, list, rows, 64);
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "OR ONE OF THIS SCRIPT'S SOUNDS");
            iy += 22;
            if (n == 0) { svl(ix, iy, iw, 20, 0, SVL_DIM, SV_ONE, "(this script plays no sound / ambience)"); iy += 24; }
            for (int k = 0; k < n && iy < g_sv_insp_rc.bottom - 60; k++) {
                snprintf(t, sizeof(t), "%s %s  (row %d)", list[k]->type == ACT_SOUND ? "Sound" : "Ambience", list[k]->file[0] ? list[k]->file : "?", rows[k] + 1);
                ui_addf(B_SV_STOPREF + k, ix, iy, iw, rh, t, "", "Stop this one if it's still playing.", a->stop_kind == STOP_ACTION && a->stop_ref == list[k]->id, 1, 3);
                iy += rh + 5;
            }
            break;
        }
        case ACT_PLACE:
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "CHARACTER");
            iy += 20;
            svl(ix, iy, iw, 20, 0, SVL_VALUE, SV_ONE, "%s", a->model[0] ? a->model : "(none yet)");
            iy += 26;
            ui_add(B_SV_PL_MODEL, ix, iy, half, rh, "Choose...", "", "Pick a character of the game (grid with previews).", 0, 1, 0);
            ui_add(B_SV_MOVESET, ix + half + 6, iy, half, rh, "Moveset...", "", "The animations this character walks with (graph), and change them.", 0, a->model[0] != 0, 0);
            iy += rh + 14;
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "POSITION");
            iy += 20;
            if (a->has_pos) svl(ix, iy, iw, 20, 0, SVL_VALUE, SV_ONE, "set  (%.1f, %.1f, %.1f)", a->pos[0], a->pos[1], a->pos[2]);
            else svl(ix, iy, iw, 20, 0, RGB(255, 150, 120), SV_ONE, "not set yet");
            iy += 26;
            ui_add(B_SV_PL_POS, ix, iy, iw, rh, "Pick in the room...", "", "Click the floor where it appears, then the point it looks toward.", 0, 1, 0);
            iy += rh + 14;
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "FACING");
            iy += 20;
            ui_add(B_SV_FACE_L, ix, iy, 44, rh, "<", "", "Turn it 45 degrees left.", 0, 1, 0);
            svl(ix + 50, iy, iw - 100, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "%.0f deg", a->facing * 57.29578f);
            ui_add(B_SV_FACE_R, ix + iw - 44, iy, 44, rh, ">", "", "Turn it 45 degrees right.", 0, 1, 0);
            iy += rh + 12;
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "SIDE");
            iy += 20;
            ui_add(B_SV_PL_SIDE, ix, iy, half, rh, "Ally", "", "On David's side: our blows don't touch it; its AI goes for the enemies. Health gone: knocked down, not dead.", !a->side, 1, 3);
            ui_add(B_SV_PL_SIDE + 1, ix + half + 6, iy, half, rh, "Enemy", "", "Our blows land on it; its AI goes for David and his allies. Health gone: it dies.", a->side, 1, 3);
            iy += rh + 6;
            ui_add(B_SV_PL_AI, ix, iy, iw, rh, a->ai_off ? "[  ]  Its AI plays" : "[x]  Its AI plays", "",
                   "Ticked: it acts on its own (its AI preset, moveset screen). 'Character settings' can change it later.", !a->ai_off, 1, 3);
            iy += rh + 12;
            svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "It stands there until a 'Move character' action (or its AI) moves it. It leaves with the room. 'Character settings' changes its side, AI and equipment later.");
            break;
        case ACT_CHAR: {
            iy = sv_actor_buttons(s, a, ix, iy, iw, rh);
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "SIDE");
            iy += 20;
            int w3 = (iw - 12) / 3;
            ui_add(B_SV_CS_SIDE, ix, iy, w3, rh, "Unchanged", "", "Its side stays as it is.", a->set_side == 0, 1, 3);
            ui_add(B_SV_CS_SIDE + 1, ix + w3 + 6, iy, w3, rh, "Ally", "", "It becomes an ally: our blows don't touch it.", a->set_side == 1, 1, 3);
            ui_add(B_SV_CS_SIDE + 2, ix + 2 * (w3 + 6), iy, w3, rh, "Enemy", "", "It becomes an enemy: our blows land on it.", a->set_side == 2, 1, 3);
            iy += rh + 12;
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "AI");
            iy += 20;
            ui_add(B_SV_CS_AI, ix, iy, w3, rh, "Unchanged", "", "Its AI stays as it is.", a->set_ai == 0, 1, 3);
            ui_add(B_SV_CS_AI + 1, ix + w3 + 6, iy, w3, rh, "On", "", "Its AI plays (never David's: the player).", a->set_ai == 1, 1, 3);
            ui_add(B_SV_CS_AI + 2, ix + 2 * (w3 + 6), iy, w3, rh, "Off", "", "Its AI stops: only scripts move it.", a->set_ai == 2, 1, 3);
            iy += rh + 12;
            char wl[80];
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "WEAPON");
            iy += 20;
            ui_add(B_SV_CS_WPN, ix, iy, half, rh, "Unchanged", "", "It keeps what it holds.", !a->set_weapon, 1, 3);
            snprintf(wl, sizeof(wl), a->set_weapon ? "%s..." : "Choose...", a->item[0] ? a->item : "none");
            ui_addf(B_SV_CS_WPN + 1, ix + half + 6, iy, half, rh, wl, "", "The weapon it takes out (as with the radial menu: sheatmp), or none: it puts its weapon away.", a->set_weapon, 1, 3);
            iy += rh + 12;
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "SHIELD");
            iy += 20;
            ui_add(B_SV_CS_SHD, ix, iy, half, rh, "Unchanged", "", "It keeps its shield (or none).", !a->set_shield, 1, 3);
            snprintf(wl, sizeof(wl), a->set_shield ? "%s..." : "Choose...", a->item2[0] ? a->item2 : "none");
            ui_addf(B_SV_CS_SHD + 1, ix + half + 6, iy, half, rh, wl, "", "The shield it takes out (shldquip), or none: it puts it away. Not with the double swords.", a->set_shield, 1, 3);
            iy += rh + 12;
            svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "The side and AI change at once; the weapon, then the shield, come out with their animations -- the row waits for them.");
            break;
        }
        case ACT_OVERLAY: {
            if (g_ov_n == 0) {
                svl(ix, iy, iw, 60, 0, SVL_TEXT, DT_LEFT | DT_WORDBREAK, "This room has no overlay (no environmental animation in its folder).");
                break;
            }
            int th = iw * (int)g_hdr.height / ((int)g_hdr.width > 0 ? (int)g_hdr.width : 1);
            if (th > 240) th = 240;
            SetRect(&g_sv_ovprev_rc, ix, iy, ix + iw, iy + th);
            iy += th + 8;
            if (g_sv_ov_sel >= a->nov) g_sv_ov_sel = 0;
            for (int i = 0; i < a->nov && iy < g_sv_insp_rc.bottom - 250; i++) {
                int k = overlay_find(a->ov_name[i]);
                char nm[96];
                snprintf(nm, sizeof(nm), "%s%s", a->ov_name[i], k < 0 ? "  (not in this room)" : (!strcmp(g_ov[k].conf, "guess") || !strcmp(g_ov[k].conf, "none")) ? "  (place?)" : "");
                ui_addf(B_SV_OVROW + i * 8, ix, iy, iw - 136, rh - 4, nm, "", "Select it: it's framed in yellow above; its place and frame are set below.", i == g_sv_ov_sel, 1, 3);
                char md[48];
                if (a->ov_mode[i] == OVM_FREEZE) snprintf(md, sizeof(md), "frozen: %d", a->ov_frame[i]);
                else snprintf(md, sizeof(md), "%s", a->ov_mode[i] == OVM_ONCE_HIDE ? "once+hide" : overlay_mode_name(a->ov_mode[i]));
                ui_addf(B_SV_OVROW + i * 8 + 1, ix + iw - 130, iy, 130, rh - 4, md, "",
                        "What it does: loop / once (stays on its last frame) / once then hidden / frozen on a frame / hidden / unchanged (as it is).", a->ov_mode[i] != OVM_KEEP, 1, 1);
                iy += rh;
            }
            iy += 6;
            if (g_sv_ov_sel < a->nov) {
                int k = overlay_find(a->ov_name[g_sv_ov_sel]);
                if (k >= 0) {
                    RoomOverlay *o = &g_ov[k];
                    char own[64];
                    split_label(current_room_label(), t, sizeof(t), own, sizeof(own));
                    svl(ix, iy, iw, 18, 3, action_color(ACT_OVERLAY), SV_ONE, "%s", o->name);
                    iy += 20;
                    svl(ix, iy, iw, 18, 0, SVL_TEXT, SV_ONE, "%d frames, %.0f fps, %dx%d at %d, %d", o->frames, o->fps, o->w, o->h, o->x, o->y);
                    iy += 18;
                    if (strcmp(o->bg, own) && o->bg[0] && strcmp(o->bg, "-")) { svl(ix, iy, iw, 18, 0, RGB(255, 190, 120), SV_ONE, "made for the picture '%s' of this room", o->bg); iy += 18; }
                    if (!strcmp(o->conf, "guess") || !strcmp(o->conf, "none")) { svl(ix, iy, iw, 18, 0, RGB(255, 150, 120), SV_ONE, "its place is a guess: check it, move it if needed"); iy += 18; }
                    iy += 4;
                    int q = 32;
                    ui_add(B_SV_OV_LEFT, ix, iy, q, rh, "<", "", "1 pixel left (Shift: 8).", 0, 1, 0);
                    ui_add(B_SV_OV_RIGHT, ix + q + 3, iy, q, rh, ">", "", "1 pixel right (Shift: 8).", 0, 1, 0);
                    ui_add(B_SV_OV_UP, ix + 2 * (q + 3), iy, q, rh, "^", "", "1 pixel up (Shift: 8).", 0, 1, 0);
                    ui_add(B_SV_OV_DOWN, ix + 3 * (q + 3), iy, q, rh, "v", "", "1 pixel down (Shift: 8).", 0, 1, 0);
                    ui_add(B_SV_OV_PICK, ix + 4 * (q + 3) + 6, iy, iw - 4 * (q + 3) - 6, rh, "Place in the room...", "",
                           "Click in the room where its centre goes. Its place belongs to the room (every script uses it).", 0, 1, 0);
                    iy += rh + 6;
                    if (a->ov_mode[g_sv_ov_sel] == OVM_FREEZE) {
                        ui_add(B_SV_OVROW + g_sv_ov_sel * 8 + 2, ix, iy, 40, rh, "-", "", "The frame before (Shift: 10).", 0, a->ov_frame[g_sv_ov_sel] > 0, 0);
                        svl(ix + 46, iy, iw - 92, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "frozen on frame %d / %d", a->ov_frame[g_sv_ov_sel], o->frames - 1);
                        ui_add(B_SV_OVROW + g_sv_ov_sel * 8 + 3, ix + iw - 40, iy, 40, rh, "+", "", "The frame after (Shift: 10).", 0, a->ov_frame[g_sv_ov_sel] < o->frames - 1, 0);
                        iy += rh + 6;
                    }
                }
            }
            ui_add(B_SV_OV_WAIT, ix, iy, iw, rh, a->wait_end ? "The row waits for the 'once' ones" : "The row doesn't wait for them", "",
                   "Yes: the row isn't over until the overlays played once have reached their end. No: they play on while the next rows start.", a->wait_end, 1, 1);
            iy += rh + 5;
            int missing = 0;
            for (int k = 0; k < g_ov_n; k++) {
                int listed = 0;
                for (int i = 0; i < a->nov; i++) if (!strcmp(a->ov_name[i], g_ov[k].name)) listed = 1;
                if (!listed) missing++;
            }
            if (missing) { ui_addf(B_SV_OV_RELOAD, ix, iy, iw, rh, "Add the room's other overlays", "", "List the overlays of this room that this action doesn't have yet (unchanged).", 0, a->nov < SCRIPT_MAX_OVERLAYS, 0); iy += rh + 5; }
            break;
        }
        case ACT_CAMERA: {
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "THE VIEW GOES TO");
            iy += 22;
            ui_add(B_SV_CAM_DAVID, ix, iy, iw, rh - 4, "David", "", "The view centres on David (where he is when the row starts).", a->cam_target == CAM_DAVID, 1, 3);
            iy += rh;
            ui_add(B_SV_CAM_POINT, ix, iy, iw, rh - 4, a->has_pos ? "A point of the room" : "A point of the room (not picked yet)", "", "The view centres on the point picked in the room.", a->cam_target == CAM_POINT, 1, 3);
            iy += rh;
            ui_add(B_SV_CAM_KEEP, ix, iy, iw, rh - 4, "Where it looks now (zoom only)", "", "The view stays centred where it is; only the zoom changes.", a->cam_target == CAM_KEEP, 1, 3);
            iy += rh + 4;
            ui_add(B_SV_CAM_PICK, ix, iy, iw, rh, "Pick the point in the room...", "", "Click the point of the room the view slides to.", 0, 1, 0);
            iy += rh + 14;
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "ZOOM");
            iy += 20;
            ui_add(B_SV_CAM_ZM, ix, iy, 40, rh, "-", "", "Zoom out (0.1).", 0, a->zoom > 0.0f, 0);
            if (a->zoom > 0.0f) svl(ix + 46, iy, iw - 92 - 110, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "x%.1f", a->zoom);
            else svl(ix + 46, iy, iw - 92 - 110, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "the room's (x%.1f)", g_room_zoom);
            ui_add(B_SV_CAM_ZP, ix + iw - 40 - 110, iy, 40, rh, "+", "", "Zoom in (0.1).", 0, a->zoom < 3.0f, 0);
            ui_add(B_SV_CAM_ZROOM, ix + iw - 104, iy, 104, rh, "Room's zoom", "", "The room's own zoom (scene editor, N: Camera zoom).", a->zoom <= 0.0f, 1, 1);
            iy += rh + 14;
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "SLIDE");
            iy += 20;
            ui_add(B_SV_CAM_SM, ix, iy, 40, rh, "-", "", "A quicker slide.", 0, a->seconds > 0.0f, 0);
            svl(ix + 46, iy, iw - 92, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, a->seconds > 0.0f ? "%.1f s" : "a cut (no slide)", a->seconds);
            ui_add(B_SV_CAM_SP, ix + iw - 40, iy, 40, rh, "+", "", "A slower slide.", 0, a->seconds < 10.0f, 0);
            iy += rh + 12;
            svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "The view stays there after the script (the player can scroll it again); a new room starts with its own zoom.");
            break;
        }
        case ACT_ROOM: {
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "ROOM");
            iy += 20;
            svl(ix, iy, iw, 20, 0, a->file[0] ? SVL_VALUE : RGB(255, 150, 120), SV_ONE, "%s", a->file[0] ? a->file : "not chosen yet");
            iy += 26;
            ui_add(B_SV_RM_ROOM, ix, iy, iw, rh, "Choose room...", "", "The room David goes to (type to filter).", 0, 1, 0);
            iy += rh + 12;
            if (a->file[0]) {
                room_info(a->file);
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "DAVID COMES IN BY");
                iy += 22;
                ui_add(B_SV_RM_DOOR, ix, iy, iw, rh - 4, "Where he'd spawn (no connector)", "", "He appears where he does when the room is loaded from the room list.", a->door == 0, 1, 3);
                iy += rh;
                for (int k = 0; k < g_rinfo.nd && k < 10; k++) {
                    snprintf(t, sizeof(t), "Connector #%d  (from %s)", g_rinfo.door_id[k], g_rinfo.door_to[k]);
                    ui_addf(B_SV_RM_DOOR + 1 + k, ix, iy, iw, rh - 4, t, "", "He appears on this connector's doorstep and walks to its arrival point.", a->door == g_rinfo.door_id[k], 1, 3);
                    iy += rh;
                }
                iy += 10;
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "THEN PLAY");
                iy += 22;
                ui_add(B_SV_RM_SCRIPT, ix, iy, iw, rh - 4, "Its auto script (if any)", "", "As when David enters a room without a connector script.", a->script[0] == 0, 1, 3);
                iy += rh;
                for (int k = 0; k < g_rinfo.ns && k < 10; k++) {
                    ui_addf(B_SV_RM_SCRIPT + 1 + k, ix, iy, iw, rh - 4, g_rinfo.script[k], "", "This script of that room plays when he arrives.", !strcmp(a->script, g_rinfo.script[k]), 1, 3);
                    iy += rh;
                }
                iy += 10;
            }
            svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "Like going through a connector (fade, loading, fade in). This script ends here: the rows after it don't play.");
            break;
        }
        case ACT_ANIM: case ACT_SPEAK: {
            iy = sv_actor_buttons(s, a, ix, iy, iw, rh);
            CharModel *m = run_actor_model(s, a->actor);
            if (type == ACT_ANIM) {
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "HOW LONG");
                iy += 20;
                ui_add(B_SV_AN_TIMES, ix, iy, half, rh, "A number of times", "", "It plays the animation that many times; the row waits for it.", a->anim_mode == ANIM_TIMES, 1, 3);
                ui_add(B_SV_AN_ROW, ix + half + 6, iy, half, rh, "While the row lasts", "", "It plays the animation over and over until the other actions of the row are over (e.g. a line being said), then stops.", a->anim_mode == ANIM_ROW, 1, 3);
                iy += rh + 6;
                ui_add(B_SV_AN_LOOP, ix, iy, half, rh, "Loop until replaced", "", "It plays the animation over and over -- after this row and this script too -- until another animation replaces it, a 'Back to normal', or the character is told to move. The row goes on at once.", a->anim_mode == ANIM_LOOP, 1, 3);
                ui_add(B_SV_AN_NORMAL, ix + half + 6, iy, half, rh, "Back to normal", "", "No animation: the character stops the one it plays (e.g. a loop) and goes back to its own behaviour.", a->anim_mode == ANIM_NORMAL, 1, 3);
                iy += rh + 8;
                if (a->anim_mode != ANIM_NORMAL) {
                    iy += 6;
                    svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "ANIMATION");
                    iy += 20;
                    svl(ix, iy, iw, 20, 0, a->file[0] ? SVL_VALUE : RGB(255, 150, 120), SV_ONE, "%s", a->file[0] ? a->file : "not chosen yet");
                    iy += 26;
                    ui_add(B_SV_AN_CLIP, ix, iy, iw, rh, "Choose animation...", "", "Pick one of the animations made for this character's skeleton (it plays in a preview).", 0, m != NULL, 0);
                    iy += rh + 14;
                    iy = sv_pool_buttons(a, ix, iy, iw, rh, 1);
                    svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "SPEED");
                    iy += 20;
                    ui_add(B_SV_AN_FOLLOW, ix, iy, iw, rh, a->speed > 0.0f ? "[  ]  Follow the general speed" : "[x]  Follow the general speed", "",
                           "Ticked: it plays at the game's animation speed (N: Animation speed). Unticked: at its own speed, set below.", a->speed <= 0.0f, 1, 3);
                    iy += rh + 6;
                    if (a->speed > 0.0f) {
                        ui_add(B_SV_AN_SPD_M, ix, iy, 34, rh, "-", "", "Slower (0.1).", 0, a->speed > 0.1f + 1e-4f, 0);
                        svl(ix + 42, iy, iw - 84, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "x%.1f%s", a->speed, a->speed < 1.0f - 1e-4f ? "  (slower)" : a->speed > 1.0f + 1e-4f ? "  (faster)" : "  (as made)");
                        ui_add(B_SV_AN_SPD_P, ix + iw - 34, iy, 34, rh, "+", "", "Faster (0.1).", 0, a->speed < 4.0f - 1e-4f, 0);
                        iy += rh + 6;
                    }
                    iy += 6;
                }
                if (a->anim_mode == ANIM_TIMES) {
                    ui_add(B_SV_REP_M, ix, iy, 34, rh, "-", "", "Once less.", 0, a->repeat > 1, 0);
                    svl(ix + 42, iy, iw - 84, rh, 0, SVL_VALUE, SV_ONE | DT_CENTER, "%d time%s", a->repeat, a->repeat > 1 ? "s" : "");
                    ui_add(B_SV_REP_P, ix + iw - 34, iy, 34, rh, "+", "", "Once more.", 0, a->repeat < 99, 0);
                    iy += rh + 6;
                    ui_add(B_SV_AN_FREEZE, ix, iy, iw, rh, a->freeze ? "[x]  Freeze on the last frame" : "[  ]  Freeze on the last frame", "",
                           "Ticked: once over, it stays on its last frame until the other actions of the row are over. Not ticked: it goes back to its own pose (unless the next row animates it).",
                           a->freeze, 1, 3);
                    iy += rh + 12;
                }
                svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK,
                    a->anim_mode == ANIM_LOOP ? "It keeps looping after the script ends. Another 'Animate character' replaces it; 'Back to normal' or a move order ends it." :
                    a->anim_mode == ANIM_NORMAL ? "Ends a looped animation (or any this character plays): it goes back to standing, walking... as usual." :
                    "It then goes back to its own pose. The animation wins over its walk while both play.");
            } else {
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "LINE (a sound)");
                iy += 20;
                svl(ix, iy, iw, 20, 0, a->file[0] ? SVL_VALUE : RGB(255, 150, 120), SV_ONE, "%s", a->file[0] ? a->file : "not chosen yet");
                iy += 26;
                ui_add(B_SV_FILE, ix, iy, half, rh, "Choose file...", "", "Choose the line in assets/sound (type to filter, Space to listen).", 0, 1, 0);
                ui_add(B_SV_LISTEN, ix + half + 6, iy, half, rh, audio_preview_playing() ? "Stop listening" : "Listen", "", "Hear it (again: stop).", audio_preview_playing(), a->file[0] != 0, 0);
                iy += rh + 14;
                svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "PORTRAIT (this character's, everywhere)");
                iy += 20;
                int num = m ? portrait_of(m->name) : 0;
                SetRect(&g_sv_port_rc, ix, iy, ix + 88, iy + 120);
                g_sv_port_num = num;
                svl(ix + 100, iy, iw - 100, 20, 0, num ? SVL_VALUE : RGB(255, 150, 120), SV_ONE, num ? "bigports.%d" : "none yet", num);
                ui_add(B_SV_PORTRAIT, ix + 100, iy + 28, iw - 100, rh, "Change portrait...", "", "Pick the portrait this character speaks with -- kept for it everywhere, in every script.", 0, m != NULL, 0);
                iy += 128;
                svl(ix, iy, iw, 60, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK, "The row waits until the line is over (a click skips it and cuts the line). Pair it with 'Animate character' (while the row lasts) to make it talk.");
            }
            break;
        }
        case ACT_MOVE: {
            iy = sv_actor_buttons(s, a, ix, iy, iw, rh);
            svl(ix, iy, iw, 16, 1, SVL_SECTION, SV_ONE, "DESTINATION");
            iy += 20;
            if (a->door) {
                int di = shape_index_by_id(a->door);
                if (di >= 0) snprintf(t, sizeof(t), "connector #%d -> %s", a->door, g_shapes[di].target[0] ? g_shapes[di].target : "(no target room)");
                else snprintf(t, sizeof(t), "connector #%d (not in this room!)", a->door);
                svl(ix, iy, iw, 20, 0, SVL_VALUE, SV_ONE, "%s", t);
            } else if (a->has_pos) svl(ix, iy, iw, 20, 0, SVL_VALUE, SV_ONE, "a point  (%.1f, %.1f, %.1f)", a->pos[0], a->pos[1], a->pos[2]);
            else svl(ix, iy, iw, 20, 0, RGB(255, 150, 120), SV_ONE, "not set yet");
            iy += 26;
            ui_add(B_SV_MV_DEST, ix, iy, iw, rh, "Pick in the room...", "", "Click the floor where it goes, or a connector to make it leave the room through it.", 0, 1, 0);
            iy += rh + 12;
            ui_add(B_SV_MV_WALK, ix, iy, half, rh, "Walk", "", "It walks there.", !a->run, 1, 3);
            ui_add(B_SV_MV_RUN, ix + half + 6, iy, half, rh, "Run", "", "It runs there.", a->run, 1, 3);
            iy += rh + 8;
            ui_add(B_SV_MOVESET, ix, iy, iw, rh, "Moveset of this character...", "", "The animations it walks with (graph), and change them.", 0, 1, 0);
            iy += rh + 12;
            svl(ix, iy, iw, 90, 0, SVL_DIM, DT_LEFT | DT_WORDBREAK,
                "The row can't be skipped: it waits until the character arrives -- through a connector, until David is in the other room (another character leaves the room).");
            break;
        }
    }
}

static void sv_paint(HWND hwnd, HDC hdc) {
    sv_layout(hwnd);
    RECT rc; GetClientRect(hwnd, &rc);
    ui_fill(hdc, &rc, RGB(14, 14, 18));
    Script *s = sv_cur();
    char t[300];
    /* scripts list */
    ui_fill(hdc, &g_sv_list_rc, RGB(22, 22, 28));
    int lrows = (g_sv_list_rc.bottom - g_sv_list_rc.top) / SV_LIST_ROW_H;
    if (g_sv_script < g_sv_list_scroll) g_sv_list_scroll = g_sv_script;
    if (g_sv_script >= g_sv_list_scroll + lrows) g_sv_list_scroll = g_sv_script - lrows + 1;
    for (int r = 0; r < lrows; r++) {
        int k = g_sv_list_scroll + r;
        if (k >= g_script_count) break;
        int y = g_sv_list_rc.top + r * SV_LIST_ROW_H;
        RECT row = { g_sv_list_rc.left, y, g_sv_list_rc.right, y + SV_LIST_ROW_H };
        if (k == g_sv_script) ui_fill(hdc, &row, RGB(60, 50, 10));
        SelectObject(hdc, ui_font(15, k == g_sv_script));
        const char *name = g_scripts[k].name;
        if (k == g_sv_script && g_sv_text) { snprintf(t, sizeof(t), "%s_", g_sv_text_buf); name = t; }
        ui_text(hdc, row.left + 8, y, row.right - row.left - 56, SV_LIST_ROW_H, name, k == g_sv_script ? RGB(255, 230, 90) : RGB(220, 220, 225), SV_ONE);
        if (g_scripts[k].auto_run) { SelectObject(hdc, ui_font(12, 1)); ui_text(hdc, row.right - 48, y, 42, SV_LIST_ROW_H, "AUTO", RGB(120, 230, 140), DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
    }
    if (g_script_count == 0) { SelectObject(hdc, ui_font(14, 0)); ui_text(hdc, g_sv_list_rc.left + 8, g_sv_list_rc.top + 8, g_sv_list_rc.right - g_sv_list_rc.left - 16, 40, "(no script)", SVL_DIM, DT_LEFT | DT_WORDBREAK); }
    if (g_sv_text) {
        SelectObject(hdc, ui_font(12, 1));
        ui_text(hdc, g_sv_list_rc.left, g_sv_list_rc.bottom - 20, g_sv_list_rc.right - g_sv_list_rc.left, 18, "TYPE THE NAME, ENTER / ESC", RGB(255, 210, 60), DT_CENTER | DT_SINGLELINE);
    }

    /* grid */
    ui_fill(hdc, &g_sv_grid_rc, RGB(20, 20, 26));
    if (s) {
        int cw = (g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W) / s->cols;
        SelectObject(hdc, ui_font(12, 1));
        for (int c = 0; c < s->cols; c++) {
            snprintf(t, sizeof(t), "COLUMN %d", c + 1);
            ui_text(hdc, g_sv_grid_rc.left + SV_ROWHDR_W + c * cw, g_sv_grid_rc.top, cw, SV_HDR_H, t, c == g_sv_col ? RGB(255, 210, 90) : SVL_SECTION, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        int vis = sv_grid_rows_visible(), total = sv_grid_rows_total();
        for (int r = g_sv_scroll; r < g_sv_scroll + vis && r < total + vis; r++) {
            int y = g_sv_grid_rc.top + SV_HDR_H + (r - g_sv_scroll) * SV_ROW_H;
            int beyond = r >= total;
            int zlo, zhi; sv_sel_range(&zlo, &zhi);
            if (zhi > zlo && r >= zlo && r <= zhi) { RECT hb = { g_sv_grid_rc.left, y + 2, g_sv_grid_rc.left + SV_ROWHDR_W - 2, y + SV_ROW_H - 2 }; ui_fill(hdc, &hb, RGB(34, 52, 92)); }
            int zrow = script_zone_at(s, r);
            if (zrow >= 0) { /* a timeline row: its time (the zone is drawn over the cells below) */
                const ScriptZone *z = &s->zone[zrow];
                SelectObject(hdc, ui_font(12, 1));
                snprintf(t, sizeof(t), "%.2gs", (r - z->row0) * z->sec_per_row);
                ui_text(hdc, g_sv_grid_rc.left, y + 1, SV_ROWHDR_W - 6, 14, t, RGB(110, 200, 200), DT_RIGHT | DT_SINGLELINE);
                SelectObject(hdc, ui_font(11, 0));
                snprintf(t, sizeof(t), "%d", r + 1);
                ui_text(hdc, g_sv_grid_rc.left, y + SV_ROW_H - 16, SV_ROWHDR_W - 6, 13, t, r == g_sv_row ? RGB(255, 210, 90) : RGB(90, 130, 130), DT_RIGHT | DT_SINGLELINE);
                continue;
            }
            SelectObject(hdc, ui_font(15, 1));
            snprintf(t, sizeof(t), "%d", r + 1);
            ui_text(hdc, g_sv_grid_rc.left, y, SV_ROWHDR_W - 6, SV_ROW_H - 14, t, beyond ? RGB(70, 70, 80) : r == g_sv_row ? RGB(255, 210, 90) : RGB(170, 170, 180), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            if (!beyond && script_row_has_move(s, r)) { SelectObject(hdc, ui_font(11, 0)); ui_text(hdc, g_sv_grid_rc.left, y + SV_ROW_H - 18, SV_ROWHDR_W - 6, 14, "no skip", RGB(150, 130, 90), DT_RIGHT | DT_SINGLELINE); }
            for (int c = 0; c < s->cols; c++) {
                RECT cr; sv_cell_rect(r, c, &cr);
                ScriptAction *a = script_at(s, r, c);
                int sel = r == g_sv_row && c == g_sv_col, hov = g_sv_hover == r * SCRIPT_MAX_COLS + c;
                if (a && a->type != ACT_NONE) {
                    ui_fill(hdc, &cr, hov ? RGB(46, 46, 56) : RGB(36, 36, 44));
                    RECT st = { cr.left, cr.top, cr.left + 5, cr.bottom };
                    ui_fill(hdc, &st, action_color(a->type));
                    SelectObject(hdc, ui_font(13, 1));
                    ui_text(hdc, cr.left + 12, cr.top + 3, cr.right - cr.left - 16, 18, action_type_name(a->type), action_color(a->type), SV_ONE);
                    action_summary(s, a, t, sizeof(t));
                    SelectObject(hdc, ui_font(14, 0));
                    ui_text(hdc, cr.left + 12, cr.top + 20, cr.right - cr.left - 16, 20, t, RGB(225, 225, 230), SV_ONE);
                } else {
                    ui_fill(hdc, &cr, beyond ? RGB(22, 22, 28) : RGB(27, 27, 33));
                    if (sel || hov) { SelectObject(hdc, ui_font(14, 0)); ui_text(hdc, cr.left, cr.top, cr.right - cr.left, cr.bottom - cr.top, "+ add (double-click)", RGB(110, 110, 125), DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
                }
                int grp = a && a->type != ACT_NONE && g_sv_sel_n > 1 && sv_sel_has(a->id);
                if (grp && !sel) { ui_frame(hdc, &cr, RGB(255, 160, 60)); RECT in = cr; InflateRect(&in, -1, -1); ui_frame(hdc, &in, RGB(255, 160, 60)); }
                if (sel) { ui_frame(hdc, &cr, RGB(255, 210, 60)); RECT in = cr; InflateRect(&in, -1, -1); ui_frame(hdc, &in, RGB(255, 210, 60)); }
            }
        }
        sv_paint_zones(hdc, s);
        if (g_sv_cdrag && (g_sv_cdrag_dr || g_sv_cdrag_dc)) { /* where the group would go: green, red if it can't */
            int ok = sv_group_move_cells(s, g_sv_cdrag_dr, g_sv_cdrag_dc, 0);
            for (int i = 0; i < g_sv_sel_n; i++) {
                int rr, cc;
                if (!script_find(s, g_sv_sel_ids[i], &rr, &cc)) continue;
                RECT dr; sv_cell_rect(rr + g_sv_cdrag_dr, cc + g_sv_cdrag_dc, &dr);
                if (dr.bottom < g_sv_grid_rc.top + SV_HDR_H || dr.top > g_sv_grid_rc.bottom) continue;
                COLORREF gc = ok ? RGB(110, 230, 140) : RGB(255, 100, 90);
                ui_frame(hdc, &dr, gc); InflateRect(&dr, -2, -2); ui_frame(hdc, &dr, gc);
            }
        }
        if (g_sv_band == 2) { /* the selecting rectangle */
            RECT br = { g_sv_band_x0 < g_sv_band_x1 ? g_sv_band_x0 : g_sv_band_x1, g_sv_band_y0 < g_sv_band_y1 ? g_sv_band_y0 : g_sv_band_y1,
                        g_sv_band_x0 > g_sv_band_x1 ? g_sv_band_x0 : g_sv_band_x1, g_sv_band_y0 > g_sv_band_y1 ? g_sv_band_y0 : g_sv_band_y1 };
            ui_frame(hdc, &br, RGB(120, 170, 255));
        }
        if (g_sv_sel_n > 1) {
            SelectObject(hdc, ui_font(12, 1));
            snprintf(t, sizeof(t), "%d ACTIONS SELECTED -- DRAG ONE TO MOVE THEM ALL, DEL: CLEAR THEM, ESC: UNSELECT", g_sv_sel_n);
            ui_text(hdc, g_sv_grid_rc.left + SV_ROWHDR_W, g_sv_grid_rc.top, g_sv_grid_rc.right - g_sv_grid_rc.left - SV_ROWHDR_W, SV_HDR_H, t, RGB(255, 170, 80), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        /* scrollbar */
        int tot = total + 1;
        if (tot > vis) {
            int th = g_sv_grid_rc.bottom - g_sv_grid_rc.top - SV_HDR_H;
            int bh = th * vis / tot; if (bh < 24) bh = 24;
            int by = g_sv_grid_rc.top + SV_HDR_H + (th - bh) * g_sv_scroll / (tot - vis > 0 ? tot - vis : 1);
            RECT sb = { g_sv_grid_rc.right - 6, by, g_sv_grid_rc.right - 2, by + bh };
            ui_fill(hdc, &sb, RGB(90, 90, 100));
        }
    }

    /* inspector */
    ui_fill(hdc, &g_sv_insp_rc, RGB(22, 22, 28));
    ui_frame(hdc, &g_sv_insp_rc, RGB(50, 50, 60));
    ScriptAction *a = sv_cell(0);
    if (g_sv_thumb_rc.right > g_sv_thumb_rc.left && a) blit_picture(hdc, &g_sv_thumb_rc, a->file);
    if (g_sv_ovprev_rc.right > g_sv_ovprev_rc.left && a) sv_overlay_preview(hdc, &g_sv_ovprev_rc, a);
    if (g_sv_port_rc.right > g_sv_port_rc.left) {
        static uint32_t buf[88 * 120];
        int pw, ph;
        const uint32_t *img = portrait_pixels(g_sv_port_num, &pw, &ph);
        for (int i = 0; i < 88 * 120; i++) buf[i] = 0x14141A;
        if (img) blend_sprite(buf, 88, 120, img, pw, ph, (88 - pw * 2) / 2, 0, 2.0f);
        blit_pixels(hdc, g_sv_port_rc.left, g_sv_port_rc.top, 88, 120, buf);
        ui_frame(hdc, &g_sv_port_rc, RGB(200, 160, 64));
    }
    svl_draw(hdc);
    ui_draw_buttons(hdc);
    /* help: hovered button, then the status line */
    int hy = g_sv_insp_rc.bottom - 110;
    const UiButton *hb = ui_find(g_hover_btn);
    SelectObject(hdc, ui_font(14, 0));
    if (hb && hb->desc && hb->desc[0]) {
        RECT hr = { g_sv_insp_rc.left + 1, hy - 4, g_sv_insp_rc.right - 1, g_sv_insp_rc.bottom - 1 };
        ui_fill(hdc, &hr, RGB(28, 28, 36));
        ui_para(hdc, g_sv_insp_rc.left + 12, hy, g_sv_insp_rc.right - g_sv_insp_rc.left - 24, hb->desc, RGB(230, 230, 235));
    } else if (g_status[0]) ui_para(hdc, g_sv_insp_rc.left + 12, hy + 40, g_sv_insp_rc.right - g_sv_insp_rc.left - 24, g_status, RGB(150, 150, 150));

    if (g_ch == CH_NONE) sv_paint_zone_tip(hdc);
    /* chooser, over the grid and the inspector */
    if (g_ch != CH_NONE) {
        int pic = g_ch == CH_PICTURE, lw = g_ch_list_rc.right - g_ch_list_rc.left;
        ui_fill(hdc, &g_ch_rc, RGB(16, 16, 22));
        ui_frame(hdc, &g_ch_rc, RGB(255, 210, 60));
        SelectObject(hdc, ui_font(20, 1));
        if (g_ch == CH_CLIP) snprintf(t, sizeof(t), "Choose an animation for %s", g_ch_model ? g_ch_model->name : "?");
        else if (g_ch == CH_ROOM) snprintf(t, sizeof(t), "Choose the room to go to");
        else if (g_ch == CH_PORTRAIT) snprintf(t, sizeof(t), "Choose the portrait of %s (kept for this character everywhere)", g_ch_model ? g_ch_model->name : "?");
        else snprintf(t, sizeof(t), "%s", pic ? "Choose a picture" : g_ch == CH_TRACK ? "Add a music track" : "Choose a sound");
        ui_text(hdc, g_ch_rc.left + 12, g_ch_rc.top + 8, 900, 26, t, RGB(255, 225, 120), SV_ONE);
        SelectObject(hdc, ui_font(14, 0));
        snprintf(t, sizeof(t), "filter: %s_   (%d %s)", g_ch_filter, g_ch_n, g_ch == CH_CLIP ? "animations" : g_ch == CH_PORTRAIT ? "portraits" : "files");
        ui_text(hdc, g_ch_rc.left + 12, g_ch_rc.top + 40, 400, 20, t, RGB(200, 200, 200), SV_ONE);
        SelectObject(hdc, ui_font(12, 1));
        ui_text(hdc, g_ch_rc.right - 620, g_ch_rc.top + 12, 606, 16,
                (g_ch == CH_SOUND || g_ch == CH_TRACK) ? "TYPE = FILTER   SPACE = LISTEN   ENTER / DOUBLE-CLICK = CHOOSE   ESC = CANCEL" : "TYPE = FILTER   ARROWS = SELECT   ENTER / DOUBLE-CLICK = CHOOSE   ESC = CANCEL",
                SVL_SECTION, DT_RIGHT | DT_SINGLELINE);
        ui_fill(hdc, &g_ch_list_rc, RGB(24, 24, 30));
        int rows = ch_grid() ? 0 : ch_rows_visible();
        if (ch_grid()) { /* portraits: a grid of the pictures themselves */
            int cols = ch_cols(), grows = (g_ch_list_rc.bottom - g_ch_list_rc.top) / PORT_CELL_H;
            int current = g_ch_model ? portrait_of(g_ch_model->name) : 0;
            for (int r = 0; r < grows; r++) for (int c = 0; c < cols; c++) {
                int k = (g_ch_scroll + r) * cols + c;
                if (k >= g_ch_n) break;
                ChEntry *e = &g_ch_all[g_ch_list[k]];
                int num = atoi(e->name + 9), pw, ph;
                RECT cell = { g_ch_list_rc.left + c * PORT_CELL_W + 4, g_ch_list_rc.top + r * PORT_CELL_H + 4,
                              g_ch_list_rc.left + (c + 1) * PORT_CELL_W - 4, g_ch_list_rc.top + (r + 1) * PORT_CELL_H - 4 };
                ui_fill(hdc, &cell, k == g_ch_sel ? RGB(70, 58, 12) : RGB(30, 30, 38));
                const uint32_t *img = portrait_pixels(num, &pw, &ph);
                if (img) {
                    static uint32_t buf[88 * 120];
                    for (int i = 0; i < 88 * 120; i++) buf[i] = k == g_ch_sel ? 0x463A0C : 0x1E1E26;
                    blend_sprite(buf, 88, 120, img, pw, ph, (88 - pw * 2) / 2, 0, 2.0f);
                    blit_pixels(hdc, cell.left + (cell.right - cell.left - 88) / 2, cell.top + 4, 88, 120, buf);
                }
                SelectObject(hdc, ui_font(12, num == current));
                snprintf(t, sizeof(t), "%d%s", num, num == current ? "  (now)" : "");
                ui_text(hdc, cell.left, cell.bottom - 18, cell.right - cell.left, 16, t, num == current ? RGB(120, 230, 140) : RGB(190, 190, 200), DT_CENTER | DT_SINGLELINE);
                if (k == g_ch_sel) ui_frame(hdc, &cell, RGB(255, 210, 60));
            }
        }
        for (int r = 0; r < rows; r++) {
            int k = g_ch_scroll + r;
            if (k >= g_ch_n) break;
            ChEntry *e = &g_ch_all[g_ch_list[k]];
            int y = g_ch_list_rc.top + r * CH_ROW_H;
            if (e->header) { /* a sound category */
                RECT hr = { g_ch_list_rc.left, y + 2, g_ch_list_rc.right, y + CH_ROW_H };
                ui_fill(hdc, &hr, RGB(32, 36, 48));
                SelectObject(hdc, ui_font(12, 1));
                ui_text(hdc, g_ch_list_rc.left + 8, y + 2, lw - 16, CH_ROW_H - 2, e->name, RGB(140, 180, 240), SV_ONE);
                continue;
            }
            if (k == g_ch_sel) { RECT hl = { g_ch_list_rc.left, y, g_ch_list_rc.right, y + CH_ROW_H }; ui_fill(hdc, &hl, RGB(60, 50, 10)); }
            SelectObject(hdc, ui_font(14, 0));
            ui_text(hdc, g_ch_list_rc.left + (g_ch == CH_SOUND || g_ch == CH_TRACK ? 20 : 8), y, lw - 110, CH_ROW_H,
                    (g_ch == CH_SOUND || g_ch == CH_TRACK) ? sound_shown(e->name) : e->name, k == g_ch_sel ? RGB(255, 230, 60) : RGB(215, 215, 220), SV_ONE);
            if (g_ch == CH_ROOM) {
            } else if (g_ch == CH_CLIP) {
                snprintf(t, sizeof(t), "%.2f s", e->secs);
                ui_text(hdc, g_ch_list_rc.right - 100, y, 92, CH_ROW_H, t, RGB(130, 130, 140), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            } else if (pic) {
                if (e->w < 0) png_size(e->name, &e->w, &e->h);
                snprintf(t, sizeof(t), "%dx%d", e->w, e->h);
                int same = e->w == (int)g_hdr.width && e->h == (int)g_hdr.height;
                ui_text(hdc, g_ch_list_rc.right - 100, y, 92, CH_ROW_H, t, same ? RGB(120, 220, 140) : RGB(130, 130, 140), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            } else {
                if (e->secs < -1.5f) e->secs = audio_file_seconds(e->name);
                if (e->secs >= 0) snprintf(t, sizeof(t), "%d:%04.1f", (int)(e->secs / 60), fmodf(e->secs, 60.0f)); else snprintf(t, sizeof(t), "?");
                ui_text(hdc, g_ch_list_rc.right - 100, y, 92, CH_ROW_H, t, RGB(130, 130, 140), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
        if (g_ch == CH_CLIP && g_ch_model && ch_current()) { /* the animation playing on the character */
            int clip = anim_lib_find_ref(ch_current());
            int W = g_ch_prev_rc.right - g_ch_prev_rc.left, H = g_ch_prev_rc.bottom - g_ch_prev_rc.top;
            if (W > 16 && H > 16) {
                static uint32_t *pv = NULL; static size_t cap = 0;
                if (cap < (size_t)W * H) { free(pv); pv = (uint32_t *)malloc((size_t)W * H * 4); cap = (size_t)W * H; }
                float dur = anim_lib_duration(clip);
                render_char_view(g_ch_model, model_right_hand(g_ch_model), model_left_shield(g_ch_model), clip, dur > 0 ? fmodf(g_ch_t, dur) : 0.0f, pv, W, H, ANIM_DEFAULT_YAW, 0.15f, 1.0f, 1, 1);
                blit_pixels(hdc, g_ch_prev_rc.left, g_ch_prev_rc.top, W, H, pv);
                SelectObject(hdc, ui_font(16, 1));
                ui_text(hdc, g_ch_prev_rc.left + 12, g_ch_prev_rc.top + 8, W - 24, 22, ch_current(), RGB(255, 255, 255), SV_ONE);
            }
        }
        if (pic) {
            const char *cur = ch_current();
            if (cur) {
                RECT pr = g_ch_prev_rc; pr.bottom = pr.top + (pr.right - pr.left) * 3 / 4;
                if (pr.bottom > g_ch_prev_rc.bottom) pr.bottom = g_ch_prev_rc.bottom;
                blit_picture(hdc, &pr, cur);
                SelectObject(hdc, ui_font(14, 0));
                ChEntry *e = &g_ch_all[g_ch_list[g_ch_sel]];
                snprintf(t, sizeof(t), "%s   %dx%d%s", cur, e->w, e->h, (e->w == (int)g_hdr.width && e->h == (int)g_hdr.height) ? "   (same size as the room)" : "   (will be stretched to the room's size)");
                ui_text(hdc, pr.left, pr.bottom + 6, pr.right - pr.left, 20, t, SVL_TEXT, SV_ONE);
            }
        }
        ui_draw_buttons(hdc);
    }
    SelectObject(hdc, GetStockObject(SYSTEM_FONT));
}

static int sv_key(HWND hwnd, int vk) {
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (g_ch != CH_NONE) {
        int page = ch_rows_visible(), step = ch_grid() ? ch_cols() : 1;
        switch (vk) {
            case VK_ESCAPE: ch_close(); return 1;
            case VK_RETURN: ch_choose(); return 1;
            case VK_UP: ch_select(g_ch_sel - step); return 1;
            case VK_DOWN: ch_select(g_ch_sel + step); return 1;
            case VK_LEFT: if (ch_grid()) ch_select(g_ch_sel - 1); return 1;
            case VK_RIGHT: if (ch_grid()) ch_select(g_ch_sel + 1); return 1;
            case VK_PRIOR: ch_select(g_ch_sel - page); return 1;
            case VK_NEXT: ch_select(g_ch_sel + page); return 1;
            case VK_HOME: ch_select(0); return 1;
            case VK_END: ch_select(g_ch_n - 1); return 1;
            case VK_SPACE: if (g_ch == CH_SOUND || g_ch == CH_TRACK) sv_action(hwnd, B_CH_LISTEN); return 1;
            case VK_BACK: if (g_ch_filter_len > 0) { g_ch_filter[--g_ch_filter_len] = 0; ch_filter(); } return 1;
        }
        return 0;
    }
    if (g_sv_text) {
        if (vk == VK_RETURN) sv_text_commit();
        else if (vk == VK_ESCAPE) g_sv_text = 0;
        else if (vk == VK_BACK) { size_t l = strlen(g_sv_text_buf); if (l) g_sv_text_buf[l - 1] = 0; }
        return 1;
    }
    Script *s = sv_cur();
    switch (vk) {
        case VK_ESCAPE: if (g_sv_sel_n > 1) { g_sv_sel_n = 0; return 1; } script_view_toggle(); return 1;
        case VK_F2: sv_action(hwnd, B_SV_RENAME); return 1;
        case VK_F5: sv_action(hwnd, B_SV_PLAY); return 1;
        case VK_F6: sv_action(hwnd, B_SV_PLAY_ROW); return 1;
        case VK_UP: if (ctrl) { if (g_sv_script > 0) { g_sv_script--; sv_select(0, 0); g_sv_scroll = 0; } } else { sv_select(g_sv_row - 1, g_sv_col); sv_sel_after_nav(); } return 1;
        case VK_DOWN: if (ctrl) { if (g_sv_script < g_script_count - 1) { g_sv_script++; sv_select(0, 0); g_sv_scroll = 0; } } else { sv_select(g_sv_row + 1, g_sv_col); sv_sel_after_nav(); } return 1;
        case VK_LEFT: sv_select(g_sv_row, g_sv_col - 1); sv_sel_after_nav(); return 1;
        case VK_RIGHT: sv_select(g_sv_row, g_sv_col + 1); sv_sel_after_nav(); return 1;
        case VK_PRIOR: sv_select(g_sv_row - sv_grid_rows_visible(), g_sv_col); sv_sel_after_nav(); return 1;
        case VK_NEXT: sv_select(g_sv_row + sv_grid_rows_visible(), g_sv_col); sv_sel_after_nav(); return 1;
        case VK_HOME: sv_select(0, g_sv_col); sv_sel_after_nav(); return 1;
        case VK_INSERT: sv_action(hwnd, B_SV_ROW_INS); return 1;
        case VK_DELETE: sv_action(hwnd, shift ? B_SV_ROW_DEL : B_SV_CLEAR); return 1;
        case VK_RETURN: if (s) sv_action(hwnd, B_SV_TYPE); return 1;
        case 'C': if (ctrl) sv_action(hwnd, B_SV_COPY); return 1;
        case 'V': if (ctrl) sv_action(hwnd, B_SV_PASTE); return 1;
    }
    return 0;
}
static void sv_char(char c) {
    if (g_ch != CH_NONE) {
        if (c > 32 && c < 127 && g_ch_filter_len < (int)sizeof(g_ch_filter) - 1) { g_ch_filter[g_ch_filter_len++] = c; g_ch_filter[g_ch_filter_len] = 0; ch_filter(); }
        return;
    }
    if (g_sv_text) {
        size_t l = strlen(g_sv_text_buf);
        if (c >= 32 && c < 127 && l < sizeof(g_sv_text_buf) - 1) { g_sv_text_buf[l] = c; g_sv_text_buf[l + 1] = 0; }
    }
}
static void sv_mouse_down(HWND hwnd, int x, int y, int dbl) {
    sv_layout(hwnd);
    if (g_ch != CH_NONE) {
        int b = ui_hit(x, y);
        if (b != B_NONE) { sv_action(hwnd, b); return; }
        if (ch_grid()) {
            int k = ch_grid_at(x, y);
            if (k >= 0) { ch_select(k); if (dbl) ch_choose(); }
            return;
        }
        if (x >= g_ch_list_rc.left && x < g_ch_list_rc.right && y >= g_ch_list_rc.top && y < g_ch_list_rc.bottom) {
            int k = g_ch_scroll + (y - g_ch_list_rc.top) / CH_ROW_H;
            if (k < g_ch_n && !g_ch_all[g_ch_list[k]].header) {
                int again = k == g_ch_sel;
                ch_select(k);
                if (dbl) ch_choose();
                else if ((g_ch == CH_SOUND || g_ch == CH_TRACK) && again) sv_action(hwnd, B_CH_LISTEN);
            }
        }
        return;
    }
    if (g_sv_text && !(x >= g_sv_list_rc.left && x < g_sv_list_rc.right)) sv_text_commit();
    int b = ui_hit(x, y);
    if (b != B_NONE) { ui_action(hwnd, b); return; }
    if (x >= g_sv_list_rc.left && x < g_sv_list_rc.right && y >= g_sv_list_rc.top && y < g_sv_list_rc.bottom) {
        int k = g_sv_list_scroll + (y - g_sv_list_rc.top) / SV_LIST_ROW_H;
        if (k < g_script_count) {
            if (k != g_sv_script) { if (g_sv_text) sv_text_commit(); g_sv_script = k; g_sv_row = g_sv_col = g_sv_scroll = 0; }
            else if (dbl) sv_action(hwnd, B_SV_RENAME);
        }
        return;
    }
    int r, c, zi;
    float zt;
    Script *s = sv_cur();
    {
        int er, ec, e = sv_event_edge_at(x, y, &er, &ec);
        if (e) { /* an action's edge: drag it to trim / lengthen it */
            sv_select(er, ec);
            ScriptAction *a = sv_cell(0);
            int k;
            g_sv_rsz = e; g_sv_drag_moved = 0;
            g_sv_rsz_start = a->start; g_sv_rsz_in = a->trim_in; g_sv_rsz_out = a->trim_out; g_sv_rsz_sec = a->seconds;
            g_sv_rsz_len = sv_event_len(s, script_zone_at(s, er), a, &k);
            g_sv_rsz_last = e == 1 ? a->start : a->start + g_sv_rsz_len;
            g_sv_drag_free = sv_ids_collide(s, script_zone_at(s, er), &a->id, 1);
            SetCapture(hwnd);
            return;
        }
    }
    int edge = sv_zone_edge_at(x, y);
    if (edge >= 0) { /* its bottom edge: drag it to make it taller / shorter */
        g_sv_zdrag = edge + 1; g_sv_drag_moved = 0;
        SetCapture(hwnd);
        return;
    }
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (sv_zone_hit(x, y, &zi, &r, &c, &zt)) { /* a timeline: an action (drag it, and its group), or a new one there */
        if (r >= 0) {
            sv_select(r, c);
            if (ctrl || shift) { sv_sel_toggle(s, r, c); return; }
            ScriptAction *a = script_at(s, r, c);
            if (!sv_sel_has(a->id)) sv_sel_one(s, r, c);
            g_sv_drag = 1; g_sv_drag_y0 = y; g_sv_drag_moved = 0; g_sv_drag_id = a->id; g_sv_drag_c0 = c; g_sv_gdc = 0; g_sv_last_dt = 0.0f;
            for (int i = 0; i < g_sv_sel_n; i++) {
                int rr, cc;
                ScriptAction *b = script_find(s, g_sv_sel_ids[i], &rr, &cc);
                g_sv_gt0[i] = b ? b->start : 0.0f; g_sv_glane0[i] = cc;
            }
            g_sv_drag_free = sv_ids_collide(s, zi, g_sv_sel_ids, g_sv_sel_n);
            SetCapture(hwnd);
            return;
        }
        if (!ctrl) g_sv_sel_n = 0;
        int rows0 = s->rows, fr = script_zone_free_row(s, zi, c);
        if (fr < 0) return;
        if (s->rows != rows0) sv_changed();
        sv_select(fr, c);
        g_sv_pending_start = zt;
        if (dbl) { sv_cell_menu(hwnd, x, y); return; }
        g_sv_band = 1; g_sv_band_zone = zi; g_sv_band_x0 = g_sv_band_x1 = x; g_sv_band_y0 = g_sv_band_y1 = y; /* or a rectangle to select */
        SetCapture(hwnd);
        return;
    }
    if (sv_cell_at(x, y, &r, &c)) {
        sv_select(r, c);
        ScriptAction *a = sv_cell(0);
        int filled = a && a->type != ACT_NONE;
        if (ctrl) { sv_sel_toggle(s, r, c); return; }
        if (shift) { int lo, hi; sv_sel_range(&lo, &hi); sv_sel_rows(s, lo, hi); return; }
        if (filled) { /* drag it (and its group) to other cells */
            if (!sv_sel_has(a->id)) sv_sel_one(s, r, c);
            if (dbl) return;
            g_sv_cdrag = 1; g_sv_cdrag_r0 = r; g_sv_cdrag_c0 = c; g_sv_cdrag_dr = g_sv_cdrag_dc = 0;
            SetCapture(hwnd);
            return;
        }
        g_sv_sel_n = 0;
        if (dbl) { sv_cell_menu(hwnd, x, y); return; }
        g_sv_band = 1; g_sv_band_zone = -1; g_sv_band_x0 = g_sv_band_x1 = x; g_sv_band_y0 = g_sv_band_y1 = y;
        SetCapture(hwnd);
    }
}
static void sv_mouse_move(int x, int y) {
    int r, c, zi;
    float zt;
    if (g_sv_cdrag) {
        if (sv_cell_at(x, y, &r, &c)) { g_sv_cdrag_dr = r - g_sv_cdrag_r0; g_sv_cdrag_dc = c - g_sv_cdrag_c0; }
        g_sv_hover = -1;
        return;
    }
    if (g_sv_band) {
        g_sv_band_x1 = x; g_sv_band_y1 = y;
        if (g_sv_band == 1 && (abs(x - g_sv_band_x0) > 4 || abs(y - g_sv_band_y0) > 4)) g_sv_band = 2;
        if (g_sv_band == 2) { Script *s = sv_cur(); if (s) sv_band_select(s); }
        g_sv_hover = -1;
        return;
    }
    if (g_sv_zdrag) { sv_zone_edge_move(y); g_sv_hover = -1; return; }
    if (g_sv_rsz) { sv_resize_move(y); g_sv_hover = -1; return; }
    {
        int er, ec, e = g_sv_drag ? 0 : sv_event_edge_at(x, y, &er, &ec);
        g_sv_edge_hover = e ? (er * SCRIPT_MAX_COLS + ec) * 4 + e : -1;
    }
    if (g_sv_drag) sv_drag_move(x, y);
    g_sv_zedge_hover = g_sv_drag ? -1 : sv_zone_edge_at(x, y);
    if (g_sv_zedge_hover >= 0) { g_sv_hover = -1; return; }
    if (sv_zone_hit(x, y, &zi, &r, &c, &zt)) { g_sv_hover = r >= 0 ? r * SCRIPT_MAX_COLS + c : -1; return; }
    g_sv_hover = sv_cell_at(x, y, &r, &c) ? r * SCRIPT_MAX_COLS + c : -1;
}
static void sv_right_click(HWND hwnd, int x, int y) {
    sv_layout(hwnd);
    if (g_ch != CH_NONE) return;
    int r, c, zi;
    float zt;
    Script *s = sv_cur();
    if (sv_zone_hit(x, y, &zi, &r, &c, &zt)) {
        if (r >= 0) sv_select(r, c);
        else {
            int rows0 = s->rows, fr = script_zone_free_row(s, zi, c);
            if (fr < 0) return;
            if (s->rows != rows0) sv_changed();
            sv_select(fr, c);
            g_sv_pending_start = zt;
        }
        sv_cell_menu(hwnd, x, y);
        return;
    }
    if (sv_cell_at(x, y, &r, &c)) { sv_select(r, c); sv_cell_menu(hwnd, x, y); }
}
static void sv_wheel(int x, int y, int delta) {
    int n = delta / 120;
    if (g_ch != CH_NONE) {
        if (ch_grid()) {
            int cols = ch_cols(), total = (g_ch_n + cols - 1) / cols, vis = (g_ch_list_rc.bottom - g_ch_list_rc.top) / PORT_CELL_H;
            g_ch_scroll -= n;
            if (g_ch_scroll > total - vis) g_ch_scroll = total - vis;
            if (g_ch_scroll < 0) g_ch_scroll = 0;
            return;
        }
        g_ch_scroll -= n * 3;
        if (g_ch_scroll > g_ch_n - ch_rows_visible()) g_ch_scroll = g_ch_n - ch_rows_visible();
        if (g_ch_scroll < 0) g_ch_scroll = 0;
        return;
    }
    if (x >= g_sv_list_rc.left && x < g_sv_list_rc.right) {
        g_sv_list_scroll -= n;
        if (g_sv_list_scroll < 0) g_sv_list_scroll = 0;
        return;
    }
    (void)y;
    g_sv_scroll -= n * 2;
    int maxs = sv_grid_rows_total() + 1 - sv_grid_rows_visible();
    if (g_sv_scroll > maxs) g_sv_scroll = maxs;
    if (g_sv_scroll < 0) g_sv_scroll = 0;
}

/* =====================================================================
   MOVESET SCREEN: the animations a character walks with, as a graph
   (stand -> start -> walk / run cycles -> turns), the selected slot's
   clip playing on the character, and the clips it can use instead.
   ===================================================================== */
enum { B_MS_FIRST = 900, B_MS_CLOSE = B_MS_FIRST, B_MS_NONE, B_MS_PRESET, B_MS_USE, B_MS_TAB_WALK, B_MS_TAB_COMBAT, B_MS_TAB_SOUND,
       B_MS_TAB_REACT, B_MS_TAB_AI, B_MS_TAB_STATS,
       B_MS_AI_NONE, B_MS_AI_MELEE, B_MS_AI_RANGE_M, B_MS_AI_RANGE_P, B_MS_AI_WALK, B_MS_AI_RUN, B_MS_AI_PMIN_M, B_MS_AI_PMIN_P,
       B_MS_AI_PMAX_M, B_MS_AI_PMAX_P, B_MS_AI_DODGE_M, B_MS_AI_DODGE_P, B_MS_AI_RESET,
       B_MS_ST_HP_M10, B_MS_ST_HP_M1, B_MS_ST_HP_P1, B_MS_ST_HP_P10, B_MS_ST_DMG_M, B_MS_ST_DMG_P, B_MS_ST_BOSS, B_MS_ST_RESET,
       B_MS_TAB_PRESET, B_MS_MODEL_PRESET, B_MS_WEAPON, B_MS_SHIELD, B_MS_ITEM_NONE, B_MS_LIST_BACK, B_MS_STEP_M, B_MS_STEP_P, B_MS_STEP_PRESET,
       B_MS_CHANCE_M, B_MS_CHANCE_P, B_MS_SAVE_OK, B_MS_SAVE_CANCEL, B_MS_SAVE_DEFAULT, B_MS_SOUNDS_RESET,
       B_MS_CUES, B_MS_CUE_SOUND, B_MS_CUE_SWING, B_MS_CUE_WAIT, B_MS_CUE_PLAY, B_MS_CUE_RESET, B_MS_CUE_DONE,
       B_MS_POOL = 2000 /* + pool * 100: the pool (select); + 1 + i: listen to sound i; + 50 + i: remove it; + 99: add */,
       B_MS_CUE_ROW = 2400 /* + row * 10 + CUEOP_*: a step of the blow's sounds */,
       B_MS_LAST = 2999 };
_Static_assert(B_MS_CUE_DONE < 1000, "the moveset screen's buttons must stay under 1000 (1000+: the scripts screen's)");
static int g_ms_tab = 0;          /* PG_WALK, PG_COMBAT, PG_SOUND, PG_REACT, PG_AI, PG_STATS */
static int g_ms_items = 0;        /* the list shows the items: 1 for the weapon, 2 for the shield */
static int g_ms_sounds = 0;       /* the list shows the sounds (the Sounds tab) */
static int g_ms_pool = POOL_SWING;/* the pool a sound is added to */
static int g_ms_text = 0;         /* saving a preset: its group + 1, while its name is typed */
static char g_ms_text_buf[48];
static int g_ms_text_default = 1; /* ...and it becomes the character's default */
static int g_ms_cues = 0;         /* the Combat tab edits the selected blow's sounds (its timeline) */
static int g_ms_cue_sel = -1;     /* the step selected (new ones go after it) */
static CuePlayer g_ms_cplay;      /* "Play with sounds": the blow from its start, its sounds in step */
static float g_ms_cplay_t = -1.0f;/* ...its time, -1 = not playing */
static RECT g_ms_ruler_rc;
enum { CUEOP_SELECT, CUEOP_UP, CUEOP_DOWN, CUEOP_DEL, CUEOP_M1, CUEOP_M01, CUEOP_P01, CUEOP_P1, CUEOP_LISTEN };
static SoundRow *g_ms_snd = NULL; /* the sound list's rows */
static int g_ms_snd_n = 0;
/* the items of assets/chars/items (a .gltf with its texture: the few without are item animations) */
static char (*g_item_names)[48] = NULL;
static int g_item_count = -1;
static void items_scan(void);
/* Character settings: a weapon / shield from the items (or none); 1 if one was chosen */
static int sv_item_menu(HWND hwnd, int shield, char *out, int n) {
    items_scan();
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, shield ? "The shield it takes out:" : "The weapon it takes out:");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, 1, shield ? "None (it puts its shield away)" : "None (it puts its weapon away)");
    static const char *orbs[8] = { "orb:fire", "orb:ice", "orb:health", "orb:earth", "orb:acid", "orb:lightning", "orb:time", "orb:light" };
    if (!shield) for (int i = 0; i < 8; i++) AppendMenuA(m, MF_STRING, 2 + i, orbs[i]);
    for (int i = 0; i < g_item_count; i++) AppendMenuA(m, MF_STRING | ((i % 30 == 0 && i) ? MF_MENUBARBREAK : 0), 10 + i, g_item_names[i]);
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd <= 0) return 0;
    snprintf(out, n, "%s", cmd == 1 ? "" : cmd < 10 ? orbs[cmd - 2] : g_item_names[cmd - 10]);
    return 1;
}
static void items_scan(void) {
    if (g_item_count >= 0) return;
    g_item_count = 0;
    char pat[1024];
    root_path(pat, sizeof(pat), "assets/chars/items/*.png");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int cap = 0;
    do {
        char nm[48], g[1024];
        snprintf(nm, sizeof(nm), "%.*s", (int)(strlen(fd.cFileName) - 4), fd.cFileName);
        for (char *q = nm; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
        root_path(g, sizeof(g), "assets/chars/items/%s.gltf", nm);
        if (GetFileAttributesA(g) == INVALID_FILE_ATTRIBUTES) continue;
        if (g_item_count == cap) { cap = cap ? cap * 2 : 256; g_item_names = (char (*)[48])realloc(g_item_names, sizeof(*g_item_names) * cap); }
        snprintf(g_item_names[g_item_count++], 48, "%s", nm);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(g_item_names, g_item_count, sizeof(*g_item_names), (int (*)(const void *, const void *))strcmp);
}
static CharModel *g_ms_model = NULL;
static int g_ms_slot = MC_WALK, g_ms_sel = 0, g_ms_scroll = 0, g_ms_preview = 0; /* g_ms_preview: the list's clip plays instead of the slot's */
static float g_ms_t = 0.0f;
static char g_ms_filter[32] = "";
static int g_ms_filter_len = 0;
static int g_ms_list[4096], g_ms_n = 0;
static RECT g_ms_graph_rc, g_ms_stage_rc, g_ms_list_rc, g_ms_node_rc[MC_COUNT], g_ms_pool_rc[POOL_COUNT];
#define MS_ROW_H 20
static const char *POOL_TITLE[POOL_COUNT] = { "WEAPON SWINGS", "WEAPON HITS", "GRUNTS WHEN HIT" };
static const char *POOL_DESC[POOL_COUNT] = {
    "One of them plays, at random, when an attack starts.",
    "One of them plays, at random, when a blow lands on a target.",
    "When it's hit, one of them may play (the chance below)." };

static Moveset *ms_cur(void) { return moveset_get(g_ms_model->name[0] ? g_ms_model->name : "david"); }
static void ms_filter(void) {
    const CharModel *m = g_ms_model;
    g_ms_n = 0;
    if (g_ms_items) {
        items_scan();
        for (int i = 0; i < g_item_count && g_ms_n < 4096; i++) if (!g_ms_filter[0] || ci_strstr(g_item_names[i], g_ms_filter)) g_ms_list[g_ms_n++] = i;
        if (g_ms_sel >= g_ms_n) g_ms_sel = g_ms_n > 0 ? g_ms_n - 1 : 0;
        g_ms_scroll = 0;
        return;
    }
    if (g_ms_sounds) { /* sound effects first; a category's title shows when one of its files does */
        static const int order[SND_KINDS] = { SND_SFX, SND_ROOM_LINES, SND_MUSIC, SND_OTHER_LINES };
        free(g_ms_snd);
        g_ms_snd_n = sound_rows(order, SND_KINDS, current_room_label(), &g_ms_snd);
        int header = -1;
        for (int i = 0; i < g_ms_snd_n && g_ms_n < 4096; i++) {
            if (g_ms_snd[i].header) { header = i; continue; }
            if (g_ms_filter[0] && !ci_strstr(g_ms_snd[i].name, g_ms_filter)) continue;
            if (header >= 0 && g_ms_n < 4095) { g_ms_list[g_ms_n++] = header; header = -1; }
            g_ms_list[g_ms_n++] = i;
        }
        g_ms_sel = g_ms_n > 1 ? 1 : 0;
        g_ms_scroll = 0;
        return;
    }
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < anim_lib_count() && g_ms_n < 4096; i++) {
            const char *src = anim_lib_source(i);
            if (pass == 0 ? strcmp(src, m->name) != 0 : pass == 1 ? strcmp(src, "anims") != 0 : (!strcmp(src, m->name) || !strcmp(src, "anims"))) continue;
            if (g_ms_filter[0] && !ci_strstr(anim_lib_name(i), g_ms_filter)) continue;
            if (anim_lib_fits(i, m->node_count)) g_ms_list[g_ms_n++] = i;
        }
    if (g_ms_sel >= g_ms_n) g_ms_sel = g_ms_n > 0 ? g_ms_n - 1 : 0;
    g_ms_scroll = 0;
}
static int ms_rows_visible(void) { int n = (g_ms_list_rc.bottom - g_ms_list_rc.top) / MS_ROW_H; return n < 1 ? 1 : n; }
static int ms_row_header(int k) { return g_ms_sounds && !g_ms_items && g_ms_snd[g_ms_list[k]].header; }
static void ms_select(int k) {
    if (g_ms_n <= 0) return;
    if (k < 0) k = 0;
    if (k >= g_ms_n) k = g_ms_n - 1;
    if (ms_row_header(k)) { /* titles skipped */
        int dir = k < g_ms_sel ? -1 : 1;
        if (k + dir >= 0 && k + dir < g_ms_n) k += dir; else if (k - dir >= 0 && k - dir < g_ms_n) k -= dir;
    }
    g_ms_sel = k; g_ms_preview = !g_ms_items && !g_ms_sounds; g_ms_t = 0.0f;
    int rows = ms_rows_visible();
    if (g_ms_sel < g_ms_scroll) g_ms_scroll = g_ms_sel;
    if (g_ms_sel >= g_ms_scroll + rows) g_ms_scroll = g_ms_sel - rows + 1;
}
/* the list on the slot's clip */
static void ms_select_slot(int slot) {
    g_ms_slot = slot; g_ms_preview = 0; g_ms_t = 0.0f;
    g_ms_cue_sel = -1; g_ms_cplay_t = -1.0f;
    if (g_ms_items || g_ms_sounds) return;
    int clip = moveset_clip(ms_cur(), slot, NULL);
    for (int k = 0; k < g_ms_n; k++) if (g_ms_list[k] == clip) {
        g_ms_sel = k;
        int rows = ms_rows_visible();
        g_ms_scroll = k - rows / 2; if (g_ms_scroll < 0) g_ms_scroll = 0;
        return;
    }
}
/* what the right-hand list shows: the tab's (clips / sounds), or items */
static void ms_list_mode(int items) {
    g_ms_items = items;
    g_ms_sounds = !items && (g_ms_tab == PG_SOUND || g_ms_cues);
    g_ms_filter[0] = 0; g_ms_filter_len = 0;
    audio_preview(NULL);
    ms_filter();
    if (!items) ms_select_slot(g_ms_slot);
}
static void ms_set_tab(int tab) {
    g_ms_tab = tab;
    g_ms_cues = 0; g_ms_cplay_t = -1.0f;
    ms_list_mode(0);
    if (group_has_slots(tab)) ms_select_slot(tab == PG_COMBAT ? MC_ATK1 : tab == PG_REACT ? MC_HIT : MC_WALK);
}
static void ms_open(CharModel *m) {
    if (!m) return;
    g_ms_model = m;
    g_ms_view = 1;
    g_ms_text = 0;
    ms_set_tab(g_ms_tab);
    ClipCursor(NULL); g_clip_active = 0;
}
/* a combat change: the changes are tied to the combat preset in use (a
   change made on another preset is forgotten first) */
static void ms_combat_change(Moveset *ms) {
    const char *now = moveset_combat_name(ms, ms->right_hand);
    if (ms->combat_ovr_for[0] && _stricmp(ms->combat_ovr_for, now))
        for (int k = MC_FIRST_COMBAT; k < MC_COUNT; k++) { ms->slot[k][0] = 0; ms->has_step[k] = 0; ms->has_cues[k - MC_FIRST_COMBAT] = 0; }
    snprintf(ms->combat_ovr_for, sizeof(ms->combat_ovr_for), "%s", now);
}
static void ms_assign(int clip) {
    Moveset *ms = ms_cur();
    if (slot_group(g_ms_slot) == PG_COMBAT) ms_combat_change(ms);
    if (clip < 0) snprintf(ms->slot[g_ms_slot], sizeof(ms->slot[0]), "-");
    else {
        char ref[100]; snprintf(ref, sizeof(ref), "%s/%s", anim_lib_source(clip), anim_lib_name(clip));
        /* the preset's own clip for this slot: no change needed */
        const MovePreset *pp = moveset_preset_w(ms, slot_group(g_ms_slot), ms->right_hand);
        if (pp && pp->clip[g_ms_slot][0] && clip == anim_lib_find_ref(pp->clip[g_ms_slot])) ms->slot[g_ms_slot][0] = 0;
        else snprintf(ms->slot[g_ms_slot], sizeof(ms->slot[0]), "%s", ref);
    }
    moveset_save(ms);
    g_ms_preview = 0;
}
/* the character's own sound pools, from its preset's the first time they change */
static SoundPools *ms_own_sounds(Moveset *ms) {
    if (!ms->own_sounds) {
        const SoundPools *sp = moveset_sounds(ms);
        if (sp) ms->snd = *sp; else memset(&ms->snd, 0, sizeof(ms->snd));
        ms->own_sounds = 1;
    }
    return &ms->snd;
}
/* does a tab (group) carry changes of its own, not in a preset yet? */
static int ms_group_changed(const Moveset *ms, int group) {
    if (group == PG_SOUND) return ms->own_sounds;
    if (group == PG_AI) return ms->own_ai;
    if (group == PG_STATS) return ms->own_stats;
    for (int k = 0; k < MC_COUNT; k++) if (slot_group(k) == group && (ms->slot[k][0] || ms->has_step[k])) return 1;
    if (group == PG_COMBAT) for (int i = 0; i < COMBAT_SLOTS; i++) if (ms->has_cues[i]) return 1;
    return 0;
}
/* The tab (walking / combat / sounds) as it is now, saved as the preset
   `name` (a built-in one can't be replaced). `make_default`: the character
   then uses it, with no change of its own. Returns 1 if saved. */
static int ms_save_group(Moveset *ms, int group, const char *name, int make_default) {
    presets_load();
    MovePreset *old = preset_find(name, group);
    if (old && old->builtin) { snprintf(g_status, sizeof(g_status), "'%s' is a built-in preset: choose another name", name); return 0; }
    MovePreset *p = old;
    if (!p) { if (g_preset_n >= MOVE_PRESET_MAX) { snprintf(g_status, sizeof(g_status), "too many presets"); return 0; } p = &g_presets[g_preset_n++]; }
    MovePreset np; memset(&np, 0, sizeof(np));
    snprintf(np.name, sizeof(np.name), "%s", name);
    np.group = group;
    if (group == PG_SOUND) { const SoundPools *sp = moveset_sounds(ms); if (sp) np.snd = *sp; }
    else if (group == PG_AI) { const AiSettings *ai = moveset_ai(ms); if (ai) np.ai = *ai; }
    else if (group == PG_STATS) np.st = *moveset_stats(ms);
    else for (int k = 0; k < MC_COUNT; k++) {
        if (slot_group(k) != group) continue;
        int c = moveset_clip(ms, k, NULL);
        if (c >= 0) snprintf(np.clip[k], sizeof(np.clip[k]), "%s/%s", anim_lib_source(c), anim_lib_name(c));
        np.step[k] = moveset_step_w(ms, k, ms->right_hand);
        if (group == PG_COMBAT) { const CueList *cl = moveset_cues_w(ms, k, ms->right_hand); if (cl) np.cues[k - MC_FIRST_COMBAT] = *cl; }
    }
    *p = np;
    preset_write(p);
    if (make_default) {
        if (group == PG_SOUND) { snprintf(ms->sound_preset, 48, "%s", name); ms->own_sounds = 0; }
        else if (group == PG_AI) { snprintf(ms->ai_preset, 48, "%s", name); ms->own_ai = 0; }
        else if (group == PG_STATS) { snprintf(ms->stats_preset, 48, "%s", name); ms->own_stats = 0; }
        else if (group == PG_REACT) {
            snprintf(ms->react_preset, 48, "%s", name);
            for (int k = 0; k < MC_COUNT; k++) if (slot_group(k) == group) ms->slot[k][0] = 0;
        }
        else {
            snprintf(group == PG_COMBAT ? ms->combat_preset : ms->walk_preset, 48, "%s", name);
            for (int k = 0; k < MC_COUNT; k++) if (slot_group(k) == group) { ms->slot[k][0] = 0; ms->has_step[k] = 0; }
            if (group == PG_COMBAT) { ms->combat_ovr_for[0] = 0; memset(ms->has_cues, 0, sizeof(ms->has_cues)); }
        }
    }
    moveset_save(ms);
    return 1;
}
/* Everything at once as a MODEL preset: a tab with changes of its own is
   first saved as its own preset ("<name> - walking"...), so the model
   preset only refers to presets. */
static void ms_save_model(Moveset *ms, const char *name, int make_default) {
    presets_load();
    MovePreset *old = preset_find(name, PG_MODEL);
    MovePreset np; memset(&np, 0, sizeof(np));
    snprintf(np.name, sizeof(np.name), "%s", name);
    np.group = PG_MODEL;
    for (int g = PG_WALK; g <= PG_STATS; g++) {
        char sub[48];
        if (ms_group_changed(ms, g)) {
            snprintf(sub, sizeof(sub), "%.30s - %s", name, PG_NAME[g]);
            ms_save_group(ms, g, sub, 1);
        }
    }
    snprintf(np.walk, sizeof(np.walk), "%s", ms->walk_preset);
    snprintf(np.combat, sizeof(np.combat), "%s", ms->combat_preset);
    snprintf(np.sound, sizeof(np.sound), "%s", ms->sound_preset);
    snprintf(np.weapon, sizeof(np.weapon), "%s", ms->right_hand);
    snprintf(np.shield, sizeof(np.shield), "%s", ms->left_hand);
    snprintf(np.react, sizeof(np.react), "%s", ms->react_preset);
    snprintf(np.aip, sizeof(np.aip), "%s", ms->ai_preset);
    snprintf(np.stats, sizeof(np.stats), "%s", ms->stats_preset);
    MovePreset *p = old;
    if (!p) { if (g_preset_n >= MOVE_PRESET_MAX) { snprintf(g_status, sizeof(g_status), "too many presets"); return; } p = &g_presets[g_preset_n++]; }
    *p = np;
    preset_write(p);
    if (make_default) snprintf(ms->model_preset, sizeof(ms->model_preset), "%s", name);
    moveset_save(ms);
    snprintf(g_status, sizeof(g_status), "model preset '%s' saved", name);
}
/* a model preset applied: its presets and equipment, no change of its own */
static void ms_apply_model(Moveset *ms, const MovePreset *p) {
    snprintf(ms->walk_preset, 48, "%s", p->walk[0] ? p->walk : "Human");
    snprintf(ms->combat_preset, 48, "%s", p->combat);
    snprintf(ms->sound_preset, 48, "%s", p->sound[0] ? p->sound : "-");
    snprintf(ms->right_hand, 48, "%s", p->weapon);
    snprintf(ms->left_hand, 48, "%s", p->shield);
    snprintf(ms->model_preset, 48, "%s", p->name);
    snprintf(ms->react_preset, 48, "%s", p->react[0] ? p->react : default_group_preset(ms->model, PG_REACT, ""));
    snprintf(ms->ai_preset, 48, "%s", p->aip[0] ? p->aip : "-");
    snprintf(ms->stats_preset, 48, "%s", p->stats[0] ? p->stats : "Human");
    ms->own_ai = ms->own_stats = 0;
    for (int k = 0; k < MC_COUNT; k++) { ms->slot[k][0] = 0; ms->has_step[k] = 0; }
    memset(ms->has_cues, 0, sizeof(ms->has_cues));
    ms->combat_ovr_for[0] = 0; ms->own_sounds = 0;
    moveset_save(ms);
    snprintf(g_status, sizeof(g_status), "model preset '%s' applied", p->name);
}
/* Delete: a menu of the group's saved presets, then "are you sure?" */
static void ms_delete_menu(int group) {
    presets_load();
    HMENU m = CreatePopupMenu();
    int any = 0;
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "Delete which preset?");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < g_preset_n; i++) if (g_presets[i].group == group && !g_presets[i].builtin) { AppendMenuA(m, MF_STRING, 10 + i, g_presets[i].name); any = 1; }
    if (!any) AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "(no saved preset -- the built-in ones can't be deleted)");
    POINT pt; GetCursorPos(&pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd < 10) return;
    char q[200];
    snprintf(q, sizeof(q), "Delete the %s preset \"%s\"?\n\nThe characters using it go back to their default. This can't be undone.", PG_NAME[group], g_presets[cmd - 10].name);
    if (MessageBoxA(g_hwnd, q, "Silver Remaster -- delete a preset", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    char nm[48]; snprintf(nm, sizeof(nm), "%s", g_presets[cmd - 10].name);
    preset_delete(&g_presets[cmd - 10]);
    snprintf(g_status, sizeof(g_status), "preset '%s' deleted", nm);
}
/* THE BLOW'S SOUNDS: the list in use (the default one: a swing of the pool, at once) */
static const CueList *ms_cues_now(const Moveset *ms, int slot) {
    static CueList def;
    const CueList *l = moveset_cues_w(ms, slot, ms->right_hand);
    if (l) return l;
    memset(&def, 0, sizeof(def));
    cue_add(&def, CUE_SWING, 0.0f, NULL);
    return &def;
}
static int ms_cues_changed(const Moveset *ms, int slot) {
    return slot >= MC_FIRST_COMBAT && ms->has_cues[slot - MC_FIRST_COMBAT] && moveset_change_on(ms, slot, ms->right_hand);
}
/* the character's own list for the blow, from the one in use the first time it changes */
static CueList *ms_own_cues(Moveset *ms) {
    int i = g_ms_slot - MC_FIRST_COMBAT;
    ms_combat_change(ms);
    if (!ms->has_cues[i]) { ms->cues[i] = *ms_cues_now(ms, g_ms_slot); ms->cues[i].set = 1; ms->has_cues[i] = 1; }
    return &ms->cues[i];
}
static void ms_cue_insert(int type, float sec, const char *file) {
    Moveset *ms = ms_cur();
    CueList *l = ms_own_cues(ms);
    if (l->n >= CUE_MAX) { snprintf(g_status, sizeof(g_status), "a blow has at most %d steps", CUE_MAX); return; }
    int at = g_ms_cue_sel >= 0 && g_ms_cue_sel < l->n ? g_ms_cue_sel + 1 : l->n;
    memmove(&l->c[at + 1], &l->c[at], sizeof(SoundCue) * (size_t)(l->n - at));
    memset(&l->c[at], 0, sizeof(SoundCue));
    l->c[at].type = type; l->c[at].sec = sec;
    if (file) snprintf(l->c[at].file, sizeof(l->c[at].file), "%s", file);
    l->n++;
    g_ms_cue_sel = at;
    moveset_save(ms);
}
/* a step of the list: moved, deleted, its wait changed, listened to */
static void ms_cue_op(int row, int op) {
    Moveset *ms = ms_cur();
    const CueList *now = ms_cues_now(ms, g_ms_slot);
    if (row < 0 || row >= now->n) return;
    if (op == CUEOP_SELECT) { g_ms_cue_sel = g_ms_cue_sel == row ? -1 : row; return; }
    if (op == CUEOP_LISTEN) {
        const SoundCue *c = &now->c[row];
        const char *f = c->type == CUE_SWING ? pool_pick(moveset_sounds(ms), POOL_SWING) : c->file;
        if (f && f[0]) audio_preview(f);
        g_ms_cue_sel = row;
        return;
    }
    CueList *l = ms_own_cues(ms);
    SoundCue tmp;
    switch (op) {
        case CUEOP_UP: if (row > 0) { tmp = l->c[row - 1]; l->c[row - 1] = l->c[row]; l->c[row] = tmp; g_ms_cue_sel = row - 1; } break;
        case CUEOP_DOWN: if (row < l->n - 1) { tmp = l->c[row + 1]; l->c[row + 1] = l->c[row]; l->c[row] = tmp; g_ms_cue_sel = row + 1; } break;
        case CUEOP_DEL:
            memmove(&l->c[row], &l->c[row + 1], sizeof(SoundCue) * (size_t)(l->n - row - 1));
            l->n--;
            if (g_ms_cue_sel >= l->n) g_ms_cue_sel = l->n - 1;
            break;
        default: {
            static const float d[4] = { -0.1f, -0.01f, 0.01f, 0.1f };
            SoundCue *c = &l->c[row];
            c->sec += d[op - CUEOP_M1];
            if (c->sec < 0.0f) c->sec = 0.0f;
            if (c->sec > 10.0f) c->sec = 10.0f;
            c->sec = roundf(c->sec * 100.0f) / 100.0f;
            g_ms_cue_sel = row;
        }
    }
    moveset_save(ms);
}
/* "Play with sounds": the blow from its start with its sounds; every step of the game */
static void ms_cue_play(void) {
    Moveset *ms = ms_cur();
    cue_start(&g_ms_cplay, ms_cues_now(ms, g_ms_slot));
    g_ms_cplay_t = 0.0f;
    g_ms_preview = 0;
    cue_run(&g_ms_cplay, 0.0f, moveset_sounds(ms));
}
static void ms_cue_tick(float dt) {
    if (!g_ms_view || g_ms_cplay_t < 0.0f) return;
    Moveset *ms = ms_cur();
    g_ms_cplay_t += dt;
    cue_run(&g_ms_cplay, dt, moveset_sounds(ms));
    float dur = anim_lib_duration(moveset_clip(ms, g_ms_slot, NULL));
    if (!g_ms_cplay.on && g_ms_cplay_t > dur + 0.75f) g_ms_cplay_t = -1.0f; /* over: the clip loops again */
}
static void ms_cue_label(const SoundCue *c, char *out, int n) {
    if (c->type == CUE_WAIT) snprintf(out, n, "Wait %.2f s", c->sec);
    else if (c->type == CUE_SWING) snprintf(out, n, "Swing sound (one of the pool's)");
    else snprintf(out, n, "Sound: %s", sound_shown(c->file));
}
/* "Single Swords (weapon)", "Human", "none" */
/* the character's own AI / stats, from its preset's the first time they change */
static AiSettings *ms_own_ai(Moveset *ms) {
    if (!ms->own_ai) {
        const AiSettings *ai = moveset_ai(ms);
        if (ai) ms->ai = *ai; else { memset(&ms->ai, 0, sizeof(ms->ai)); ms->ai.range = 1.5f; ms->ai.pause_min = 0.8f; ms->ai.pause_max = 1.6f; }
        ms->own_ai = 1;
    }
    return &ms->ai;
}
static StatSettings *ms_own_stats(Moveset *ms) {
    if (!ms->own_stats) { ms->st = *moveset_stats(ms); ms->own_stats = 1; }
    return &ms->st;
}
static void ms_preset_label(const Moveset *ms, int group, char *out, int n) {
    const char *v = ms_group_name((Moveset *)ms, group);
    if (!strcmp(v, "-")) snprintf(out, n, "none");
    else if (group == PG_COMBAT && !v[0]) snprintf(out, n, "%s (weapon)", weapon_preset(ms->right_hand));
    else snprintf(out, n, "%s", v);
    if (ms_group_changed(ms, group)) { size_t l = strlen(out); snprintf(out + l, n - l, " + changes"); }
}

static void ms_action(int id) {
    Moveset *ms = ms_cur();
    switch (id) {
        case B_MS_CLOSE: g_ms_view = 0; audio_preview(NULL); return;
        case B_MS_TAB_WALK: ms_set_tab(PG_WALK); return;
        case B_MS_CUES: g_ms_cues = 1; g_ms_cue_sel = -1; g_ms_cplay_t = -1.0f; ms_list_mode(0); return;
        case B_MS_CUE_DONE: g_ms_cues = 0; g_ms_cplay_t = -1.0f; ms_list_mode(0); return;
        case B_MS_CUE_SOUND:
            if (g_ms_sounds && g_ms_n > 0 && !ms_row_header(g_ms_sel)) ms_cue_insert(CUE_SOUND, 0.0f, g_ms_snd[g_ms_list[g_ms_sel]].name);
            return;
        case B_MS_CUE_SWING: ms_cue_insert(CUE_SWING, 0.0f, NULL); return;
        case B_MS_CUE_WAIT: ms_cue_insert(CUE_WAIT, 0.1f, NULL); return;
        case B_MS_CUE_PLAY: ms_cue_play(); return;
        case B_MS_CUE_RESET: ms->has_cues[g_ms_slot - MC_FIRST_COMBAT] = 0; g_ms_cue_sel = -1; moveset_save(ms); return;
        case B_MS_TAB_COMBAT: ms_set_tab(PG_COMBAT); return;
        case B_MS_TAB_SOUND: ms_set_tab(PG_SOUND); return;
        case B_MS_TAB_REACT: ms_set_tab(PG_REACT); return;
        case B_MS_TAB_AI: ms_set_tab(PG_AI); return;
        case B_MS_TAB_STATS: ms_set_tab(PG_STATS); return;
        case B_MS_AI_NONE: case B_MS_AI_MELEE: case B_MS_AI_RANGE_M: case B_MS_AI_RANGE_P: case B_MS_AI_WALK: case B_MS_AI_RUN:
        case B_MS_AI_PMIN_M: case B_MS_AI_PMIN_P: case B_MS_AI_PMAX_M: case B_MS_AI_PMAX_P: case B_MS_AI_DODGE_M: case B_MS_AI_DODGE_P: {
            AiSettings *ai = ms_own_ai(ms);
            switch (id) {
                case B_MS_AI_NONE: ai->kind = AI_NONE; break;
                case B_MS_AI_MELEE: ai->kind = AI_MELEE; break;
                case B_MS_AI_RANGE_M: ai->range = fmaxf(0.5f, ai->range - 0.1f); break;
                case B_MS_AI_RANGE_P: ai->range = fminf(6.0f, ai->range + 0.1f); break;
                case B_MS_AI_WALK: ai->run = 0; break;
                case B_MS_AI_RUN: ai->run = 1; break;
                case B_MS_AI_PMIN_M: ai->pause_min = fmaxf(0.0f, ai->pause_min - 0.1f); break;
                case B_MS_AI_PMIN_P: ai->pause_min = fminf(ai->pause_max, ai->pause_min + 0.1f); break;
                case B_MS_AI_PMAX_M: ai->pause_max = fmaxf(ai->pause_min, ai->pause_max - 0.1f); break;
                case B_MS_AI_PMAX_P: ai->pause_max = fminf(10.0f, ai->pause_max + 0.1f); break;
                case B_MS_AI_DODGE_M: ai->dodge = ai->dodge >= 5 ? ai->dodge - 5 : 0; break;
                case B_MS_AI_DODGE_P: ai->dodge = ai->dodge <= 95 ? ai->dodge + 5 : 100; break;
            }
            ai->range = roundf(ai->range * 10.0f) / 10.0f;
            ai->pause_min = roundf(ai->pause_min * 10.0f) / 10.0f; ai->pause_max = roundf(ai->pause_max * 10.0f) / 10.0f;
            moveset_save(ms);
            return;
        }
        case B_MS_AI_RESET: ms->own_ai = 0; moveset_save(ms); return;
        case B_MS_ST_HP_M10: case B_MS_ST_HP_M1: case B_MS_ST_HP_P1: case B_MS_ST_HP_P10: case B_MS_ST_DMG_M: case B_MS_ST_DMG_P: case B_MS_ST_BOSS: {
            StatSettings *st = ms_own_stats(ms);
            if (id == B_MS_ST_HP_M10) st->hp -= 10;
            if (id == B_MS_ST_HP_M1) st->hp -= 1;
            if (id == B_MS_ST_HP_P1) st->hp += 1;
            if (id == B_MS_ST_HP_P10) st->hp += 10;
            if (id == B_MS_ST_DMG_M) st->damage -= 1;
            if (id == B_MS_ST_DMG_P) st->damage += 1;
            if (id == B_MS_ST_BOSS) st->boss = !st->boss;
            if (st->hp < 1) st->hp = 1;
            if (st->hp > 9999) st->hp = 9999;
            if (st->damage < 0) st->damage = 0;
            if (st->damage > 999) st->damage = 999;
            moveset_save(ms);
            return;
        }
        case B_MS_ST_RESET: ms->own_stats = 0; moveset_save(ms); return;
        case B_MS_TAB_PRESET: { /* this tab's preset: choose, save, delete */
            presets_load();
            HMENU m = CreatePopupMenu();
            static const char *head[PG_COUNT] = { "Walking preset (this tab's changes are forgotten):", "Combat preset (this tab's changes are forgotten):",
                                                   "Sound preset (this tab's changes are forgotten):", "Reactions preset (this tab's changes are forgotten):",
                                                   "AI preset (this tab's changes are forgotten):", "Stats preset (this tab's changes are forgotten):", "" };
            AppendMenuA(m, MF_STRING | MF_GRAYED, 0, head[g_ms_tab]);
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            const char *cur = ms_group_name(ms, g_ms_tab);
            if (g_ms_tab == PG_COMBAT) AppendMenuA(m, MF_STRING | (!cur[0] ? MF_CHECKED : 0), 1, "From the equipped weapon (Double Swords for dualswrd, else Single Swords)");
            for (int i = 0; i < g_preset_n; i++) if (g_presets[i].group == g_ms_tab) {
                char lab[80]; snprintf(lab, sizeof(lab), "%s%s", g_presets[i].name, g_presets[i].builtin ? "   (built in)" : "");
                AppendMenuA(m, MF_STRING | (!_stricmp(cur, g_presets[i].name) ? MF_CHECKED : 0), 10 + i, lab);
            }
            if (g_ms_tab != PG_STATS)
                AppendMenuA(m, MF_STRING | (!strcmp(cur, "-") ? MF_CHECKED : 0), 2, g_ms_tab == PG_SOUND ? "No sounds" : g_ms_tab == PG_AI ? "No AI (it does nothing on its own)" : "No preset (every slot empty until given a clip)");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, 3, "Save this tab as a preset...");
            AppendMenuA(m, MF_STRING, 4, "Delete a preset...");
            POINT pt; GetCursorPos(&pt);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
            DestroyMenu(m);
            if (cmd <= 0) return;
            if (cmd == 3) { g_ms_text = g_ms_tab + 1; g_ms_text_buf[0] = 0; g_ms_text_default = 1; return; }
            if (cmd == 4) { ms_delete_menu(g_ms_tab); return; }
            char *dst = ms_group_name(ms, g_ms_tab);
            snprintf(dst, 48, "%s", cmd == 1 ? "" : cmd == 2 ? "-" : g_presets[cmd - 10].name);
            if (g_ms_tab == PG_SOUND) ms->own_sounds = 0;
            else if (g_ms_tab == PG_AI) ms->own_ai = 0;
            else if (g_ms_tab == PG_STATS) ms->own_stats = 0;
            else {
                for (int k = 0; k < MC_COUNT; k++) if (slot_group(k) == g_ms_tab) { ms->slot[k][0] = 0; ms->has_step[k] = 0; }
                if (g_ms_tab == PG_COMBAT) { ms->combat_ovr_for[0] = 0; memset(ms->has_cues, 0, sizeof(ms->has_cues)); }
            }
            moveset_save(ms);
            ms_select_slot(g_ms_slot);
            return;
        }
        case B_MS_MODEL_PRESET: { /* everything at once */
            presets_load();
            HMENU m = CreatePopupMenu();
            AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "Model preset: walking + combat + sounds + equipment (its changes are forgotten):");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            int any = 0;
            for (int i = 0; i < g_preset_n; i++) if (g_presets[i].group == PG_MODEL) {
                AppendMenuA(m, MF_STRING | (!_stricmp(ms->model_preset, g_presets[i].name) ? MF_CHECKED : 0), 10 + i, g_presets[i].name); any = 1;
            }
            if (!any) AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "(none saved yet)");
            AppendMenuA(m, MF_SEPARATOR, 0, NULL);
            AppendMenuA(m, MF_STRING, 3, "Save everything as a model preset...");
            AppendMenuA(m, MF_STRING, 4, "Delete a model preset...");
            POINT pt; GetCursorPos(&pt);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
            DestroyMenu(m);
            if (cmd == 3) { g_ms_text = PG_MODEL + 1; snprintf(g_ms_text_buf, sizeof(g_ms_text_buf), "%s", g_ms_model->name); g_ms_text_default = 1; }
            else if (cmd == 4) ms_delete_menu(PG_MODEL);
            else if (cmd >= 10) ms_apply_model(ms, &g_presets[cmd - 10]);
            ms_list_mode(0);
            return;
        }
        case B_MS_SAVE_DEFAULT: g_ms_text_default = !g_ms_text_default; return;
        case B_MS_SAVE_CANCEL: g_ms_text = 0; return;
        case B_MS_SAVE_OK: {
            int group = g_ms_text - 1;
            g_ms_text = 0;
            if (!g_ms_text_buf[0]) return;
            if (group == PG_MODEL) ms_save_model(ms, g_ms_text_buf, g_ms_text_default);
            else if (ms_save_group(ms, group, g_ms_text_buf, g_ms_text_default))
                snprintf(g_status, sizeof(g_status), "%s preset '%s' saved%s", PG_NAME[group], g_ms_text_buf, g_ms_text_default ? " -- now its default" : "");
            ms_select_slot(g_ms_slot);
            return;
        }
        case B_MS_WEAPON: ms_list_mode(g_ms_items == 1 ? 0 : 1); return;
        case B_MS_SHIELD: ms_list_mode(g_ms_items == 2 ? 0 : 2); return;
        case B_MS_LIST_BACK: ms_list_mode(0); return;
        case B_MS_ITEM_NONE:
            if (g_ms_items == 2) ms->left_hand[0] = 0; else ms->right_hand[0] = 0;
            moveset_save(ms); ms_list_mode(0);
            return;
        case B_MS_STEP_M: case B_MS_STEP_P: {
            float v = moveset_step_w(ms, g_ms_slot, ms->right_hand) + (id == B_MS_STEP_P ? 0.25f : -0.25f);
            if (v < -8.0f) v = -8.0f;
            if (v > 8.0f) v = 8.0f;
            ms_combat_change(ms);
            ms->step[g_ms_slot] = v; ms->has_step[g_ms_slot] = 1;
            moveset_save(ms);
            return;
        }
        case B_MS_STEP_PRESET: ms->has_step[g_ms_slot] = 0; moveset_save(ms); return;
        case B_MS_NONE: ms_assign(-1); return;
        case B_MS_PRESET: ms->slot[g_ms_slot][0] = 0; moveset_save(ms); g_ms_preview = 0; return;
        case B_MS_CHANCE_M: case B_MS_CHANCE_P: {
            SoundPools *sp = ms_own_sounds(ms);
            sp->grunt_chance += id == B_MS_CHANCE_P ? 10 : -10;
            if (sp->grunt_chance < 0) sp->grunt_chance = 0;
            if (sp->grunt_chance > 100) sp->grunt_chance = 100;
            moveset_save(ms);
            return;
        }
        case B_MS_SOUNDS_RESET: ms->own_sounds = 0; moveset_save(ms); return;
        case B_MS_USE:
            if (g_ms_n <= 0) return;
            if (g_ms_items) {
                snprintf(g_ms_items == 2 ? ms->left_hand : ms->right_hand, 48, "%s", g_item_names[g_ms_list[g_ms_sel]]);
                moveset_save(ms); ms_list_mode(0);
                return;
            }
            if (g_ms_sounds && g_ms_cues) { ms_action(B_MS_CUE_SOUND); return; }
            if (g_ms_sounds) {
                const SoundRow *r = &g_ms_snd[g_ms_list[g_ms_sel]];
                if (!r->header) { pool_add(ms_own_sounds(ms), g_ms_pool, r->name); moveset_save(ms); }
                return;
            }
            ms_assign(g_ms_list[g_ms_sel]);
            return;
    }
    if (id >= B_MS_CUE_ROW && id < B_MS_CUE_ROW + CUE_MAX * 10) { ms_cue_op((id - B_MS_CUE_ROW) / 10, (id - B_MS_CUE_ROW) % 10); return; }
    if (id >= B_MS_POOL && id < B_MS_POOL + POOL_COUNT * 100) {
        int pool = (id - B_MS_POOL) / 100, op = (id - B_MS_POOL) % 100;
        const SoundPools *cur = moveset_sounds(ms);
        g_ms_pool = pool;
        if (op >= 1 && op < 1 + POOL_MAX && cur && op - 1 < cur->n[pool]) audio_preview(cur->file[pool][op - 1]);
        if (op >= 50 && op < 50 + POOL_MAX && cur && op - 50 < cur->n[pool]) {
            SoundPools *sp = ms_own_sounds(ms);
            int i = op - 50;
            memmove(sp->file[pool][i], sp->file[pool][i + 1], 96 * (size_t)(sp->n[pool] - i - 1));
            sp->n[pool]--;
            moveset_save(ms);
        }
    }
}

static void ms_layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    g_btn_count = 0;
    Moveset *ms = ms_cur();
    char t[160], pn[64];
    int top = 104;
    int gw = rc.right * 55 / 100;
    SetRect(&g_ms_graph_rc, 10, top, gw, rc.bottom - 10);
    int rx = gw + 14, rw = rc.right - 10 - rx;
    int stage_h = (rc.bottom - top) * (g_ms_tab == PG_SOUND ? 34 : (g_ms_tab == PG_AI || g_ms_tab == PG_STATS) ? 70 : 45) / 100;
    SetRect(&g_ms_stage_rc, rx, top, rc.right - 10, top + stage_h);
    /* header: model preset, close */
    snprintf(t, sizeof(t), "Model preset: %s", ms->model_preset[0] ? ms->model_preset : "none");
    ui_addf(B_MS_MODEL_PRESET, rc.right - 430, 10, 314, 28, t, "",
            "Everything of this character at once -- its walking, combat and sound presets and its equipment: apply one, save everything as one (with its changes), delete one.", ms->model_preset[0] != 0, 1, 1);
    ui_add(B_MS_CLOSE, rc.right - 106, 10, 96, 28, "Close", "Esc", "Back.", 0, 1, 0);
    /* tabs + the tab's preset (left); the equipment (right) */
    ui_add(B_MS_TAB_WALK, 12, 66, 84, 28, "Walking", "", "Stand, walk, run, their starts and turns.", g_ms_tab == PG_WALK, 1, 3);
    ui_add(B_MS_TAB_REACT, 100, 66, 90, 28, "Reactions", "", "A blow taken, a dodge, dying, knocked down and getting up.", g_ms_tab == PG_REACT, 1, 3);
    ui_add(B_MS_TAB_COMBAT, 194, 66, 80, 28, "Combat", "", "The attacks of attack mode (Ctrl held): a click, and the swings up / left / right / down.", g_ms_tab == PG_COMBAT, 1, 3);
    ui_add(B_MS_TAB_SOUND, 278, 66, 80, 28, "Sounds", "", "Its sound pools: weapon swings, weapon hits, grunts when hit.", g_ms_tab == PG_SOUND, 1, 3);
    ui_add(B_MS_TAB_AI, 362, 66, 56, 28, "AI", "", "What it does on its own: close in and strike, dodge...", g_ms_tab == PG_AI, 1, 3);
    ui_add(B_MS_TAB_STATS, 422, 66, 70, 28, "Stats", "", "Its health, the damage of its blows, a boss or not.", g_ms_tab == PG_STATS, 1, 3);
    ms_preset_label(ms, g_ms_tab, pn, sizeof(pn));
    snprintf(t, sizeof(t), "Preset: %s", pn);
    ui_addf(B_MS_TAB_PRESET, 504, 66, 260, 28, t, "", "This tab's preset: choose one, save this tab as a preset, delete one. The presets themselves never change.", 0, 1, 1);
    int ew = 230;
    snprintf(t, sizeof(t), "Equipped weapon: %s", ms->right_hand[0] ? ms->right_hand : "none");
    ui_addf(B_MS_WEAPON, rc.right - 10 - 2 * ew - 6, 66, ew, 28, t, "", "What it holds in its right hand to begin with (the radial menu changes it in the game). Click: choose in the list.",
            g_ms_items == 1, grip_node(g_ms_model, 0) >= 0, 1);
    snprintf(t, sizeof(t), "Equipped shield: %s", ms->left_hand[0] ? ms->left_hand : "none");
    ui_addf(B_MS_SHIELD, rc.right - 10 - ew, 66, ew, 28, t, "", "What it holds on its left arm to begin with (a shield). Click: choose in the list.",
            g_ms_items == 2, shield_node(g_ms_model) >= 0, 1);
    /* right column: under the stage */
    int by = g_ms_stage_rc.bottom + 62, bw = (rw - 12) / 3;
    int how = 0; moveset_clip(ms, g_ms_slot, &how);
    int list_top = by + 62;
    if (g_ms_items) {
        ui_add(B_MS_USE, rx, by, bw, 26, g_ms_items == 2 ? "Hold this shield" : "Hold this weapon", "Enter", "The item selected in the list is equipped (double-click does the same).", 0, g_ms_n > 0, 0);
        ui_add(B_MS_ITEM_NONE, rx + bw + 6, by, bw, 26, "Nothing", "", "It holds nothing there.", 0, 1, 0);
        ui_add(B_MS_LIST_BACK, rx + 2 * (bw + 6), by, bw, 26, "Back", "Esc", "The list shows the tab's again.", 0, 1, 0);
    } else if (g_ms_cues) {
        ui_add(B_MS_CUE_SOUND, rx, by, bw * 2 + 6, 26, g_ms_cue_sel >= 0 ? "Add this sound after the selected step" : "Add this sound at the end", "Enter",
               "The sound selected in the list below joins the blow's sounds (double-click does the same). Space: listen.", 0, g_ms_n > 0, 0);
        ui_add(B_MS_CUE_DONE, rx + 2 * (bw + 6), by, bw, 26, "Done", "Esc", "Back to the attacks.", 0, 1, 0);
        list_top = by + 40;
    } else if (g_ms_tab == PG_AI || g_ms_tab == PG_STATS) {
        list_top = rc.bottom; /* no list */
    } else if (g_ms_tab == PG_SOUND) {
        snprintf(t, sizeof(t), "Add to %s", g_ms_pool == POOL_SWING ? "swings" : g_ms_pool == POOL_HIT ? "hits" : "grunts");
        ui_addf(B_MS_USE, rx, by, bw * 2, 26, t, "Enter", "The sound selected in the list joins the chosen pool (double-click does the same). Space: listen.", 0, g_ms_n > 0, 0);
        ui_add(B_MS_SOUNDS_RESET, rx + 2 * (bw + 6), by, bw, 26, "Back to preset", "", "Forget this character's changes to the pools: its sound preset's again.", 0, ms->own_sounds, 0);
        list_top = by + 40;
    } else {
        ui_add(B_MS_USE, rx, by, bw, 26, "Use this clip", "Enter", "The clip selected in the list below plays for this slot (double-click a clip does the same).", 0, g_ms_n > 0 && g_ms_preview, 0);
        ui_add(B_MS_NONE, rx + bw + 6, by, bw, 26, "No animation", "", "This slot plays nothing: a start / turn just turns the character, a cycle shows it in its rest pose.",
               how == 2 && ms->slot[g_ms_slot][0], 1, 0);
        ui_add(B_MS_PRESET, rx + 2 * (bw + 6), by, bw, 26, "Back to preset", "", "Forget this slot's change: it plays the preset's clip again.", 0, how == 1 || (how == 2 && ms->slot[g_ms_slot][0]), 0);
        if (g_ms_tab == PG_COMBAT) { /* the step taken with the blow */
            int sy = by + 34;
            float v = moveset_step_w(ms, g_ms_slot, ms->right_hand);
            int changed = ms->has_step[g_ms_slot] && moveset_change_on(ms, g_ms_slot, ms->right_hand);
            snprintf(t, sizeof(t), "Step: %+.2f hitbox%s%s", v, v < 0 ? " (back)" : "", changed ? "  (changed)" : "");
            ui_add(B_MS_STEP_M, rx, sy, 34, 24, "-", "", "A shorter step (0.25 hitbox diameter); below 0 it steps back.", 0, 1, 0);
            ui_addf(B_MS_STEP_PRESET, rx + 40, sy, rw - 80, 24, t, "", "How far it moves with the blow, in diameters of its hitbox (< 0: backward). Click: back to the preset's.", changed, 1, 1);
            ui_add(B_MS_STEP_P, rx + rw - 34, sy, 34, 24, "+", "", "A longer step (0.25 hitbox diameter).", 0, 1, 0);
            sy += 30;
            const CueList *cl = ms_cues_now(ms, g_ms_slot);
            int nsnd = 0; for (int i = 0; i < cl->n; i++) nsnd += cl->c[i].type != CUE_WAIT;
            if (!moveset_cues_w(ms, g_ms_slot, ms->right_hand)) snprintf(t, sizeof(t), "Sounds: a swing at once (default)  --  edit...");
            else snprintf(t, sizeof(t), "Sounds: %d sound%s over %.2f s%s  --  edit...", nsnd, nsnd == 1 ? "" : "s", cue_time(cl, cl->n), ms_cues_changed(ms, g_ms_slot) ? "  (changed)" : "");
            ui_addf(B_MS_CUES, rx, sy, rw, 24, t, "", "The sounds of this blow, like a little script: sounds and waits in order from its start, to match the animation.",
                    ms_cues_changed(ms, g_ms_slot), 1, 1);
            list_top = sy + 24 + 30;
        }
    }
    SetRect(&g_ms_list_rc, rx, list_top, rc.right - 10, rc.bottom - 10);
    /* the save box */
    if (g_ms_text) {
        int bx = rc.right / 2 - 260, byy = 120;
        ui_add(B_MS_SAVE_DEFAULT, bx + 16, byy + 80, 488, 26, "", "", "Ticked: the character uses the new preset from now on (it's its default, until changed).", g_ms_text_default, 1, 1);
        ui_add(B_MS_SAVE_OK, bx + 290, byy + 118, 104, 28, "Save", "Enter", "Save the preset.", 0, g_ms_text_buf[0] != 0, 0);
        ui_add(B_MS_SAVE_CANCEL, bx + 400, byy + 118, 104, 28, "Cancel", "Esc", "Don't save.", 0, 1, 0);
        return;
    }
    /* left: the graph, or the sound pools */
    memset(g_ms_node_rc, 0, sizeof(g_ms_node_rc));
    memset(g_ms_pool_rc, 0, sizeof(g_ms_pool_rc));
    int gx = g_ms_graph_rc.left, gy = g_ms_graph_rc.top, gwid = g_ms_graph_rc.right - g_ms_graph_rc.left, gh = g_ms_graph_rc.bottom - g_ms_graph_rc.top;
    if (g_ms_tab == PG_SOUND) {
        const SoundPools *sp = moveset_sounds(ms);
        int ph = gh / POOL_COUNT;
        for (int q = 0; q < POOL_COUNT; q++) {
            int py = gy + q * ph;
            SetRect(&g_ms_pool_rc[q], gx, py, gx + gwid, py + ph - 8);
            int n = sp ? sp->n[q] : 0, rowy = py + 50, cols = 2, cw = (gwid - 24 - 10) / cols;
            for (int i = 0; i < n; i++) {
                int cx = gx + 12 + (i % cols) * (cw + 10), cy = rowy + (i / cols) * 26;
                if (cy + 24 > py + ph - 44) break;
                ui_addf(B_MS_POOL + q * 100 + 1 + i, cx, cy, cw - 34, 24, sound_shown(sp->file[q][i]), "", "Listen to it.", 0, 1, 0);
                ui_add(B_MS_POOL + q * 100 + 50 + i, cx + cw - 30, cy, 30, 24, "x", "", "Take it out of the pool.", 0, 1, 0);
            }
            snprintf(t, sizeof(t), g_ms_pool == q ? "Adding to this pool: pick sounds in the list (right)" : "Add sounds to this pool...");
            ui_addf(B_MS_POOL + q * 100 + 99, gx + 12, py + ph - 42, 330, 26, t, "", "Pick sounds in the list on the right: they join this pool.", g_ms_pool == q, n < POOL_MAX, 1);
            if (q == POOL_GRUNT) {
                ui_add(B_MS_CHANCE_M, gx + gwid - 190, py + ph - 42, 34, 26, "-", "", "A grunt less often.", 0, sp && sp->grunt_chance > 0, 0);
                ui_add(B_MS_CHANCE_P, gx + gwid - 46, py + ph - 42, 34, 26, "+", "", "A grunt more often.", 0, !sp || sp->grunt_chance < 100, 0);
            }
        }
        return;
    }
    if (g_ms_tab == PG_COMBAT && g_ms_cues) { /* the blow's sounds: a ruler, then its steps as rows */
        const CueList *cl = ms_cues_now(ms, g_ms_slot);
        SetRect(&g_ms_ruler_rc, gx + 12, gy + 62, gx + gwid - 12, gy + 130);
        int rowh = 30, ry = gy + 170, x0 = gx + 12, x1 = gx + gwid - 12;
        for (int i = 0; i < cl->n; i++) {
            int y = ry + i * rowh;
            if (y + rowh > gy + gh - 52) break;
            const SoundCue *c = &cl->c[i];
            int bid = B_MS_CUE_ROW + i * 10, xr = x1;
            ui_add(bid + CUEOP_DEL, xr - 30, y, 30, 26, "x", "Del", "Remove this step.", 0, 1, 0); xr -= 34;
            ui_add(bid + CUEOP_DOWN, xr - 30, y, 30, 26, "v", "", "Move it down (later).", 0, i < cl->n - 1, 0); xr -= 34;
            ui_add(bid + CUEOP_UP, xr - 30, y, 30, 26, "^", "", "Move it up (sooner).", 0, i > 0, 0); xr -= 42;
            if (c->type == CUE_WAIT) {
                static const char *lab[4] = { "-0.1", "-.01", "+.01", "+0.1" };
                for (int k = 3; k >= 0; k--) { ui_add(bid + CUEOP_M1 + k, xr - 44, y, 44, 26, lab[k], "", "A longer / shorter wait (seconds).", 0, 1, 0); xr -= 48; }
            } else { ui_add(bid + CUEOP_LISTEN, xr - 70, y, 70, 26, "Listen", "", "Hear it.", 0, 1, 0); xr -= 74; }
            snprintf(t, sizeof(t), "%5.2f s    ", cue_time(cl, i));
            ms_cue_label(c, t + strlen(t), (int)(sizeof(t) - strlen(t)));
            ui_addf(bid + CUEOP_SELECT, x0, y, xr - x0 - 4, 26, t, "", "Select it: new steps go after it. Click again: new steps go at the end.", i == g_ms_cue_sel, 1, c->type == CUE_WAIT ? 1 : 3);
        }
        int bw6 = (gwid - 24 - 5 * 6) / 6, byy = gy + gh - 40;
        ui_add(B_MS_CUE_SOUND, gx + 12, byy, bw6, 28, "+ Sound", "", "The sound selected in the list (right) is added after the selected step (or at the end).", 0, g_ms_n > 0, 0);
        ui_add(B_MS_CUE_SWING, gx + 12 + (bw6 + 6), byy, bw6, 28, "+ Swing (pool)", "", "One of the swing sounds of its sound pool (Sounds tab), at random.", 0, 1, 0);
        ui_add(B_MS_CUE_WAIT, gx + 12 + 2 * (bw6 + 6), byy, bw6, 28, "+ Wait", "", "A pause before the next sounds (0.10 s; change it with its buttons).", 0, 1, 0);
        ui_add(B_MS_CUE_PLAY, gx + 12 + 3 * (bw6 + 6), byy, bw6, 28, g_ms_cplay_t >= 0.0f ? "Playing..." : "Play with sounds", "",
               "The blow plays once from its start on the character, with its sounds -- to check they match.", g_ms_cplay_t >= 0.0f, 1, 0);
        ui_add(B_MS_CUE_RESET, gx + 12 + 4 * (bw6 + 6), byy, bw6, 28, "Back to preset", "", "Forget this blow's changes: its preset's sounds again.", 0, ms_cues_changed(ms, g_ms_slot), 0);
        ui_add(B_MS_CUE_DONE, gx + 12 + 5 * (bw6 + 6), byy, bw6, 28, "Done", "Esc", "Back to the attacks.", 0, 1, 0);
        return;
    }
    int nw = gwid / 5, nh = 48;
    if (nw < 130) nw = 130;
    #define MS_NODE(k, fx, fy) SetRect(&g_ms_node_rc[k], gx + (int)(gwid * (fx)) - nw / 2, gy + (int)(gh * (fy)) - nh / 2, gx + (int)(gwid * (fx)) + nw / 2, gy + (int)(gh * (fy)) + nh / 2)
    if (g_ms_tab == PG_AI) { /* the AI panel: rows of controls (their titles and values: ms_paint) */
        const AiSettings *ai = moveset_ai(ms);
        int x = gx + 20, w = gwid - 40, y0 = gy + 24;
        #define MS_ROW(i) (y0 + (i) * 78 + 22)
        ui_add(B_MS_AI_NONE, x, MS_ROW(0), 220, 28, "None: it does nothing on its own", "", "Only scripts move it.", !ai || ai->kind == AI_NONE, 1, 3);
        ui_add(B_MS_AI_MELEE, x + 226, MS_ROW(0), 260, 28, "Close in and strike", "", "It goes to the nearest opponent until close enough to strike, strikes, pauses, strikes again.", ai && ai->kind == AI_MELEE, 1, 3);
        int on = ai && ai->kind == AI_MELEE;
        ui_add(B_MS_AI_RANGE_M, x, MS_ROW(1), 34, 28, "-", "", "It strikes from closer.", 0, on, 0);
        ui_add(B_MS_AI_RANGE_P, x + 234, MS_ROW(1), 34, 28, "+", "", "It strikes from further.", 0, on, 0);
        ui_add(B_MS_AI_WALK, x, MS_ROW(2), 130, 28, "Walking", "", "It walks to its target.", on && !ai->run, on, 3);
        ui_add(B_MS_AI_RUN, x + 136, MS_ROW(2), 130, 28, "Running", "", "It runs to its target.", on && ai->run, on, 3);
        ui_add(B_MS_AI_PMIN_M, x, MS_ROW(3), 34, 28, "-", "", "A shorter shortest pause.", 0, on, 0);
        ui_add(B_MS_AI_PMIN_P, x + 150, MS_ROW(3), 34, 28, "+", "", "A longer shortest pause.", 0, on, 0);
        ui_add(B_MS_AI_PMAX_M, x + 220, MS_ROW(3), 34, 28, "-", "", "A shorter longest pause.", 0, on, 0);
        ui_add(B_MS_AI_PMAX_P, x + 370, MS_ROW(3), 34, 28, "+", "", "A longer longest pause.", 0, on, 0);
        ui_add(B_MS_AI_DODGE_M, x, MS_ROW(4), 34, 28, "-", "", "It dodges less often.", 0, on, 0);
        ui_add(B_MS_AI_DODGE_P, x + 234, MS_ROW(4), 34, 28, "+", "", "It dodges more often.", 0, on, 0);
        ui_add(B_MS_AI_RESET, x, MS_ROW(5), 200, 28, "Back to preset", "", "Forget this character's changes to its AI: its AI preset's again.", 0, ms->own_ai, 0);
        (void)w;
        return;
    }
    if (g_ms_tab == PG_STATS) {
        const StatSettings *st = moveset_stats(ms);
        int x = gx + 20, y0 = gy + 24;
        ui_add(B_MS_ST_HP_M10, x, MS_ROW(0), 44, 28, "-10", "", "Less health.", 0, st->hp > 1, 0);
        ui_add(B_MS_ST_HP_M1, x + 48, MS_ROW(0), 44, 28, "-1", "", "Less health.", 0, st->hp > 1, 0);
        ui_add(B_MS_ST_HP_P1, x + 216, MS_ROW(0), 44, 28, "+1", "", "More health.", 0, 1, 0);
        ui_add(B_MS_ST_HP_P10, x + 264, MS_ROW(0), 44, 28, "+10", "", "More health.", 0, 1, 0);
        ui_add(B_MS_ST_DMG_M, x, MS_ROW(1), 34, 28, "-", "", "Weaker blows.", 0, st->damage > 0, 0);
        ui_add(B_MS_ST_DMG_P, x + 234, MS_ROW(1), 34, 28, "+", "", "Stronger blows.", 0, 1, 0);
        ui_add(B_MS_ST_BOSS, x, MS_ROW(2), 420, 28, st->boss ? "[x]  Boss: its health bar on the screen" : "[  ]  Boss: its health bar on the screen", "",
               "Ticked: while it's alive (and an enemy), its health bar shows at the top of the screen.", st->boss, 1, 3);
        ui_add(B_MS_ST_RESET, x, MS_ROW(3), 200, 28, "Back to preset", "", "Forget this character's changes to its stats: its stats preset's again.", 0, ms->own_stats, 0);
        #undef MS_ROW
        return;
    }
    if (g_ms_tab == PG_REACT) { /* a blow -> hit / dodge; health gone -> death (enemy) or down, then up (ally) */
        MS_NODE(MC_HIT, 0.30f, 0.18f); MS_NODE(MC_DODGE, 0.30f, 0.36f);
        MS_NODE(MC_DEATH, 0.72f, 0.18f); MS_NODE(MC_DOWN, 0.16f, 0.66f); MS_NODE(MC_DOWNLOOP, 0.50f, 0.66f); MS_NODE(MC_GETUP, 0.84f, 0.66f);
        return;
    }
    if (g_ms_tab == PG_COMBAT) { /* combat: attack mode -> a click / a held swing */
        MS_NODE(MC_ATK1, 0.50f, 0.10f); MS_NODE(MC_ATK2, 0.50f, 0.22f); MS_NODE(MC_ATK3, 0.50f, 0.34f);
        MS_NODE(MC_SW_UP, 0.83f, 0.52f); MS_NODE(MC_SW_LEFT, 0.83f, 0.64f); MS_NODE(MC_SW_RIGHT, 0.83f, 0.76f);
        MS_NODE(MC_SW_BACKL, 0.83f, 0.88f); MS_NODE(MC_SW_BACKR, 0.50f, 0.88f);
        return;
    }
    MS_NODE(MC_STAND, 0.11f, 0.50f);
    MS_NODE(MC_TOWALK, 0.35f, 0.22f);
    MS_NODE(MC_TORUN, 0.35f, 0.78f);
    MS_NODE(MC_WALK, 0.58f, 0.22f);
    MS_NODE(MC_RUN, 0.58f, 0.78f);
    MS_NODE(MC_TOWALKA, 0.87f, 0.06f);
    MS_NODE(MC_TOWALKC, 0.87f, 0.17f);
    MS_NODE(MC_TO180A, 0.87f, 0.28f);
    MS_NODE(MC_TO180C, 0.87f, 0.39f);
    MS_NODE(MC_RUN90A, 0.87f, 0.61f);
    MS_NODE(MC_RUN90C, 0.87f, 0.72f);
    MS_NODE(MC_RUN180A, 0.87f, 0.83f);
    MS_NODE(MC_RUN180C, 0.87f, 0.94f);
    #undef MS_NODE
    /* the turn nodes are smaller */
    static const int turns[8] = { MC_TOWALKA, MC_TOWALKC, MC_TO180A, MC_TO180C, MC_RUN90A, MC_RUN90C, MC_RUN180A, MC_RUN180C };
    for (int i = 0; i < 8; i++) InflateRect(&g_ms_node_rc[turns[i]], 0, -5);
}

static void ms_arrow(HDC hdc, int x0, int y0, int x1, int y1, COLORREF col, int dashed) {
    HPEN pen = CreatePen(dashed ? PS_DOT : PS_SOLID, 1, col);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    SetBkMode(hdc, TRANSPARENT);
    MoveToEx(hdc, x0, y0, NULL); LineTo(hdc, x1, y1);
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0), l = sqrtf(dx * dx + dy * dy);
    if (l > 1) {
        dx /= l; dy /= l;
        POINT p[3] = { { x1, y1 }, { (int)(x1 - dx * 10 - dy * 5), (int)(y1 - dy * 10 + dx * 5) }, { (int)(x1 - dx * 10 + dy * 5), (int)(y1 - dy * 10 - dx * 5) } };
        HBRUSH br = CreateSolidBrush(col); HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
        Polygon(hdc, p, 3);
        SelectObject(hdc, ob); DeleteObject(br);
    }
    SelectObject(hdc, old); DeleteObject(pen);
}

static void ms_paint(HWND hwnd, HDC hdc) {
    ms_layout(hwnd);
    RECT rc; GetClientRect(hwnd, &rc);
    ui_fill(hdc, &rc, RGB(14, 14, 18));
    Moveset *ms = ms_cur();
    char t[300];
    SelectObject(hdc, ui_font(20, 1));
    snprintf(t, sizeof(t), "Moveset -- %s", g_ms_model->name);
    ui_text(hdc, 12, 10, 600, 28, t, RGB(255, 225, 120), SV_ONE);
    /* the line under the title: the hovered button, else what the tab is */
    const UiButton *hb = ui_find(g_hover_btn);
    SelectObject(hdc, ui_font(14, 0));
    if (hb && hb->desc && hb->desc[0]) ui_text(hdc, 12, 42, rc.right - 24, 20, hb->desc, RGB(255, 230, 150), SV_ONE);
    else {
        static const char *what[PG_COUNT] = { "How it walks: click a box to see its clip, then pick another one in the list on the right.",
                                       "Its attacks in attack mode (Ctrl held): click a box to see its clip, then pick another one in the list on the right.",
                                       "Its sounds: each pool plays one of its sounds at random. Pick a pool, then add sounds from the list on the right.",
                                       "How it reacts: a blow taken, a dodge, dying (an enemy), knocked down and getting up (an ally). Click a box, pick a clip on the right.",
                                       "What it does on its own. An enemy goes for David and his allies; an ally for the enemies.",
                                       "Its health, the damage of its blows, and whether it's a boss (its health bar on the screen).", "" };
        ui_text(hdc, 12, 42, rc.right - 24, 20, what[g_ms_tab], RGB(200, 200, 205), SV_ONE);
    }
    /* left: the graph / the sound pools */
    ui_fill(hdc, &g_ms_graph_rc, RGB(20, 20, 26));
    RECT *n = g_ms_node_rc;
    COLORREF ac = RGB(110, 110, 125);
    #define CX(k) ((n[k].left + n[k].right) / 2)
    #define CY(k) ((n[k].top + n[k].bottom) / 2)
    if (g_ms_tab == PG_SOUND) {
        const SoundPools *sp = moveset_sounds(ms);
        for (int q = 0; q < POOL_COUNT; q++) {
            RECT *r = &g_ms_pool_rc[q];
            if (r->right <= r->left) continue;
            ui_fill(hdc, r, g_ms_pool == q ? RGB(34, 32, 20) : RGB(26, 26, 34));
            ui_frame(hdc, r, g_ms_pool == q ? RGB(200, 170, 70) : RGB(60, 60, 72));
            SelectObject(hdc, ui_font(15, 1));
            snprintf(t, sizeof(t), "%s  (%d)", POOL_TITLE[q], sp ? sp->n[q] : 0);
            ui_text(hdc, r->left + 12, r->top + 6, 400, 20, t, RGB(235, 235, 240), SV_ONE);
            SelectObject(hdc, ui_font(13, 0));
            ui_text(hdc, r->left + 12, r->top + 26, r->right - r->left - 24, 18, POOL_DESC[q], RGB(160, 160, 175), SV_ONE);
            if (!sp || sp->n[q] == 0) ui_text(hdc, r->left + 12, r->top + 52, 400, 20, "(empty)", RGB(120, 120, 130), SV_ONE);
            if (q == POOL_GRUNT) {
                snprintf(t, sizeof(t), "Chance: %d%%", sp ? sp->grunt_chance : 0);
                ui_text(hdc, r->right - 150, r->bottom - 34, 100, 26, t, SVL_VALUE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
    } else if (g_ms_tab == PG_AI || g_ms_tab == PG_STATS) {
        RECT *g = &g_ms_graph_rc;
        int x = g->left + 20, y0 = g->top + 24;
        #define MS_ROWT(i) (y0 + (i) * 78)
        char v[80];
        if (g_ms_tab == PG_AI) {
            const AiSettings *ai = moveset_ai(ms);
            static const char *titles[6] = { "BEHAVIOUR", "STRIKES FROM", "GOES TO ITS TARGET", "PAUSE BETWEEN ITS ATTACKS", "DODGES OUR BLOWS", "" };
            for (int i = 0; i < 5; i++) { SelectObject(hdc, ui_font(12, 1)); ui_text(hdc, x, MS_ROWT(i), 500, 16, titles[i], SVL_SECTION, SV_ONE); }
            SelectObject(hdc, ui_font(16, 1));
            if (ai && ai->kind == AI_MELEE) {
                snprintf(v, sizeof(v), "%.1f hitbox diameters", ai->range); ui_text(hdc, x + 40, MS_ROWT(1) + 22, 190, 28, v, SVL_VALUE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                snprintf(v, sizeof(v), "min %.1f s", ai->pause_min); ui_text(hdc, x + 38, MS_ROWT(3) + 22, 110, 28, v, SVL_VALUE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                snprintf(v, sizeof(v), "max %.1f s", ai->pause_max); ui_text(hdc, x + 258, MS_ROWT(3) + 22, 110, 28, v, SVL_VALUE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                snprintf(v, sizeof(v), "%d %% of them", ai->dodge); ui_text(hdc, x + 40, MS_ROWT(4) + 22, 190, 28, v, SVL_VALUE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            SelectObject(hdc, ui_font(13, 0));
            ui_text(hdc, x, MS_ROWT(6), g->right - g->left - 40, 60,
                    "The AI runs whenever the character is in the room, unless a script turns it off (Character settings). "
                    "It targets the nearest character of the other side: an enemy goes for David and his allies, an ally for the enemies.",
                    RGB(170, 170, 180), DT_LEFT | DT_WORDBREAK);
        } else {
            const StatSettings *st = moveset_stats(ms);
            static const char *titles[3] = { "HEALTH", "DAMAGE OF ITS BLOWS", "BOSS" };
            for (int i = 0; i < 3; i++) { SelectObject(hdc, ui_font(12, 1)); ui_text(hdc, x, MS_ROWT(i), 500, 16, titles[i], SVL_SECTION, SV_ONE); }
            SelectObject(hdc, ui_font(16, 1));
            snprintf(v, sizeof(v), "%d", st->hp); ui_text(hdc, x + 96, MS_ROWT(0) + 22, 116, 28, v, SVL_VALUE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            snprintf(v, sizeof(v), "%d per blow", st->damage); ui_text(hdc, x + 40, MS_ROWT(1) + 22, 190, 28, v, SVL_VALUE, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, ui_font(13, 0));
            ui_text(hdc, x, MS_ROWT(4), g->right - g->left - 40, 60,
                    "A blow that lands takes the attacker's damage from the health. At 0 an enemy dies; an ally (David too) is knocked down "
                    "and gets up again, its health back, once the enemies of the scene are beaten.",
                    RGB(170, 170, 180), DT_LEFT | DT_WORDBREAK);
        }
        #undef MS_ROWT
    } else if (g_ms_tab == PG_REACT) {
        SelectObject(hdc, ui_font(12, 1));
        ui_text(hdc, n[MC_HIT].left, n[MC_HIT].top - 20, 400, 16, "A BLOW AIMED AT IT", RGB(90, 110, 140), DT_LEFT | DT_SINGLELINE);
        ui_text(hdc, n[MC_DEATH].left, n[MC_DEATH].top - 20, 400, 16, "HEALTH GONE, AN ENEMY", RGB(90, 110, 140), DT_LEFT | DT_SINGLELINE);
        ui_text(hdc, n[MC_DOWN].left, n[MC_DOWN].top - 20, 600, 16, "HEALTH GONE, AN ALLY: DOWN UNTIL THE ENEMIES ARE BEATEN, THEN UP", RGB(90, 110, 140), DT_LEFT | DT_SINGLELINE);
        ms_arrow(hdc, n[MC_DOWN].right, CY(MC_DOWN), n[MC_DOWNLOOP].left, CY(MC_DOWNLOOP), ac, 0);
        ms_arrow(hdc, n[MC_DOWNLOOP].right, CY(MC_DOWNLOOP), n[MC_GETUP].left, CY(MC_GETUP), ac, 0);
    } else if (g_ms_tab == PG_COMBAT && g_ms_cues) {
        const CueList *cl = ms_cues_now(ms, g_ms_slot);
        int sclip = moveset_clip(ms, g_ms_slot, NULL);
        float dur = sclip >= 0 ? anim_lib_duration(sclip) : 0.0f, total = cue_time(cl, cl->n);
        float span = fmaxf(fmaxf(dur, total) + 0.1f, 0.5f);
        RECT *g = &g_ms_graph_rc;
        SelectObject(hdc, ui_font(17, 1));
        snprintf(t, sizeof(t), "SOUNDS OF \"%s\"", MOVE_SLOT_LABEL[g_ms_slot]);
        ui_text(hdc, g->left + 12, g->top + 8, 600, 24, t, RGB(255, 225, 120), SV_ONE);
        SelectObject(hdc, ui_font(13, 0));
        snprintf(t, sizeof(t), "%s (%.2f s).  In order from the start of the blow: sounds play at once, waits delay the ones after them.%s",
                 sclip >= 0 ? anim_lib_name(sclip) : "no animation", dur, ms_cues_changed(ms, g_ms_slot) ? "  Changed for this character." : "");
        ui_text(hdc, g->left + 12, g->top + 34, g->right - g->left - 24, 20, t, RGB(190, 190, 200), SV_ONE);
        /* the ruler: the clip (blue), each sound (gold), the time playing (red) */
        RECT *r = &g_ms_ruler_rc;
        int rw2 = r->right - r->left;
        ui_fill(hdc, r, RGB(24, 24, 32)); ui_frame(hdc, r, RGB(70, 70, 84));
        #define TX(tt) (r->left + (int)((tt) / span * rw2))
        RECT cb = { r->left + 1, r->top + 28, TX(dur), r->top + 40 };
        if (dur > 0) ui_fill(hdc, &cb, RGB(50, 90, 150));
        SelectObject(hdc, ui_font(11, 0));
        for (int k = 0; k * 0.1f <= span; k++) {
            int x = TX(k * 0.1f), big = k % 5 == 0;
            RECT tk = { x, r->bottom - (big ? 12 : 6), x + 1, r->bottom };
            ui_fill(hdc, &tk, RGB(110, 110, 125));
            if (big) { snprintf(t, sizeof(t), "%.1f", k * 0.1f); ui_text(hdc, x + 3, r->bottom - 16, 40, 14, t, RGB(130, 130, 145), SV_ONE); }
        }
        ui_text(hdc, TX(dur) + 4, r->top + 26, 120, 14, "clip ends", RGB(110, 150, 210), SV_ONE);
        int lane = 0;
        for (int i = 0; i < cl->n; i++) {
            if (cl->c[i].type == CUE_WAIT) continue;
            int x = TX(cue_time(cl, i));
            COLORREF col = i == g_ms_cue_sel ? RGB(255, 240, 150) : RGB(230, 180, 60);
            RECT ln = { x, r->top + 2, x + 2, r->bottom - 2 };
            ui_fill(hdc, &ln, col);
            char nm[64];
            if (cl->c[i].type == CUE_SWING) snprintf(nm, sizeof(nm), "swing");
            else { snprintf(nm, sizeof(nm), "%s", sound_shown(cl->c[i].file)); char *dot = strrchr(nm, '.'); if (dot) *dot = 0; }
            ui_text(hdc, x + 4, r->top + 2 + (lane % 2) * 12, 140, 13, nm, col, SV_ONE);
            lane++;
        }
        if (g_ms_cplay_t >= 0.0f) { int x = TX(fminf(g_ms_cplay_t, span)); RECT ph = { x, r->top, x + 2, r->bottom }; ui_fill(hdc, &ph, RGB(255, 80, 70)); }
        #undef TX
        SelectObject(hdc, ui_font(12, 1));
        ui_text(hdc, g->left + 12, g->top + 146, 400, 16, "STEPS (TIME  -  WHAT)", SVL_SECTION, SV_ONE);
        if (cl->n == 0) { SelectObject(hdc, ui_font(13, 0)); ui_text(hdc, g->left + 12, g->top + 172, 500, 20, "(no sound: the blow is silent)", RGB(150, 150, 160), SV_ONE); }
    } else if (g_ms_tab == PG_COMBAT) {
        RECT mode = { g_ms_graph_rc.left + 20, CY(MC_SW_UP) - 70, g_ms_graph_rc.left + 20 + (n[MC_ATK1].right - n[MC_ATK1].left), CY(MC_SW_UP) - 22 };
        ui_fill(hdc, &mode, RGB(52, 30, 30)); ui_frame(hdc, &mode, RGB(200, 100, 90));
        SelectObject(hdc, ui_font(16, 1));
        ui_text(hdc, mode.left + 8, mode.top, mode.right - mode.left - 12, 24, "ATTACK MODE", RGB(255, 200, 190), SV_ONE);
        SelectObject(hdc, ui_font(12, 0));
        ui_text(hdc, mode.left + 8, mode.top + 24, mode.right - mode.left - 12, 20, "Ctrl held", RGB(220, 190, 180), SV_ONE);
        int mx = mode.right, my = (mode.top + mode.bottom) / 2;
        for (int k = MC_ATK1; k <= MC_ATK3; k++) ms_arrow(hdc, mx, my - 8, n[k].left, CY(k), ac, 0);
        static const int sw[5] = { MC_SW_UP, MC_SW_LEFT, MC_SW_RIGHT, MC_SW_BACKL, MC_SW_BACKR };
        for (int i = 0; i < 5; i++) ms_arrow(hdc, mx, my + 8, n[sw[i]].left, CY(sw[i]), ac, 0);
        SelectObject(hdc, ui_font(12, 1));
        ui_text(hdc, n[MC_ATK1].left, n[MC_ATK1].top - 20, 400, 16, "A CLICK: ONE OF THE THREE, AT RANDOM", RGB(90, 110, 140), DT_LEFT | DT_SINGLELINE);
        ui_text(hdc, n[MC_SW_BACKR].left, n[MC_SW_UP].top - 20, 500, 16, "BUTTON HELD + A SWING OF THE MOUSE: UP / LEFT / RIGHT / DOWN", RGB(90, 110, 140), DT_LEFT | DT_SINGLELINE);
        ui_text(hdc, g_ms_graph_rc.left + 8, g_ms_graph_rc.bottom - 20, 700, 16, "EVERY BLOW STEPS WITH IT (SEE 'STEP'); A BLOW THAT TURNS THE BODY LEAVES IT FACING THE NEW WAY", RGB(90, 110, 140), DT_LEFT | DT_SINGLELINE);
    } else {
        ms_arrow(hdc, n[MC_STAND].right, CY(MC_STAND) - 8, n[MC_TOWALK].left, CY(MC_TOWALK), ac, 0);
        ms_arrow(hdc, n[MC_STAND].right, CY(MC_STAND) + 8, n[MC_TORUN].left, CY(MC_TORUN), ac, 0);
        ms_arrow(hdc, n[MC_TOWALK].right, CY(MC_TOWALK), n[MC_WALK].left, CY(MC_WALK), ac, 0);
        ms_arrow(hdc, n[MC_TORUN].right, CY(MC_TORUN), n[MC_RUN].left, CY(MC_RUN), ac, 0);
        static const int wt[4] = { MC_TOWALKA, MC_TOWALKC, MC_TO180A, MC_TO180C }, rt[4] = { MC_RUN90A, MC_RUN90C, MC_RUN180A, MC_RUN180C };
        for (int i = 0; i < 4; i++) {
            ms_arrow(hdc, n[MC_WALK].right, CY(MC_WALK), n[wt[i]].left, CY(wt[i]), ac, 0);
            ms_arrow(hdc, n[MC_RUN].right, CY(MC_RUN), n[rt[i]].left, CY(rt[i]), ac, 0);
        }
        ms_arrow(hdc, CX(MC_WALK), n[MC_WALK].bottom, CX(MC_STAND) + 20, n[MC_STAND].top, RGB(80, 80, 95), 1);
        ms_arrow(hdc, CX(MC_RUN), n[MC_RUN].top, CX(MC_STAND) + 20, n[MC_STAND].bottom, RGB(80, 80, 95), 1);
        ms_arrow(hdc, CX(MC_WALK) - 20, n[MC_WALK].bottom, CX(MC_RUN) - 20, n[MC_RUN].top, RGB(80, 80, 95), 1);
        ms_arrow(hdc, CX(MC_RUN) + 20, n[MC_RUN].top, CX(MC_WALK) + 20, n[MC_WALK].bottom, RGB(80, 80, 95), 1);
        SelectObject(hdc, ui_font(12, 0));
        ui_text(hdc, (CX(MC_STAND) + CX(MC_WALK)) / 2 - 90, (CY(MC_WALK) + CY(MC_RUN)) / 2 - 30, 150, 16, "arrives (no clip)", RGB(120, 120, 135), DT_CENTER | DT_SINGLELINE);
        ui_text(hdc, CX(MC_WALK) - 70, (CY(MC_WALK) + CY(MC_RUN)) / 2 - 8, 140, 16, "walk <-> run (no clip)", RGB(120, 120, 135), DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, ui_font(12, 1));
        ui_text(hdc, g_ms_graph_rc.left + 8, g_ms_graph_rc.bottom - 20, 600, 16, "CHANGES OF DIRECTION (45 DEG OR MORE) PLAY THE TURN CLIPS, THEN BACK TO THE CYCLE", RGB(90, 110, 140), DT_LEFT | DT_SINGLELINE);
    }
    #undef CX
    #undef CY
    for (int k = 0; k < MC_COUNT; k++) {
        if (n[k].right <= n[k].left) continue; /* not on this tab */
        int how = 0, clip = moveset_clip(ms, k, &how);
        int fits = clip >= 0 && anim_lib_fits(clip, g_ms_model->node_count);
        int big = k == MC_STAND || k == MC_WALK || k == MC_RUN;
        COLORREF border = how == 1 ? RGB(230, 190, 70) : RGB(80, 110, 150);
        ui_fill(hdc, &n[k], k == g_ms_slot ? RGB(60, 50, 12) : big ? RGB(34, 40, 52) : RGB(30, 30, 38));
        ui_frame(hdc, &n[k], k == g_ms_slot ? RGB(255, 210, 60) : border);
        if (k == g_ms_slot) { RECT in = n[k]; InflateRect(&in, -1, -1); ui_frame(hdc, &in, RGB(255, 210, 60)); }
        SelectObject(hdc, ui_font(big ? 16 : 13, 1));
        ui_text(hdc, n[k].left + 8, n[k].top + 2, n[k].right - n[k].left - 12, (n[k].bottom - n[k].top) / 2, MOVE_SLOT_LABEL[k], RGB(235, 235, 240), SV_ONE);
        if (how == 2) snprintf(t, sizeof(t), "no animation");
        else if (clip < 0) snprintf(t, sizeof(t), "missing clip");
        else if (!fits) snprintf(t, sizeof(t), "%s -- doesn't fit", anim_lib_name(clip));
        else snprintf(t, sizeof(t), "%s%s", anim_lib_name(clip), how == 1 ? "  (changed)" : "");
        SelectObject(hdc, ui_font(12, 0));
        ui_text(hdc, n[k].left + 8, (n[k].top + n[k].bottom) / 2, n[k].right - n[k].left - 12, (n[k].bottom - n[k].top) / 2 - 2, t,
                how == 2 ? RGB(150, 110, 110) : (clip < 0 || !fits) ? RGB(255, 110, 100) : how == 1 ? RGB(255, 210, 110) : RGB(150, 200, 255), SV_ONE);
    }
    /* stage: the slot's clip (or the list's) on the character, with its equipment (or the item tried) */
    int how = 0, slot_clip = !group_has_slots(g_ms_tab) ? moveset_clip(ms, MC_STAND, &how) : moveset_clip(ms, g_ms_slot, &how);
    int clip = g_ms_preview && g_ms_n > 0 && !g_ms_items && !g_ms_sounds ? g_ms_list[g_ms_sel] : slot_clip;
    int W = g_ms_stage_rc.right - g_ms_stage_rc.left, H = g_ms_stage_rc.bottom - g_ms_stage_rc.top;
    if (W > 16 && H > 16) {
        static uint32_t *px = NULL; static size_t cap = 0;
        if (cap < (size_t)W * H) { free(px); px = (uint32_t *)malloc((size_t)W * H * 4); cap = (size_t)W * H; }
        float dur = anim_lib_duration(clip);
        const char *tried = g_ms_items && g_ms_n > 0 ? g_item_names[g_ms_list[g_ms_sel]] : NULL;
        const CharModel *held = g_ms_items == 1 && tried ? item_held(tried, 0) : model_right_hand(g_ms_model);
        const CharModel *shield = g_ms_items == 2 && tried ? item_model(tried) : model_left_shield(g_ms_model);
        float ct = dur > 0 ? fmodf(g_ms_t, dur) : 0.0f;
        if (g_ms_cplay_t >= 0.0f) ct = dur > 0 ? fminf(g_ms_cplay_t, dur - 0.001f) : 0.0f; /* once, from its start */
        render_char_view(g_ms_model, held, shield, clip, ct, px, W, H, ANIM_DEFAULT_YAW + g_ms_t * 0.25f, 0.15f, 1.0f, 1, 1);
        blit_pixels(hdc, g_ms_stage_rc.left, g_ms_stage_rc.top, W, H, px);
    }
    ui_frame(hdc, &g_ms_stage_rc, RGB(60, 60, 70));
    SelectObject(hdc, ui_font(16, 1));
    snprintf(t, sizeof(t), "%s%s", g_ms_preview ? "Trying: " : "", clip >= 0 ? anim_lib_name(clip) : "(no animation: rest pose)");
    ui_text(hdc, g_ms_stage_rc.left + 12, g_ms_stage_rc.top + 8, W - 24, 22, t, g_ms_preview ? RGB(255, 210, 90) : RGB(255, 255, 255), SV_ONE);
    int ry = g_ms_stage_rc.bottom + 6;
    if (group_has_slots(g_ms_tab) && !g_ms_items && !g_ms_cues) {
        SelectObject(hdc, ui_font(15, 1));
        ui_text(hdc, g_ms_stage_rc.left, ry, W, 20, MOVE_SLOT_LABEL[g_ms_slot], RGB(255, 230, 90), SV_ONE);
        SelectObject(hdc, ui_font(13, 0));
        snprintf(t, sizeof(t), "%s  Now: %s%s", MOVE_SLOT_DESC[g_ms_slot], slot_clip >= 0 ? anim_lib_name(slot_clip) : "nothing",
                 how == 0 ? " (preset)" : how == 1 ? " (changed)" : " (no animation)");
        ui_text(hdc, g_ms_stage_rc.left, ry + 22, W, 32, t, RGB(200, 200, 205), DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    } else if (g_ms_cues) {
        SelectObject(hdc, ui_font(15, 1));
        ui_text(hdc, g_ms_stage_rc.left, ry, W, 20, MOVE_SLOT_LABEL[g_ms_slot], RGB(255, 230, 90), SV_ONE);
        SelectObject(hdc, ui_font(13, 0));
        ui_text(hdc, g_ms_stage_rc.left, ry + 22, W, 32, "Space or a second click: listen. Enter or a double-click: add it to the blow's sounds.", RGB(200, 200, 205), DT_LEFT | DT_WORDBREAK);
    } else if (g_ms_items) {
        SelectObject(hdc, ui_font(15, 1));
        ui_text(hdc, g_ms_stage_rc.left, ry, W, 20, g_ms_items == 2 ? "Equipped shield" : "Equipped weapon", RGB(255, 230, 90), SV_ONE);
        SelectObject(hdc, ui_font(13, 0));
        ui_text(hdc, g_ms_stage_rc.left, ry + 22, W, 32, "Select an item in the list to see it on the character; Enter / double-click: equip it.", RGB(200, 200, 205), DT_LEFT | DT_WORDBREAK);
    } else {
        SelectObject(hdc, ui_font(15, 1));
        ui_text(hdc, g_ms_stage_rc.left, ry, W, 20, "Sounds", RGB(255, 230, 90), SV_ONE);
        SelectObject(hdc, ui_font(13, 0));
        ui_text(hdc, g_ms_stage_rc.left, ry + 22, W, 32, "Space or a second click: listen. Enter or a double-click: add to the chosen pool.", RGB(200, 200, 205), DT_LEFT | DT_WORDBREAK);
    }
    /* list */
    SelectObject(hdc, ui_font(12, 1));
    int no_list = (g_ms_tab == PG_AI || g_ms_tab == PG_STATS) && !g_ms_items;
    if (no_list) t[0] = 0;
    else if (g_ms_items) snprintf(t, sizeof(t), "ITEMS (%d)   filter: %s_", g_ms_n, g_ms_filter);
    else if (g_ms_sounds) snprintf(t, sizeof(t), "SOUNDS   filter: %s_", g_ms_filter);
    else snprintf(t, sizeof(t), "CLIPS FOR THIS SKELETON (%d)   filter: %s_", g_ms_n, g_ms_filter);
    ui_text(hdc, g_ms_list_rc.left, g_ms_list_rc.top - 20, g_ms_list_rc.right - g_ms_list_rc.left, 16, t, SVL_SECTION, SV_ONE);
    if (!no_list) ui_fill(hdc, &g_ms_list_rc, RGB(22, 22, 28));
    int rows = no_list ? 0 : ms_rows_visible();
    for (int r = 0; r < rows; r++) {
        int k = g_ms_scroll + r;
        if (k >= g_ms_n) break;
        int y = g_ms_list_rc.top + r * MS_ROW_H;
        if (ms_row_header(k)) {
            RECT hr = { g_ms_list_rc.left, y + 1, g_ms_list_rc.right, y + MS_ROW_H };
            ui_fill(hdc, &hr, RGB(32, 36, 48));
            SelectObject(hdc, ui_font(12, 1));
            ui_text(hdc, g_ms_list_rc.left + 8, y, g_ms_list_rc.right - g_ms_list_rc.left - 16, MS_ROW_H, g_ms_snd[g_ms_list[k]].name, RGB(140, 180, 240), SV_ONE);
            continue;
        }
        if (k == g_ms_sel) { RECT hl = { g_ms_list_rc.left, y, g_ms_list_rc.right, y + MS_ROW_H }; ui_fill(hdc, &hl, RGB(60, 50, 10)); }
        if (g_ms_items) {
            const char *nm = g_item_names[g_ms_list[k]];
            int held = !strcmp(nm, g_ms_items == 2 ? ms->left_hand : ms->right_hand);
            SelectObject(hdc, ui_font(14, held));
            snprintf(t, sizeof(t), "%s%s", nm, held ? "   <- equipped" : "");
            ui_text(hdc, g_ms_list_rc.left + 8, y, g_ms_list_rc.right - g_ms_list_rc.left - 16, MS_ROW_H, t, k == g_ms_sel ? RGB(255, 230, 60) : RGB(210, 210, 210), SV_ONE);
        } else if (g_ms_sounds) {
            SelectObject(hdc, ui_font(14, 0));
            ui_text(hdc, g_ms_list_rc.left + 20, y, g_ms_list_rc.right - g_ms_list_rc.left - 28, MS_ROW_H, sound_shown(g_ms_snd[g_ms_list[k]].name), k == g_ms_sel ? RGB(255, 230, 60) : RGB(210, 210, 210), SV_ONE);
        } else {
            int id = g_ms_list[k];
            int own = strcmp(anim_lib_source(id), g_ms_model->name) == 0;
            SelectObject(hdc, ui_font(14, id == slot_clip));
            snprintf(t, sizeof(t), "%s%s%s", anim_lib_name(id), own ? "  (own)" : "", id == slot_clip ? "   <- this slot" : "");
            ui_text(hdc, g_ms_list_rc.left + 8, y, g_ms_list_rc.right - g_ms_list_rc.left - 80, MS_ROW_H, t,
                    k == g_ms_sel ? RGB(255, 230, 60) : own ? RGB(140, 200, 255) : RGB(210, 210, 210), SV_ONE);
            snprintf(t, sizeof(t), "%.2fs", anim_lib_duration(id));
            ui_text(hdc, g_ms_list_rc.right - 70, y, 62, MS_ROW_H, t, RGB(130, 130, 140), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    if (g_ms_text) { /* the save box, over the rest */
        int bx = rc.right / 2 - 260, byy = 120;
        RECT box = { bx, byy, bx + 520, byy + 160 };
        RECT shade = { 0, 0, rc.right, rc.bottom };
        (void)shade;
        ui_fill(hdc, &box, RGB(40, 34, 14)); ui_frame(hdc, &box, RGB(255, 210, 60));
        SelectObject(hdc, ui_font(16, 1));
        static const char *title[PG_COUNT] = { "Save the walking animations as a preset", "Save the attacks as a preset", "Save the sounds as a preset",
                                               "Save the reactions as a preset", "Save the AI as a preset", "Save the stats as a preset",
                                               "Save everything as a model preset" };
        ui_text(hdc, bx + 16, byy + 10, 488, 22, title[g_ms_text - 1], RGB(255, 225, 120), SV_ONE);
        SelectObject(hdc, ui_font(12, 1));
        ui_text(hdc, bx + 16, byy + 34, 488, 14, "NAME", SVL_SECTION, SV_ONE);
        RECT field = { bx + 16, byy + 48, bx + 504, byy + 74 };
        ui_fill(hdc, &field, RGB(16, 16, 20)); ui_frame(hdc, &field, RGB(120, 120, 130));
        SelectObject(hdc, ui_font(16, 0));
        snprintf(t, sizeof(t), "%s_", g_ms_text_buf);
        ui_text(hdc, field.left + 8, field.top, 470, 26, t, RGB(255, 255, 255), SV_ONE);
        ui_draw_buttons(hdc);
        const UiButton *cb = ui_find(B_MS_SAVE_DEFAULT);
        if (cb) { /* the tick box */
            RECT tick = { cb->r.left + 8, cb->r.top + 5, cb->r.left + 24, cb->r.top + 21 };
            ui_fill(hdc, &tick, RGB(16, 16, 20)); ui_frame(hdc, &tick, RGB(200, 200, 210));
            SelectObject(hdc, ui_font(15, 1));
            if (g_ms_text_default) ui_text(hdc, tick.left, tick.top - 2, 16, 18, "x", RGB(255, 220, 90), DT_CENTER | DT_SINGLELINE);
            SelectObject(hdc, ui_font(14, 0));
            snprintf(t, sizeof(t), "Make it %s's default", g_ms_model->name);
            ui_text(hdc, cb->r.left + 32, cb->r.top, 440, 26, t, RGB(230, 230, 235), SV_ONE);
        }
        if (g_ms_text == PG_MODEL + 1) {
            SelectObject(hdc, ui_font(12, 0));
            ui_text(hdc, bx + 16, byy + 122, 270, 30, "Tabs with changes are saved as presets too.", RGB(180, 180, 190), DT_LEFT | DT_WORDBREAK);
        }
    } else ui_draw_buttons(hdc);
    SelectObject(hdc, GetStockObject(SYSTEM_FONT));
}

static int ms_key(int vk) {
    if (g_ms_text) {
        if (vk == VK_RETURN) ms_action(B_MS_SAVE_OK);
        else if (vk == VK_ESCAPE) g_ms_text = 0;
        else if (vk == VK_TAB) g_ms_text_default = !g_ms_text_default;
        else if (vk == VK_BACK) { size_t l = strlen(g_ms_text_buf); if (l) g_ms_text_buf[l - 1] = 0; }
        return 1;
    }
    if (g_ms_items && vk == VK_ESCAPE) { ms_list_mode(0); return 1; }
    if (g_ms_cues) {
        if (vk == VK_ESCAPE) { ms_action(B_MS_CUE_DONE); return 1; }
        if (vk == VK_DELETE && g_ms_cue_sel >= 0) { ms_cue_op(g_ms_cue_sel, CUEOP_DEL); return 1; }
    }
    if (g_ms_sounds && vk == VK_SPACE) { /* listen */
        if (g_ms_n > 0 && !ms_row_header(g_ms_sel)) { if (audio_preview_playing()) audio_preview(NULL); else audio_preview(g_ms_snd[g_ms_list[g_ms_sel]].name); }
        return 1;
    }
    switch (vk) {
        case VK_ESCAPE: g_ms_view = 0; audio_preview(NULL); return 1;
        case VK_UP: ms_select(g_ms_sel - 1); return 1;
        case VK_DOWN: ms_select(g_ms_sel + 1); return 1;
        case VK_PRIOR: ms_select(g_ms_sel - ms_rows_visible()); return 1;
        case VK_NEXT: ms_select(g_ms_sel + ms_rows_visible()); return 1;
        case VK_RETURN: ms_action(B_MS_USE); return 1;
        case VK_TAB: { /* the next slot of the tab */
            if (g_ms_tab == PG_SOUND) { g_ms_pool = (g_ms_pool + 1) % POOL_COUNT; return 1; }
            if (!group_has_slots(g_ms_tab)) return 1;
            int first = g_ms_tab == PG_COMBAT ? MC_FIRST_COMBAT : g_ms_tab == PG_REACT ? MC_FIRST_REACT : 0;
            int count = g_ms_tab == PG_COMBAT ? MC_COUNT - MC_FIRST_COMBAT : g_ms_tab == PG_REACT ? MC_FIRST_COMBAT - MC_FIRST_REACT : MC_FIRST_REACT;
            ms_select_slot(first + (g_ms_slot - first + 1 + count) % count);
            return 1;
        }
        case VK_BACK: if (g_ms_filter_len > 0) { g_ms_filter[--g_ms_filter_len] = 0; ms_filter(); } return 1;
    }
    return 0;
}
static void ms_char(char c) {
    if (g_ms_text) { /* a new preset's name */
        size_t l = strlen(g_ms_text_buf);
        if (c >= 32 && c < 127 && l < sizeof(g_ms_text_buf) - 1) { g_ms_text_buf[l] = c; g_ms_text_buf[l + 1] = 0; }
        return;
    }
    if (c > 32 && c < 127 && g_ms_filter_len < (int)sizeof(g_ms_filter) - 1) { g_ms_filter[g_ms_filter_len++] = c; g_ms_filter[g_ms_filter_len] = 0; ms_filter(); }
}
static void ms_mouse_down(HWND hwnd, int x, int y, int dbl) {
    ms_layout(hwnd);
    int b = ui_hit(x, y);
    if (g_ms_text) { if (b == B_MS_SAVE_OK || b == B_MS_SAVE_CANCEL || b == B_MS_SAVE_DEFAULT) ms_action(b); return; }
    if (b != B_NONE) { ms_action(b); return; }
    POINT p = { x, y };
    for (int k = 0; k < MC_COUNT; k++) if (PtInRect(&g_ms_node_rc[k], p)) { ms_select_slot(k); return; }
    for (int q = 0; q < POOL_COUNT; q++) if (PtInRect(&g_ms_pool_rc[q], p)) { g_ms_pool = q; return; }
    if (PtInRect(&g_ms_list_rc, p)) {
        int k = g_ms_scroll + (y - g_ms_list_rc.top) / MS_ROW_H;
        if (k < g_ms_n && !ms_row_header(k)) {
            int again = k == g_ms_sel;
            ms_select(k);
            if (dbl) ms_action(B_MS_USE);
            else if (g_ms_sounds && again) audio_preview(g_ms_snd[g_ms_list[k]].name); /* a second click: listen */
        }
    }
}
static void ms_wheel(int delta) {
    g_ms_scroll -= (delta / 120) * 3;
    if (g_ms_scroll > g_ms_n - ms_rows_visible()) g_ms_scroll = g_ms_n - ms_rows_visible();
    if (g_ms_scroll < 0) g_ms_scroll = 0;
}

/* the point being given a facing (PLACE, second click): marked in the room */
static int sv_pick_marker(float out[3]) {
    ScriptAction *a = sv_cell(0);
    if (g_sv_pick != SV_PICK_PLACE_FACE || !a || !a->has_pos) return 0;
    memcpy(out, a->pos, sizeof(float) * 3);
    return 1;
}

/* =====================================================================
   ROOM TRANSITION: through a connector, the picture fades to black while
   the sounds and ambiences fade out (not the music), the "loading"
   animation (the blockouts' sprites spinall.0-29, 30 fps, looping) plays
   on black, the new room is loaded at its start and fades in after it.
   The characters and the scripts wait while it's black.
   ===================================================================== */
#define TR_FADE_OUT 1.0f
#define TR_LOADING 2.0f
#define TR_FADE_IN 0.5f
#define SPIN_FRAMES 30

static void transition_start(int door) {
    g_tr.phase = 1; g_tr.t = 0.0f; g_tr.door = door; g_tr.room[0] = 0;
    audio_fade_kind(AUDIO_SOUND, TR_FADE_OUT);
    audio_fade_kind(AUDIO_AMBIENCE, TR_FADE_OUT);
    g_click_marker_active = 0;
}
static void transition_start_room(const char *label, int door, const char *script) {
    transition_start(-1);
    snprintf(g_tr.room, sizeof(g_tr.room), "%s", label);
    g_tr.room_door = door;
    snprintf(g_tr.script, sizeof(g_tr.script), "%s", script);
}
/* game_tick: returns 1 when the room changed (whole window to repaint) */
static int transition_tick(HWND hwnd, float dt) {
    if (!g_tr.phase) return 0;
    g_tr.t += dt;
    if (g_tr.phase == 1 && g_tr.t >= TR_FADE_OUT) {
        g_tr.phase = 2; g_tr.t = 0.0f;
        if (g_tr.room[0]) room_travel(hwnd, g_tr.room, g_tr.room_door, g_tr.script);
        else door_travel(hwnd, g_tr.door);
        return 1;
    }
    if (g_tr.phase == 2 && g_tr.t >= TR_LOADING) { g_tr.phase = 3; g_tr.t = 0.0f; }
    else if (g_tr.phase == 3 && g_tr.t >= TR_FADE_IN) g_tr.phase = 0;
    return 0;
}
static int transition_frozen(void) { return g_tr.phase == 2; }

/* Alpha-blends a sprite (0xAARRGGBB) into 0x00RRGGBB pixels, scaled (nearest). */
static void blend_sprite(uint32_t *px, int W, int H, const uint32_t *spr, int sw, int sh, int x0, int y0, float sc) {
    int dw = (int)(sw * sc), dh = (int)(sh * sc);
    for (int y = 0; y < dh; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= H) continue;
        const uint32_t *srow = spr + (size_t)(int)(y / sc) * sw;
        for (int x = 0; x < dw; x++) {
            int xx = x0 + x;
            if (xx < 0 || xx >= W) continue;
            uint32_t c = srow[(int)(x / sc)], a = c >> 24;
            if (!a) continue;
            uint32_t *d = &px[(size_t)yy * W + xx];
            *d = a >= 255 ? (c & 0x00FFFFFFu) : lerp_rgb(*d, c & 0x00FFFFFFu, a);
        }
    }
}

/* the view layer (the game picture, screen pixels) darkened / black + spinner */
static void transition_draw(uint32_t *px, int W, int H, float sc) {
    if (!g_tr.phase) return;
    size_t n = (size_t)W * H;
    float k = g_tr.phase == 1 ? 1.0f - g_tr.t / TR_FADE_OUT : g_tr.phase == 3 ? g_tr.t / TR_FADE_IN : 0.0f;
    if (k <= 0.0f) memset(px, 0, n * sizeof(uint32_t));
    else if (k < 1.0f) { uint32_t w = (uint32_t)((1.0f - k) * 256.0f); for (size_t i = 0; i < n; i++) px[i] = lerp_rgb(px[i], 0, w); }
    if (g_tr.phase != 2) return;
    static uint32_t *frames[SPIN_FRAMES];
    static int fw[SPIN_FRAMES], fh[SPIN_FRAMES], loaded = 0;
    if (!loaded) {
        loaded = 1;
        for (int i = 0; i < SPIN_FRAMES; i++) {
            char path[1024];
            root_path(path, sizeof(path), "assets/sprites/spinall.%d.png", i);
            frames[i] = image_load(path, &fw[i], &fh[i]);
        }
    }
    int f = (int)(g_tr.t * 30.0f) % SPIN_FRAMES;
    if (!frames[f]) return;
    float s2 = sc * 2.0f;
    blend_sprite(px, W, H, frames[f], fw[f], fh[f], W / 2 - (int)(fw[f] * s2) / 2, H / 2 - (int)(fh[f] * s2) / 2, s2);
}

/* ===================================================================== */
/* SILVER_PERF=<file>: once per second, appends frames drawn, average and
   worst paint time and the worst gap between frames (measuring tool). */
static double perf_now_ms(void) {
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static double g_perf_sec[4]; /* scene, stretch, david, panels (ms, this frame) */
static void perf_frame(double paint_ms) {
    static const char *path = (const char *)-1;
    static double win_start = 0, sum = 0, worst = 0, last_end = 0, worst_gap = 0, sec[4];
    static int frames = 0;
    if (path == (const char *)-1) path = getenv("SILVER_PERF");
    if (!path) return;
    double now = perf_now_ms();
    if (last_end > 0 && now - last_end > worst_gap) worst_gap = now - last_end;
    last_end = now;
    if (win_start == 0) win_start = now;
    frames++; sum += paint_ms; if (paint_ms > worst) worst = paint_ms;
    for (int k = 0; k < 4; k++) { sec[k] += g_perf_sec[k]; g_perf_sec[k] = 0; }
    if (now - win_start >= 1000.0) {
        FILE *f = fopen(path, "a");
        if (f) { fprintf(f, "fps %.1f  paint avg %.2f ms  worst %.2f ms  worst gap %.1f ms  [scene %.2f  stretch %.2f  david %.2f  panels %.2f]\n", frames * 1000.0 / (now - win_start), sum / frames, worst, worst_gap, sec[0] / frames, sec[1] / frames, sec[2] / frames, sec[3] / frames); fclose(f); }
        win_start = now; sum = worst = worst_gap = 0; frames = 0; sec[0] = sec[1] = sec[2] = sec[3] = 0;
    }
}

/* Normal view: the characters drawn straight into the SCALED picture (screen
   pixels, true colour from his real texture) instead of into the 8-bit
   640x480 room frame that is then stretched -- he stays sharp at any
   scale and keeps his real colours. Hidden by the room exactly as before
   (room depth sampled under each screen pixel) and by FOREGROUND shapes.
   px: 0x00RRGGBB top-down, W x H = the displayed part of the room;
   screen (x, y) <-> room (cam_x + x / sc, cam_y + y / sc). */
/* One mesh (a body, an item) already projected into room pixels: rasterized
   at screen resolution, hidden by the room and the foreground shapes. */
static void render_mesh_hires(const CharModel *m, float *vx, float *vy, const float *vz, const int *vis,
                              uint32_t *px, int W, int H, float sc, float *zb, uint32_t *zst, uint32_t stamp) {
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    for (int i = 0; i < m->vertex_count; i++) {
        if (!vis[i]) continue;
        vx[i] = (vx[i] - g_cam_x) * sc; vy[i] = (vy[i] - g_cam_y) * sc;
        bx0 = fminf(bx0, vx[i]); by0 = fminf(by0, vy[i]); bx1 = fmaxf(bx1, vx[i]); by1 = fmaxf(by1, vy[i]);
    }
    int X0 = (int)floorf(bx0), Y0 = (int)floorf(by0), X1 = (int)ceilf(bx1), Y1 = (int)ceilf(by1);
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 >= W) X1 = W - 1;
    if (Y1 >= H) Y1 = H - 1;
    if (X0 > X1 || Y0 > Y1) return;
    const float *room_depth = room_depth_cached();
    ensure_fg_mask();
    int fg = g_fg_any && !g_view_mode_3d && !g_free_cam;
    int RW = (int)g_hdr.width, RH = (int)g_hdr.height;
    for (int t = 0; t < m->index_count / 3; t++) {
        int i0 = m->indices[t * 3], i1 = m->indices[t * 3 + 1], i2 = m->indices[t * 3 + 2];
        if (!vis[i0] || !vis[i1] || !vis[i2]) continue;
        float x0 = vx[i0], y0 = vy[i0], x1 = vx[i1], y1 = vy[i1], x2 = vx[i2], y2 = vy[i2];
        float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (fabsf(den) < 1e-8f) continue;
        int mx0 = (int)floorf(fminf(x0, fminf(x1, x2))), mx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
        int my0 = (int)floorf(fminf(y0, fminf(y1, y2))), my1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
        if (mx0 < X0) mx0 = X0;
        if (my0 < Y0) my0 = Y0;
        if (mx1 > X1) mx1 = X1;
        if (my1 > Y1) my1 = Y1;
        float iz0 = 1.0f / vz[i0], iz1 = 1.0f / vz[i1], iz2 = 1.0f / vz[i2];
        float u0 = m->uvs_px[i0][0] * iz0, u1 = m->uvs_px[i1][0] * iz1, u2 = m->uvs_px[i2][0] * iz2;
        float v0 = m->uvs_px[i0][1] * iz0, v1 = m->uvs_px[i1][1] * iz1, v2 = m->uvs_px[i2][1] * iz2;
        for (int y = my0; y <= my1; y++) {
            float fy = y + 0.5f;
            int ry = g_cam_y + (int)(fy / sc);
            for (int x = mx0; x <= mx1; x++) {
                float fx = x + 0.5f;
                float w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / den;
                float w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / den;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                float iz = w0 * iz0 + w1 * iz1 + w2 * iz2;
                float z = 1.0f / iz;
                size_t i = (size_t)y * W + x;
                if (zst[i] == stamp && z >= zb[i]) continue; /* a nearer triangle (this character or another) */
                int rx = g_cam_x + (int)(fx / sc);
                if (rx >= 0 && rx < RW && ry >= 0 && ry < RH) {
                    size_t ridx = (size_t)ry * g_dib_stride + rx;
                    if (room_depth && z >= room_depth[ridx]) continue; /* behind the scenery */
                    if (fg && g_fg_mask[ridx]) continue;              /* foreground shape */
                }
                zb[i] = z; zst[i] = stamp;
                int iu = (int)((w0 * u0 + w1 * u1 + w2 * u2) / iz), iv = (int)((w0 * v0 + w1 * v1 + w2 * v2) / iz);
                if (iu < 0) iu = 0;
                if (iu >= m->tex_w) iu = m->tex_w - 1;
                if (iv < 0) iv = 0;
                if (iv >= m->tex_h) iv = m->tex_h - 1;
                const uint8_t *col = m->tex_rgb + ((size_t)iv * m->tex_w + iu) * 3;
                px[i] = ((uint32_t)col[0] << 16) | ((uint32_t)col[1] << 8) | col[2];
            }
        }
    }
}
static void render_actor_hires(Actor *a, uint32_t *px, int W, int H, float sc, float *zb, uint32_t *zst, uint32_t stamp) {
    static float vx[DAVID_MAX_VERTS], vy[DAVID_MAX_VERTS], vz[DAVID_MAX_VERTS];
    static int vis[DAVID_MAX_VERTS];
    actor_project(a, vx, vy, vz, vis);
    render_mesh_hires(a->model, vx, vy, vz, vis, px, W, H, sc, zb, zst, stamp);
    if (actor_project_item(a, 0, vx, vy, vz, vis)) render_mesh_hires(a->item, vx, vy, vz, vis, px, W, H, sc, zb, zst, stamp);
    if (actor_project_item(a, 1, vx, vy, vz, vis)) render_mesh_hires(a->item_l, vx, vy, vz, vis, px, W, H, sc, zb, zst, stamp);
    if (actor_project_item(a, 2, vx, vy, vz, vis)) render_mesh_hires(a->shield_m, vx, vy, vz, vis, px, W, H, sc, zb, zst, stamp);
    if (actor_project_item(a, 3, vx, vy, vz, vis)) render_mesh_hires(a->potion_m, vx, vy, vz, vis, px, W, H, sc, zb, zst, stamp);
    orb_glow_draw(a, px, W, H, sc);
    blade_sample(a);
    if (a->trail) trail_sample(a);
}
/* Every character of the room. One depth buffer for all of them, never
   cleared: a pixel's depth only counts if it was written this frame
   (zst == stamp), so the characters hide each other correctly. */
static void debris_draw(uint32_t *px, int W, int H, float sc, float *zb, uint32_t *zst, uint32_t stamp);
static void trails_draw(uint32_t *px, int W, int H, float sc);
static void proj_models_draw(uint32_t *px, int W, int H, float sc, float *zb, uint32_t *zst, uint32_t stamp);
static void fx_draw(uint32_t *px, int W, int H, float sc);
static void orb_glow_draw(Actor *a, uint32_t *px, int W, int H, float sc);
static void render_david_hires(uint32_t *px, int W, int H, float sc) {
    if (!g_has_3d_character || !g_loaded || W <= 0 || H <= 0) return;
    static float *zb = NULL; static uint32_t *zst = NULL; static size_t zbn = 0; static uint32_t stamp = 0;
    if (zbn < (size_t)W * H) {
        free(zb); free(zst);
        zb = (float *)malloc((size_t)W * H * sizeof(float)); zst = (uint32_t *)calloc((size_t)W * H, sizeof(uint32_t));
        zbn = (zb && zst) ? (size_t)W * H : 0;
        if (!zbn) return;
        stamp = 0;
    }
    stamp++;
    for (int k = 0; k < MAX_ACTORS; k++) if (g_actors[k].used) render_actor_hires(&g_actors[k], px, W, H, sc, zb, zst, stamp);
    debris_draw(px, W, H, sc, zb, zst, stamp);
    proj_models_draw(px, W, H, sc, zb, zst, stamp);
    fx_draw(px, W, H, sc);
    trails_draw(px, W, H, sc);
}

/* The visible part of the 8-bit room frame -> the 32-bit view layer,
   scaled with BILINEAR filtering (what GDI's HALFTONE StretchBlt did, at
   ~6.4 ms a frame in 1920x1080 -- this is several times faster): each
   source row is scaled horizontally once, then output rows blend two of
   those, 2 channels at a time in one 32-bit multiply. */
static inline uint32_t lerp_rgb(uint32_t a, uint32_t b, uint32_t w) { /* w in 0..256 */
    uint32_t iw = 256 - w;
    uint32_t rb = (((a & 0xFF00FFu) * iw + (b & 0xFF00FFu) * w) >> 8) & 0xFF00FFu;
    uint32_t g = (((a & 0x00FF00u) * iw + (b & 0x00FF00u) * w) >> 8) & 0x00FF00u;
    return rb | g;
}
static void scale_room_rows(uint32_t *dst, int dw, int dh, int win_w, int win_h, int ya, int yb) {
    const uint32_t *src0 = g_render_pixels + (size_t)g_cam_y * g_dib_stride + g_cam_x;
    if (dw == win_w && dh == win_h) { /* 1:1 (windowed) */
        for (int y = ya; y < yb; y++) {
            memcpy(dst + (size_t)y * dw, src0 + (size_t)y * g_dib_stride, sizeof(uint32_t) * dw);
        }
        return;
    }
    static int *xi = NULL; static uint16_t *xw = NULL; static uint32_t *rows = NULL; static int cap_w = 0;
    if (cap_w < dw) {
        free(xi); free(xw); free(rows);
        xi = (int *)malloc(sizeof(int) * dw); xw = (uint16_t *)malloc(sizeof(uint16_t) * dw);
        rows = (uint32_t *)malloc(sizeof(uint32_t) * dw * 2);
        if (!xi || !xw || !rows) { cap_w = 0; return; }
        cap_w = dw;
    }
    float kx = (float)win_w / dw, ky = (float)win_h / dh;
    for (int x = 0; x < dw; x++) {
        float sx = (x + 0.5f) * kx - 0.5f;
        if (sx < 0) sx = 0;
        int x0 = (int)sx; if (x0 > win_w - 1) x0 = win_w - 1;
        xi[x] = x0; xw[x] = (x0 + 1 < win_w) ? (uint16_t)((sx - x0) * 256.0f) : 0;
    }
    uint32_t *ra = rows, *rb = rows + cap_w; /* horizontally scaled source rows y0 / y1 */
    int ia = -1, ib = -1;
    for (int y = ya; y < yb; y++) {
        float sy = (y + 0.5f) * ky - 0.5f;
        if (sy < 0) sy = 0;
        int y0 = (int)sy; if (y0 > win_h - 1) y0 = win_h - 1;
        int y1 = y0 + 1 < win_h ? y0 + 1 : y0;
        uint32_t wy = (uint32_t)((sy - y0) * 256.0f);
        if (ia != y0 && ib == y0) { uint32_t *t = ra; ra = rb; rb = t; ia = y0; ib = -1; } /* moved down one row */
        for (int k = 0; k < 2; k++) {
            int want = k ? y1 : y0; int *have = k ? &ib : &ia; uint32_t *o = k ? rb : ra;
            if (*have == want) continue;
            const uint32_t *s = src0 + (size_t)want * g_dib_stride;
            for (int x = 0; x < dw; x++) {
                int x0 = xi[x];
                o[x] = xw[x] ? lerp_rgb(s[x0], s[x0 + 1], xw[x]) : s[x0];
            }
            *have = want;
        }
        uint32_t *r[2] = { ra, rb };
        uint32_t *d = dst + (size_t)y * dw;
        if (wy == 0 || y0 == y1) memcpy(d, r[0], sizeof(uint32_t) * dw);
        else for (int x = 0; x < dw; x++) d[x] = lerp_rgb(r[0][x], r[1][x], wy);
    }
}

/* The scaled room picture only changes where the 8-bit frame changed
   (David's shadow, animated overlays, tints...) or when the view scrolls:
   it's kept scaled, only the destination rows depending on changed
   source rows are re-scaled, then it's copied into the layer (which
   David and the markers are drawn over, fresh, every frame). */
static void compose_room_layer(uint32_t *layer, int dw, int dh, int win_w, int win_h) {
    static uint32_t *clean = NULL, *prev = NULL; static size_t cap_c = 0, cap_p = 0;
    static int k_cx = -1, k_cy = -1, k_dw = -1, k_dh = -1, k_ww = -1, k_wh = -1;
    static const uint32_t *k_frame = NULL;
    size_t nc = (size_t)dw * dh, np = (size_t)win_w * win_h;
    const uint32_t *src0 = g_render_pixels + (size_t)g_cam_y * g_dib_stride + g_cam_x;
    int same = clean && prev && cap_c >= nc && cap_p >= np && k_cx == g_cam_x && k_cy == g_cam_y && k_dw == dw && k_dh == dh &&
               k_ww == win_w && k_wh == win_h && k_frame == g_render_pixels;
    if (!same) {
        if (cap_c < nc) { free(clean); clean = (uint32_t *)malloc(nc * sizeof(uint32_t)); cap_c = clean ? nc : 0; }
        if (cap_p < np) { free(prev); prev = (uint32_t *)malloc(np * sizeof(uint32_t)); cap_p = prev ? np : 0; }
        if (!clean || !prev) { scale_room_rows(layer, dw, dh, win_w, win_h, 0, dh); return; }
        scale_room_rows(clean, dw, dh, win_w, win_h, 0, dh);
        for (int r = 0; r < win_h; r++) memcpy(prev + (size_t)r * win_w, src0 + (size_t)r * g_dib_stride, sizeof(uint32_t) * win_w);
        k_cx = g_cam_x; k_cy = g_cam_y; k_dw = dw; k_dh = dh; k_ww = win_w; k_wh = win_h; k_frame = g_render_pixels;
    } else {
        int r0 = -1, r1 = -1;
        for (int r = 0; r < win_h; r++) {
            const uint32_t *s = src0 + (size_t)r * g_dib_stride;
            uint32_t *q = prev + (size_t)r * win_w;
            if (memcmp(q, s, sizeof(uint32_t) * win_w) != 0) { if (r0 < 0) r0 = r; r1 = r; memcpy(q, s, sizeof(uint32_t) * win_w); }
        }
        if (r0 >= 0) {
            float ky = (float)win_h / dh; /* dest y reads source rows floor(sy), floor(sy)+1 with sy = (y+0.5)*ky-0.5 */
            int ya = (int)floorf((r0 - 1 + 0.5f) / ky - 0.5f) - 1, yb = (int)ceilf((r1 + 1 + 0.5f) / ky - 0.5f) + 2;
            if (ya < 0) ya = 0;
            if (yb > dh) yb = dh;
            scale_room_rows(clean, dw, dh, win_w, win_h, ya, yb);
        }
    }
    memcpy(layer, clean, nc * sizeof(uint32_t));
}

/* 32-bit view layer the scaled picture is composed into (cached). */
static uint32_t *view_layer(HDC ref, int w, int h, HDC *out_dc) {
    static HDC dc = NULL; static HBITMAP bmp = NULL, old = NULL; static void *bits = NULL; static int cw = 0, ch = 0;
    if (!dc || w != cw || h != ch) {
        if (dc) { SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); }
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(ref);
        bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        old = (HBITMAP)SelectObject(dc, bmp);
        cw = w; ch = h;
    }
    *out_dc = dc;
    return (uint32_t *)bits;
}

/* One game step (movement, turn-starts, doors, edge scrolling) + a
   repaint. Called by the main loop once per DISPLAY frame (DwmFlush), with
   a precise clock -- GetTickCount's 15.6 ms steps made motion stutter. */
static double g_last_game_tick_ms = 0;
static void game_tick(HWND hwnd) {
        double now = perf_now_ms();
        double dt = g_last_game_tick_ms > 0 ? (now - g_last_game_tick_ms) / 1000.0 : 0.0;
        g_last_game_tick_ms = now;
        if (dt > 0.1) dt = 0.1; /* clamp huge gaps (e.g. window drag) */
        int frozen = transition_frozen(); /* black between two rooms: everything waits */
        if (!frozen) script_tick(hwnd, (float)dt); /* a script playing: its rows, before the characters move */
        int need_repaint = 0;
        if (g_has_3d_character && !frozen) {
            for (int k = 0; k < MAX_ACTORS; k++) {
                if (!g_actors[k].used) continue;
                actor_begin(&g_actors[k]); need_repaint |= advance_character((float)dt * (g_actors[k].slow_t > 0.0f ? 0.35f : 1.0f)); actor_end();
                actor_cue_tick(&g_actors[k], (float)dt);
            }
            combat_tick((float)dt);
        } else need_repaint = advance_player(dt);
        if (!frozen && overlays_tick((float)dt)) need_repaint = 1;
        if (debris_tick((float)dt) || g_trail_n > 0) need_repaint = 1;
        if (camera_tick((float)dt)) need_repaint = 1;
        if (fcam_tick((float)dt)) need_repaint = 1;
        if (radial_tick((float)dt)) need_repaint = 1;
        guard_tick();
        if (g_anim_view) { if (!g_anim_paused) g_anim_t += (float)dt * g_anim_speed; need_repaint = 1; }
        if (g_ms_view) { g_ms_t += (float)dt; ms_cue_tick((float)dt); }
        if (g_script_view) g_ch_t += (float)dt;
        static int last_run_row = -1;
        int run_row = script_playing() ? g_run.row : -1;
        int full_repaint = screen_view() || run_row != last_run_row; /* whole-window screens; the script's row in the side panel */
        last_run_row = run_row;
        if (g_door_travel_request >= 0 && !g_tr.phase) { /* he reached a connector: fade out, then the other room */
            transition_start(g_door_travel_request);
            g_door_travel_request = -1;
        }
        if (g_tr.phase) { need_repaint = 1; if (transition_tick(hwnd, (float)dt)) full_repaint = 1; }
        /* CAPS LOCK / focus -> cursor clip; the panel shows the state */
        static int last_caps = -1;
        int caps = caps_on();
        if (caps != last_caps) { last_caps = caps; full_repaint = 1; }
        update_cursor_clip(hwnd);
        /* Playing: mouse at an edge of the game picture scrolls the
           VIEW over the composite (never the 3D camera), with the blockouts'
           directional cursors (mouse.1..8). */
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        int dir = (GetForegroundWindow() == hwnd) ? pan_direction(pt.x, pt.y) : 0;
        if (dir != g_pan_dir) { g_pan_dir = dir; SetCursor(cursor_for(pt.x, pt.y)); }
        static float scroll_acc = 0.0f;
        if (dir) {
            /* 480 composite pixels per second, whatever the frame rate */
            scroll_acc += 480.0f * (float)dt;
            int S = (int)scroll_acc;
            scroll_acc -= S;
            int old_cx = g_cam_x, old_cy = g_cam_y;
            if (dir == 1 || dir == 5 || dir == 6) g_cam_y -= S;
            if (dir == 2 || dir == 7 || dir == 8) g_cam_y += S;
            if (dir == 3 || dir == 5 || dir == 7) g_cam_x -= S;
            if (dir == 4 || dir == 6 || dir == 8) g_cam_x += S;
            clamp_camera();
            if (g_cam_x != old_cx || g_cam_y != old_cy) need_repaint = 1;
        } else scroll_acc = 0.0f;
        if (full_repaint) { InvalidateRect(hwnd, NULL, FALSE); UpdateWindow(hwnd); }
        else if (need_repaint) {
            /* only the game picture: the side panels are redrawn when
               something invalidates the whole window (input, room, status) */
            RECT gr; game_rect_client(&gr);
            InvalidateRect(hwnd, &gr, FALSE); UpdateWindow(hwnd);
        }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            audio_shutdown();
            ClipCursor(NULL);
            PostQuitMessage(0);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) { ClipCursor(NULL); g_clip_active = 0; }
            return 0;
        case WM_KEYDOWN: {
            int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wParam == VK_CAPITAL) { update_cursor_clip(hwnd); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == VK_CONTROL && !(lParam & 0x40000000)) { /* attack mode on: its cursor, its help */
                POINT cp; GetCursorPos(&cp); ScreenToClient(hwnd, &cp); SetCursor(cursor_for(cp.x, cp.y));
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (g_ms_view) { if (ms_key((int)wParam)) InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_anim_view) { if (anim_view_key((int)wParam)) InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_script_view) { if (sv_key(hwnd, (int)wParam)) InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == VK_F4 && g_loaded && (g_edit_mode || g_sv_pick)) { fcam_toggle(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_free_cam) { /* flying: its keys are its own (fcam_tick reads them) */
                if (wParam == VK_ESCAPE) { fcam_toggle(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
                if (strchr("WASDQE", (int)wParam) || wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT) return 0;
            }
            if (g_sv_pick) { /* picking a script action's point: Esc cancels (a facing: keeps it) */
                if (wParam == VK_ESCAPE) sv_pick_click(0, 0, 1);
                else if (wParam == VK_F11) ui_action(hwnd, B_FULL);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_radial && wParam == VK_ESCAPE) { radial_close(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (wParam == 'K' && ctrl && g_has_3d_character && !g_edit_mode) { shield_break(DAVID_ACTOR); InvalidateRect(hwnd, NULL, FALSE); return 0; } /* test */
            if (wParam == 'H' && ctrl && g_has_3d_character && !g_edit_mode) { actor_hit(DAVID_ACTOR); return 0; } /* test */
            if (script_playing()) { /* a cutscene: nothing but a click to skip (and Esc to stop, for testing) */
                if (wParam == VK_ESCAPE) script_stop("stopped");
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_TAB) {
                if (g_map_pick_target) { wizard_cancel(NULL); g_map_mode = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
                if (g_map_mode) g_map_mode = 0; else ui_action(hwnd, B_LIST);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_map_mode) {
                if (wParam == VK_ESCAPE) {
                    if (g_map_pick_target) wizard_cancel(NULL);
                    g_map_mode = 0;
                } else if (wParam == VK_UP) {
                    if (g_map_selected > 0) g_map_selected--;
                    if (g_map_selected < g_map_scroll) g_map_scroll = g_map_selected;
                } else if (wParam == VK_DOWN) {
                    if (g_map_selected < g_map_filtered_count - 1) g_map_selected++;
                    if (g_map_selected >= g_map_scroll + MAP_VISIBLE_ROWS) g_map_scroll = g_map_selected - MAP_VISIBLE_ROWS + 1;
                } else if (wParam == VK_PRIOR) {
                    g_map_selected -= MAP_VISIBLE_ROWS; if (g_map_selected < 0) g_map_selected = 0;
                    g_map_scroll = g_map_selected;
                } else if (wParam == VK_NEXT) {
                    g_map_selected += MAP_VISIBLE_ROWS;
                    if (g_map_selected > g_map_filtered_count - 1) g_map_selected = g_map_filtered_count > 0 ? g_map_filtered_count - 1 : 0;
                    g_map_scroll = g_map_selected - MAP_VISIBLE_ROWS + 1; if (g_map_scroll < 0) g_map_scroll = 0;
                } else if (wParam == VK_BACK) {
                    if (g_map_filter_len > 0) { g_map_filter[--g_map_filter_len] = 0; map_recompute_filter(); }
                } else if (wParam == VK_RETURN && g_map_filtered_count > 0) {
                    if (g_map_pick_target) wizard_target_chosen(hwnd, g_map_filtered[g_map_selected]);
                    else { go_to_room_index(hwnd, g_map_filtered[g_map_selected]); g_map_mode = 0; }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (wParam == VK_PRIOR) { ui_action(hwnd, B_PREV); return 0; }
            if (wParam == VK_NEXT) { ui_action(hwnd, B_NEXT); return 0; }
            if (g_edit_mode && g_has_3d_character) {
                int handled = 1;
                if (wParam == 'V') edit_set_tool(TOOL_SELECT);
                else if (wParam == '1' || wParam == VK_NUMPAD1) edit_set_tool(TOOL_RECT);
                else if (wParam == '2' || wParam == VK_NUMPAD2) edit_set_tool(TOOL_CIRCLE);
                else if (wParam == '3' || wParam == VK_NUMPAD3) edit_set_tool(TOOL_POLY);
                else if (wParam == 'Z' && ctrl) ui_action(hwnd, B_UNDO);
                else if (wParam == 'N') ui_action(hwnd, B_SETTINGS);
                else if (wParam == VK_DELETE) ui_action(hwnd, B_DELETE);
                else if (wParam == VK_RETURN) {
                    if (g_tool == TOOL_POLY && g_poly.n >= 3) { g_poly.type = 2; Shape z = g_poly; g_poly.n = 0; shape_add(&z); }
                } else if (wParam == VK_BACK) {
                    if (g_tool == TOOL_POLY && g_poly.n > 0) g_poly.n--;
                } else if (wParam == VK_ESCAPE) {
                    if (g_wiz != WIZ_NONE) wizard_cancel(g_wiz == WIZ_OTHER_SHAPE ? "the connector stays one-way" : NULL);
                    else if (g_poly.n > 0 || g_drag != DRAG_NONE) { g_poly.n = 0; g_drag = DRAG_NONE; }
                    else g_sel = -1;
                } else handled = 0;
                if (handled) { InvalidateRect(hwnd, NULL, FALSE); return 0; }
            }
            switch (wParam) {
                case 'P': ui_action(hwnd, B_WALK); return 0;
                case 'A': ui_action(hwnd, B_ANIMS); g_anim_skip_char = g_anim_view; return 0;
                case 'E': ui_action(hwnd, B_EDIT); return 0;
                case 'S': ui_action(hwnd, B_SCRIPTS); g_anim_skip_char = g_script_view; return 0;
                case 'B': ui_action(hwnd, B_HITBOX); return 0;
                case 'C': ui_action(hwnd, B_COLL); return 0;
                case VK_F11: ui_action(hwnd, B_FULL); return 0;
                case VK_F3: ui_action(hwnd, B_MESH); return 0;
            }
            int moved = 1;
            switch (wParam) {
                case VK_LEFT:  g_cam_x -= PAN_STEP; break;
                case VK_RIGHT: g_cam_x += PAN_STEP; break;
                case VK_UP:    g_cam_y -= PAN_STEP; break;
                case VK_DOWN:  g_cam_y += PAN_STEP; break;
                default: moved = 0; break;
            }
            if (moved) { clamp_camera(); InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }
        case WM_RBUTTONUP:
            if (g_fcam_look) { g_fcam_look = 0; ReleaseCapture(); return 0; }
            if (g_guard_press) { guard_release(); InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        case WM_KEYUP:
            if (wParam == VK_CONTROL) { /* attack mode off */
                POINT cp; GetCursorPos(&cp); ScreenToClient(hwnd, &cp); SetCursor(cursor_for(cp.x, cp.y));
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_CHAR: {
            if (g_ms_view) { ms_char((char)wParam); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_anim_view) { anim_view_char((char)wParam); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_script_view) {
                if (g_anim_skip_char) { g_anim_skip_char = 0; if (wParam == 's' || wParam == 'S') return 0; } /* the S that opened it */
                sv_char((char)wParam); InvalidateRect(hwnd, NULL, FALSE); return 0;
            }
            if (g_map_mode) {
                char c = (char)wParam;
                if (c >= 32 && c < 127 && g_map_filter_len < (int)sizeof(g_map_filter) - 1) {
                    g_map_filter[g_map_filter_len++] = c;
                    g_map_filter[g_map_filter_len] = 0;
                    map_recompute_filter();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int cx = GET_X_LPARAM(lParam), cy = GET_Y_LPARAM(lParam);
            int dbl = (msg == WM_LBUTTONDBLCLK);
            if (g_ms_view) { ms_mouse_down(hwnd, cx, cy, dbl); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_anim_view) { anim_view_mouse_down(hwnd, cx, cy, dbl); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_script_view) { sv_mouse_down(hwnd, cx, cy, dbl); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_sv_pick) {
                if (g_loaded && mouse_in_game(cx, cy)) { int rx, ry; client_to_room_point(cx, cy, &rx, &ry); sv_pick_click((float)rx, (float)ry, 0); }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_tr.phase) return 0; /* going to another room */
            if (g_radial) { radial_click(cx, cy); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (script_playing()) { script_skip_row(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_map_mode) { /* room list: click a row to select it, double-click to go */
                int row = (cy - 60) / 20;
                if (row >= 0 && row < MAP_VISIBLE_ROWS && g_map_scroll + row < g_map_filtered_count) {
                    g_map_selected = g_map_scroll + row;
                    if (dbl) {
                        if (g_map_pick_target) wizard_target_chosen(hwnd, g_map_filtered[g_map_selected]);
                        else { go_to_room_index(hwnd, g_map_filtered[g_map_selected]); g_map_mode = 0; }
                    }
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            ui_layout(hwnd);
            int b = ui_hit(cx, cy);
            if (b != B_NONE) { ui_action(hwnd, b); return 0; }
            if (!g_loaded || !mouse_in_game(cx, cy)) return 0;
            int rx, ry;
            client_to_room_point(cx, cy, &rx, &ry);
            if (attack_mode()) { attack_press(hwnd, cx, cy); return 0; }
            if (g_edit_mode && g_has_3d_character) {
                edit_mouse_down((float)rx, (float)ry);
                if (g_drag != DRAG_NONE) { SetCapture(hwnd); g_edit_dragging = 1; }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_has_3d_character && DAVID_ACTOR->down) return 0; /* knocked down */
            if (g_has_3d_character) {
                int dd = door_at_pixel((float)rx, (float)ry);
                if (dd >= 0) { door_click(dd, dbl); InvalidateRect(hwnd, NULL, FALSE); return 0; }
                if (!try_click_to_move((float)rx, (float)ry, dbl)) report_click_refusal();
            } else {
                set_player_target((double)rx, (double)ry);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            g_mouse_client_x = GET_X_LPARAM(lParam);
            g_mouse_client_y = GET_Y_LPARAM(lParam);
            if (g_fcam_look) { /* the free camera looks where the mouse goes */
                g_fcam_yaw -= (g_mouse_client_x - g_fcam_lx) * 0.006f;
                g_fcam_pitch -= (g_mouse_client_y - g_fcam_ly) * 0.006f;
                if (g_fcam_pitch > 1.5f) g_fcam_pitch = 1.5f;
                if (g_fcam_pitch < -1.5f) g_fcam_pitch = -1.5f;
                g_fcam_lx = g_mouse_client_x; g_fcam_ly = g_mouse_client_y;
                fcam_update();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (screen_view()) {
                ui_layout(hwnd);
                g_hover_btn = ui_hit(g_mouse_client_x, g_mouse_client_y);
                if (!g_ms_view && g_anim_view) anim_view_mouse_move(g_mouse_client_x, g_mouse_client_y);
                else if (!g_ms_view) sv_mouse_move(g_mouse_client_x, g_mouse_client_y);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (attack_drag(g_mouse_client_x, g_mouse_client_y)) return 0;
            if (g_radial) {
                int h0 = g_radial_hover;
                radial_hover(g_mouse_client_x, g_mouse_client_y);
                if (h0 != g_radial_hover) InvalidateRect(hwnd, NULL, FALSE);
            }
            int repaint = 0;
            ui_layout(hwnd);
            int hb = mouse_in_game(g_mouse_client_x, g_mouse_client_y) ? B_NONE : ui_hit(g_mouse_client_x, g_mouse_client_y);
            if (hb != g_hover_btn) { g_hover_btn = hb; repaint = 1; }
            if (g_loaded) {
                int rx, ry;
                client_to_room_point(g_mouse_client_x, g_mouse_client_y, &rx, &ry);
                if (g_edit_mode) {
                    edit_mouse_move((float)rx, (float)ry);
                    if (g_drag != DRAG_NONE || (g_tool == TOOL_POLY && g_poly.n > 0)) repaint = 1;
                }
                int hd = (!g_edit_mode && g_has_3d_character && mouse_in_game(g_mouse_client_x, g_mouse_client_y)) ? door_at_pixel((float)rx, (float)ry) : -1;
                if (hd != g_hovered_door) g_hovered_door = hd;
            }
            if (repaint) InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_sv_drag || g_sv_zdrag || g_sv_rsz || g_sv_cdrag || g_sv_band) { sv_drag_end(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (attack_release(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
            if (g_anim_sb_page) { g_anim_sb_page = 0; KillTimer(hwnd, ANIM_SB_TIMER_ID); ReleaseCapture(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_anim_drag) { g_anim_drag = 0; ReleaseCapture(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_drag != DRAG_NONE) {
                int rx, ry;
                client_to_room_point(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &rx, &ry);
                ReleaseCapture();
                edit_mouse_up((float)rx, (float)ry);
                g_edit_dragging = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_free_cam && !screen_view()) { /* the free camera: forward / back */
                float k = GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f * 1.5f, cp = cosf(g_fcam_pitch);
                g_fcam.translation[0] += sinf(g_fcam_yaw) * cp * k; g_fcam.translation[1] += sinf(g_fcam_pitch) * k; g_fcam.translation[2] += cosf(g_fcam_yaw) * cp * k;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (!screen_view()) break;
            POINT wp = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &wp);
            if (g_ms_view) ms_wheel(GET_WHEEL_DELTA_WPARAM(wParam));
            else if (g_anim_view) anim_view_wheel(wp.x, wp.y, GET_WHEEL_DELTA_WPARAM(wParam));
            else sv_wheel(wp.x, wp.y, GET_WHEEL_DELTA_WPARAM(wParam));
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            int cx = GET_X_LPARAM(lParam), cy = GET_Y_LPARAM(lParam);
            if (g_ms_view || g_anim_view) return 0;
            if (g_script_view) { sv_right_click(hwnd, cx, cy); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_free_cam && mouse_in_game(cx, cy)) { g_fcam_look = 1; g_fcam_lx = cx; g_fcam_ly = cy; SetCapture(hwnd); return 0; } /* looking around */
            if (g_sv_pick) { sv_pick_click(0, 0, 1); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (script_playing() || g_tr.phase) return 0;
            if (attack_mode() && mouse_in_game(cx, cy)) { guard_press(hwnd); return 0; } /* a dodge, or the shield held */
            if (g_radial) { radial_back(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
            if (g_loaded && !g_map_mode && !g_edit_mode && g_has_3d_character && mouse_in_game(cx, cy)) {
                radial_open(cx, cy); InvalidateRect(hwnd, NULL, FALSE); return 0;
            }
            if (g_loaded && !g_map_mode && g_edit_mode && g_has_3d_character && mouse_in_game(cx, cy) && g_wiz == WIZ_NONE) {
                int rx, ry;
                client_to_room_point(cx, cy, &rx, &ry);
                edit_context_menu(hwnd, (float)rx, (float)ry, cx, cy);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
                SetCursor(cursor_for(pt.x, pt.y));
                return TRUE;
            }
            break;
        }
        case WM_TIMER: {
            if (wParam == ANIM_SB_TIMER_ID) {
                if (!g_anim_sb_page) { KillTimer(hwnd, ANIM_SB_TIMER_ID); return 0; }
                SetTimer(hwnd, ANIM_SB_TIMER_ID, 60, NULL);
                anim_view_layout(hwnd);
                anim_sb_page_step();
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == GAME_TIMER_ID) {
                /* fallback while Windows runs a modal loop (menus, window
                   drag): the main loop normally ticks every display frame */
                if (perf_now_ms() - g_last_game_tick_ms > 30.0) game_tick(hwnd);
            }
            return 0;
        }
        case WM_PAINT: {
            double perf_t0 = perf_now_ms();
            PAINTSTRUCT ps;
            HDC screen_hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            /* whole frame double-buffered, blitted once (no flicker) */
            /* back buffer kept between frames (re-created on resize) */
            static HDC bb_dc = NULL; static HBITMAP bb_bmp = NULL, bb_old = NULL; static int bb_w = 0, bb_h = 0;
            int cw = rc.right > 0 ? rc.right : 1, ch = rc.bottom > 0 ? rc.bottom : 1;
            if (!bb_dc || cw != bb_w || ch != bb_h) {
                if (bb_dc) { SelectObject(bb_dc, bb_old); DeleteObject(bb_bmp); DeleteDC(bb_dc); }
                bb_dc = CreateCompatibleDC(screen_hdc);
                bb_bmp = CreateCompatibleBitmap(screen_hdc, cw, ch);
                bb_old = (HBITMAP)SelectObject(bb_dc, bb_bmp);
                bb_w = cw; bb_h = ch;
            }
            HDC hdc = bb_dc;
            /* FULL frame (panels + background) only when the invalid area
               goes beyond the game picture, the status line / room changed,
               a full-window screen is up, or every second (safety net);
               otherwise only the game picture is redrawn and copied */
            RECT game_rc; game_rect_client(&game_rc);
            static char last_status[sizeof(g_status)] = "";
            static int last_room = -2;
            static double last_full_ms = 0;
            double now_ms = perf_now_ms();
            int full = g_map_mode || screen_view() || !g_loaded || bb_w != cw || now_ms - last_full_ms > 1000.0 ||
                       ps.rcPaint.left < game_rc.left || ps.rcPaint.top < game_rc.top ||
                       ps.rcPaint.right > game_rc.right || ps.rcPaint.bottom > game_rc.bottom ||
                       strcmp(last_status, g_status) != 0 || last_room != g_current_map_room;
            if (full) {
                memcpy(last_status, g_status, sizeof(last_status));
                last_room = g_current_map_room;
                last_full_ms = now_ms;
                ui_fill(hdc, &rc, RGB(0, 0, 0));
            }

            double perf_s0 = perf_now_ms();
            if (g_loaded && !screen_view()) {
                if (g_view_mode_3d || g_free_cam) {
                    render_room_mesh_3d(); /* F3: raw 3D blockout instead of the picture (F4: from the free camera) */
                } else {
                    /* the real pre-rendered photo + a depth-only pass of the
                       real mesh so David is hidden behind the scenery */
                    composite_frame();
                    overlays_draw(); /* the room's animations are part of the picture */
                    fg_capture(); /* foreground shapes: the photo as it is here, before any character */
                    size_t npix = (size_t)g_dib_stride * g_hdr.height;
                    const float *room_depth = g_has_3d_character ? room_depth_cached() : NULL;
                    if (room_depth) {
                        if (g_depth_buffer) memcpy(g_depth_buffer, room_depth, npix * sizeof(float));
                        if (g_depth_buffer_char) memcpy(g_depth_buffer_char, room_depth, npix * sizeof(float));
                        for (int k = 0; k < MAX_ACTORS; k++) {
                            if (!g_actors[k].used) continue;
                            actor_begin(&g_actors[k]); draw_character_shadow(); actor_end();
                        }
                    } else if (g_depth_buffer) {
                        for (size_t i = 0; i < npix; i++) g_depth_buffer[i] = 1e29f;
                    }
                }
                if (!g_free_cam) apply_nav_overlays(); /* P / editor tints, before David so he isn't tinted */
                if (g_view_mode_3d && !g_free_cam) render_3d_character(); /* normal view: David is drawn at screen resolution below */
                if (!g_view_mode_3d && !g_free_cam) fg_restore(); /* foreground parts of the picture back over his shadow */
                if (!g_free_cam) apply_fg_tint();

                /* The visible part of the room COMPOSITE is drawn 1:1 into
                   an off-screen buffer of the composite window's size, then
                   scaled onto the screen in ONE blit (see get_view_window). */
                int win_w, win_h, dst_x, dst_y; float view_scale;
                get_view_window(&win_w, &win_h, &view_scale, &dst_x, &dst_y);
                HDC frame_hdc = hdc;
                int dw = (int)(win_w * view_scale + 0.5f), dh = (int)(win_h * view_scale + 0.5f);
                HDC vdc = NULL;
                uint32_t *vbits = view_layer(frame_hdc, dw, dh, &vdc);
                g_perf_sec[0] = perf_now_ms() - perf_s0;
                if (vbits) {
                    double perf_s1 = perf_now_ms();
                    GdiFlush();
                    compose_room_layer(vbits, dw, dh, win_w, win_h);
                    double perf_s2 = perf_now_ms(); g_perf_sec[1] = perf_s2 - perf_s1;
                    if (!g_view_mode_3d && !g_free_cam) render_david_hires(vbits, dw, dh, view_scale);
                    g_perf_sec[2] = perf_now_ms() - perf_s2;
                    SetGraphicsMode(vdc, GM_ADVANCED);
                    XFORM xf = { view_scale, 0, 0, view_scale, 0, 0 };
                    SetWorldTransform(vdc, &xf);
                    if (!g_has_3d_character) draw_player(vdc, g_cam_x, g_cam_y);
                    if (!g_free_cam) { draw_click_marker(vdc, g_cam_x, g_cam_y); draw_collision_box(vdc, g_cam_x, g_cam_y); }
                    ModifyWorldTransform(vdc, NULL, MWT_IDENTITY);
                    SetGraphicsMode(vdc, GM_COMPATIBLE);
                    GdiFlush();
                    script_draw_portraits(vbits, dw, dh, view_scale);
                    radial_draw(vbits, dw, dh, dst_x, dst_y);
                    transition_draw(vbits, dw, dh, view_scale);
                    BitBlt(hdc, dst_x, dst_y, dw, dh, vdc, 0, 0, SRCCOPY);
                }
                /* editor outlines / handles / labels: screen space, crisp */
                RECT gr; game_rect_client(&gr);
                HRGN clip = CreateRectRgn(gr.left, gr.top, gr.right, gr.bottom);
                SelectClipRgn(hdc, clip);
                if (!g_tr.phase) draw_editor_overlays(hdc);
                radial_draw_text(hdc);
                boss_bar_draw(hdc);
                SelectClipRgn(hdc, NULL);
                DeleteObject(clip);
            }

            /* tool panels (black bars in fullscreen, sidebar in windowed) */
            if (full && !g_map_mode && !screen_view()) {
                double perf_s3 = perf_now_ms();
                ui_layout(hwnd);
                ui_draw_panels(hwnd, hdc);
                g_perf_sec[3] = perf_now_ms() - perf_s3;
            }
            if (g_ms_view) ms_paint(hwnd, hdc);
            else if (g_anim_view) anim_view_paint(hwnd, hdc);
            else if (g_script_view) sv_paint(hwnd, hdc);

            if (g_map_mode) {
                ui_fill(hdc, &rc, RGB(8, 10, 16));
                HFONT font = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          DEFAULT_QUALITY, FIXED_PITCH, "Consolas");
                HFONT oldFont = (HFONT)SelectObject(hdc, font);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 230, 60));
                char header[200];
                if (g_map_pick_target) snprintf(header, sizeof(header), "CONNECTOR -- step 3/3: CHOOSE THE ROOM IT LEADS TO  (type to filter, Enter or double-click = select, Esc = cancel)");
                else snprintf(header, sizeof(header), "ROOM LIST (%d/%d rooms)  --  type to filter, arrows/PgUp/PgDn, Enter or double-click = go, Esc = close",
                              g_map_filtered_count, g_map_room_count);
                TextOutA(hdc, 16, 12, header, (int)strlen(header));
                char filter_line[100];
                snprintf(filter_line, sizeof(filter_line), "filter: %s_", g_map_filter);
                SetTextColor(hdc, RGB(255, 255, 255));
                TextOutA(hdc, 16, 34, filter_line, (int)strlen(filter_line));
                int row_h = 20, list_top = 60;
                for (int row = 0; row < MAP_VISIBLE_ROWS; row++) {
                    int idx = g_map_scroll + row;
                    if (idx >= g_map_filtered_count) break;
                    MapRoomEntry *e = &g_map_rooms[g_map_filtered[idx]];
                    int y = list_top + row * row_h;
                    int ndoors = room_door_count(e->label);
                    if (idx == g_map_selected) {
                        RECT hl = {12, y - 2, rc.right - 12, y + row_h - 2};
                        ui_fill(hdc, &hl, RGB(60, 50, 10));
                        SetTextColor(hdc, RGB(255, 230, 60));
                    } else {
                        SetTextColor(hdc, ndoors > 0 ? RGB(140, 200, 255) : RGB(200, 200, 200));
                    }
                    char doors_txt[64] = "";
                    if (ndoors > 0) snprintf(doors_txt, sizeof(doors_txt), "(%d connector%s)", ndoors, ndoors == 1 ? "" : "s");
                    char navp[600]; room_settings_path(navp, sizeof(navp), room_of_label(e->label));
                    FILE *navf = fopen(navp, "r"); /* room has its own David & navigation settings */
                    if (navf) { fclose(navf); strncat(doors_txt, doors_txt[0] ? " (custom nav)" : "(custom nav)", sizeof(doors_txt) - strlen(doors_txt) - 1); }
                    char entry_line[220];
                    snprintf(entry_line, sizeof(entry_line), "%-40s %s", e->label, doors_txt);
                    TextOutA(hdc, 24, y, entry_line, (int)strlen(entry_line));
                }
                SelectObject(hdc, oldFont);
                DeleteObject(font);
            }

            if (full) BitBlt(screen_hdc, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
            else BitBlt(screen_hdc, game_rc.left, game_rc.top, game_rc.right - game_rc.left, game_rc.bottom - game_rc.top,
                        hdc, game_rc.left, game_rc.top, SRCCOPY);
            EndPaint(hwnd, &ps);
            perf_frame(perf_now_ms() - perf_t0);
            return 0;
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;

    int verify_spawns_mode = (lpCmdLine && strstr(lpCmdLine, "VERIFY_SPAWNS") != NULL);
    int calibrate_mode = (lpCmdLine && strstr(lpCmdLine, "CALIBRATE_CAMERAS") != NULL);
    int verify_paths_mode = (lpCmdLine && strstr(lpCmdLine, "VERIFY_PATHS") != NULL);
    if (calibrate_mode || verify_paths_mode) verify_spawns_mode = 1; /* same "no room args" launch path */

    init_root();
    srand(GetTickCount()); /* attack mode: which of the three attacks */
    const char *start_label = "gno/boilarea";
    if (!verify_spawns_mode && lpCmdLine && lpCmdLine[0]) { /* optional: a room "level/room" to start in */
        static char arg[128];
        int n = 0;
        for (const char *s = lpCmdLine; *s && n < (int)sizeof(arg) - 1; s++)
            if (*s != '"' && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') arg[n++] = (*s == '\\') ? '/' : (char)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s);
        arg[n] = 0;
        if (n) start_label = arg;
    }
    {
        char dir[1024], dir2[1024];
        root_path(dir, sizeof(dir), "assets/chars/david");
        int david_ok = david_load(dir);
        actor_reset(DAVID_ACTOR, &g_david, 0);
        if (!david_ok) {
            MessageBoxA(NULL, "David's model wasn't found in assets/chars/david.\n\n"
                        "./assets must contain the content of the \"Silver Blockouts\" folder "
                        "(chars, levels, sprites...) -- see README.md.", "Silver Remaster", MB_ICONERROR);
            return 1;
        }
        root_path(dir, sizeof(dir), "assets/chars/anims");
        root_path(dir2, sizeof(dir2), "assets/chars/david");
        anim_lib_init(dir, dir2);
    }
    scan_room_list();
    {
        char sdir[1024];
        root_path(sdir, sizeof(sdir), "assets/sound");
        audio_init(sdir); /* music, sounds, ambiences (scripts) */
    }
    david_settings_load(); /* David's global speeds */
    g_nav_zone_at = shapes_world_zone; /* editor red/green zones -> navigation */
    /* Same both-CDs fallback as change_room: a room given on the command
       line may live in the other disc's archive. */
    if (!verify_spawns_mode) change_room(start_label);
    map_recompute_filter();

    g_cursor_arrow = LoadCursorA(NULL, IDC_ARROW);
    g_cursor_door = LoadCursorA(NULL, IDC_HAND);
    load_game_cursors(); /* the blockouts' mouse.0 / mouse.1-8 / mouse.11 */

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "SilverRemasterWindow";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW; /* CS_DBLCLKS for double-click=run;
        CS_HREDRAW/CS_VREDRAW force a full repaint on resize (point 2 of the user's request --
        resizable/fullscreen without bugs -- otherwise Windows can leave stale edge pixels
        when the window grows). */
    RegisterClassA(&wc);

    int win_w = g_loaded ? ((int)g_hdr.width < VIEWPORT_W ? (int)g_hdr.width : VIEWPORT_W) : VIEWPORT_W;
    int win_h = g_loaded ? ((int)g_hdr.height < VIEWPORT_H ? (int)g_hdr.height : VIEWPORT_H) : VIEWPORT_H;

    if (win_h < 800) win_h = 800; /* room for the tool sidebar's buttons */
    RECT wr = {0, 0, win_w + TOOL_SIDEBAR_W, win_h};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExA(
        0, "SilverRemasterWindow", "Silver Remaster",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInstance, NULL);
    if (!hwnd) return 1;
    g_hwnd = hwnd;
    center_view_on_david(); /* the first room loaded before the window existed */

    if (verify_paths_mode) {
        run_path_verification();
        return 0;
    }
    if (calibrate_mode) {
        run_camera_calibration();
        return 0;
    }
    if (verify_spawns_mode) {
        run_spawn_verification();
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetTimer(hwnd, GAME_TIMER_ID, 33, NULL); /* modal-loop fallback only, see game_tick */

    /* main loop: messages, then one game step + frame per display refresh */
    MSG msg;
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        game_tick(hwnd);
        if (IsIconic(hwnd)) Sleep(50);
        else if (FAILED(DwmFlush())) Sleep(8); /* waits for the next display refresh */
    }
}
