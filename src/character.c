#include "character.h"
#include "gltf.h"
#include "image.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

Mat4 mat4_identity(void) {
    Mat4 r = {{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    return r;
}

Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a.m[k*4+row] * b.m[col*4+k];
            r.m[col*4+row] = s;
        }
    }
    return r;
}

static void quat_to_mat3(const float q[4], float m3[9]) {
    float x=q[0], y=q[1], z=q[2], w=q[3];
    float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, wx=w*x, wy=w*y, wz=w*z;
    m3[0]=1-2*(yy+zz); m3[1]=2*(xy+wz);   m3[2]=2*(xz-wy);
    m3[3]=2*(xy-wz);   m3[4]=1-2*(xx+zz); m3[5]=2*(yz+wx);
    m3[6]=2*(xz+wy);   m3[7]=2*(yz-wx);   m3[8]=1-2*(xx+yy);
}

static void mat3_transpose(const float m[9], float out[9]) {
    out[0]=m[0]; out[1]=m[3]; out[2]=m[6];
    out[3]=m[1]; out[4]=m[4]; out[5]=m[7];
    out[6]=m[2]; out[7]=m[5]; out[8]=m[8];
}

static void mat3_vec(const float m[9], const float v[3], float out[3]) {
    out[0] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2];
    out[1] = m[3]*v[0] + m[4]*v[1] + m[5]*v[2];
    out[2] = m[6]*v[0] + m[7]*v[1] + m[8]*v[2];
}

Mat4 mat4_from_trs(const float t[3], const float q[4], const float s[3]) {
    float m3[9];
    /* quat_to_mat3 returns the rotation TRANSPOSED (row-major m3 = R^T,
       see camera_init), so R's column j is m3[3j..3j+2]. Reading it as R
       itself (as this used to) made every bone rotate the OPPOSITE way:
       legs folding into each other, head turned backwards. */
    quat_to_mat3(q, m3);
    Mat4 r;
    r.m[0]=m3[0]*s[0]; r.m[1]=m3[1]*s[0]; r.m[2]=m3[2]*s[0]; r.m[3]=0;
    r.m[4]=m3[3]*s[1]; r.m[5]=m3[4]*s[1]; r.m[6]=m3[5]*s[1]; r.m[7]=0;
    r.m[8]=m3[6]*s[2]; r.m[9]=m3[7]*s[2]; r.m[10]=m3[8]*s[2]; r.m[11]=0;
    r.m[12]=t[0]; r.m[13]=t[1]; r.m[14]=t[2]; r.m[15]=1;
    return r;
}

void mat4_vec3(const Mat4 *m, const float v[3], float out[3]) {
    out[0] = m->m[0]*v[0] + m->m[4]*v[1] + m->m[8]*v[2] + m->m[12];
    out[1] = m->m[1]*v[0] + m->m[5]*v[1] + m->m[9]*v[2] + m->m[13];
    out[2] = m->m[2]*v[0] + m->m[6]*v[1] + m->m[10]*v[2] + m->m[14];
}

void quat_nlerp(const float a[4], const float b_in[4], float t, float out[4]) {
    float b[4] = {b_in[0], b_in[1], b_in[2], b_in[3]};
    float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    if (dot < 0) { b[0]=-b[0]; b[1]=-b[1]; b[2]=-b[2]; b[3]=-b[3]; }
    float o[4];
    float n = 0;
    for (int i = 0; i < 4; i++) { o[i] = a[i] + (b[i]-a[i])*t; n += o[i]*o[i]; }
    n = sqrtf(n);
    if (n < 1e-8f) { memcpy(out, a, 4*sizeof(float)); return; }
    for (int i = 0; i < 4; i++) out[i] = o[i]/n;
}

/* ---------------- skeleton ---------------- */

void skeleton_skin_matrices_for(const CharModel *m, const NodeOverride *overrides, Mat4 *out_skin_mats) {
    static Mat4 local[DAVID_MAX_NODES];
    static Mat4 global[DAVID_MAX_NODES];
    for (int i = 0; i < m->node_count; i++) {
        const NodeOverride *ov = &overrides[i];
        const float *t = ov->has_t ? ov->t : m->node_t[i];
        const float *q = ov->has_r ? ov->r : m->node_r[i];
        const float *s = ov->has_s ? ov->s : m->node_s[i];
        local[i] = mat4_from_trs(t, q, s);
    }
    /* node_parent is always < node index in these exports: one forward pass */
    for (int i = 0; i < m->node_count; i++) {
        int p = m->node_parent[i];
        global[i] = (p >= 0 && p < i) ? mat4_mul(global[p], local[i]) : local[i];
    }
    for (int ji = 0; ji < m->joint_count; ji++) {
        Mat4 ib;
        memcpy(ib.m, m->inv_bind[ji], 16 * sizeof(float));
        int j = m->joints[ji];
        out_skin_mats[ji] = mat4_mul(global[(j >= 0 && j < m->node_count) ? j : 0], ib);
    }
}

void skeleton_compute_skin_matrices(const NodeOverride *overrides, Mat4 *out_skin_mats) {
    skeleton_skin_matrices_for(&g_david, overrides, out_skin_mats);
}

/* ---------------- animation sampling ---------------- */

static void anim_sample_generic(float t, NodeOverride *overrides,
                                 float duration, int n_channels,
                                 const int *channel_node, const int *channel_path,
                                 const int *channel_key_offset, const int *channel_key_count,
                                 const float *times, const float (*values)[4]) {
    memset(overrides, 0, DAVID_MAX_NODES * sizeof(NodeOverride));
    float tt = duration > 0 ? fmodf(t, duration) : 0.0f;
    if (tt < 0) tt += duration;

    for (int c = 0; c < n_channels; c++) {
        int off = channel_key_offset[c];
        int cnt = channel_key_count[c];
        const float *ktimes = times + off;
        const float (*kvalues)[4] = values + off;

        int i0, i1; float frac;
        if (cnt <= 1 || tt <= ktimes[0]) { i0 = i1 = 0; frac = 0.0f; }
        else if (tt >= ktimes[cnt-1]) { i0 = i1 = cnt-1; frac = 0.0f; }
        else {
            i1 = 0;
            while (i1 < cnt && ktimes[i1] <= tt) i1++;
            if (i1 >= cnt) i1 = cnt - 1;
            i0 = i1 - 1;
            if (i0 < 0) i0 = 0;
            float denom = ktimes[i1] - ktimes[i0];
            frac = denom > 1e-8f ? (tt - ktimes[i0]) / denom : 0.0f;
        }

        int node = channel_node[c];
        NodeOverride *ov = &overrides[node];
        int path = channel_path[c];
        if (path == 0) { /* translation */
            for (int k = 0; k < 3; k++) ov->t[k] = kvalues[i0][k] + (kvalues[i1][k]-kvalues[i0][k])*frac;
            ov->has_t = 1;
        } else if (path == 1) { /* rotation */
            if (i0 == i1) memcpy(ov->r, kvalues[i0], 4*sizeof(float));
            else quat_nlerp(kvalues[i0], kvalues[i1], frac, ov->r);
            ov->has_r = 1;
        } else { /* scale */
            for (int k = 0; k < 3; k++) ov->s[k] = kvalues[i0][k] + (kvalues[i1][k]-kvalues[i0][k])*frac;
            ov->has_s = 1;
        }
    }
}

/* ---------------- David (the blockouts' glTF) ---------------- */
DavidModel g_david;

void char_model_free(CharModel *m) {
    free(m->positions); free(m->uvs_px); free(m->joints_idx); free(m->weights); free(m->indices); free(m->tex_rgb);
    memset(m, 0, sizeof(*m));
}

