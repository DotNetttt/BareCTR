#ifndef CHUNKVIEW_H
#define CHUNKVIEW_H
/* Send the new view minus its overlap with the previous view. */
static inline int chunk_enters_view(int x, int z, int old_x, int old_z, int radius) {
  return x<old_x-radius || x>old_x+radius || z<old_z-radius || z>old_z+radius;
}
#endif
