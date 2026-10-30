#include "anvil.h"
#include "legacy_blocks.h"
#include "registries.h"
#include "blockstates.h"
#include "redstone.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <zlib.h>

#define NBT_LIMIT (1024 * 1024)
#define CACHE_COUNT 12
typedef struct { const uint8_t *p, *end; int error; } Reader;
typedef struct { int type; const uint8_t *data; size_t length; } Tag;
static uint8_t compressed[NBT_LIMIT], decoded[NBT_LIMIT];
static AnvilChunk *cache;
static const AnvilChunk *pinned;
void anvil_pin(const AnvilChunk *chunk) { pinned=chunk; }
static int cache_count=4;
void anvil_cache_configure(int count) { if (!cache && count>=4 && count<=CACHE_COUNT) cache_count=count; }
static uint32_t clock_count;
static char world_directory[384];
int anvil_enabled;
int world_spawn_x = 8, world_spawn_y = 80, world_spawn_z = 8;
char world_save_path[512] = "world.bin";

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static const uint8_t *take(Reader *r, size_t n) {
  if (r->error || n > (size_t)(r->end-r->p)) { r->error=1; return NULL; }
  const uint8_t *p=r->p; r->p+=n; return p;
}
static unsigned u8(Reader *r) { const uint8_t *p=take(r,1); return p ? *p : 0; }
static unsigned u16(Reader *r) { const uint8_t *p=take(r,2); return p ? (p[0]<<8)|p[1] : 0; }
static uint32_t u32(Reader *r) { const uint8_t *p=take(r,4); return p ? be32(p) : 0; }
static int skip(Reader *r, unsigned type, int depth) {
  if (depth>64) { r->error=1; return 0; }
  switch(type) {
    case 1: take(r,1); break;
    case 2: take(r,2); break;
    case 3: case 5: take(r,4); break;
    case 4: case 6: take(r,8); break;
    case 8: { unsigned n=u16(r); take(r,n); break; }
    case 7: case 11: case 12: {
      uint32_t n=u32(r), size=type==7?1:type==11?4:8;
      if (n>NBT_LIMIT/size) r->error=1; else take(r,n*size);
      break;
    }
    case 9: {
      unsigned t=u8(r); uint32_t n=u32(r);
      if (n>NBT_LIMIT || (n && !t)) { r->error=1; break; }
      for (uint32_t i=0;i<n && !r->error;i++) skip(r,t,depth+1);
      break;
    }
    case 10:
      while (!r->error) {
        unsigned t=u8(r); if (!t) break;
        unsigned n=u16(r); take(r,n); skip(r,t,depth+1);
      }
      break;
    default: r->error=1;
  }
  return !r->error;
}
static Tag find(Tag parent, const char *name) {
  Tag none={0};
  if (parent.type!=10) return none;
  Reader r={parent.data,parent.data+parent.length,0};
  while (!r.error) {
    unsigned t=u8(&r); if (!t) break;
    unsigned n=u16(&r); const uint8_t *key=take(&r,n), *start=r.p;
    if (!skip(&r,t,0)) break;
    if (key && strlen(name)==n && !memcmp(key,name,n)) return (Tag){t,start,(size_t)(r.p-start)};
  }
  return none;
}
static int integer(Tag parent, const char *name, int32_t *out) {
  Tag t=find(parent,name);
  if (t.type!=3 || t.length!=4) return 0;
  *out=(int32_t)be32(t.data); return 1;
}
static const uint8_t *array(Tag parent, const char *name, size_t expected) {
  Tag t=find(parent,name);
  if (t.type!=7 || t.length!=expected+4 || be32(t.data)!=expected) return NULL;
  return t.data+4;
}
static int string(Tag parent, const char *name, char *out, size_t cap) {
  Tag t=find(parent,name);
  if (t.type!=8 || t.length<2) return 0;
  size_t n=(t.data[0]<<8)|t.data[1];
  if (n>=cap || n+2!=t.length) return 0;
  memcpy(out,t.data+2,n); out[n]=0; return 1;
}
static int inflate_nbt(size_t length, int compression, Tag *root) {
  size_t size;
  if (compression==3) { memcpy(decoded,compressed,length); size=length; }
  else {
    z_stream stream={0};
    if (compression!=1 && compression!=2) return 0;
    stream.next_in=compressed; stream.avail_in=length;
    stream.next_out=decoded; stream.avail_out=sizeof(decoded);
    if (inflateInit2(&stream,compression==1?31:15)!=Z_OK) return 0;
    int result=inflate(&stream,Z_FINISH); size=stream.total_out;
    int complete=result==Z_STREAM_END && stream.avail_in==0;
    inflateEnd(&stream); if (!complete) return 0;
  }
  Reader r={decoded,decoded+size,0};
  if (u8(&r)!=10) return 0;
  unsigned n=u16(&r); take(&r,n); const uint8_t *start=r.p;
  if (!skip(&r,10,0) || r.p!=r.end) return 0;
  *root=(Tag){10,start,(size_t)(r.p-start)}; return 1;
}
static int floor16(int n) { return n>=0?n/16:(n-15)/16; }
static int floor32(int n) { return n>=0?n/32:(n-31)/32; }
static unsigned nibble(const uint8_t *p, unsigned i) { return (p[i/2]>>((i&1)*4))&15; }
static int spawn_clear(uint16_t value) {
  unsigned id=value>>4;
  return id==0 || id==8 || id==9 || id==31 || id==32 || id==37 || id==38 || id==39 || id==40 || id==50 || id==63 || id==65 || id==68 || id==75 || id==76;
}