int char_model_load(CharModel *m, const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s\\%s.gltf", dir, name);
    char_model_free(m);
    snprintf(m->name, sizeof(m->name), "%.47s", name);
    Gltf g;
    if (!gltf_load(path, &g)) return 0;
    int ok = 0;
    const JsonValue *nodes = json_get(g.root, "nodes");
    m->node_count = json_len(nodes);
    const JsonValue *skin = json_at(json_get(g.root, "skins"), 0);
    const JsonValue *prims = json_get(json_at(json_get(g.root, "meshes"), 0), "primitives");
    float *ib = NULL;
    int nib = 0, c;
    if (m->node_count <= 0 || m->node_count > DAVID_MAX_NODES || !skin || json_len(prims) <= 0) goto done;
    for (int i = 0; i < m->node_count; i++) {
        const JsonValue *n = json_at(nodes, i);
        const JsonValue *t = json_get(n, "translation"), *r = json_get(n, "rotation"), *s = json_get(n, "scale");
        for (int k = 0; k < 3; k++) m->node_t[i][k] = (float)json_num(json_at(t, k), 0.0);
        for (int k = 0; k < 4; k++) m->node_r[i][k] = (float)json_num(json_at(r, k), k == 3 ? 1.0 : 0.0);
        for (int k = 0; k < 3; k++) m->node_s[i][k] = (float)json_num(json_at(s, k), 1.0);
        m->node_parent[i] = -1;
    }
    for (int i = 0; i < m->node_count; i++) {
        const JsonValue *ch = json_get(json_at(nodes, i), "children");
        for (int k = 0; k < json_len(ch); k++) {
            int cidx = json_int(json_at(ch, k), -1);
            if (cidx >= 0 && cidx < m->node_count) m->node_parent[cidx] = i;
        }
    }
    const JsonValue *joints = json_get(skin, "joints");
    m->joint_count = json_len(joints);
    if (m->joint_count <= 0 || m->joint_count > DAVID_MAX_JOINTS) goto done;
    for (int j = 0; j < m->joint_count; j++) m->joints[j] = json_int(json_at(joints, j), 0);
    ib = gltf_read_floats(&g, json_int(json_get(skin, "inverseBindMatrices"), -1), &nib, &c);
    if (!ib || nib != m->joint_count || c != 16) goto done;
    memcpy(m->inv_bind, ib, sizeof(float) * 16 * nib);

    /* texture: the first primitive's material, else <name>.png */
    char tex_name[256];
    snprintf(tex_name, sizeof(tex_name), "%s.png", name);
    {
        int mat = json_int(json_get(json_at(prims, 0), "material"), 0);
        int ti = json_int(json_get(json_get(json_get(json_at(json_get(g.root, "materials"), mat), "pbrMetallicRoughness"), "baseColorTexture"), "index"), -1);
        int src = json_int(json_get(json_at(json_get(g.root, "textures"), ti), "source"), -1);
        const char *uri = json_str(json_get(json_at(json_get(g.root, "images"), src), "uri"), NULL);
        if (uri) snprintf(tex_name, sizeof(tex_name), "%s", uri);
    }
    snprintf(path, sizeof(path), "%s\\%s", dir, tex_name);
    int tw = 0, th = 0;
    uint32_t *tex = image_load(path, &tw, &th);
    if (!tex) { tw = th = 1; tex = (uint32_t *)malloc(4); tex[0] = 0xFFA0A0A0u; } /* untextured: grey */
    m->tex_w = tw; m->tex_h = th;
    m->tex_rgb = (uint8_t *)malloc((size_t)tw * th * 3);
    for (int i = 0; i < tw * th; i++) {
        m->tex_rgb[i * 3] = (uint8_t)(tex[i] >> 16); m->tex_rgb[i * 3 + 1] = (uint8_t)(tex[i] >> 8); m->tex_rgb[i * 3 + 2] = (uint8_t)tex[i];
    }
    free(tex);

    /* every primitive, merged (the small second ones are double-sided bits) */
    for (int p = 0; p < json_len(prims); p++) {
        const JsonValue *prim = json_at(prims, p), *attr = json_get(prim, "attributes");
        int nv = 0, nuv = 0, nw = 0, nj = 0, ni = 0;
        float *pos = gltf_read_floats(&g, json_int(json_get(attr, "POSITION"), -1), &nv, &c);
        if (!pos || c != 3) { free(pos); continue; }
        float *uv = gltf_read_floats(&g, json_int(json_get(attr, "TEXCOORD_0"), -1), &nuv, &c);
        if (uv && (nuv != nv || c != 2)) { free(uv); uv = NULL; }
        float *wt = gltf_read_floats(&g, json_int(json_get(attr, "WEIGHTS_0"), -1), &nw, &c);
        uint32_t *jn = gltf_read_uints(&g, json_int(json_get(attr, "JOINTS_0"), -1), &nj, &c);
        uint32_t *idx = gltf_read_uints(&g, json_int(json_get(prim, "indices"), -1), &ni, &c);
        if (!wt || nw != nv || !jn || nj != nv || !idx || ni % 3 || m->vertex_count + nv > DAVID_MAX_VERTS) {
            free(pos); free(uv); free(wt); free(jn); free(idx); continue;
        }
        int base = m->vertex_count, nvt = base + nv, nit = m->index_count + ni;
        m->positions = (float (*)[3])realloc(m->positions, sizeof(float) * 3 * nvt);
        m->uvs_px = (float (*)[2])realloc(m->uvs_px, sizeof(float) * 2 * nvt);
        m->weights = (float (*)[4])realloc(m->weights, sizeof(float) * 4 * nvt);
        m->joints_idx = (uint8_t (*)[4])realloc(m->joints_idx, 4 * (size_t)nvt);
        m->indices = (uint32_t *)realloc(m->indices, sizeof(uint32_t) * nit);
        for (int v = 0; v < nv; v++) {
            memcpy(m->positions[base + v], pos + v * 3, sizeof(float) * 3);
            memcpy(m->weights[base + v], wt + v * 4, sizeof(float) * 4);
            m->uvs_px[base + v][0] = uv ? uv[v * 2] * tw : 0.0f;
            m->uvs_px[base + v][1] = uv ? uv[v * 2 + 1] * th : 0.0f;
            for (int k = 0; k < 4; k++) m->joints_idx[base + v][k] = (uint8_t)(jn[v * 4 + k] < (uint32_t)m->joint_count ? jn[v * 4 + k] : 0);
        }
        for (int i = 0; i < ni; i++) m->indices[m->index_count + i] = base + (idx[i] < (uint32_t)nv ? idx[i] : 0);
        m->vertex_count = nvt; m->index_count = nit;
        free(pos); free(uv); free(wt); free(jn); free(idx);
    }
    ok = m->vertex_count > 0 && m->index_count > 0;
done:
    free(ib);
    gltf_free(&g);
    if (!ok) { char keep[48]; snprintf(keep, sizeof(keep), "%s", m->name); char_model_free(m); snprintf(m->name, sizeof(m->name), "%s", keep); }
    return ok;
}

int david_load(const char *dir) { return char_model_load(&g_david, dir, "david"); }

/* ---------------- animation library ---------------- */
typedef struct {
    char name[48], source[8];
    char path[600];
    int state;             /* 0 not loaded, 1 loaded, -1 unreadable / no animation */
    int nodes;             /* the clip file's skeleton node count */
    float duration;
    int n_channels, n_keys;
    int *node, *kind, *off, *cnt; /* per channel; kind 0=T 1=R 2=S */
    float *times;          /* n_keys */
    float (*values)[4];    /* n_keys */
} LibClip;
static LibClip *g_lib = NULL;
static int g_lib_count = 0;
static int g_lib_cap = 0;

static int lib_name_cmp(const void *a, const void *b) {
    const LibClip *x = (const LibClip *)a, *y = (const LibClip *)b;
    int s = strcmp(x->source, y->source); /* "anims" before "david" */
    return s ? s : strcmp(x->name, y->name);
}

