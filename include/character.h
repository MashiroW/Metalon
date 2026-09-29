/* David and the room's 3D side: matrix/quaternion math, glTF skinning,
   keyframe animation, the per-room camera, collision and pathfinding
   against the room's geometry. David's model and animations are the blockouts'
   glTF exports (assets/chars). */
#ifndef SILVER_CHARACTER_H
#define SILVER_CHARACTER_H

#include <stdint.h>

typedef struct { float m[16]; } Mat4; /* column-major, glTF convention */

Mat4 mat4_identity(void);
Mat4 mat4_mul(Mat4 a, Mat4 b);
Mat4 mat4_from_trs(const float t[3], const float q[4], const float s[3]);
void mat4_vec3(const Mat4 *m, const float v[3], float out[3]);
void quat_nlerp(const float a[4], const float b[4], float t, float out[4]);

/* --- David, loaded at startup from the blockouts' glTF (assets/chars/david):
   mesh (primitive 0 of mesh 0), skin, skeleton rest pose and texture. --- */
#define DAVID_MAX_VERTS 8192 /* any character: the biggest (apocalyp) has 4365 */
#define DAVID_MAX_NODES 64
#define DAVID_MAX_JOINTS 64
typedef struct {
    char name[48];
    int vertex_count, index_count, node_count, joint_count;
    float (*positions)[3];
    float (*uvs_px)[2];          /* texture coordinates in texels */
    uint8_t (*joints_idx)[4];
    float (*weights)[4];
    uint32_t *indices;
    float node_t[DAVID_MAX_NODES][3], node_r[DAVID_MAX_NODES][4], node_s[DAVID_MAX_NODES][3];
    int node_parent[DAVID_MAX_NODES];
    int joints[DAVID_MAX_JOINTS];
    float inv_bind[DAVID_MAX_JOINTS][16]; /* glTF column-major */
    int tex_w, tex_h;
    uint8_t *tex_rgb;            /* tex_w * tex_h * 3 */
} CharModel;
typedef CharModel DavidModel;
extern DavidModel g_david;
/* Any character of assets/chars: <dir>/<name>.gltf, every primitive of
   its mesh, its texture. 1 on success (0: no skinned mesh). */
int char_model_load(CharModel *m, const char *dir, const char *name);
void char_model_free(CharModel *m);
/* Loads <dir>/david.gltf into g_david. 1 on success. */
int david_load(const char *dir);
#define DAVID_VERTEX_COUNT (g_david.vertex_count)
#define DAVID_INDEX_COUNT (g_david.index_count)
#define DAVID_NODE_COUNT (g_david.node_count)
#define DAVID_JOINT_COUNT (g_david.joint_count)
#define DAVID_TEX_W (g_david.tex_w)
#define DAVID_TEX_H (g_david.tex_h)
#define DAVID_TEXEL(v, u) (g_david.tex_rgb + ((size_t)(v) * g_david.tex_w + (size_t)(u)) * 3)
#define david_positions (g_david.positions)
#define david_uvs_px (g_david.uvs_px)
#define david_joints_idx (g_david.joints_idx)
#define david_weights (g_david.weights)
#define david_indices (g_david.indices)
#define david_node_translation (g_david.node_t)
#define david_node_rotation (g_david.node_r)
#define david_node_scale (g_david.node_s)
#define david_node_parent (g_david.node_parent)
#define david_joints (g_david.joints)
#define david_inv_bind (g_david.inv_bind)

/* --- Skeleton: computes DAVID_JOINT_COUNT skin matrices from a set of
   per-node TRS overrides (NULL entries mean "use the node's own base
   TRS", i.e. bind pose for that node). --- */
typedef struct {
    int has_t, has_r, has_s;
    float t[3], r[4], s[3];
} NodeOverride;

void skeleton_compute_skin_matrices(const NodeOverride *overrides /* [DAVID_NODE_COUNT] */,
                                     Mat4 *out_skin_mats /* [DAVID_JOINT_COUNT] */);
/* same, for any character */
void skeleton_skin_matrices_for(const CharModel *m, const NodeOverride *overrides, Mat4 *out_skin_mats);
/* a bone's model-space matrix in the pose the last call above computed
   (e.g. the hand holding a weapon); 0 if the model has no such bone */
int skeleton_node_global(int node, Mat4 *out);

/* --- Animation library: the .gltf clips of chars/anims, chars/david and
   (added on demand) any character's own folder, each clip loaded the
   first time it's used. A clip plays on a character whose skeleton has
   the same number of nodes as the clip's file. --- */