int anvil_open(const char *directory) {
  anvil_enabled=0; clock_count=0;
  if (!cache) cache=calloc(cache_count,sizeof(*cache));
  if (!cache) return -1;
  memset(cache,0,cache_count*sizeof(*cache));
  world_spawn_x=8; world_spawn_y=80; world_spawn_z=8;
  strcpy(world_save_path,"world.bin");
  if (strlen(directory)>=sizeof(world_directory)) return -1;
  strcpy(world_directory,directory);
  char path[512]; snprintf(path,sizeof(path),"%s/level.dat",directory);
  FILE *f=fopen(path,"rb");
  if (!f) return errno==ENOENT?0:-1;
  size_t n=fread(compressed,1,sizeof(compressed),f);
  int bad=ferror(f) || !feof(f); fclose(f);
  Tag root;
  if (bad || !inflate_nbt(n,1,&root)) return -1;
  Tag data=find(root,"Data"); int32_t x,y,z,version;
  if (!integer(data,"version",&version) || version!=19133 ||
      !integer(data,"SpawnX",&x) || !integer(data,"SpawnY",&y) || !integer(data,"SpawnZ",&z) ||
      x<SHRT_MIN || x>SHRT_MAX || z<SHRT_MIN || z>SHRT_MAX || y<0 || y>255) return -1;
  world_spawn_x=x; world_spawn_y=y; world_spawn_z=z;
  anvil_enabled=1;
  snprintf(world_save_path,sizeof(world_save_path),"%s/barectr-world.bin",directory);
  const AnvilChunk *spawn=anvil_chunk(floor16(x),floor16(z));
  if (!spawn || !spawn->sections) { anvil_enabled=0; return -1; }
  /* Legacy SpawnY may identify the floor. Search locally, never the roof. */
  unsigned column=((unsigned)z&15)*16+((unsigned)x&15);
  int safe=0;
  for (int sy=y;sy<=y+8 && sy<255;sy++) {
    if (spawn_clear(spawn->blocks[sy*256+column]) && spawn_clear(spawn->blocks[(sy+1)*256+column])) {
      world_spawn_y=sy; safe=1; break;
    }
  }
  if (!safe) { anvil_enabled=0; return -1; }
  return 1;
}