static void lib_add_dir(const char *dir, const char *source, int *cap) {
    char pattern[700];
    snprintf(pattern, sizeof(pattern), "%s\\*.gltf", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_lib_count == *cap) { *cap = *cap ? *cap * 2 : 256; g_lib = (LibClip *)realloc(g_lib, sizeof(LibClip) * *cap); }
        LibClip *c = &g_lib[g_lib_count];
        memset(c, 0, sizeof(*c));
        snprintf(c->path, sizeof(c->path), "%s\\%s", dir, fd.cFileName);
        snprintf(c->name, sizeof(c->name), "%.47s", fd.cFileName);
        char *dot = strrchr(c->name, '.'); if (dot) *dot = 0;
        snprintf(c->source, sizeof(c->source), "%s", source);
        g_lib_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int anim_lib_init(const char *anims_dir, const char *david_dir) {
    lib_add_dir(anims_dir, "anims", &g_lib_cap);
    lib_add_dir(david_dir, "david", &g_lib_cap);
    if (g_lib_count) qsort(g_lib, g_lib_count, sizeof(LibClip), lib_name_cmp);
    return g_lib_count;
}
int anim_lib_add_dir(const char *dir, const char *source) {
    int before = g_lib_count;
    lib_add_dir(dir, source, &g_lib_cap);
    if (g_lib_count > before) qsort(g_lib + before, g_lib_count - before, sizeof(LibClip), lib_name_cmp);
    return g_lib_count - before;
}
int anim_lib_count(void) { return g_lib_count; }
const char *anim_lib_name(int i) { return (i >= 0 && i < g_lib_count) ? g_lib[i].name : ""; }
const char *anim_lib_source(int i) { return (i >= 0 && i < g_lib_count) ? g_lib[i].source : ""; }
int anim_lib_find(const char *name) {
    for (int i = 0; i < g_lib_count; i++) if (strcmp(g_lib[i].name, name) == 0) return i;
    return -1;
}

/* glTF clip -> channels / key times / values (4 floats per key, T/S w=0) */
static int lib_load_clip(LibClip *c) {
    Gltf g;
    if (!gltf_load(c->path, &g)) return -1;
    int result = -1;
    const JsonValue *anim = json_at(json_get(g.root, "animations"), 0);
    const JsonValue *chans = json_get(anim, "channels"), *samplers = json_get(anim, "samplers");
    int nch = json_len(chans);
    c->nodes = json_len(json_get(g.root, "nodes"));
    if (c->nodes <= 0 || c->nodes > DAVID_MAX_NODES || !anim || nch <= 0) goto done;
    c->node = (int *)calloc(nch, sizeof(int)); c->kind = (int *)calloc(nch, sizeof(int));
    c->off = (int *)calloc(nch, sizeof(int)); c->cnt = (int *)calloc(nch, sizeof(int));
    int cap = 0;
    for (int k = 0; k < nch; k++) {
        const JsonValue *ch = json_at(chans, k), *tgt = json_get(ch, "target");
        const char *path = json_str(json_get(tgt, "path"), "");
        int kind = !strcmp(path, "translation") ? 0 : !strcmp(path, "rotation") ? 1 : !strcmp(path, "scale") ? 2 : -1;
        int node = json_int(json_get(tgt, "node"), -1);
        if (kind < 0 || node < 0 || node >= c->nodes) continue;
        const JsonValue *smp = json_at(samplers, json_int(json_get(ch, "sampler"), -1));
        int nt = 0, nvv = 0, ct = 0, cv = 0;
        float *t = gltf_read_floats(&g, json_int(json_get(smp, "input"), -1), &nt, &ct);
        float *v = gltf_read_floats(&g, json_int(json_get(smp, "output"), -1), &nvv, &cv);
        if (!t || !v || nt != nvv || ct != 1 || cv < 3 || cv > 4) { free(t); free(v); continue; }
        if (c->n_keys + nt > cap) {
            while (c->n_keys + nt > cap) cap = cap ? cap * 2 : 1024;
            c->times = (float *)realloc(c->times, sizeof(float) * cap);
            c->values = (float (*)[4])realloc(c->values, sizeof(float) * 4 * cap);
        }
        int ch_i = c->n_channels++;
        c->node[ch_i] = node; c->kind[ch_i] = kind; c->off[ch_i] = c->n_keys; c->cnt[ch_i] = nt;
        for (int i = 0; i < nt; i++) {
            c->times[c->n_keys + i] = t[i];
            if (t[i] > c->duration) c->duration = t[i];
            for (int q = 0; q < 4; q++) c->values[c->n_keys + i][q] = q < cv ? v[i * cv + q] : 0.0f;
        }
        c->n_keys += nt;
        free(t); free(v);
    }
    result = c->n_channels > 0 ? 1 : -1;
done:
    gltf_free(&g);
    return result;
}

static int lib_loaded(int i) {
    if (i < 0 || i >= g_lib_count) return 0;
    if (g_lib[i].state == 0) g_lib[i].state = lib_load_clip(&g_lib[i]);
    return g_lib[i].state == 1;
}
int anim_lib_nodes(int i) { return lib_loaded(i) ? g_lib[i].nodes : -1; }
int anim_lib_fits(int i, int node_count) { return lib_loaded(i) && g_lib[i].nodes == node_count; }
int anim_lib_ready(int i) { return anim_lib_fits(i, g_david.node_count); }
float anim_lib_duration(int i) { return lib_loaded(i) ? g_lib[i].duration : 0.0f; }
int anim_lib_channels(int i) { return lib_loaded(i) ? g_lib[i].n_channels : 0; }
int anim_lib_keys(int i) { return lib_loaded(i) ? g_lib[i].n_keys : 0; }

void anim_lib_sample(int i, float t, NodeOverride *overrides) {
    if (!lib_loaded(i)) { memset(overrides, 0, DAVID_MAX_NODES * sizeof(NodeOverride)); return; }
    LibClip *c = &g_lib[i];
    anim_sample_generic(t, overrides, c->duration, c->n_channels, c->node, c->kind, c->off, c->cnt, c->times, (const float (*)[4])c->values);
}

static void sample_named(const char *name, int *cache, float t, NodeOverride *overrides) {
    if (*cache == -2) *cache = anim_lib_find(name);
    anim_lib_sample(*cache, t, overrides);
}
void anim_sample_idle(float t, NodeOverride *overrides) { static int id = -2; sample_named("stand", &id, t, overrides); }
void anim_sample_walk(float t, NodeOverride *overrides) { static int id = -2; sample_named("walk", &id, t, overrides); }
void anim_sample_run(float t, NodeOverride *overrides) { static int id = -2; sample_named("run", &id, t, overrides); }

/* ---------------- camera ---------------- */

void camera_init(RoomCamera *cam, const float translation[3], const float rotation[4], float yfov) {
    memcpy(cam->translation, translation, 3*sizeof(float));
    /* quat_to_mat3's sign convention (matching the skeleton code, which
       needs it that way) is the TRANSPOSE of what the camera math here
       needs -- world_to_pixel/pixel_to_ray were derived against
       the Python prototype's quat_to_mat3, which flips the
       off-diagonal signs relative to this one. Rather than keep two
       divergent quat_to_mat3 implementations, compute this one and
       swap which side gets transposed. */
    quat_to_mat3(rotation, cam->Rinv);
    mat3_transpose(cam->Rinv, cam->R);
    cam->yfov = yfov;
    cam->focal = 0.0f; cam->cx = 0.0f; cam->cy = 0.0f; /* legacy intrinsics until calibrated */
}

void camera_make_upright(const RoomCamera *src, RoomCamera *out) {
    memcpy(out->translation, src->translation, 3*sizeof(float));
    out->yfov = src->yfov;
    out->focal = src->focal; out->cx = src->cx; out->cy = src->cy;
    /* R's column 2 is -forward (glTF: camera looks down local -Z), row-
       major storage means column j of row i is R[i*3+j]. */
    float fwd[3] = { -src->R[2], -src->R[5], -src->R[8] };
    float flen = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (flen < 1e-8f) { memcpy(out->R, src->R, 9*sizeof(float)); memcpy(out->Rinv, src->Rinv, 9*sizeof(float)); return; }
    fwd[0]/=flen; fwd[1]/=flen; fwd[2]/=flen;
    float world_up[3] = {0.0f, 1.0f, 0.0f};
    float right[3] = {
        fwd[1]*world_up[2]-fwd[2]*world_up[1],
        fwd[2]*world_up[0]-fwd[0]*world_up[2],
        fwd[0]*world_up[1]-fwd[1]*world_up[0]
    };
    float rlen = sqrtf(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
    if (rlen < 1e-6f) { memcpy(out->R, src->R, 9*sizeof(float)); memcpy(out->Rinv, src->Rinv, 9*sizeof(float)); return; } /* looking straight up/down -- degenerate, keep original rather than guess */
    right[0]/=rlen; right[1]/=rlen; right[2]/=rlen;
    float up[3] = {
        right[1]*fwd[2]-right[2]*fwd[1],
        right[2]*fwd[0]-right[0]*fwd[2],
        right[0]*fwd[1]-right[1]*fwd[0]
    };
    out->R[0]=right[0]; out->R[1]=up[0]; out->R[2]=-fwd[0];
    out->R[3]=right[1]; out->R[4]=up[1]; out->R[5]=-fwd[1];
    out->R[6]=right[2]; out->R[7]=up[2]; out->R[8]=-fwd[2];
    mat3_transpose(out->R, out->Rinv);
}

/* Pinhole intrinsics in pixels. A room calibrated against its original
   depth mask (cam->focal > 0, see CALIBRATE_CAMERAS in main.c) uses its
   fitted values. Default rule, measured on all rooms against the game's
   own depth masks (see PROGRESS.md): the blockouts' exporter wrote yfov as if the
   viewport were 16:9, while the real field of view is HORIZONTAL over the
   image width, so tan(hfov/2) = tan(yfov/2)*16/9, f = (W/2)/tan(hfov/2),
   principal point centered. The old "yfov spans the image height"
   reading was 1.34x too zoomed on 640x480 rooms, worse on tall/wide ones. */
static void camera_intrinsics(const RoomCamera *cam, int img_w, int img_h, float *f, float *cx, float *cy) {
    if (cam->focal > 0.0f) { *f = cam->focal; *cx = cam->cx; *cy = cam->cy; return; }
    *f = (img_w * 0.5f) / (tanf(cam->yfov / 2.0f) * (16.0f / 9.0f));
    *cx = img_w * 0.5f;
    *cy = img_h * 0.5f;
}

int camera_world_to_pixel(const RoomCamera *cam, const float world[3], int img_w, int img_h, float *px, float *py) {
    float d;
    return camera_world_to_pixel_z(cam, world, img_w, img_h, px, py, &d);
}

int camera_world_to_pixel_z(const RoomCamera *cam, const float world[3], int img_w, int img_h,
                             float *px, float *py, float *out_depth) {
    float rel[3] = { world[0]-cam->translation[0], world[1]-cam->translation[1], world[2]-cam->translation[2] };
    float v[3];
    mat3_vec(cam->Rinv, rel, v);
    if (v[2] >= -1e-6f) return 0;
    float f, cx, cy;
    camera_intrinsics(cam, img_w, img_h, &f, &cx, &cy);
    *px = cx + f * v[0] / (-v[2]);
    *py = cy - f * v[1] / (-v[2]);
    *out_depth = -v[2]; /* camera-space distance along view axis, always positive for a visible point */
    return 1;
}

void camera_pixel_to_ray(const RoomCamera *cam, float px, float py, int img_w, int img_h,
                          float out_origin[3], float out_dir[3]) {
    float f, cx, cy;
    camera_intrinsics(cam, img_w, img_h, &f, &cx, &cy);
    float view_dir[3] = { (px - cx) / f, (cy - py) / f, -1.0f };
    float dir[3];
    mat3_vec(cam->R, view_dir, dir);
    float n = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    if (n < 1e-8f) n = 1.0f;
    out_dir[0] = dir[0]/n; out_dir[1] = dir[1]/n; out_dir[2] = dir[2]/n;
    out_origin[0] = cam->translation[0];
    out_origin[1] = cam->translation[1];
    out_origin[2] = cam->translation[2];
}
int camera_pixel_to_floor_point(const RoomCamera *cam, float px, float py, int img_w, int img_h,
                                 float floor_y, float world[3]) {
    float origin[3], dir[3];
    camera_pixel_to_ray(cam, px, py, img_w, img_h, origin, dir);
    if (fabsf(dir[1]) < 1e-8f) return 0;
    float t = (floor_y - origin[1]) / dir[1];
    if (t <= 0) return 0;
    world[0] = origin[0] + dir[0]*t;
    world[1] = origin[1] + dir[1]*t;
    world[2] = origin[2] + dir[2]*t;
    return 1;
}

/* ---------------- ray-mesh collision (real environment geometry) ---------------- */

/* Möller-Trumbore ray-triangle intersection. Returns 1 and fills *out_t
   (ray-parameter distance) if the ray hits the triangle at t>eps. */
static int ray_triangle_intersect(const float orig[3], const float dir[3],
                                   const float v0[3], const float v1[3], const float v2[3],
                                   float *out_t) {
    float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
    float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
    float pvec[3] = { dir[1]*e2[2]-dir[2]*e2[1], dir[2]*e2[0]-dir[0]*e2[2], dir[0]*e2[1]-dir[1]*e2[0] };
    float det = e1[0]*pvec[0] + e1[1]*pvec[1] + e1[2]*pvec[2];
    if (fabsf(det) < 1e-9f) return 0;
    float inv_det = 1.0f / det;
    float tvec[3] = { orig[0]-v0[0], orig[1]-v0[1], orig[2]-v0[2] };
    float u = (tvec[0]*pvec[0] + tvec[1]*pvec[1] + tvec[2]*pvec[2]) * inv_det;
    if (u < -1e-5f || u > 1.0f + 1e-5f) return 0;
    float qvec[3] = { tvec[1]*e1[2]-tvec[2]*e1[1], tvec[2]*e1[0]-tvec[0]*e1[2], tvec[0]*e1[1]-tvec[1]*e1[0] };
    float v = (dir[0]*qvec[0] + dir[1]*qvec[1] + dir[2]*qvec[2]) * inv_det;
    if (v < -1e-5f || u + v > 1.0f + 1e-5f) return 0;
    float t = (e2[0]*qvec[0] + e2[1]*qvec[1] + e2[2]*qvec[2]) * inv_det;
    if (t <= 1e-4f) return 0;
    *out_t = t;
    return 1;
}

int raycast_room_mesh(const float ray_origin[3], const float ray_dir[3],
                       const float *triangles, int triangle_count,
                       int floor_only, float min_up_dot,
                       float out_point[3], float out_normal[3]) {
    float best_t = 1e30f;
    int found = 0;
    float best_normal[3] = {0, 1, 0};
    for (int i = 0; i < triangle_count; i++) {
        const float *v0 = triangles + i * 9;
        const float *v1 = v0 + 3;
        const float *v2 = v0 + 6;
        float t;
        if (!ray_triangle_intersect(ray_origin, ray_dir, v0, v1, v2, &t)) continue;
        if (t >= best_t) continue;
        float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
        float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        float nlen = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if (nlen < 1e-8f) continue;
        n[0]/=nlen; n[1]/=nlen; n[2]/=nlen;
        if (n[1] < 0) { n[0]=-n[0]; n[1]=-n[1]; n[2]=-n[2]; } /* normalize to point "up-ish" --
            NOTE: mesh0's winding is not reliably consistent (verified: requiring the RAW,
            un-flipped normal to face up made the room's own start-position probe find no
            floor at all in rooms that work fine today), so this can't distinguish a true
            floor from a downward-facing backface by winding alone. Callers that probe
            straight down through a large chunk of the mesh (nav grid, path walkability,
            the shadow) instead bound the probe's travel distance -- see raycast_room_mesh's
            max_dist param -- to keep from ever reaching a deep, wrongly-classified surface
            far below the real floor (verified case: the level's own outer shell bottom,
            ~50 units down) rather than trying to classify it away here. */
        if (floor_only && n[1] < min_up_dot) continue; /* wall/steep surface, not floor */
        best_t = t;
        found = 1;
        best_normal[0]=n[0]; best_normal[1]=n[1]; best_normal[2]=n[2];
    }
    if (!found) return 0;
    out_point[0] = ray_origin[0] + ray_dir[0]*best_t;
    out_point[1] = ray_origin[1] + ray_dir[1]*best_t;
    out_point[2] = ray_origin[2] + ray_dir[2]*best_t;
    out_normal[0] = best_normal[0]; out_normal[1] = best_normal[1]; out_normal[2] = best_normal[2];
    return 1;
}

/* Camera/view-ray variant: same as raycast_room_mesh, but restricted to
   triangles FACING the ray (glTF CCW front faces) whenever the ray hits
   any at all, falling back to back faces only when it hits none. This is
   backface culling without losing isolated wrongly-wound geometry: the blockouts'
   blockouts are closed rooms, and some cameras sit outside a wall the
   original pre-rendered image never showed (the wall is seen from its
   back -- verified: chains/bigroom5, deadgate/pit, palace/s_room_2, where
   100% of the frame's nearest surface was such a wall). Measured across
   all rooms: ~91% of first-hit surfaces are front-facing, so winding IS
   reliable enough to prefer, just not to require. */
int raycast_room_mesh_view(const float ray_origin[3], const float ray_dir[3],
                            const float *triangles, int triangle_count,
                            int floor_only, float min_up_dot,
                            float out_point[3], float out_normal[3]) {
    for (int want_front = 1; want_front >= 0; want_front--) {
        float best_t = 1e30f;
        int any_facing = 0, found = 0;
        float best_normal[3] = {0, 1, 0};
        for (int i = 0; i < triangle_count; i++) {
            const float *v0 = triangles + i * 9;
            const float *v1 = v0 + 3;
            const float *v2 = v0 + 6;
            float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
            float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
            float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
            int is_front = (n[0]*ray_dir[0] + n[1]*ray_dir[1] + n[2]*ray_dir[2]) < 0;
            if (is_front != want_front) continue;
            float t;
            if (!ray_triangle_intersect(ray_origin, ray_dir, v0, v1, v2, &t)) continue;
            any_facing = 1;
            if (t >= best_t) continue;
            float nlen = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            if (nlen < 1e-8f) continue;
            n[0]/=nlen; n[1]/=nlen; n[2]/=nlen;
            if (n[1] < 0) { n[0]=-n[0]; n[1]=-n[1]; n[2]=-n[2]; }
            if (floor_only && n[1] < min_up_dot) continue;
            best_t = t;
            found = 1;
            best_normal[0]=n[0]; best_normal[1]=n[1]; best_normal[2]=n[2];
        }
        if (!any_facing) continue; /* no front face on this ray at all -> try back faces */
        if (!found) return 0;
        out_point[0] = ray_origin[0] + ray_dir[0]*best_t;
        out_point[1] = ray_origin[1] + ray_dir[1]*best_t;
        out_point[2] = ray_origin[2] + ray_dir[2]*best_t;
        out_normal[0] = best_normal[0]; out_normal[1] = best_normal[1]; out_normal[2] = best_normal[2];
        return 1;
    }
    return 0;
}

/* ---------------- spatial bins (XZ) over the room mesh ----------------
   Every walkability query below is a vertical probe or a short segment,
   so bucketing triangles by their XZ footprint makes them cheap enough
   to run on every A* edge and every movement frame. Rebuilt whenever a
   different triangle buffer is passed (see trimesh_invalidate_cache). */
typedef struct {
    const float *tris; int n;
    float min_x, min_z, cell; int cols, rows;
    int *start; int *items;
} TriBins;
static TriBins g_bins;

void trimesh_invalidate_cache(void) {
    free(g_bins.start); free(g_bins.items);
    memset(&g_bins, 0, sizeof(g_bins));
}

static void ensure_bins(const float *tris, int n) {
    if (g_bins.tris == tris && g_bins.n == n && g_bins.start) return;
    trimesh_invalidate_cache();
    g_bins.tris = tris; g_bins.n = n;
    if (n <= 0) return;
    float min_x = 1e30f, max_x = -1e30f, min_z = 1e30f, max_z = -1e30f;
    for (int i = 0; i < n * 3; i++) {
        const float *v = tris + i * 3;
        if (v[0] < min_x) min_x = v[0];
        if (v[0] > max_x) max_x = v[0];
        if (v[2] < min_z) min_z = v[2];
        if (v[2] > max_z) max_z = v[2];
    }
    float cell = 2.0f;
    int cols = (int)((max_x - min_x) / cell) + 1, rows = (int)((max_z - min_z) / cell) + 1;
    while ((long)cols * rows > 65536) { cell *= 1.4f; cols = (int)((max_x - min_x) / cell) + 1; rows = (int)((max_z - min_z) / cell) + 1; }
    g_bins.min_x = min_x; g_bins.min_z = min_z; g_bins.cell = cell; g_bins.cols = cols; g_bins.rows = rows;
    int nb = cols * rows;
    int *count = (int *)calloc((size_t)nb + 1, sizeof(int));
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < n; t++) {
            const float *v = tris + (size_t)t * 9;
            float lx = fminf(v[0], fminf(v[3], v[6])), hx = fmaxf(v[0], fmaxf(v[3], v[6]));
            float lz = fminf(v[2], fminf(v[5], v[8])), hz = fmaxf(v[2], fmaxf(v[5], v[8]));
            int c0 = (int)((lx - min_x) / cell), c1 = (int)((hx - min_x) / cell);
            int r0 = (int)((lz - min_z) / cell), r1 = (int)((hz - min_z) / cell);
            if (c1 >= cols) c1 = cols - 1;
            if (r1 >= rows) r1 = rows - 1;
            for (int r = r0; r <= r1; r++) for (int c = c0; c <= c1; c++) {
                int b = r * cols + c;
                if (pass == 0) count[b + 1]++;
                else g_bins.items[g_bins.start[b] + count[b]++] = t;
            }
        }
        if (pass == 0) {
            g_bins.start = (int *)malloc(sizeof(int) * ((size_t)nb + 1));
            g_bins.start[0] = 0;
            for (int b = 0; b < nb; b++) g_bins.start[b + 1] = g_bins.start[b] + count[b + 1];
            g_bins.items = (int *)malloc(sizeof(int) * ((size_t)g_bins.start[nb] + 1));
            memset(count, 0, sizeof(int) * ((size_t)nb + 1));
        }
    }
    free(count);
}

