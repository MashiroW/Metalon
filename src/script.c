/* Room scripts: the grid of actions, its editing operations and its file
   (see script.h). The game side -- playing them, the editor screen --
   lives in main.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"

void script_init(Script *s, const char *name) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
    s->cols = SCRIPT_DEFAULT_COLS;
    s->next_id = 1;
}

void script_free(Script *s) {
    free(s->cell);
    s->cell = NULL; s->rows = 0;
}

void script_copy(Script *dst, const Script *src) {
    *dst = *src;
    dst->cell = NULL;
    if (src->rows > 0) {
        dst->cell = (ScriptAction *)malloc(sizeof(ScriptAction) * (size_t)src->rows * src->cols);
        memcpy(dst->cell, src->cell, sizeof(ScriptAction) * (size_t)src->rows * src->cols);
    }
}

ScriptAction *script_at(Script *s, int row, int col) {
    if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) return NULL;
    return &s->cell[(size_t)row * s->cols + col];
}

void script_ensure_rows(Script *s, int rows) {
    if (rows <= s->rows) return;
    s->cell = (ScriptAction *)realloc(s->cell, sizeof(ScriptAction) * (size_t)rows * s->cols);
    memset(&s->cell[(size_t)s->rows * s->cols], 0, sizeof(ScriptAction) * (size_t)(rows - s->rows) * s->cols);
    s->rows = rows;
}

void script_insert_row(Script *s, int row) {
    if (row < 0) row = 0;
    if (row > s->rows) row = s->rows;
    script_ensure_rows(s, s->rows + 1);
    memmove(&s->cell[(size_t)(row + 1) * s->cols], &s->cell[(size_t)row * s->cols], sizeof(ScriptAction) * (size_t)(s->rows - 1 - row) * s->cols);
    memset(&s->cell[(size_t)row * s->cols], 0, sizeof(ScriptAction) * s->cols);
}

void script_delete_row(Script *s, int row) {
    if (row < 0 || row >= s->rows) return;
    memmove(&s->cell[(size_t)row * s->cols], &s->cell[(size_t)(row + 1) * s->cols], sizeof(ScriptAction) * (size_t)(s->rows - 1 - row) * s->cols);
    s->rows--;
}

/* rebuilds the grid with `cols` columns; column `drop` (if >= 0) removed */
static void script_regrid(Script *s, int cols, int drop) {
    ScriptAction *c = s->rows > 0 ? (ScriptAction *)calloc((size_t)s->rows * cols, sizeof(ScriptAction)) : NULL;
    for (int r = 0; r < s->rows; r++)
        for (int k = 0, d = 0; k < s->cols; k++) {
            if (k == drop) continue;
            if (d < cols) c[(size_t)r * cols + d] = s->cell[(size_t)r * s->cols + k];
            d++;
        }
    free(s->cell);
    s->cell = c; s->cols = cols;
}
void script_add_col(Script *s) { if (s->cols < SCRIPT_MAX_COLS) script_regrid(s, s->cols + 1, -1); }
void script_delete_col(Script *s, int col) { if (s->cols > 1 && col >= 0 && col < s->cols) script_regrid(s, s->cols - 1, col); }

int script_row_empty(const Script *s, int row) {
    if (row < 0 || row >= s->rows) return 1;
    for (int k = 0; k < s->cols; k++) if (s->cell[(size_t)row * s->cols + k].type != ACT_NONE) return 0;
    return 1;
}

int script_used_rows(const Script *s) {
    int n = s->rows;
    while (n > 0 && script_row_empty(s, n - 1)) n--;
    return n;
}

ScriptAction *script_find(Script *s, int id, int *row, int *col) {
    for (int r = 0; r < s->rows; r++)
        for (int k = 0; k < s->cols; k++) {
            ScriptAction *a = &s->cell[(size_t)r * s->cols + k];
            if (a->type != ACT_NONE && a->id == id) { if (row) *row = r; if (col) *col = k; return a; }
        }
    return NULL;
}

void action_init(Script *s, ScriptAction *a, int type) {
    memset(a, 0, sizeof(*a));
    a->type = type;
    a->id = s->next_id++;
    if (type == ACT_WAIT) a->seconds = 1.0f;
    if (type == ACT_AMBIENCE) a->loop = 1;
    if (type == ACT_STOP) a->stop_kind = STOP_ALL_SOUNDS;
    if (type == ACT_ANIM) a->repeat = 1;
    if (type == ACT_CAMERA) { a->seconds = 1.0f; a->cam_target = CAM_DAVID; }
}

