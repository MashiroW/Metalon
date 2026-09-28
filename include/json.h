/* Minimal JSON reader (enough for glTF): parses a whole document into a
   tree of JsonValue nodes allocated in one arena. */
#ifndef SILVER_JSON_H
#define SILVER_JSON_H

#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct JsonValue JsonValue;
struct JsonValue {
    JsonType type;
    double number;          /* JSON_NUMBER, JSON_BOOL (0/1) */
    const char *string;     /* JSON_STRING (unescaped, NUL-terminated) */
    int count;              /* JSON_ARRAY / JSON_OBJECT: number of items */
    JsonValue **items;      /* JSON_ARRAY / JSON_OBJECT values */
    const char **keys;      /* JSON_OBJECT keys (same order as items) */
};

typedef struct {
    JsonValue *root;
    void *arena;            /* everything the tree points to */
} JsonDoc;

/* Parses `text` (len bytes). Returns 1 on success; free with json_free. */
int json_parse(const char *text, size_t len, JsonDoc *doc);
void json_free(JsonDoc *doc);

/* Lookups (NULL-safe: any NULL argument or wrong type returns NULL / the default). */
const JsonValue *json_get(const JsonValue *obj, const char *key);
const JsonValue *json_at(const JsonValue *arr, int i);
double json_num(const JsonValue *v, double def);
int json_int(const JsonValue *v, int def);
const char *json_str(const JsonValue *v, const char *def);
int json_len(const JsonValue *arr); /* 0 unless array/object */

#endif
