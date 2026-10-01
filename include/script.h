/* Room scripts (cutscenes): a grid of ACTIONS. The rows are played one
   after the other; every action of a row starts at the same time and the
   next row starts once all of them are finished. Any number of rows,
   3 columns by default (more can be added).
   A range of rows can be an ADVANCED TIMELINE (a zone): its actions start
   at their own time (`start`, seconds from the zone's start) and may
   overlap; the columns are its lanes, the rows only hold the actions (and
   give the zone its height: `sec_per_row` seconds each). The zone is over
   once all its actions are, then the rows after it go on.
   Saved per room in data/rooms/<level>/<room>_scripts.cfg -- see
   scripts_save for the format. */
#ifndef SILVER_SCRIPT_H
#define SILVER_SCRIPT_H

#include "audio.h"

enum {
    ACT_NONE,       /* empty cell */
    ACT_WAIT,       /* wait `seconds` (the game goes on) */
    ACT_BACKGROUND, /* the room's picture becomes `file` ("" = its own again) */
    ACT_MUSIC,      /* the music playlist becomes `track` (0 tracks = stop the music) */
    ACT_SOUND,      /* play `file` 1 + `repeat` times; `wait_end`: the row waits for it */
    ACT_AMBIENCE,   /* play `file`, `loop` or not */
    ACT_STOP,       /* stop a sound / ambience of this script, all of them, or the music */
    ACT_PLACE,      /* a character (`model`) appears at `pos`, facing `facing` */
    ACT_MOVE,       /* a character walks / runs to `pos`, or through connector `door` */
    ACT_ANIM,       /* a character plays clip `file` (anim_mode: ANIM_*) */
    ACT_SPEAK,      /* a character says `file` (a sound): its portrait is shown until the line is over */
    ACT_OVERLAY,    /* the room's overlays (environmental animations): each one looped, played once, frozen, hidden... */
    ACT_ROOM,       /* go to room `file` (as through a connector): David appears at its connector `door`, then `script` plays */
    ACT_CAMERA,     /* the view slides (`seconds`) to its target (`cam_target`: CAM_*, pos[0..1] a point of the room picture) and zoom */
    ACT_CHAR,       /* a character's settings change: its side, its AI on / off, its weapon / shield (as with the radial menu) */
    ACT_COUNT
};
/* ACT_OVERLAY: what each overlay listed does */
enum { OVM_KEEP, OVM_LOOP, OVM_ONCE, OVM_ONCE_HIDE, OVM_FREEZE, OVM_HIDE, OVM_COUNT };
#define SCRIPT_MAX_OVERLAYS 16
#define SCRIPT_POOL_MAX 8
enum { STOP_ACTION, STOP_ALL_SOUNDS, STOP_MUSIC };
enum { CAM_KEEP, CAM_POINT, CAM_DAVID };

/* ANIM: how long the animation plays */
enum {
    ANIM_TIMES,     /* `repeat` times; the row waits for it */
    ANIM_ROW,       /* over and over until the rest of the row is over */
    ANIM_LOOP,      /* over and over, kept after the row (and the script), until another animation replaces it,
                       an ANIM_NORMAL, or the character is told to move */
    ANIM_NORMAL     /* no clip: the character goes back to its own behaviour (ends a loop) */
};