const char *overlay_mode_name(int mode) {
    static const char *names[OVM_COUNT] = { "unchanged", "loop", "once", "once, then hidden", "frozen", "hidden" };
    return (mode >= 0 && mode < OVM_COUNT) ? names[mode] : "?";
}

const char *action_type_name(int type) {
    static const char *names[ACT_COUNT] = { "Empty", "Wait", "Background", "Music", "Sound", "Ambience", "Stop sound", "Place character", "Move character",
                                            "Animate character", "Speak", "Overlays", "Change room", "Camera" };
    return (type >= 0 && type < ACT_COUNT) ? names[type] : "?";
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void script_actor_name(const Script *s, int actor, char *out, int n) {
    if (actor == 0) { snprintf(out, n, "David"); return; }
    int r = -1;
    ScriptAction *p = script_find((Script *)s, actor, &r, NULL);
    if (!p || p->type != ACT_PLACE) { snprintf(out, n, "(character removed)"); return; }
    snprintf(out, n, "%s (row %d)", p->model[0] ? p->model : "?", r + 1);
}

void action_summary(const Script *s, const ScriptAction *a, char *out, int n) {
    char t[160];
    out[0] = 0;
    switch (a->type) {
        case ACT_WAIT: snprintf(out, n, "%.1f s", a->seconds); break;
        case ACT_BACKGROUND: snprintf(out, n, "%s", a->file[0] ? base_name(a->file) : "the room's own picture"); break;
        case ACT_MUSIC:
            if (a->fade > 0) snprintf(out, n, "(fade out %.1f s) ", a->fade);
            if (a->ntracks == 0) { size_t l = strlen(out); snprintf(out + l, n - l, "None (no music)"); break; }
            for (int i = 0; i < a->ntracks; i++) {
                size_t l = strlen(out);
                snprintf(out + l, n - l, "%s%s%s", i ? " > " : "", a->track[i], a->track_loop[i] ? " (loop)" : "");
            }
            break;
        case ACT_SOUND:
            snprintf(out, n, "%s", a->file[0] ? base_name(a->file) : "(no file)");
            if (a->repeat > 0) { size_t l = strlen(out); snprintf(out + l, n - l, "  x%d", a->repeat + 1); }
            if (a->wait_end) { size_t l = strlen(out); snprintf(out + l, n - l, "  (wait)"); }
            break;
        case ACT_AMBIENCE: snprintf(out, n, "%s%s", a->file[0] ? base_name(a->file) : "(no file)", a->loop ? "  (loop)" : ""); break;
        case ACT_STOP:
            if (a->stop_kind == STOP_MUSIC) snprintf(out, n, "the music");
            else if (a->stop_kind == STOP_ALL_SOUNDS) snprintf(out, n, "all sounds & ambiences");
            else {
                int r = -1;
                ScriptAction *p = script_find((Script *)s, a->stop_ref, &r, NULL);
                if (p) snprintf(out, n, "%s (row %d)", p->file[0] ? p->file : action_type_name(p->type), r + 1);
                else snprintf(out, n, "(nothing chosen)");
            }
            if (a->fade > 0) { size_t l = strlen(out); snprintf(out + l, n - l, "  (fade out %.1f s)", a->fade); }
            break;
        case ACT_PLACE: snprintf(out, n, "%s%s", a->model[0] ? a->model : "(no character)", a->has_pos ? "" : "  -- no position yet"); break;
        case ACT_MOVE:
            script_actor_name(s, a->actor, t, sizeof(t));
            if (a->door) snprintf(out, n, "%s %s through connector #%d", t, a->run ? "runs" : "walks", a->door);
            else if (a->has_pos) snprintf(out, n, "%s %s to a point", t, a->run ? "runs" : "walks");
            else snprintf(out, n, "%s -- no destination yet", t);
            break;
        case ACT_ANIM:
            script_actor_name(s, a->actor, t, sizeof(t));
            if (a->anim_mode == ANIM_NORMAL) { snprintf(out, n, "%s: back to normal", t); break; }
            if (!a->file[0]) { snprintf(out, n, "%s -- no animation yet", t); break; }
            if (a->anim_mode == ANIM_ROW) snprintf(out, n, "%s: %s (while the row)", t, base_name(a->file));
            else if (a->anim_mode == ANIM_LOOP) snprintf(out, n, "%s: %s (looped until replaced)", t, base_name(a->file));
            else snprintf(out, n, "%s: %s", t, base_name(a->file));
            if (a->anim_mode == 0 && a->repeat > 1) { size_t l = strlen(out); snprintf(out + l, n - l, "  x%d", a->repeat); }
            if (a->speed > 0) { size_t l = strlen(out); snprintf(out + l, n - l, "  at speed x%.2f", a->speed); }
            if (a->freeze && a->anim_mode == ANIM_TIMES) { size_t l = strlen(out); snprintf(out + l, n - l, "  (freezes)"); }
            break;
        case ACT_SPEAK:
            script_actor_name(s, a->actor, t, sizeof(t));
            snprintf(out, n, "%s: %s", t, a->file[0] ? base_name(a->file) : "(no line yet)");
            break;
        case ACT_OVERLAY: {
            int shown = 0;
            for (int i = 0; i < a->nov; i++) {
                if (a->ov_mode[i] == OVM_KEEP) continue;
                size_t l = strlen(out);
                if (a->ov_mode[i] == OVM_FREEZE) snprintf(out + l, n - l, "%s%s frame %d", shown ? ", " : "", a->ov_name[i], a->ov_frame[i]);
                else snprintf(out + l, n - l, "%s%s %s", shown ? ", " : "", a->ov_name[i], overlay_mode_name(a->ov_mode[i]));
                shown++;
            }
            if (!shown) snprintf(out, n, "(nothing changes)");
            break;
        }
        case ACT_CAMERA: {
            char z[32];
            if (a->zoom > 0) snprintf(z, sizeof(z), "zoom x%.1f", a->zoom); else snprintf(z, sizeof(z), "the room's zoom");
            snprintf(out, n, "%s, %s, %.1f s", a->cam_target == CAM_POINT ? (a->has_pos ? "to a point" : "(no point yet)") : a->cam_target == CAM_DAVID ? "to David" : "where it looks",
                     z, a->seconds);
            break;
        }
        case ACT_ROOM:
            if (!a->file[0]) { snprintf(out, n, "(no room yet)"); break; }
            snprintf(out, n, "%s%s%s", a->file, a->script[0] ? ", then " : "", a->script);
            break;
    }
}

/* ---------------- file ----------------
   script <name>
   auto 0|1
   columns <n>
   cell <row> <col> <id> wait <seconds>
   cell <row> <col> <id> background <level/room/picture.png | ->
   cell <row> <col> <id> music <n> [<file> <loop>]...
   cell <row> <col> <id> sound <file> <repeat> <wait>
   cell <row> <col> <id> ambience <file> <loop>
   cell <row> <col> <id> stop action <id> | stop sounds | stop music
   cell <row> <col> <id> place <model> <has_pos> <x> <y> <z> <facing>
   cell <row> <col> <id> move <actor> <run> <door> <has_pos> <x> <y> <z>
   cell <row> <col> <id> anim <actor> <mode> <times> <source/clip> <speed, 0 = the game's> <freeze>
   cell <row> <col> <id> speak <actor> <file>
   cell <row> <col> <id> overlays <wait for the 'once' ones> <n> [<name> <OVM_* mode> <frozen frame>]...
   cell <row> <col> <id> room <level/room> <connector id> <script name (rest of the line) | ->
   cell <row> <col> <id> camera <CAM_* target> <zoom, 0 = the room's> <slide seconds> <has_pos> <x> <y>
   end */
static void parse_cell(Script *s, const char *line) {
    int r, c, id, used = 0;
    char kind[32];
    if (sscanf(line, "cell %d %d %d %31s %n", &r, &c, &id, kind, &used) < 4 || r < 0 || c < 0 || c >= s->cols || r > 100000) return;
    const char *p = line + used;
    ScriptAction a; memset(&a, 0, sizeof(a));
    a.id = id;
    if (!strcmp(kind, "wait")) { a.type = ACT_WAIT; sscanf(p, "%f", &a.seconds); }
    else if (!strcmp(kind, "background")) { a.type = ACT_BACKGROUND; sscanf(p, "%159s", a.file); if (!strcmp(a.file, "-")) a.file[0] = 0; }
    else if (!strcmp(kind, "music")) {
        a.type = ACT_MUSIC;
        int k = 0, nt = 0;
        if (sscanf(p, "%d %n", &nt, &k) >= 1) {
            p += k;
            for (int i = 0; i < nt && a.ntracks < AUDIO_MAX_TRACKS; i++) {
                if (sscanf(p, "%63s %d %n", a.track[a.ntracks], &a.track_loop[a.ntracks], &k) < 2) break;
                p += k; a.ntracks++;
            }
            if (!strncmp(p, "fade ", 5)) sscanf(p + 5, "%f", &a.fade);
        }
    }
    else if (!strcmp(kind, "sound")) { a.type = ACT_SOUND; sscanf(p, "%159s %d %d", a.file, &a.repeat, &a.wait_end); }
    else if (!strcmp(kind, "ambience")) { a.type = ACT_AMBIENCE; sscanf(p, "%159s %d", a.file, &a.loop); }
    else if (!strcmp(kind, "stop")) {
        a.type = ACT_STOP;
        if (!strncmp(p, "action", 6)) { a.stop_kind = STOP_ACTION; sscanf(p + 6, "%d", &a.stop_ref); }
        else if (!strncmp(p, "music", 5)) a.stop_kind = STOP_MUSIC;
        else a.stop_kind = STOP_ALL_SOUNDS;
        const char *fd = strstr(p, " fade ");
        if (fd) sscanf(fd + 6, "%f", &a.fade);
    }
    else if (!strcmp(kind, "place")) { a.type = ACT_PLACE; sscanf(p, "%47s %d %f %f %f %f", a.model, &a.has_pos, &a.pos[0], &a.pos[1], &a.pos[2], &a.facing); if (!strcmp(a.model, "-")) a.model[0] = 0; }
    else if (!strcmp(kind, "anim")) { a.type = ACT_ANIM; sscanf(p, "%d %d %d %159s %f %d", &a.actor, &a.anim_mode, &a.repeat, a.file, &a.speed, &a.freeze); }
    else if (!strcmp(kind, "speak")) { a.type = ACT_SPEAK; sscanf(p, "%d %159s", &a.actor, a.file); }
    else if (!strcmp(kind, "overlays")) {
        a.type = ACT_OVERLAY;
        int k = 0, nov = 0;
        if (sscanf(p, "%d %d %n", &a.wait_end, &nov, &k) >= 2) {
            p += k;
            for (int i = 0; i < nov && a.nov < SCRIPT_MAX_OVERLAYS; i++) {
                if (sscanf(p, "%31s %d %d %n", a.ov_name[a.nov], &a.ov_mode[a.nov], &a.ov_frame[a.nov], &k) < 3) break;
                p += k; a.nov++;
            }
        }
    }
    else if (!strcmp(kind, "camera")) { a.type = ACT_CAMERA; sscanf(p, "%d %f %f %d %f %f", &a.cam_target, &a.zoom, &a.seconds, &a.has_pos, &a.pos[0], &a.pos[1]); }
    else if (!strcmp(kind, "room")) {
        a.type = ACT_ROOM;
        int k = 0;
        if (sscanf(p, "%159s %d %n", a.file, &a.door, &k) >= 2) {
            snprintf(a.script, sizeof(a.script), "%s", p + k);
            if (!strcmp(a.script, "-")) a.script[0] = 0;
        }
    }
    else if (!strcmp(kind, "move")) { a.type = ACT_MOVE; sscanf(p, "%d %d %d %d %f %f %f", &a.actor, &a.run, &a.door, &a.has_pos, &a.pos[0], &a.pos[1], &a.pos[2]); }
    else return;
    if (!strcmp(a.file, "-")) a.file[0] = 0;
    script_ensure_rows(s, r + 1);
    *script_at(s, r, c) = a;
    if (id >= s->next_id) s->next_id = id + 1;
}

int scripts_load(const char *path, Script **out) {
    *out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0, cap = 0, in = 0;
    char line[2048];
    Script cur;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "script ", 7)) { script_init(&cur, s + 7); in = 1; }
        else if (!in) continue;
        else if (!strncmp(s, "auto ", 5)) cur.auto_run = atoi(s + 5) != 0;
        else if (!strncmp(s, "columns ", 8)) { int c = atoi(s + 8); if (c >= 1 && c <= SCRIPT_MAX_COLS && cur.rows == 0) cur.cols = c; }
        else if (!strncmp(s, "cell ", 5)) parse_cell(&cur, s);
        else if (!strcmp(s, "end")) {
            if (n == cap) { cap = cap ? cap * 2 : 8; *out = (Script *)realloc(*out, sizeof(Script) * cap); }
            (*out)[n++] = cur;
            in = 0;
        }
    }
    if (in) script_free(&cur);
    fclose(f);
    return n;
}

