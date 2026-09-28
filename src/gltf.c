#include "gltf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; *size = (size_t)n; }
    return buf;
}

int gltf_load(const char *path, Gltf *g) {
    memset(g, 0, sizeof(*g));
    size_t len = 0;
    uint8_t *text = read_file(path, &len);
    if (!text) return 0;
    int ok = json_parse((const char *)text, len, &g->doc);
    free(text);
    if (!ok) return 0;
    g->root = g->doc.root;
    const JsonValue *bufs = json_get(g->root, "buffers");
    g->buffer_count = json_len(bufs);
    if (g->buffer_count > 0) {
        g->buffers = (uint8_t **)calloc(g->buffer_count, sizeof(uint8_t *));
        g->buffer_sizes = (size_t *)calloc(g->buffer_count, sizeof(size_t));
    }
    /* buffer uris are relative to the .gltf's folder */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '\\'), *slash2 = strrchr(dir, '/');
    if (slash2 > slash) slash = slash2;
    if (slash) slash[1] = 0; else dir[0] = 0;
    for (int i = 0; i < g->buffer_count; i++) {
        const char *uri = json_str(json_get(json_at(bufs, i), "uri"), NULL);
        if (!uri) continue;
        char bpath[1400];
        snprintf(bpath, sizeof(bpath), "%s%s", dir, uri);
        g->buffers[i] = read_file(bpath, &g->buffer_sizes[i]);
        if (!g->buffers[i]) { gltf_free(g); return 0; }
    }
    return 1;
}

void gltf_free(Gltf *g) {
    for (int i = 0; i < g->buffer_count; i++) free(g->buffers ? g->buffers[i] : NULL);
    free(g->buffers); free(g->buffer_sizes);
    json_free(&g->doc);
    memset(g, 0, sizeof(*g));
}

static int type_components(const char *t) {
    if (!t) return 0;
    if (!strcmp(t, "SCALAR")) return 1;
    if (!strcmp(t, "VEC2")) return 2;
    if (!strcmp(t, "VEC3")) return 3;
    if (!strcmp(t, "VEC4")) return 4;
    if (!strcmp(t, "MAT4")) return 16;
    return 0;
}
static int component_size(int ct) {
    switch (ct) { case 5120: case 5121: return 1; case 5122: case 5123: return 2; case 5125: case 5126: return 4; }
    return 0;
}

/* Locates an accessor's data: base pointer, element stride, count, components, component type. */
static const uint8_t *accessor_data(const Gltf *g, int accessor, int *count, int *comps, int *ctype, int *stride, int *normalized) {
    const JsonValue *acc = json_at(json_get(g->root, "accessors"), accessor);
    if (!acc) return NULL;
    *count = json_int(json_get(acc, "count"), 0);
    *comps = type_components(json_str(json_get(acc, "type"), NULL));
    *ctype = json_int(json_get(acc, "componentType"), 0);
    *normalized = json_num(json_get(acc, "normalized"), 0) != 0;
    int csize = component_size(*ctype);
    if (*count <= 0 || !*comps || !csize) return NULL;
    const JsonValue *bv = json_at(json_get(g->root, "bufferViews"), json_int(json_get(acc, "bufferView"), -1));
    if (!bv) return NULL;
    int buf = json_int(json_get(bv, "buffer"), -1);
    if (buf < 0 || buf >= g->buffer_count || !g->buffers[buf]) return NULL;
    size_t off = (size_t)json_int(json_get(bv, "byteOffset"), 0) + (size_t)json_int(json_get(acc, "byteOffset"), 0);
    *stride = json_int(json_get(bv, "byteStride"), 0);
    if (*stride <= 0) *stride = csize * *comps;
    size_t need = off + (size_t)(*count - 1) * *stride + (size_t)csize * *comps;
    if (need > g->buffer_sizes[buf]) return NULL;
    return g->buffers[buf] + off;
}

static double read_component(const uint8_t *p, int ctype, int normalized) {
    switch (ctype) {
        case 5126: { float f; memcpy(&f, p, 4); return f; }
        case 5121: return normalized ? p[0] / 255.0 : p[0];
        case 5120: { int8_t v = (int8_t)p[0]; return normalized ? (v < -127 ? -1.0 : v / 127.0) : v; }
        case 5123: { uint16_t v; memcpy(&v, p, 2); return normalized ? v / 65535.0 : v; }
        case 5122: { int16_t v; memcpy(&v, p, 2); return normalized ? (v < -32767 ? -1.0 : v / 32767.0) : v; }
        case 5125: { uint32_t v; memcpy(&v, p, 4); return v; }
    }
    return 0;
}

float *gltf_read_floats(const Gltf *g, int accessor, int *count, int *components) {
    int n, c, ct, stride, norm;
    const uint8_t *base = accessor_data(g, accessor, &n, &c, &ct, &stride, &norm);
    if (!base) return NULL;
    float *out = (float *)malloc(sizeof(float) * n * c);
    if (!out) return NULL;
    int cs = component_size(ct);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < c; k++) out[i * c + k] = (float)read_component(base + (size_t)i * stride + (size_t)k * cs, ct, norm);
    if (count) *count = n;
    if (components) *components = c;
    return out;
}

uint32_t *gltf_read_uints(const Gltf *g, int accessor, int *count, int *components) {
    int n, c, ct, stride, norm;
    const uint8_t *base = accessor_data(g, accessor, &n, &c, &ct, &stride, &norm);
    if (!base) return NULL;
    uint32_t *out = (uint32_t *)malloc(sizeof(uint32_t) * n * c);
    if (!out) return NULL;
    int cs = component_size(ct);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < c; k++) out[i * c + k] = (uint32_t)read_component(base + (size_t)i * stride + (size_t)k * cs, ct, 0);
    if (count) *count = n;
    if (components) *components = c;
    return out;
}