/* Calls fn(t) for every triangle whose XZ box touches [lx,hx]x[lz,hz].
   A triangle may be visited more than once (it can span several bins);
   callers only take minima/any-hit, so duplicates are harmless. */
#define FOR_BINNED_TRIS(lx, hx, lz, hz, T, ...) do {                                   \
    int c0_ = (int)(((lx) - g_bins.min_x) / g_bins.cell), c1_ = (int)(((hx) - g_bins.min_x) / g_bins.cell); \
    int r0_ = (int)(((lz) - g_bins.min_z) / g_bins.cell), r1_ = (int)(((hz) - g_bins.min_z) / g_bins.cell); \
    if (c0_ < 0) { c0_ = 0; } if (r0_ < 0) { r0_ = 0; }                                            \
    if (c1_ >= g_bins.cols) { c1_ = g_bins.cols - 1; } if (r1_ >= g_bins.rows) { r1_ = g_bins.rows - 1; } \
    for (int r_ = r0_; r_ <= r1_; r_++) for (int c_ = c0_; c_ <= c1_; c_++) {           \
        int b_ = r_ * g_bins.cols + c_;                                                    \
        for (int k_ = g_bins.start[b_]; k_ < g_bins.start[b_ + 1]; k_++) {                \
            int T = g_bins.items[k_];                                                      \
            __VA_ARGS__                                                                    \
        }                                                                                  \
    }                                                                                      \
} while (0)