typedef struct {
    int type, id;                      /* id: unique in its script, never reused */
    float seconds;                     /* WAIT */
    char file[160];                    /* BACKGROUND: "level/room/name.png" under assets/levels; SOUND / AMBIENCE / SPEAK: file of assets/sound;
                                          ANIM: the clip, "source/name" */
    int ntracks;                       /* MUSIC */
    char track[AUDIO_MAX_TRACKS][64];
    int track_loop[AUDIO_MAX_TRACKS];
    int repeat, wait_end;              /* SOUND (ANIM: repeat = times played) */
    int anim_mode;                     /* ANIM: ANIM_TIMES, ANIM_ROW, ANIM_LOOP, ANIM_NORMAL */
    float speed;                       /* ANIM: playback factor, 0 = the game's animation speed */
    int freeze;                        /* ANIM (ANIM_TIMES): once over, held on its last frame until the rest of the row is over */
    float fade;                        /* MUSIC: the music playing fades out over it first; STOP: fades out instead of cutting (s, 0 = at once) */
    int loop;                          /* AMBIENCE */
    int stop_kind, stop_ref;           /* STOP: STOP_*; STOP_ACTION: id of the SOUND / AMBIENCE action */
    char model[48];                    /* PLACE: folder name in assets/chars */
    int has_pos;                       /* PLACE / MOVE: pos was picked */
    float pos[3], facing;              /* world point; facing in radians (PLACE) */
    int actor;                         /* MOVE / ANIM / SPEAK: 0 = David, else the id of the PLACE action that brought the character in */
    int run;                           /* MOVE: 0 walk, 1 run */
    int face_end;                      /* MOVE: once there, it turns to `facing` (0: as it arrives) */
    int door;                          /* MOVE: connector shape id to go through, 0 = go to pos; ROOM: connector of the target room David
                                          comes in by, 0 = where he'd spawn */
    int nov;                           /* OVERLAY: the overlays it sets (by name, in the room's _overlays.cfg) */
    char ov_name[SCRIPT_MAX_OVERLAYS][32];
    int ov_mode[SCRIPT_MAX_OVERLAYS];  /* OVM_* */
    int ov_frame[SCRIPT_MAX_OVERLAYS]; /* OVM_FREEZE: the frame shown */
    char script[64];                   /* ROOM: the target room's script played on arrival ("" = its auto script, like a connector without one) */
    int side, ai_off;                  /* PLACE: 0 an ally, 1 an enemy; its AI off */
    int set_side, set_ai;              /* CHAR: 0 unchanged; side 1 ally / 2 enemy; AI 1 on / 2 off */
    int set_weapon, set_shield;        /* CHAR: 0 unchanged, 1 becomes item / item2 ("" = none) */
    char item[48], item2[48];
    int npool;                         /* ANIM / SOUND: others picked at random with `file` (one of them each time) */
    char pool[SCRIPT_POOL_MAX][96];
    float start;                       /* in a timeline zone: when it starts, seconds from the zone's start */
    float trim_in, trim_out;           /* in a timeline zone (SOUND, SPEAK, ANIM played N times): seconds of it skipped
                                          at its start / cut at its end */
    int cam_target;                    /* CAMERA: CAM_* */
    float zoom;                        /* CAMERA: x1..x3, 0 = the room's own */
} ScriptAction;
const char *overlay_mode_name(int mode);

#define SCRIPT_MAX_ZONES 16
typedef struct { int row0, rows; float sec_per_row; } ScriptZone;

typedef struct {
    char name[64];
    int auto_run;                      /* played when David enters the room */
    int nzones;                        /* advanced timeline zones, by row */
    ScriptZone zone[SCRIPT_MAX_ZONES];
    int cols, rows;
    ScriptAction *cell;                /* rows * cols, row by row */
    int next_id;
} Script;

#define SCRIPT_DEFAULT_COLS 3
#define SCRIPT_MAX_COLS 16

void script_init(Script *s, const char *name);
void script_free(Script *s);
void script_copy(Script *dst, const Script *src);
ScriptAction *script_at(Script *s, int row, int col);   /* NULL outside the grid */
void script_ensure_rows(Script *s, int rows);
void script_insert_row(Script *s, int row);
void script_delete_row(Script *s, int row);
void script_add_col(Script *s);
void script_delete_col(Script *s, int col);
int script_used_rows(const Script *s);                  /* last row holding an action + 1 */
int script_row_empty(const Script *s, int row);
ScriptAction *script_find(Script *s, int id, int *row, int *col);
/* timeline zones: the one holding `row` (-1: none); rows row0..row0+n-1 become one
   (their actions start at their row's time; -1 if it would overlap another); back to rows */
int script_zone_at(const Script *s, int row);
int script_zone_make(Script *s, int row0, int n);
void script_zone_remove(Script *s, int z);
/* an empty cell of a zone's lane (column) for a new action, the zone grown by a row if full (-1: none) */
int script_zone_free_row(Script *s, int z, int col);
/* a zone made taller / shorter (rows added / removed at its end; never fewer
   than its lanes' actions need): returns its rows */
int script_zone_resize(Script *s, int z, int rows);
void action_init(Script *s, ScriptAction *a, int type); /* defaults + a new id */

const char *action_type_name(int type);
/* one line describing the action (the grid cells) */
void action_summary(const Script *s, const ScriptAction *a, char *out, int n);
/* "David", or the model brought in by PLACE action `actor` */
void script_actor_name(const Script *s, int actor, char *out, int n);

/* every script of a room file (returns how many, *out malloc'ed) */
int scripts_load(const char *path, Script **out);
/* writes them all (no scripts: the file is removed) */
int scripts_save(const char *path, const Script *s, int n);

#endif