static int load_chunk(AnvilChunk *c, int x, int z) {
  memset(c,0,sizeof(*c)); c->x=x; c->z=z;
  /* Absent chunks are empty, never procedural terrain. */
  memset(c->sky,255,sizeof(c->sky));
  char path[512]; snprintf(path,sizeof(path),"%s/region/r.%d.%d.mca",world_directory,floor32(x),floor32(z));
  FILE *f=fopen(path,"rb");
  if (!f) return errno==ENOENT;
  uint8_t loc[4], header[5];
  unsigned slot=((unsigned)x&31)+((unsigned)z&31)*32;
  if (fseek(f,slot*4,SEEK_SET) || fread(loc,1,4,f)!=4) { fclose(f); return 0; }
  unsigned sector=(loc[0]<<16)|(loc[1]<<8)|loc[2], sectors=loc[3];
  if (!sector && !sectors) { fclose(f); return 1; }
  if (sector<2 || !sectors || fseek(f,(long)sector*4096,SEEK_SET) || fread(header,1,5,f)!=5) { fclose(f); return 0; }
  uint32_t length=be32(header);
  if (length<2 || length>sectors*4096u-4 || length-1>sizeof(compressed) || fread(compressed,1,length-1,f)!=length-1) { fclose(f); return 0; }
  fclose(f); Tag root;
  if (!inflate_nbt(length-1,header[4],&root)) return 0;
  Tag level=find(root,"Level"), sections=find(level,"Sections");
  int32_t nx,nz;
  if (!integer(level,"xPos",&nx) || !integer(level,"zPos",&nz) || nx!=x || nz!=z || sections.type!=9) return 0;
  Reader r={sections.data,sections.data+sections.length,0};
  unsigned type=u8(&r); uint32_t count=u32(&r);
  if (count>16 || (count && type!=10)) return 0;
  for (unsigned s=0;s<count;s++) {
    const uint8_t *start=r.p; if (!skip(&r,10,0)) return 0;
    Tag section={10,start,(size_t)(r.p-start)}, y=find(section,"Y");
    if (y.type!=1 || y.data[0]>15) return 0;
    unsigned sy=y.data[0]; if (c->sections&(1u<<sy)) return 0;
    c->sections|=1u<<sy;
    const uint8_t *blocks=array(section,"Blocks",4096), *meta=array(section,"Data",2048);
    const uint8_t *add=array(section,"Add",2048), *sky=array(section,"SkyLight",2048), *light=array(section,"BlockLight",2048);
    if (!blocks || !meta || !sky || !light || (find(section,"Add").type && !add)) return 0;
    for (unsigned i=0;i<4096;i++) {
      unsigned id=blocks[i]+(add?nibble(add,i)*256:0);
      c->blocks[sy*4096+i]=(id<<4)|nibble(meta,i);
    }
    memcpy(c->sky+sy*2048,sky,2048); memcpy(c->light+sy*2048,light,2048);
  }
  Tag entities=find(level,"TileEntities");
  if (entities.type==9) {
    Reader e={entities.data,entities.data+entities.length,0};
    unsigned et=u8(&e); uint32_t n=u32(&e);
    if (n>65536 || (n && et!=10)) return 0;
    for (uint32_t i=0;i<n;i++) {
      const uint8_t *start=e.p; if (!skip(&e,10,0)) return 0;
      Tag entity={10,start,(size_t)(e.p-start)}; char id[64];
      if (!string(entity,"id",id,sizeof(id)) || strcmp(id,"Sign")) continue;
      if (c->sign_count==ANVIL_SIGNS) return 0;
      AnvilSign *sign=&c->signs[c->sign_count++]; int32_t sx,sy,sz;
      if (!integer(entity,"x",&sx) || !integer(entity,"y",&sy) || !integer(entity,"z",&sz) || floor16(sx)!=x || floor16(sz)!=z || sy<0 || sy>255) return 0;
      sign->x=(unsigned)sx&15; sign->z=(unsigned)sz&15; sign->y=sy;
      for (int line=0;line<4;line++) { char key[8]; snprintf(key,sizeof(key),"Text%d",line+1); if (!string(entity,key,sign->text[line],256)) return 0; }
    }
  }
  return 1;
}

