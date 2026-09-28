#include "json.h"
#include <stdlib.h>
#include <string.h>

/* Arena: big blocks, never freed individually. */
typedef struct ArenaBlock { struct ArenaBlock *next; size_t used, cap; } ArenaBlock;

static void *arena_alloc(ArenaBlock **head, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!*head || (*head)->used + n > (*head)->cap) {
        size_t cap = n > (1u << 20) ? n : (1u << 20);
        ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
        if (!b) return NULL;
        b->next = *head; b->used = 0; b->cap = cap;
        *head = b;
    }
    void *p = (char *)(*head + 1) + (*head)->used;
    (*head)->used += n;
    return p;
}

typedef struct {
    const char *p, *end;
    ArenaBlock *arena;
    int ok;
} Parser;

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static JsonValue *new_value(Parser *ps, JsonType t) {
    JsonValue *v = (JsonValue *)arena_alloc(&ps->arena, sizeof(JsonValue));
    if (!v) { ps->ok = 0; return NULL; }
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static const char *parse_string_raw(Parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') { ps->ok = 0; return NULL; }
    ps->p++;
    const char *s = ps->p;
    size_t n = 0;
    while (ps->p < ps->end && *ps->p != '"') { if (*ps->p == '\\') ps->p++; ps->p++; n++; }
    if (ps->p >= ps->end) { ps->ok = 0; return NULL; }
    char *out = (char *)arena_alloc(&ps->arena, n * 4 + 1);
    if (!out) { ps->ok = 0; return NULL; }
    char *o = out;
    for (const char *q = s; q < ps->p; q++) {
        if (*q != '\\') { *o++ = *q; continue; }
        q++;
        switch (*q) {
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = '\t'; break;
            case 'r': *o++ = '\r'; break;
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'u': { /* keep ASCII, replace the rest (names/uris here are ASCII) */
                unsigned c = 0;
                for (int k = 1; k <= 4 && q + k < ps->p; k++) {
                    char h = q[k]; c <<= 4;
                    c |= (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0;
                }
                *o++ = c < 128 ? (char)c : '?';
                q += 4;
                break;
            }
            default: *o++ = *q; break; /* \" \\ \/ */
        }
    }
    *o = 0;
    ps->p++; /* closing quote */
    return out;
}

static JsonValue *parse_value(Parser *ps, int depth);

static JsonValue *parse_container(Parser *ps, int depth, int is_object) {
    JsonValue *v = new_value(ps, is_object ? JSON_OBJECT : JSON_ARRAY);
    if (!v) return NULL;
    ps->p++; /* [ or { */
    int cap = 0;
    JsonValue **items = NULL; const char **keys = NULL;
    skip_ws(ps);
    char close = is_object ? '}' : ']';
    if (ps->p < ps->end && *ps->p == close) { ps->p++; return v; }
    for (;;) {
        skip_ws(ps);
        const char *key = NULL;
        if (is_object) {
            key = parse_string_raw(ps);
            if (!ps->ok) return NULL;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { ps->ok = 0; return NULL; }
            ps->p++;
        }
        JsonValue *item = parse_value(ps, depth + 1);
        if (!ps->ok) return NULL;
        if (v->count == cap) { /* grow: copy into a bigger arena slice */
            int ncap = cap ? cap * 2 : 8;
            JsonValue **ni = (JsonValue **)arena_alloc(&ps->arena, sizeof(JsonValue *) * ncap);
            const char **nk = is_object ? (const char **)arena_alloc(&ps->arena, sizeof(char *) * ncap) : NULL;
            if (!ni || (is_object && !nk)) { ps->ok = 0; return NULL; }
            if (cap) { memcpy(ni, items, sizeof(JsonValue *) * cap); if (is_object) memcpy(nk, keys, sizeof(char *) * cap); }
            items = ni; keys = nk; cap = ncap;
        }
        items[v->count] = item;
        if (is_object) keys[v->count] = key;
        v->count++;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == close) { ps->p++; break; }
        ps->ok = 0; return NULL;
    }
    v->items = items; v->keys = keys;
    return v;
}

static JsonValue *parse_value(Parser *ps, int depth) {
    if (depth > 64) { ps->ok = 0; return NULL; }
    skip_ws(ps);
    if (ps->p >= ps->end) { ps->ok = 0; return NULL; }
    char c = *ps->p;
    if (c == '{') return parse_container(ps, depth, 1);
    if (c == '[') return parse_container(ps, depth, 0);
    if (c == '"') {
        JsonValue *v = new_value(ps, JSON_STRING);
        if (v) v->string = parse_string_raw(ps);
        return v;
    }
    if (c == 't' && ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) { ps->p += 4; JsonValue *v = new_value(ps, JSON_BOOL); if (v) v->number = 1; return v; }
    if (c == 'f' && ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) { ps->p += 5; return new_value(ps, JSON_BOOL); }
    if (c == 'n' && ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) { ps->p += 4; return new_value(ps, JSON_NULL); }
    /* number */
    char buf[64]; int n = 0;
    while (ps->p < ps->end && n < 63 && (strchr("+-0123456789.eE", *ps->p))) buf[n++] = *ps->p++;
    if (n == 0) { ps->ok = 0; return NULL; }
    buf[n] = 0;
    JsonValue *v = new_value(ps, JSON_NUMBER);
    if (v) v->number = strtod(buf, NULL);
    return v;
}

int json_parse(const char *text, size_t len, JsonDoc *doc) {
    Parser ps = { text, text + len, NULL, 1 };
    doc->root = parse_value(&ps, 0);
    doc->arena = ps.arena;
    if (!ps.ok || !doc->root) { json_free(doc); return 0; }
    return 1;
}

void json_free(JsonDoc *doc) {
    ArenaBlock *b = (ArenaBlock *)doc->arena;
    while (b) { ArenaBlock *n = b->next; free(b); b = n; }
    doc->arena = NULL; doc->root = NULL;
}

const JsonValue *json_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (int i = 0; i < obj->count; i++) if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}
const JsonValue *json_at(const JsonValue *arr, int i) {
    if (!arr || (arr->type != JSON_ARRAY && arr->type != JSON_OBJECT) || i < 0 || i >= arr->count) return NULL;
    return arr->items[i];
}
double json_num(const JsonValue *v, double def) { return (v && (v->type == JSON_NUMBER || v->type == JSON_BOOL)) ? v->number : def; }
int json_int(const JsonValue *v, int def) { return (v && v->type == JSON_NUMBER) ? (int)v->number : def; }
const char *json_str(const JsonValue *v, const char *def) { return (v && v->type == JSON_STRING) ? v->string : def; }
int json_len(const JsonValue *arr) { return (arr && (arr->type == JSON_ARRAY || arr->type == JSON_OBJECT)) ? arr->count : 0; }