static void navgrid_label_components(NavGrid *grid, const float *tris, int n);
NavParams g_nav = NAV_DEFAULT_PARAMS;
void nav_params_changed(void) { g_nav.min_up = cosf(g_nav.max_slope_deg * 3.14159265f / 180.0f); }
#define NAV_HEADROOM (g_nav.headroom) /* David is ~1.93 units tall (bind pose) */
#define NAV_MIN_UP   (g_nav.min_up)   /* 40 degrees max floor slope by default: stairs are modelled with flat treads, so they are unaffected; steeper inclined planes (boilarea's 42.7-degree ramp against the boiler, rock embankments) are not walkable */

/* All standable floor heights under (x,z), highest first. A floor is an
   UP-facing surface (glTF CCW winding -- a ceiling's underside or a
   roof's inner face points DOWN, which is exactly what made the old
   top-down probe report roofs/ceilings as "floor") with NAV_HEADROOM of
   free space above it (not under a table, not wedged under a slab).
   Winding is right for ~91% of surfaces (measured), so a column with no
   up-facing floor at all falls back to accepting horizontal surfaces of
   either orientation. Returns the layer count. */
/* User zones (see character.h): 0 none, NAV_ZONE_BLOCK (red), NAV_ZONE_FORCE (green). */
int (*g_nav_zone_at)(float x, float y, float z) = NULL;
static int nav_zone(float x, float y, float z) { return g_nav_zone_at ? g_nav_zone_at(x, y, z) : NAV_ZONE_NONE; }

static int floor_layers_at(float x, float z, const float *tris, int n, float *out_y, int max_layers) {
    ensure_bins(tris, n);
    if (!g_bins.start) return 0;
    float hy[64], hny[64]; int hup[64]; int nh = 0;
    float o[3] = { x, 1e4f, z }, d[3] = { 0, -1, 0 };
    FOR_BINNED_TRIS(x, x, z, z, t, {
        const float *v0 = tris + (size_t)t * 9, *v1 = v0 + 3, *v2 = v0 + 6;
        float tt;
        if (nh < 64 && ray_triangle_intersect(o, d, v0, v1, v2, &tt)) {
            float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
            float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
            float nn[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
            float len = sqrtf(nn[0]*nn[0]+nn[1]*nn[1]+nn[2]*nn[2]);
            if (len < 1e-8f) continue;
            float ny = nn[1] / len;
            hy[nh] = o[1] - tt;
            hup[nh] = ny >= NAV_MIN_UP ? 1 : (ny <= -NAV_MIN_UP ? -1 : 0);
            hny[nh] = fabsf(ny);
            nh++;
        }
    });
    int nl = 0;
    {
        /* candidates sorted high -> low */
        for (int i = 0; i < nh; i++) for (int j = i + 1; j < nh; j++)
            if (hy[j] > hy[i]) { float ty = hy[i]; hy[i] = hy[j]; hy[j] = ty; int tu = hup[i]; hup[i] = hup[j]; hup[j] = tu;
                                 float tn = hny[i]; hny[i] = hny[j]; hny[j] = tn; }
        for (int i = 0; i < nh && nl < max_layers; i++) {
            /* Either orientation: ~9% of surfaces are modelled upside
               down (measured), and ignoring them made real walkways
               unwalkable whenever another floor existed below them
               (chains/bigroom4). A slab's underside is still rejected by
               the headroom test just below (its own top is right above). */
            /* inside a user GREEN zone: steeper surfaces count as floor
               (up to ~78 deg) and much less headroom is required */
            int forced = nav_zone(x, hy[i], z) == NAV_ZONE_FORCE;
            int is_floor = forced ? hny[i] >= NAV_FORCED_MIN_UP : hup[i] != 0;
            if (!is_floor) continue;
            float y = hy[i];
            float headroom = forced ? NAV_FORCED_HEADROOM : NAV_HEADROOM;
            if (nl > 0 && fabsf(out_y[nl - 1] - y) < 0.05f) continue; /* coincident duplicate */
            int blocked = 0;
            for (int k = 0; k < nh; k++)
                if (hy[k] > y + 0.05f && hy[k] < y + headroom) { blocked = 1; break; }
            if (blocked) continue;
            out_y[nl++] = y;
        }
    }
    return nl;
}

int floor_below(float x, float z, float y_ref, float max_up, const float *tris, int n, float *out_y) {
    float ys[8];
    int nl = floor_layers_at(x, z, tris, n, ys, 8);
    for (int i = 0; i < nl; i++) {        /* highest first: first one not above the reach */
        if (ys[i] <= y_ref + max_up) { *out_y = ys[i]; return 1; }
    }
    return 0;
}

/* ---------------- David's body volume ----------------
   Vertical cylinder, radius NAV_BODY_RADIUS, tested only from
   NAV_BODY_Y0 above the feet up to NAV_BODY_Y1 (head). Anything lower
   than NAV_BODY_Y0 is a STEP, handled by the floor/step rules -- that's
   what keeps stairs usable (measured on the room meshes: stair risers
   are 0.2-0.6, rarely up to 0.8, while walls/ledges are > 0.9).
   Exact test: clip the triangle to the height band, then circle vs
   polygon in the XZ plane. */
#define NAV_BODY_RADIUS (g_nav.body_radius) /* shoulders ~0.55 wide (bind pose is a T-pose, 1.0 with arms out) */
#define NAV_FOOT_RADIUS (g_nav.foot_radius)   /* NAV_BODY_Y0/Y1: character.h */

static int clip_poly_y(const float (*in)[3], int n, float y, int keep_above, float (*out)[3]) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const float *a = in[i], *b = in[(i + 1) % n];
        int ia = keep_above ? a[1] >= y : a[1] <= y;
        int ib = keep_above ? b[1] >= y : b[1] <= y;
        if (ia) { out[m][0] = a[0]; out[m][1] = a[1]; out[m][2] = a[2]; m++; }
        if (ia != ib) {
            float t = (y - a[1]) / (b[1] - a[1]);
            out[m][0] = a[0] + (b[0] - a[0]) * t; out[m][1] = y; out[m][2] = a[2] + (b[2] - a[2]) * t; m++;
        }
    }
    return m;
}