int anim_lib_init(const char *anims_dir, const char *david_dir); /* returns files indexed */
int anim_lib_add_dir(const char *dir, const char *source);       /* appends a folder's clips; returns how many */
int anim_lib_nodes(int i);                                       /* the clip's skeleton node count (loads it), -1 unreadable */
int anim_lib_count(void);
const char *anim_lib_name(int i);
const char *anim_lib_source(int i); /* "anims" or the character's folder name */
int anim_lib_find(const char *name); /* index or -1 */
int anim_lib_ready(int i);           /* loads it if needed; 1 = usable on David */
int anim_lib_fits(int i, int node_count); /* 1 = usable on a skeleton of node_count nodes */
float anim_lib_duration(int i);
int anim_lib_channels(int i);
int anim_lib_keys(int i);
void anim_lib_sample(int i, float t, NodeOverride *overrides); /* on David; all zero if unusable */
/* on any character: the clip applied as offsets from the rest pose of the
   skeleton it was made on (keeps the character's own proportions) */
void anim_lib_sample_for(int i, float t, const CharModel *m, NodeOverride *overrides);
/* David's own cycles: stand / walk / run from the library */
void anim_sample_idle(float t, NodeOverride *overrides);
void anim_sample_walk(float t, NodeOverride *overrides);
void anim_sample_run(float t, NodeOverride *overrides);

/* --- Camera: real per-room perspective projection + its inverse. --- */
typedef struct {
    float translation[3];
    float R[9];     /* view -> world (camera basis), row-major 3x3 */
    float Rinv[9];  /* world -> view (transpose of R) */
    float yfov;
    /* Pixel intrinsics measured against the room's original depth mask
       (focal > 0), else 0 = legacy "yfov spans image height" convention. */
    float focal, cx, cy;
} RoomCamera;

void camera_init(RoomCamera *cam, const float translation[3], const float rotation[4], float yfov);
/* Builds a ROLL-FREE variant of `src` -- same position, same forward
   (look) direction and FOV (so depth/perspective/framing still match
   the room), but with "up"/"right" rebuilt from world-up via a
   standard look-at basis instead of keeping src's own (possibly
   rolled) up vector. Some rooms' pre-rendered-background camera has a
   deliberate dramatic roll/dutch angle (verified case: rain/aftastep,
   whose camera's own local "up" points mostly toward world -Y) -- a
   pre-rendered PHOTO absorbs that fine, but a live 3D character
   projected through the same rolled camera renders visibly tilted/
   upside-down. Classic pre-rendered-background adventure games always
   kept their (separately composited) character sprites screen-upright
   regardless of the background camera's own framing, for exactly this
   reason -- this reproduces that generically: a no-op for the (large
   majority of) rooms whose camera has negligible roll, a real
   correction only where the room's own camera data has one. Use for
   character-mesh projection ONLY; the room mesh/photo/raycasting all
   keep using the real, unmodified camera. */
void camera_make_upright(const RoomCamera *src, RoomCamera *out);
/* Returns 1 and fills (px,py) if the point is in front of the camera, else 0. */
int camera_world_to_pixel(const RoomCamera *cam, const float world[3], int img_w, int img_h, float *px, float *py);
/* Same, plus camera-space depth (positive, larger = farther) for a
   Z-buffer -- used by the raw 3D debug view (room mesh + David sharing
   one depth buffer for correct mutual occlusion). */
int camera_world_to_pixel_z(const RoomCamera *cam, const float world[3], int img_w, int img_h,
                             float *px, float *py, float *out_depth);
/* Casts a ray through screen pixel (px,py) and intersects it with the
   horizontal plane y=floor_y. Returns 1 and fills world[3] on success. */
int camera_pixel_to_floor_point(const RoomCamera *cam, float px, float py, int img_w, int img_h,
                                 float floor_y, float world[3]);
/* Same screen->world mapping, but returns the ray itself (origin =
   camera position, dir = normalized world-space direction) instead of
   intersecting a flat plane -- for real ray-MESH intersection against
   the room's actual geometry (see raycast_room_mesh), not an infinite
   plane. */
void camera_pixel_to_ray(const RoomCamera *cam, float px, float py, int img_w, int img_h,
                          float out_origin[3], float out_dir[3]);

/* --- Walkability: point-in-polygon test against the room's floor hull
   (a coarse footprint from the room's geometry; click-to-move uses
   raycast_room_mesh below). --- */
int walkable_point_in_hull(float x, float z, const float (*hull)[2], int hull_count);

