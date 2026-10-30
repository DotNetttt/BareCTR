#ifndef CHUNKPACKET_H
#define CHUNKPACKET_H
#include <stddef.h>
#include <stdint.h>
/* The returned payload is valid until the next call. NULL indicates failure. */
const uint8_t *build_chunk_packet(int cx, int cz, size_t *length);
#endif