/* 1 if the vertical cylinder (center cx,cz, radius r, heights y0..y1)
   intersects the triangle. */
static int cylinder_hits_triangle(float cx, float cz, float r, float y0, float y1,
                                  const float *v0, const float *v1, const float *v2) {
    float ymin = fminf(v0[1], fminf(v1[1], v2[1])), ymax = fmaxf(v0[1], fmaxf(v1[1], v2[1]));
    if (ymax < y0 || ymin > y1) return 0;
    float xmin = fminf(v0[0], fminf(v1[0], v2[0])), xmax = fmaxf(v0[0], fmaxf(v1[0], v2[0]));
    float zmin = fminf(v0[2], fminf(v1[2], v2[2])), zmax = fmaxf(v0[2], fmaxf(v1[2], v2[2]));
    if (xmax < cx - r || xmin > cx + r || zmax < cz - r || zmin > cz + r) return 0;
    float p0[3][3] = { {v0[0], v0[1], v0[2]}, {v1[0], v1[1], v1[2]}, {v2[0], v2[1], v2[2]} };
    float p1[8][3], p2[8][3];
    int n1 = clip_poly_y((const float (*)[3])p0, 3, y0, 1, p1);
    if (n1 < 1) return 0;
    int n2 = clip_poly_y((const float (*)[3])p1, n1, y1, 0, p2);
    if (n2 < 1) return 0;
    /* circle center inside the projected polygon? (sign-consistent test) */
    int pos = 0, neg = 0;
    for (int i = 0; i < n2; i++) {
        const float *a = p2[i], *b = p2[(i + 1) % n2];
        float cr = (b[0] - a[0]) * (cz - a[2]) - (b[2] - a[2]) * (cx - a[0]);
        if (cr > 1e-9f) pos++; else if (cr < -1e-9f) neg++;
    }
    if (n2 >= 3 && (pos == 0 || neg == 0)) return 1;
    /* else: any polygon edge (or degenerate point/segment) within r */
    for (int i = 0; i < n2; i++) {
        const float *a = p2[i], *b = p2[(i + 1) % n2];
        float ex = b[0] - a[0], ez = b[2] - a[2];
        float l2 = ex * ex + ez * ez;
        float t = l2 > 1e-12f ? ((cx - a[0]) * ex + (cz - a[2]) * ez) / l2 : 0.0f;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        float dx = a[0] + ex * t - cx, dz = a[2] + ez * t - cz;
        if (dx * dx + dz * dz < r * r) return 1;
    }
    return 0;
}

int body_clear_at(float x, float y, float z, float radius, const float *tris, int n) {
    ensure_bins(tris, n);
    if (!g_bins.start) return 1;
    int hit = 0;
    FOR_BINNED_TRIS(x - radius, x + radius, z - radius, z + radius, t, {
        if (hit) break;
        const float *v0 = tris + (size_t)t * 9;
        if (cylinder_hits_triangle(x, z, radius, y + NAV_BODY_Y0, y + NAV_BODY_Y1, v0, v0 + 3, v0 + 6)) hit = 1;
    });
    return !hit;
}

/* Floor under the whole footprint: every point of a NAV_FOOT_RADIUS
   circle has a standable floor within a step of the feet (a stair edge
   is fine, hanging over a void/ledge is not). */
static int footprint_ok(float x, float y, float z, const float *tris, int n) {
    for (int k = 0; k < 8; k++) {
        float a = 0.785398f * k;
        float fy;
        if (!floor_below(x + cosf(a) * NAV_FOOT_RADIUS, z + sinf(a) * NAV_FOOT_RADIUS, y, NAV_MAX_STEP, tris, n, &fy)) return 0;
        if (y - fy > NAV_MAX_STEP) return 0;
    }
    return 1;
}

int stand_ok(float x, float y, float z, const float *tris, int n) {
    int zone = nav_zone(x, y, z);
    if (zone == NAV_ZONE_BLOCK) return 0; /* user red zone: never */
    if (zone == NAV_ZONE_FORCE) return 1; /* user green zone: a floor surface is enough */
    return body_clear_at(x, y, z, NAV_BODY_RADIUS, tris, n) && footprint_ok(x, y, z, tris, n);
}

int segment_blocked_by_wall(const float from[3], const float to[3],
                             const float *triangles, int triangle_count,
                             float max_up_dot) {
    ensure_bins(triangles, triangle_count);
    if (!g_bins.start) return 0;
    /* two heights: knee (catches low walls/railings higher than a step)
       and chest (catches everything else). */
    static const float heights[2] = { 0.6f, 1.3f };
    for (int hi = 0; hi < 2; hi++) {
        float origin[3] = { from[0], from[1] + heights[hi], from[2] };
        float end[3]    = { to[0],   to[1] + heights[hi],   to[2] };
        float dir[3] = { end[0]-origin[0], end[1]-origin[1], end[2]-origin[2] };
        float len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        if (len < 1e-5f) return 0;
        dir[0] /= len; dir[1] /= len; dir[2] /= len;
        int blocked = 0;
        FOR_BINNED_TRIS(fminf(origin[0], end[0]), fmaxf(origin[0], end[0]),
                        fminf(origin[2], end[2]), fmaxf(origin[2], end[2]), t, {
            if (blocked) break;
            const float *v0 = triangles + (size_t)t * 9, *v1 = v0 + 3, *v2 = v0 + 6;
            float tt;
            if (!ray_triangle_intersect(origin, dir, v0, v1, v2, &tt)) continue;
            if (tt >= len) continue;
            float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
            float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
            float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
            float nlen = sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            if (nlen < 1e-8f) continue;
            if (fabsf(n[1]) / nlen > max_up_dot) continue; /* floor/ramp-like, not a wall */
            blocked = 1;
        });
        if (blocked) return 1;
    }
    return 0;
}

int path_is_walkable(const float from[3], const float to[3],
                      const float *triangles, int triangle_count,
                      float radius, float max_step_height, float floor_min_y) {
    (void)radius; /* the body volume (NAV_BODY_RADIUS) is always used now */
    float dx = to[0] - from[0], dz = to[2] - from[2];
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist < 1e-4f) return 1;
    /* sweep the body along the line: samples closer than the body
       radius, so consecutive cylinders overlap and nothing thinner than
       that can slip between two checks */
    const float STEP_LEN = 0.2f;
    int steps = (int)(dist / STEP_LEN) + 1;
    float prev_y = from[1];
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float sx = from[0] + dx * t, sz = from[2] + dz * t;
        float y;
        float step = nav_zone(sx, prev_y, sz) == NAV_ZONE_FORCE ? NAV_FORCED_STEP : max_step_height;
        if (!floor_below(sx, sz, prev_y, step, triangles, triangle_count, &y) || y < floor_min_y)
            return 0;                                      /* void/pit */
        if (fabsf(y - prev_y) > step) return 0; /* ledge/cliff */
        if (!stand_ok(sx, y, sz, triangles, triangle_count)) return 0; /* body would clip / feet would hang */
        prev_y = y;
    }
    /* must actually arrive at the requested floor, not a different layer */
    return fabsf(prev_y - to[1]) <= max_step_height;
}

/* ---------------- grid-based pathfinding (multi-layer) ---------------- */