/* --- Real ray-mesh collision against a room's actual environment
   geometry (the room glTF's mesh 0: walls, floor, props).
   Triangles are a flat array of 9 floats each (v0.xyz,v1.xyz,v2.xyz).
   Finds the CLOSEST triangle the ray hits (Möller-Trumbore) and, if
   `floor_only` is set, only accepts hits whose triangle normal is
   close enough to "up" (dot(normal,(0,1,0)) >= min_up_dot) -- so
   clicking a wall or a prop's side doesn't move the character there,
   only real floor does. Returns 1 and fills world[3]/out_normal[3] on
   a valid hit, 0 otherwise (ray hits nothing, or only non-floor
   geometry). */
int raycast_room_mesh(const float ray_origin[3], const float ray_dir[3],
                       const float *triangles, int triangle_count,
                       int floor_only, float min_up_dot,
                       float out_point[3], float out_normal[3]);
/* Same, for rays cast FROM THE CAMERA: prefers front-facing triangles
   (backface culling with a fallback) -- see character.c. */
int raycast_room_mesh_view(const float ray_origin[3], const float ray_dir[3],
                            const float *triangles, int triangle_count,
                            int floor_only, float min_up_dot,
                            float out_point[3], float out_normal[3]);

/* Checks the straight 3D line from `from` to `to` (each offset up by
   ~waist height internally) against the room mesh's WALL-like triangles
   only (normal.y <= max_up_dot, i.e. steep/vertical surfaces) -- floor
   and ramp triangles are deliberately excluded so David can walk up a
   staircase/ramp without the ramp's own surface counting as a wall.
   Returns 1 if some wall geometry sits between the two points (the move
   should be rejected/clamped), 0 if the straight path is clear. This
   is what stops David walking through a wall separating two otherwise-
   valid floor points -- raycast_room_mesh alone only validates the
   destination, not the path to it. */
int segment_blocked_by_wall(const float from[3], const float to[3],
                             const float *triangles, int triangle_count,
                             float max_up_dot);

/* Walks the straight XZ line from `from` to `to` in short steps,
   re-sampling the REAL floor height at each step via a downward
   raycast (handles ramps/stairs naturally, since each step's floor
   height is queried fresh rather than assumed from a single line
   through 3D space) and rejects the path if any step:
   - has no floor beneath it at all (a gap/pit -- David would fall), or
   - jumps more than max_step_height from the previous step's floor
     (a ledge/cliff, not a walkable ramp), or
   - has a wall triangle between it and the previous step at chest
     height, offset by +-radius perpendicular to the direction of
     travel (a coarse capsule/hitbox approximation instead of a single
     infinitely-thin line, so David doesn't clip a corner he's brushing
     past).
   `floor_min_y` rejects any downward-probe hit below it as "no floor"
   -- mesh0's winding isn't reliable enough to tell a true floor from a
   downward-facing backface by normal direction alone (verified: the
   level's own outer shell bottom, tens of units below the real floor,
   can get raycast-classified as floor when probing straight down
   through the whole mesh depth, something a normal camera-to-floor
   click never does but this path/grid probing does), so distance from
   a known-good floor reference is what actually separates them. Pass
   the room's real floor_y minus a few units of slack. Returns 1 if the
   whole path is walkable, 0 otherwise. */
int path_is_walkable(const float from[3], const float to[3],
                      const float *triangles, int triangle_count,
                      float radius, float max_step_height, float floor_min_y);

/* ---------------- grid-based pathfinding ---------------- */

/* A walkability grid over a room's mesh XZ footprint, built once per
   room load. Each cell stores up to NAV_MAX_LAYERS standable floor
   heights (bridges over lower floors, mezzanines, multi-storey rooms):
   up-facing surfaces with NAV headroom above them -- see
   floor_layers_at in character.c. A* runs over (cell, layer) nodes,
   only between floors within NAV_MAX_STEP of each other, and every
   edge it uses is checked for a wall in between. */
#define NAV_MAX_LAYERS 4
/* David's movement limits -- runtime settings (scene editor: "David &
   navigation settings", saved in data/rooms/<level>/<room>_nav.cfg). The
   defaults are the values everything was tuned/verified with. After a
   change call nav_params_changed(), then rebuild the room's NavGrid. */
