/* PNG (and any format Windows GDI+ reads) -> 32-bit pixels. */
#ifndef SILVER_IMAGE_H
#define SILVER_IMAGE_H

#include <stdint.h>

/* Loads `path` as top-down 0xAARRGGBB pixels (w*h, malloc'ed).
   Returns NULL if the file can't be read. */
uint32_t *image_load(const char *path, int *w, int *h);

#endif