void navgrid_build(NavGrid *grid, const float *triangles, int triangle_count, float cell_size, float floor_min_y) {
    memset(grid, 0, sizeof(*grid));
    if (triangle_count <= 0) return;
    ensure_bins(triangles, triangle_count);
    float min_x = 1e30f, max_x = -1e30f, min_z = 1e30f, max_z = -1e30f;
    for (int i = 0; i < triangle_count * 3; i++) {
        const float *v = triangles + i * 3;
        if (v[0] < min_x) min_x = v[0];
        if (v[0] > max_x) max_x = v[0];
        if (v[2] < min_z) min_z = v[2];
        if (v[2] > max_z) max_z = v[2];
    }
    int cols = (int)((max_x - min_x) / cell_size) + 2;
    int rows = (int)((max_z - min_z) / cell_size) + 2;
    const int MAX_CELLS = 60000;
    while (cols * rows > MAX_CELLS) { cell_size *= 1.2f; cols = (int)((max_x - min_x) / cell_size) + 2; rows = (int)((max_z - min_z) / cell_size) + 2; }
    grid->min_x = min_x; grid->min_z = min_z;
    grid->cols = cols; grid->rows = rows; grid->cell_size = cell_size;
    grid->walkable = (uint8_t *)calloc((size_t)cols * rows, sizeof(uint8_t));
    grid->floor_y = (float *)calloc((size_t)cols * rows * NAV_MAX_LAYERS, sizeof(float));
    if (!grid->walkable || !grid->floor_y) { navgrid_free(grid); return; }
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
        float ys[NAV_MAX_LAYERS];
        int nl = floor_layers_at(min_x + (c + 0.5f) * cell_size, min_z + (r + 0.5f) * cell_size,
                                 triangles, triangle_count, ys, NAV_MAX_LAYERS);
        int kept = 0;
        float cxw = min_x + (c + 0.5f) * cell_size, czw = min_z + (r + 0.5f) * cell_size;
        for (int i = 0; i < nl; i++) {
            if (ys[i] < floor_min_y) continue;
            if (!stand_ok(cxw, ys[i], czw, triangles, triangle_count)) continue; /* body would clip / feet hang */
            grid->floor_y[(size_t)(r * cols + c) * NAV_MAX_LAYERS + kept++] = ys[i];
        }
        grid->walkable[r * cols + c] = (uint8_t)kept; /* number of floor layers here */
    }
    navgrid_label_components(grid, triangles, triangle_count);
}

void navgrid_free(NavGrid *grid) {
    free(grid->walkable);
    free(grid->floor_y);
    free(grid->comp);
    free(grid->comp_size);
    grid->walkable = NULL;
    grid->floor_y = NULL;
    grid->comp = NULL;
    grid->comp_size = NULL;
    grid->comp_count = 0;
    grid->cols = grid->rows = 0;
}

static int navgrid_cell(const NavGrid *grid, float x, float z, int *out_c, int *out_r) {
    int c = (int)((x - grid->min_x) / grid->cell_size);
    int r = (int)((z - grid->min_z) / grid->cell_size);
    if (c < 0 || c >= grid->cols || r < 0 || r >= grid->rows) return 0;
    *out_c = c; *out_r = r;
    return 1;
}

/* Node = (cell, layer) whose floor is closest to y, searching the cell
   and then rings of radius up to 3 (a real point on real floor can sit
   in a cell whose snapped center just misses it). -1 if none. */
static int navgrid_nearest_node(const NavGrid *grid, float x, float y, float z, float max_dy) {
    int c0, r0;
    if (!navgrid_cell(grid, x, z, &c0, &r0)) return -1;
    for (int radius = 0; radius <= 3; radius++) {
        int best = -1; float best_d = max_dy;
        for (int dr = -radius; dr <= radius; dr++) for (int dc = -radius; dc <= radius; dc++) {
            if (abs(dr) != radius && abs(dc) != radius) continue;
            int c = c0 + dc, r = r0 + dr;
            if (c < 0 || c >= grid->cols || r < 0 || r >= grid->rows) continue;
            int cell = r * grid->cols + c;
            for (int l = 0; l < grid->walkable[cell]; l++) {
                float d = fabsf(grid->floor_y[(size_t)cell * NAV_MAX_LAYERS + l] - y);
                if (d < best_d) { best_d = d; best = cell * NAV_MAX_LAYERS + l; }
            }
        }
        if (best >= 0) return best;
    }
    return -1;
}

static void node_point(const NavGrid *grid, int node, float out[3]) {
    int cell = node / NAV_MAX_LAYERS;
    out[0] = grid->min_x + (cell % grid->cols + 0.5f) * grid->cell_size;
    out[1] = grid->floor_y[node];
    out[2] = grid->min_z + (cell / grid->cols + 0.5f) * grid->cell_size;
}

/* The ONE edge rule used by A* and by area labelling: floors within a
   step of each other, no corner cutting, no wall in between. */
static int nav_edge_ok(const NavGrid *grid, const float *tris, int n, int cur, int nb) {
    int cell = cur / NAV_MAX_LAYERS, bc = cell % grid->cols, br = cell / grid->cols;
    int ncell = nb / NAV_MAX_LAYERS, nc = ncell % grid->cols, nr = ncell / grid->cols;
    float cy = grid->floor_y[cur], ny = grid->floor_y[nb];
    float pa[3], pb[3];
    node_point(grid, cur, pa); node_point(grid, nb, pb);
    float lim = (nav_zone(pa[0], pa[1], pa[2]) == NAV_ZONE_FORCE || nav_zone(pb[0], pb[1], pb[2]) == NAV_ZONE_FORCE)
                ? NAV_FORCED_STEP : NAV_MAX_STEP;
    if (fabsf(ny - cy) > lim) return 0;
    /* (no orthogonal-neighbour corner rule any more: the body test at
       the edge midpoint below already catches corner clipping, and the
       rule broke narrow stairs running diagonally to the grid --
       verified: chains/bigroom4's spiral staircase) */
    (void)bc; (void)br; (void)nc; (void)nr;
    float a[3], b[3];
    node_point(grid, cur, a); node_point(grid, nb, b);
    float mx = (a[0] + b[0]) * 0.5f, mz = (a[2] + b[2]) * 0.5f, my;
    if (!floor_below(mx, mz, fmaxf(a[1], b[1]), lim, tris, n, &my)) return 0;
    if (fabsf(my - a[1]) > lim || fabsf(my - b[1]) > lim) return 0;
    return stand_ok(mx, my, mz, tris, n);
}

static void navgrid_label_components(NavGrid *grid, const float *tris, int n) {
    int nn = grid->cols * grid->rows * NAV_MAX_LAYERS;
    grid->comp = (int *)malloc(sizeof(int) * nn);
    int *q = (int *)malloc(sizeof(int) * nn);
    int *sizes = (int *)malloc(sizeof(int) * (nn + 1));
    if (!grid->comp || !q || !sizes) { free(q); free(sizes); free(grid->comp); grid->comp = NULL; return; }
    for (int i = 0; i < nn; i++) grid->comp[i] = -1;
    int ncomp = 0;
    for (int s = 0; s < nn; s++) {
        int cell = s / NAV_MAX_LAYERS, l = s % NAV_MAX_LAYERS;
        if (l >= grid->walkable[cell] || grid->comp[s] >= 0) continue;
        int head = 0, tail = 0, count = 0;
        grid->comp[s] = ncomp; q[tail++] = s;
        while (head < tail) {
            int cur = q[head++]; count++;
            int cc = cur / NAV_MAX_LAYERS, bc = cc % grid->cols, br = cc / grid->cols;
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
                if (!dr && !dc) continue;
                int nc = bc + dc, nr = br + dr;
                if (nc < 0 || nc >= grid->cols || nr < 0 || nr >= grid->rows) continue;
                int ncell = nr * grid->cols + nc;
                for (int k = 0; k < grid->walkable[ncell]; k++) {
                    int nb = ncell * NAV_MAX_LAYERS + k;
                    if (grid->comp[nb] >= 0) continue;
                    if (!nav_edge_ok(grid, tris, n, cur, nb)) continue;
                    grid->comp[nb] = ncomp; q[tail++] = nb;
                }
            }
        }
        sizes[ncomp++] = count;
    }
    grid->comp_size = (int *)malloc(sizeof(int) * (ncomp + 1));
    if (grid->comp_size) memcpy(grid->comp_size, sizes, sizeof(int) * ncomp);
    grid->comp_count = ncomp;
    free(q); free(sizes);
}

int navgrid_node_at(const NavGrid *grid, float x, float y, float z) {
    if (!grid->walkable) return -1;
    return navgrid_nearest_node(grid, x, y, z, NAV_MAX_STEP * 2);
}

int navgrid_snap(const NavGrid *grid, float x, float y, float z, float out[3]) {
    int nd = navgrid_node_at(grid, x, y, z);
    if (nd < 0) return 0;
    node_point(grid, nd, out);
    return 1;
}

int navgrid_area_size_at(const NavGrid *grid, float x, float y, float z) {
    int nd = navgrid_node_at(grid, x, y, z);
    if (nd < 0 || !grid->comp || !grid->comp_size || grid->comp[nd] < 0) return 0;
    return grid->comp_size[grid->comp[nd]];
}

