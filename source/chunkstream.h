#ifndef CHUNKSTREAM_H
#define CHUNKSTREAM_H
#include "globals.h"
void chunk_stream_reset(PlayerData *player);
void chunk_stream_request(PlayerData *player,int x,int z);
void chunk_stream_tick(void);
#endif