typedef struct {
    float body_radius;   /* hitbox cylinder radius (0.3; shoulders ~0.55 wide) */
    float max_step;      /* max floor height change per step (0.6; stair risers 0.2-0.6, David ~1.93 tall) */
    float max_slope_deg; /* steepest walkable floor (40 deg) */
    float foot_radius;   /* every point this far around the feet needs floor within a step (0.2) */
    float headroom;      /* free height needed above a floor (1.7) */
    float forced_step;   /* max step inside GREEN zones (1.5) */
    float min_up;        /* derived: cos(max_slope_deg) */
} NavParams;
extern NavParams g_nav;
void nav_params_changed(void);
#define NAV_DEFAULT_PARAMS { 0.3f, 0.6f, 39.997f /* = acos(0.766), the tuned value */, 0.2f, 1.7f, 1.5f, 0.766f }
#define NAV_MAX_STEP (g_nav.max_step)
typedef struct {
    float min_x, min_z;
    int cols, rows;
    float cell_size;
    uint8_t *walkable;   /* cols*rows: number of floor layers in the cell (0 = none) */
    float *floor_y;      /* cols*rows*NAV_MAX_LAYERS floor heights, highest first */
    int *comp;           /* per node: connected-area id (same edge rules as A*), -1 = no floor */
    int *comp_size;      /* per area id: node count */
    int comp_count;
} NavGrid;

/* Node (cell*NAV_MAX_LAYERS+layer) nearest to a world point, or -1. */
int navgrid_node_at(const NavGrid *grid, float x, float y, float z);
/* Size (node count) of the connected area containing the node nearest
   to a world point, 0 if none. */
int navgrid_area_size_at(const NavGrid *grid, float x, float y, float z);
/* Nearest valid (standable) grid node point to a world point. */
int navgrid_snap(const NavGrid *grid, float x, float y, float z, float out[3]);
/* Closest floor point (XZ distance, within max_dist) to `target` that
   lies in the same connected area as `from`. Returns 1 and writes out. */
int navgrid_nearest_reachable(const NavGrid *grid, const float from[3], const float target[3], float max_dist, float out[3]);

/* Highest standable floor at (x,z) that is at most `max_up` above
   y_ref (i.e. the floor someone standing at height y_ref would be on).
   Returns 1 and writes *out_y if found. */
int floor_below(float x, float z, float y_ref, float max_up, const float *tris, int n, float *out_y);

/* Must be called when the room mesh buffer is replaced (spatial bins). */
void trimesh_invalidate_cache(void);

/* David's body volume (see character.c): 1 if a `radius` cylinder
   from NAV_BODY_Y0 above the feet to the head touches no triangle. */
#define NAV_BODY_Y0 (g_nav.max_step + 0.1f) /* just above the max step: anything lower is a step */
#define NAV_BODY_Y1 1.9f
int body_clear_at(float x, float y, float z, float radius, const float *tris, int n);
/* body clear AND floor under the whole footprint within a step */
int stand_ok(float x, float y, float z, const float *tris, int n);
/* Optional hook (set by the game): user zones drawn in the editor, per
   world point. RED = David may never stand/walk there; GREEN = forced
   walkable: any surface up to ~78 deg counts as floor, steps up to
   NAV_FORCED_STEP, little headroom needed, no body/footprint checks.
   Red wins where both overlap. Used by stand_ok, the floor detection, the
   grid edges and path checks, so everything (grid, paths, clicks, spawn)
   obeys it. */
#define NAV_ZONE_NONE 0
#define NAV_ZONE_BLOCK 1
#define NAV_ZONE_FORCE 2
#define NAV_FORCED_STEP (g_nav.forced_step)
#define NAV_FORCED_MIN_UP 0.2f
#define NAV_FORCED_HEADROOM 0.6f
extern int (*g_nav_zone_at)(float x, float y, float z);

/* why the last navgrid_find_path returned 0: 1 no start node, 2 no goal
   node, 3 goal not connected, 4 no grid */
extern int g_nav_last_fail;

void navgrid_build(NavGrid *grid, const float *triangles, int triangle_count, float cell_size, float floor_min_y);
void navgrid_free(NavGrid *grid);



/* A* over the grid from `start` to `goal` (world space), then a greedy
   string-pulling pass (skip ahead to the farthest waypoint reachable
   by a direct path_is_walkable line) to turn the raw cell-by-cell path
   into a short, natural-looking waypoint list. Returns the waypoint
   count written to out_waypoints (each a world-space [x,y,z], y from
   the grid's recorded floor height), 0 if no path exists (goal is in
   an unreachable pocket of the room -- expected/normal, not an error). */
int navgrid_find_path(const NavGrid *grid, const float *triangles, int triangle_count,
                       const float start[3], const float goal[3],
                       float out_waypoints[][3], int max_waypoints, float floor_min_y);

#endif