int navgrid_nearest_reachable(const NavGrid *grid, const float from[3], const float target[3], float max_dist, float out[3]) {
    int s = navgrid_node_at(grid, from[0], from[1], from[2]);
    if (s < 0 || !grid->comp || grid->comp[s] < 0) return 0;
    int area = grid->comp[s], best = -1;
    float best_d2 = max_dist * max_dist;
    int nn = grid->cols * grid->rows * NAV_MAX_LAYERS;
    for (int i = 0; i < nn; i++) {
        if (grid->comp[i] != area) continue;
        float p[3]; node_point(grid, i, p);
        float dx = p[0] - target[0], dz = p[2] - target[2];
        /* small vertical penalty so a floor right under/over the target
           doesn't beat a slightly farther one at the right height */
        float d2 = dx * dx + dz * dz + 0.02f * (p[1] - target[1]) * (p[1] - target[1]); /* top-view distance; height only breaks ties (a click on something tall should still find the floor at its foot) */
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    if (best < 0) return 0;
    node_point(grid, best, out);
    return 1;
}

/* binary min-heap on f-score */
typedef struct { int node; float f; } HeapItem;
static void heap_push(HeapItem *h, int *n, int node, float f) {
    int i = (*n)++;
    h[i].node = node; h[i].f = f;
    while (i > 0) { int p = (i - 1) / 2; if (h[p].f <= h[i].f) break; HeapItem t = h[p]; h[p] = h[i]; h[i] = t; i = p; }
}
static HeapItem heap_pop(HeapItem *h, int *n) {
    HeapItem top = h[0];
    h[0] = h[--(*n)];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < *n && h[l].f < h[m].f) m = l;
        if (r < *n && h[r].f < h[m].f) m = r;
        if (m == i) break;
        HeapItem t = h[m]; h[m] = h[i]; h[i] = t; i = m;
    }
    return top;
}

int g_nav_last_fail = 0;
int navgrid_find_path(const NavGrid *grid, const float *triangles, int triangle_count,
                       const float start[3], const float goal[3],
                       float out_waypoints[][3], int max_waypoints, float floor_min_y) {
    g_nav_last_fail = 0;
    if (!grid->walkable || grid->cols <= 0 || grid->rows <= 0) { g_nav_last_fail = 4; return 0; }
    const float MAX_STEP = NAV_MAX_STEP;
    int s_node = navgrid_nearest_node(grid, start[0], start[1], start[2], MAX_STEP * 2);
    int g_node = navgrid_nearest_node(grid, goal[0], goal[1], goal[2], MAX_STEP * 2);
    if (s_node < 0) { g_nav_last_fail = 1; return 0; }
    if (g_node < 0) { g_nav_last_fail = 2; return 0; }
    int n = grid->cols * grid->rows * NAV_MAX_LAYERS;
    float *gscore = (float *)malloc(sizeof(float) * n);
    int *came_from = (int *)malloc(sizeof(int) * n);
    uint8_t *closed = (uint8_t *)calloc(n, 1);
    HeapItem *heap = (HeapItem *)malloc(sizeof(HeapItem) * ((size_t)n * 8 + 16));
    if (!gscore || !came_from || !closed || !heap) {
        free(gscore); free(came_from); free(closed); free(heap);
        return 0;
    }
    for (int i = 0; i < n; i++) { gscore[i] = 1e30f; came_from[i] = -1; }
    float gp[3]; node_point(grid, g_node, gp);
    int hn = 0;
    gscore[s_node] = 0.0f;
    heap_push(heap, &hn, s_node, 0.0f);
    int found = 0;
    float cs = grid->cell_size;
    while (hn > 0) {
        HeapItem it = heap_pop(heap, &hn);
        int cur = it.node;
        if (closed[cur]) continue;
        if (cur == g_node) { found = 1; break; }
        closed[cur] = 1;
        float cp[3]; node_point(grid, cur, cp);
        int cell = cur / NAV_MAX_LAYERS, bc = cell % grid->cols, br = cell / grid->cols;
        for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nc = bc + dc, nr = br + dr;
            if (nc < 0 || nc >= grid->cols || nr < 0 || nr >= grid->rows) continue;
            int ncell = nr * grid->cols + nc;
            for (int l = 0; l < grid->walkable[ncell]; l++) {
                int nb = ncell * NAV_MAX_LAYERS + l;
                if (closed[nb]) continue;
                float ny = grid->floor_y[nb];
                if (fabsf(ny - cp[1]) > MAX_STEP) continue; /* different level / ledge (cheap pre-check) */
                float step_cost = (dr != 0 && dc != 0) ? cs * 1.41421356f : cs;
                step_cost += fabsf(ny - cp[1]) * 2.0f;
                float tentative = gscore[cur] + step_cost;
                if (tentative >= gscore[nb]) continue;
                /* the grid alone can't see a thin wall between two
                   floor cells -- test the actual edge (lazily, only
                   for edges A* actually considers). */
                if (!nav_edge_ok(grid, triangles, triangle_count, cur, nb)) continue;
                float np_[3]; node_point(grid, nb, np_);
                gscore[nb] = tentative;
                came_from[nb] = cur;
                float hx = np_[0] - gp[0], hz = np_[2] - gp[2];
                if (hn < n * 8) heap_push(heap, &hn, nb, tentative + sqrtf(hx * hx + hz * hz));
            }
        }
    }
    int raw_count = 0;
    static int raw_nodes[8192];
    if (found) {
        int cur = g_node;
        while (cur != -1 && raw_count < 8192) {
            raw_nodes[raw_count++] = cur;
            if (cur == s_node) break;
            cur = came_from[cur];
        }
    }
    free(gscore); free(came_from); free(closed); free(heap);
    if (!found || raw_count == 0) { g_nav_last_fail = 3; return 0; }

    /* raw path: exact start, the A* cell centres, exact goal. The two
       end legs (start -> its cell centre, last centre -> goal) are the
       only ones A* didn't validate, so they're checked here: an exact
       endpoint that can't be reached in a straight line from its cell
       centre is dropped in favour of the centre itself. */
    static float raw_pts[8192 + 2][3];
    int raw_pt_count = 0;
    raw_pts[raw_pt_count][0] = start[0]; raw_pts[raw_pt_count][1] = start[1]; raw_pts[raw_pt_count][2] = start[2]; raw_pt_count++;
    for (int i = 0; i < raw_count; i++) node_point(grid, raw_nodes[raw_count - 1 - i], raw_pts[raw_pt_count++]);
    if (!path_is_walkable(raw_pts[0], raw_pts[1], triangles, triangle_count, 0.0f, MAX_STEP, floor_min_y)) {
        memmove(raw_pts[0], raw_pts[1], sizeof(float) * 3 * (size_t)(raw_pt_count - 1)); /* start from the centre */
        raw_pt_count--;
    }
    if (path_is_walkable(raw_pts[raw_pt_count - 1], goal, triangles, triangle_count, 0.0f, MAX_STEP, floor_min_y)) {
        raw_pts[raw_pt_count][0] = goal[0]; raw_pts[raw_pt_count][1] = goal[1]; raw_pts[raw_pt_count][2] = goal[2]; raw_pt_count++;
    }

    /* greedy string-pulling: skip ahead to the farthest later point
       reachable by a direct walkable line (bounded look-ahead). */
    int out_count = 0, idx = 0;
    out_waypoints[0][0] = raw_pts[0][0]; out_waypoints[0][1] = raw_pts[0][1]; out_waypoints[0][2] = raw_pts[0][2];
    out_count = 1;
    while (idx < raw_pt_count - 1 && out_count < max_waypoints) {
        int farthest = idx + 1;
        int limit = idx + 40 < raw_pt_count - 1 ? idx + 40 : raw_pt_count - 1;
        for (int j = limit; j > idx + 1; j--) {
            if (path_is_walkable(raw_pts[idx], raw_pts[j], triangles, triangle_count, 0.4f, MAX_STEP, floor_min_y)) {
                farthest = j;
                break;
            }
        }
        out_waypoints[out_count][0] = raw_pts[farthest][0];
        out_waypoints[out_count][1] = raw_pts[farthest][1];
        out_waypoints[out_count][2] = raw_pts[farthest][2];
        out_count++;
        idx = farthest;
    }
    return out_count;
}

/* ---------------- walkability ---------------- */

int walkable_point_in_hull(float x, float z, const float (*hull)[2], int hull_count) {
    int inside = 0;
    for (int i = 0, j = hull_count - 1; i < hull_count; j = i++) {
        float xi = hull[i][0], yi = hull[i][1];
        float xj = hull[j][0], yj = hull[j][1];
        if (((yi > z) != (yj > z)) &&
            (x < (xj - xi) * (z - yi) / (yj - yi + 1e-12f) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}
