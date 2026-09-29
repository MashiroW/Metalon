/* Room scripts (cutscenes): a grid of ACTIONS. The rows are played one
   after the other; every action of a row starts at the same time and the
   next row starts once all of them are finished. Any number of rows,
   3 columns by default (more can be added).
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
    ACT_ANIM,       /* a character plays clip `file` `repeat` times, or (anim_mode 1) until the rest of the row is over */
    ACT_SPEAK,      /* a character says `file` (a sound): its portrait is shown until the line is over */
    ACT_COUNT
};
enum { STOP_ACTION, STOP_ALL_SOUNDS, STOP_MUSIC };

typedef struct {
    int type, id;                      /* id: unique in its script, never reused */
    float seconds;                     /* WAIT */
    char file[160];                    /* BACKGROUND: "level/room/name.png" under assets/levels; SOUND / AMBIENCE / SPEAK: file of assets/sound;
                                          ANIM: the clip, "source/name" */
    int ntracks;                       /* MUSIC */
    char track[AUDIO_MAX_TRACKS][64];
    int track_loop[AUDIO_MAX_TRACKS];
    int repeat, wait_end;              /* SOUND (ANIM: repeat = times played) */
    int anim_mode;                     /* ANIM: 0 = `repeat` times, 1 = until the rest of the row is over */
    int loop;                          /* AMBIENCE */
    int stop_kind, stop_ref;           /* STOP: STOP_*; STOP_ACTION: id of the SOUND / AMBIENCE action */
    char model[48];                    /* PLACE: folder name in assets/chars */
    int has_pos;                       /* PLACE / MOVE: pos was picked */
    float pos[3], facing;              /* world point; facing in radians (PLACE) */
    int actor;                         /* MOVE / ANIM / SPEAK: 0 = David, else the id of the PLACE action that brought the character in */
    int run;                           /* MOVE: 0 walk, 1 run */
    int door;                          /* MOVE: connector shape id to go through, 0 = go to pos */
} ScriptAction;

typedef struct {
    char name[64];
    int auto_run;                      /* played when David enters the room */
    int cols, rows;
    ScriptAction *cell;                /* rows * cols, row by row */
    int next_id;
} Script;

#define SCRIPT_DEFAULT_COLS 3
#define SCRIPT_MAX_COLS 8

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