const AnvilChunk *anvil_chunk(int x, int z) {
  AnvilChunk *oldest=NULL;
  for (int i=0;i<cache_count;i++) {
    if (cache[i].valid && cache[i].x==x && cache[i].z==z) { cache[i].used=++clock_count; return &cache[i]; }
    if (&cache[i]!=pinned && (!oldest || !cache[i].valid || cache[i].used<oldest->used)) oldest=&cache[i];
  }
  if (!load_chunk(oldest,x,z)) { oldest->valid=0; printf("Invalid Anvil chunk: %d, %d\n",x,z); return NULL; }
  oldest->valid=1; oldest->used=++clock_count; return oldest;
}
uint16_t anvil_state(uint16_t legacy) { return legacy<4096?legacy_states[legacy]:legacy_states[166*16]; }
uint8_t anvil_physics(uint16_t legacy) { return legacy<4096?legacy_physics[legacy]:B_barrier; }
uint16_t anvil_state_at(int x, int y, int z) {
  if (y<0 || y>255) return 0;
  const AnvilChunk *c=anvil_chunk(floor16(x),floor16(z));
  if (!c) return legacy_states[166*16];
  uint16_t value=c->blocks[y*256+((unsigned)z&15)*16+((unsigned)x&15)];
  state_edit_get(x,y,z,&value);
  return anvil_value_state(c,(unsigned)x&15,y,(unsigned)z&15,value);
}
uint16_t anvil_legacy_at(int x,int y,int z) {
  if (y<0 || y>255) return 0;
  uint16_t value;if(state_edit_get(x,y,z,&value))return value;
  const AnvilChunk *c=anvil_chunk(floor16(x),floor16(z));
  return c?c->blocks[y*256+((unsigned)z&15)*16+((unsigned)x&15)]:166*16;
}
uint8_t anvil_block(int x, int y, int z) {
  if (y<0 || y>255) return B_air;
  const AnvilChunk *c=anvil_chunk(floor16(x),floor16(z));
  if (!c) return B_barrier;
  uint16_t value=c->blocks[y*256+((unsigned)z&15)*16+((unsigned)x&15)];
  state_edit_get(x,y,z,&value);
  return value<4096?legacy_physics[value]:B_barrier;
}
uint16_t anvil_section_state(const AnvilChunk *c, int x, int y, int z) {
  if (y<0 || y>255) return 0;
  return anvil_value_state(c,x,y,z,c->blocks[y*256+z*16+x]);
}
uint16_t anvil_value_state(const AnvilChunk *c,int x,int y,int z,uint16_t value) {
  unsigned id=value>>4, meta=value&15;
  if(id==55)return circuit_wire_state(c->x*16+x,y,c->z*16+z,value);
  /* Door orientation lives in the lower half; hinge/power live in the upper half. */
  if (id==64 || id==71) {
    int lower_y=(meta&8)?y-1:y, upper_y=lower_y+1;
    if (lower_y>=0 && upper_y<256) {
      uint16_t lower=c->blocks[lower_y*256+z*16+x], upper=c->blocks[upper_y*256+z*16+x];
      state_edit_get(c->x*16+x,lower_y,c->z*16+z,&lower);
      state_edit_get(c->x*16+x,upper_y,c->z*16+z,&upper);
      if ((lower>>4)==id && (upper>>4)==id) {
        uint16_t base=anvil_state((id<<4)|(lower&7));
        /* State order: facing, half, hinge, open, powered. */
        unsigned start=id==64?4686:5828;
        unsigned offset=base-start;
        offset=(offset&~15u)|((meta&8)?0:8)|((upper&1)?4:0)|((lower&4)?0:2)|((upper&2)?0:1);
        return start+offset;
      }
    }
  }
  return anvil_state(value);
}
