#include "chunkstream.h"
#include "runtime.h"
#include "transport.h"
#include "packets.h"
#include "procedures.h"
#include "tools.h"
#include "interactions.h"
#include <stdlib.h>
#include <string.h>
typedef struct { int active,fd,x,z; uint8_t sent[81]; } View;
static View views[MAX_PLAYERS];
static int cursor;
static int64_t last_chunk;
void chunk_stream_reset(PlayerData *p) {
  int slot=p-player_data;if(slot<0 || slot>=MAX_PLAYERS)return;
  memset(&views[slot],0,sizeof(View));views[slot].fd=p->client_fd;
}
void chunk_stream_request(PlayerData *p,int x,int z) {
  int slot=p-player_data;if(slot<0 || slot>=MAX_PLAYERS)return;
  View *v=&views[slot];int r=server_view_distance,w=r*2+1;uint8_t retained[81]={0};
  if(v->active && v->fd==p->client_fd) {
    for(int oz=-r;oz<=r;oz++)for(int ox=-r;ox<=r;ox++) {
      int old_x=x+ox-v->x,old_z=z+oz-v->z;
      if(abs(old_x)>r || abs(old_z)>r)continue;
      if(v->sent[(old_z+r)*w+old_x+r])retained[(oz+r)*w+ox+r]=1;
    }
  }
  v->active=1;v->fd=p->client_fd;v->x=x;v->z=z;memcpy(v->sent,retained,sizeof(retained));
  sc_setCenterChunk(p->client_fd,x,z);
}
void chunk_stream_tick(void) {
  int64_t now=get_program_time();if(now-last_chunk<server_chunk_interval_us)return;
  for(int visit=0;visit<MAX_PLAYERS;visit++) {
    int slot=cursor;cursor=(cursor+1)%MAX_PLAYERS;View *v=&views[slot];PlayerData *p=&player_data[slot];
    if(!v->active || p->client_fd<0 || v->fd!=p->client_fd)continue;
    if(transport_failed(v->fd) || transport_pending(v->fd)>8192)continue;
    int r=server_view_distance,w=2*r+1;
    for(int radius=0;radius<=r;radius++)for(int x=-radius;x<=radius;x++)for(int z=-radius;z<=radius;z++) {
      if(abs(x)!=radius && abs(z)!=radius)continue;
      unsigned bit=(z+r)*w+x+r;if(v->sent[bit])continue;
      if(sc_chunkDataAndUpdateLight(v->fd,v->x+x,v->z+z)<0) {v->active=0;return;}
      legacy_seed_chunk(v->x+x,v->z+z);
      v->sent[bit]=1;last_chunk=get_program_time();
      if(radius==0 && (p->flags&0x20))handlePlayerJoin(p);
      return;
    }
  }
}
