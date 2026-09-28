/* glTF 2.0 files as exported in the Silver Blockouts (.gltf + .bin next
   to it): the JSON tree plus its binary buffers, and accessor reading. */
#ifndef SILVER_GLTF_H
#define SILVER_GLTF_H

#include "json.h"
#include <stdint.h>

typedef struct {
    JsonDoc doc;
    const JsonValue *root;
    int buffer_count;
    uint8_t **buffers;
    size_t *buffer_sizes;
} Gltf;

int gltf_load(const char *path, Gltf *g); /* 1 on success */
void gltf_free(Gltf *g);

/* Accessor as floats: count x components (1..16). Normalized integer
   types are mapped to [0,1] / [-1,1]; other integers keep their value.
   Returns a malloc'ed array (count * components), NULL on error. */
float *gltf_read_floats(const Gltf *g, int accessor, int *count, int *components);
/* Accessor as unsigned ints (indices, joint ids). malloc'ed, NULL on error. */
uint32_t *gltf_read_uints(const Gltf *g, int accessor, int *count, int *components);

#endif
