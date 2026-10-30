#include "interactions.h"
#include "anvil.h"
#include "blockstates.h"
#include "procedures.h"
#include "packets.h"
#include "registries.h"
#include "tools.h"
#include "worldgen.h"
#include "varnum.h"
#include "commands.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static uint16_t reverse_blocks[256];
static uint8_t reverse_ready[256];
static uint16_t from_internal(uint8_t block) {
  if(!reverse_ready[block]) {
    reverse_blocks[block]=166*16;
    for(unsigned v=0;v<4096;v++)if(anvil_state(v)==block_palette[block]){reverse_blocks[block]=v;break;}
    reverse_ready[block]=1;
  }
  return reverse_blocks[block];
}
uint16_t legacy_get(int x,int y,int z) {
  if(y<0 || y>255 || x< -32768 || x>32767 || z< -32768 || z>32767)return 7*16;
  uint8_t change=getBlockChange(x,y,z);
  if(change!=255)return from_internal(change);
  uint16_t value;if(state_edit_get(x,y,z,&value))return value;
  return anvil_enabled?anvil_legacy_at(x,y,z):from_internal(getBlockAt(x,y,z));
}
uint16_t legacy_render(int x,int y,int z) {
  uint16_t v=legacy_get(x,y,z);unsigned id=v>>4;
  if(id==55)return circuit_wire_state(x,y,z,v);
  if(id==64 || id==71) {
    int low=y-((v&8)?1:0);uint16_t lower=legacy_get(x,low,z),upper=legacy_get(x,low+1,z);
    if((lower>>4)==id && (upper>>4)==id) {
      unsigned start=id==64?4686:5828,offset=anvil_state((id<<4)|(lower&7))-start;
      offset=(offset&~15u)|((v&8)?0:8)|((upper&1)?4:0)|((lower&4)?0:2)|((upper&2)?0:1);
      return start+offset;
    }
  }
  return anvil_state(v);
}
static void broadcast_state(int x,int y,int z) {
  uint16_t state=legacy_render(x,y,z);
  for(int i=0;i<MAX_PLAYERS;i++) {
    int fd=player_data[i].client_fd;if(fd<0)continue;
    if(abs(div_floor(player_data[i].x,16)-div_floor(x,16))>5 || abs(div_floor(player_data[i].z,16)-div_floor(z,16))>5)continue;
    writeVarInt(fd,9+sizeVarInt(state));writeByte(fd,0x08);
    writeUint64(fd,((uint64_t)(x&0x3ffffff)<<38)|((uint64_t)(z&0x3ffffff)<<12)|(y&0xfff));writeVarInt(fd,state);
  }
}
int legacy_write(const CircuitChange *changes,int count) {
  /* Reserve the whole transaction before moving blocks or changing door halves. */
  for(int i=0;i<count;i++)if(state_edit_reserve(changes[i].x,changes[i].y,changes[i].z))return -1;
  for(int i=0;i<count;i++) {
    const CircuitChange *s=&changes[i];state_edit_set(s->x,s->y,s->z,s->value);
    for(int j=0;j<block_changes_count;j++) {
      BlockChange *b=&block_changes[j];if(b->block==255)continue;
      if(b->block==B_chest){j+=14;continue;}
      if(b->x==s->x && b->y==s->y && b->z==s->z)b->block=255;
    }
  }
  for(int i=0;i<count;i++) {
    const CircuitChange *s=&changes[i];broadcast_state(s->x,s->y,s->z);
    /* Dust shapes change even when its signal strength remains zero. */
    for(int dx=-1;dx<=1;dx++)for(int dz=-1;dz<=1;dz++)if(abs(dx)+abs(dz)==1)
      for(int dy=-1;dy<=1;dy++)if((legacy_get(s->x+dx,s->y+dy,s->z+dz)>>4)==55)broadcast_state(s->x+dx,s->y+dy,s->z+dz);
  }
  return 0;
}
static void push_players(int x,int y,int z,int dx,int dy,int dz,int length) {
  for(int i=0;i<MAX_PLAYERS;i++) {
    PlayerData *p=&player_data[i];if(p->client_fd<0 || player_mode(p)==3)continue;
    for(int n=0;n<length;n++)if(p->x==x+dx*n && p->z==z+dz*n && (p->y==y+dy*n || p->y+1==y+dy*n)) {
      int nx=x+dx*length,ny=y+dy*length,nz=z+dz*length;
      if(ny<0 || ny>254 || nx< -32768 || nx>32767 || nz< -32768 || nz>32767)break;
      if(circuit_solid(legacy_get(nx,ny,nz)) || circuit_solid(legacy_get(nx,ny+1,nz)))break;
      command_teleport(p,nx+0.5,ny,nz+0.5);break;
    }
  }
}
void legacy_init(void) { circuit_init(legacy_get,legacy_write,push_players); }
void legacy_refresh_power(int x,int y,int z) {
  circuit_notify(x,y,z);
  for(int dx=-1;dx<=1;dx++)for(int dz=-1;dz<=1;dz++)if(abs(dx)+abs(dz)==1)
    for(int dy=-1;dy<=1;dy++)if((legacy_get(x+dx,y+dy,z+dz)>>4)==55)broadcast_state(x+dx,y+dy,z+dz);
}
int legacy_interact(PlayerData *p,int x,int y,int z) {
  if((p->flags&4) || player_mode(p)==3)return 0;
  return circuit_use(x,y,z);
}
void legacy_break_pair(int x,int y,int z) {
  uint16_t v=legacy_get(x,y,z);unsigned id=v>>4;
  if(id==64 || id==71) {
    int other=y+((v&8)?-1:1);
    if(other>=0 && other<=255 && (legacy_get(x,other,z)>>4)==id)makeBlockChange(x,other,z,B_air);
  }
  if(id==29 || id==33 || id==34) {
    static const int d[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
    unsigned f=v&7;if(f>5)return;
    int sign=id==34?-1:1,nx=x+d[f][0]*sign,ny=y+d[f][1]*sign,nz=z+d[f][2]*sign;
    unsigned other=legacy_get(nx,ny,nz)>>4;
    if((id==34 && (other==29 || other==33)) || (id!=34 && (v&8) && other==34))makeBlockChange(nx,ny,nz,B_air);
  }
}
void legacy_seed_chunk(int cx,int cz) {
  if(!anvil_enabled)return;
  const AnvilChunk *c=anvil_chunk(cx,cz);if(!c || c->activated)return;
  ((AnvilChunk *)c)->activated=1;
  for(int i=0;i<65536;i++) {
    unsigned id=c->blocks[i]>>4;
    if(circuit_component(id) && id!=55 && id!=64 && id!=71)circuit_seed(cx*16+(i&15),i/256,cz*16+((i/16)&15));
  }
}
int legacy_place(PlayerData *p,int x,int y,int z,int face,uint16_t item) {
  unsigned id=0,meta=0;
  int quarter=((int)floor(p->yaw*180.0/127.0/90.0+0.5)+4)&3;
  static const int piston_facing[4]={2,5,3,4};
  switch(item) {
    case I_redstone:id=55;break;
    case I_lever:id=69;meta=face==1?5:face==0?0:face==2?4:face==3?3:face==4?2:1;break;
    case I_stone_button:id=77;meta=face==1?5:face==0?0:face==2?4:face==3?3:face==4?2:1;break;
    case I_redstone_torch:id=76;meta=face==1?5:face==2?4:face==3?3:face==4?2:face==5?1:0;break;
    case I_ladder:id=65;if(face<2)return -1;meta=face;break;
    case I_repeater:id=93;meta=(quarter+2)&3;break;
    case I_stone_pressure_plate:id=70;break;
    case I_oak_pressure_plate:id=72;break;
    case I_piston:case I_sticky_piston:id=item==I_piston?33:29;meta=piston_facing[quarter];if(p->pitch>64)meta=1;else if(p->pitch< -64)meta=0;break;
    case I_oak_door:case I_iron_door:id=item==I_oak_door?64:71;meta=(quarter+1)&3;break;
    case I_oak_trapdoor:id=96;meta=face==2?0:face==3?1:face==4?2:3;break;
    case I_oak_fence_gate:id=107;meta=quarter;break;
    default:return 0;
  }
  if(y<0 || y>255 || (id==64 || id==71 ? y>=255:0))return -1;
  if(!isReplaceableBlock(getBlockAt(x,y,z)))return -1;
  if((id==55 || id==93 || id==70 || id==72 || id==64 || id==71) && !circuit_solid(legacy_get(x,y-1,z)))return -1;
  if(id==65 || id==69 || id==77 || id==76) {
    static const int d[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
    if(face<0 || face>5 || !circuit_solid(legacy_get(x-d[face][0],y-d[face][1],z-d[face][2])))return -1;
  }
  CircuitChange changes[2]={{x,y,z,(id<<4)|meta},{x,y+1,z,(id<<4)|8}};int n=1;
  if(id==64 || id==71) {if(!isReplaceableBlock(getBlockAt(x,y+1,z)))return -1;n=2;}
  if(legacy_write(changes,n))return -1;
  for(int i=0;i<n;i++)circuit_notify(changes[i].x,changes[i].y,changes[i].z);
  return 1;
}
void legacy_pressure_tick(void) {
  static CircuitChange previous[MAX_PLAYERS];static int previous_count;
  CircuitChange current[MAX_PLAYERS];int n=0;
  for(int i=0;i<MAX_PLAYERS;i++) {
    PlayerData *p=&player_data[i];if(p->client_fd<0 || player_mode(p)==3)continue;
    uint16_t v=legacy_get(p->x,p->y,p->z);unsigned id=v>>4;
    if(id!=70 && id!=72)continue;
    int duplicate=0;for(int j=0;j<n;j++)if(current[j].x==p->x && current[j].y==p->y && current[j].z==p->z)duplicate=1;
    if(!duplicate)current[n++]=(CircuitChange){p->x,p->y,p->z,v|1};
    if(!(v&1)){CircuitChange c={p->x,p->y,p->z,v|1};legacy_write(&c,1);circuit_notify(p->x,p->y,p->z);}
  }
  for(int i=0;i<previous_count;i++) {
    CircuitChange *c=&previous[i];int present=0;
    for(int j=0;j<n;j++)if(current[j].x==c->x && current[j].y==c->y && current[j].z==c->z)present=1;
    uint16_t v=legacy_get(c->x,c->y,c->z);
    if(!present && ((v>>4)==70 || (v>>4)==72)){c->value=v&~1u;legacy_write(c,1);circuit_notify(c->x,c->y,c->z);}
  }
  memcpy(previous,current,n*sizeof(*current));previous_count=n;
}