static void write_cell(FILE *f, int r, int c, const ScriptAction *a) {
    fprintf(f, "cell %d %d %d ", r, c, a->id);
    switch (a->type) {
        case ACT_WAIT: fprintf(f, "wait %.3f\n", a->seconds); break;
        case ACT_BACKGROUND: fprintf(f, "background %s\n", a->file[0] ? a->file : "-"); break;
        case ACT_MUSIC:
            fprintf(f, "music %d", a->ntracks);
            for (int i = 0; i < a->ntracks; i++) fprintf(f, " %s %d", a->track[i], a->track_loop[i]);
            if (a->fade > 0) fprintf(f, " fade %.2f", a->fade);
            fprintf(f, "\n");
            break;
        case ACT_SOUND: fprintf(f, "sound %s %d %d\n", a->file[0] ? a->file : "-", a->repeat, a->wait_end); break;
        case ACT_AMBIENCE: fprintf(f, "ambience %s %d\n", a->file[0] ? a->file : "-", a->loop); break;
        case ACT_STOP:
            if (a->stop_kind == STOP_ACTION) fprintf(f, "stop action %d", a->stop_ref);
            else fprintf(f, "stop %s", a->stop_kind == STOP_MUSIC ? "music" : "sounds");
            if (a->fade > 0) fprintf(f, " fade %.2f", a->fade);
            fprintf(f, "\n");
            break;
        case ACT_PLACE: fprintf(f, "place %s %d %.4f %.4f %.4f %.4f\n", a->model[0] ? a->model : "-", a->has_pos, a->pos[0], a->pos[1], a->pos[2], a->facing); break;
        case ACT_ANIM: fprintf(f, "anim %d %d %d %s %.2f %d\n", a->actor, a->anim_mode, a->repeat, a->file[0] ? a->file : "-", a->speed, a->freeze); break;
        case ACT_SPEAK: fprintf(f, "speak %d %s\n", a->actor, a->file[0] ? a->file : "-"); break;
        case ACT_OVERLAY:
            fprintf(f, "overlays %d %d", a->wait_end, a->nov);
            for (int i = 0; i < a->nov; i++) fprintf(f, " %s %d %d", a->ov_name[i], a->ov_mode[i], a->ov_frame[i]);
            fprintf(f, "\n");
            break;
        case ACT_CAMERA: fprintf(f, "camera %d %.2f %.2f %d %.1f %.1f\n", a->cam_target, a->zoom, a->seconds, a->has_pos, a->pos[0], a->pos[1]); break;
        case ACT_ROOM: fprintf(f, "room %s %d %s\n", a->file[0] ? a->file : "-", a->door, a->script[0] ? a->script : "-"); break;
        case ACT_MOVE: fprintf(f, "move %d %d %d %d %.4f %.4f %.4f\n", a->actor, a->run, a->door, a->has_pos, a->pos[0], a->pos[1], a->pos[2]); break;
    }
}

int scripts_save(const char *path, const Script *s, int n) {
    if (n <= 0) { remove(path); return 1; }
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Silver Remaster room scripts (Scripts screen: S). One action per cell; rows play one after the other.\n");
    for (int i = 0; i < n; i++) {
        fprintf(f, "script %s\nauto %d\ncolumns %d\n", s[i].name, s[i].auto_run, s[i].cols);
        for (int r = 0; r < s[i].rows; r++)
            for (int c = 0; c < s[i].cols; c++) {
                const ScriptAction *a = &s[i].cell[(size_t)r * s[i].cols + c];
                if (a->type != ACT_NONE) write_cell(f, r, c, a);
            }
        fprintf(f, "end\n");
    }
    fclose(f);
    return 1;
}
