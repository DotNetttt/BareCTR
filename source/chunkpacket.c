#include "chunkpacket.h"
#include "anvil.h"
#include "worldgen.h"
#include "globals.h"
#include "registries.h"
#include "blockstates.h"
#include "redstone.h"
#include <string.h>

static uint8_t packet[384*1024];
static uint16_t states[4096], palette[256], indices[4096];
static int16_t palette_index[65536];
static uint16_t touched_states[4096];
static unsigned touched_count;
static int palette_initialized;
typedef struct { size_t n; int error; } Writer;
static void bytes(Writer *w, const void *data, size_t n) {
  if (w->error || n>sizeof(packet)-w->n) { w->error=1; return; }
  memcpy(packet+w->n,data,n); w->n+=n;
}
static void byte(Writer *w, uint8_t v) { bytes(w,&v,1); }
static void short_be(Writer *w, uint16_t v) { byte(w,v>>8); byte(w,v); }
static void int_be(Writer *w, uint32_t v) { for (int i=3;i>=0;i--) byte(w,v>>(i*8)); }
static void long_be(Writer *w, uint64_t v) { for (int i=7;i>=0;i--) byte(w,v>>(i*8)); }
static void varint(Writer *w, uint32_t v) { do { byte(w,(v&127)|(v>127?128:0)); v>>=7; } while(v); }
static void text(Writer *w, const char *s) { size_t n=strlen(s); short_be(w,n); bytes(w,s,n); }
static void tag(Writer *w, int type, const char *name) { byte(w,type); text(w,name); }

static void section(Writer *w, uint8_t biome) {
  unsigned count=0, nonair=0;
  if (!palette_initialized) { memset(palette_index,255,sizeof(palette_index)); palette_initialized=1; }
  for (unsigned i=0;i<touched_count;i++) palette_index[touched_states[i]]=-1;
  touched_count=0;
  for (int i=0;i<4096;i++) {
    if (states[i]) nonair++;
    int index=palette_index[states[i]];
    if (index<0) {
      index=count++;
      palette_index[states[i]]=index;
      touched_states[touched_count++]=states[i];
      if (index<256) palette[index]=states[i];
    }
    indices[i]=index;
  }
  short_be(w,nonair);
  if (count==1) { byte(w,0); varint(w,palette[0]); }
  else {
    int bits=4;
    while ((1u<<bits)<count && bits<9) bits++;
    if (bits>8) bits=15;
    byte(w,bits);
    if (bits<=8) { varint(w,count); for (unsigned i=0;i<count;i++) varint(w,palette[i]); }
    int per_long=64/bits;
    /* Protocol 772 omits the packed-long array length. Entries never cross a long. */
    for (int i=0;i<4096;i+=per_long) {
      uint64_t value=0;
      for (int j=0;j<per_long && i+j<4096;j++) value|=(uint64_t)(bits==15?states[i+j]:indices[i+j])<<(bits*j);
      long_be(w,value);
    }
  }
  byte(w,0); varint(w,biome);
}
static void sign_text(Writer *w, const char *name, const AnvilSign *sign) {
  tag(w,10,name);
  tag(w,9,"messages"); byte(w,8); int_be(w,4);
  for (int i=0;i<4;i++) text(w,sign?sign->text[i]:"");
  tag(w,8,"color"); text(w,"black");
  tag(w,1,"has_glowing_text"); byte(w,0);
  byte(w,0);
}
static void lighting(Writer *w, const AnvilChunk *c) {
  uint32_t sky_mask=0, block_mask=0;
  uint8_t full[2048]; memset(full,255,sizeof(full));
  for (int s=0;s<26;s++) {
    const uint8_t *sky=NULL,*light=NULL;
    if (c && s>=5 && s<=20) { sky=c->sky+(s-5)*2048; light=c->light+(s-5)*2048; }
    else if (s>=5) sky=full;
    for (int i=0;i<2048;i++) {
      if (sky && sky[i]) sky_mask|=1u<<s;
      if (light && light[i]) block_mask|=1u<<s;
    }
  }
  uint32_t masks[]={sky_mask,block_mask,0x3ffffffu^sky_mask,0x3ffffffu^block_mask};
  for (int i=0;i<4;i++) { varint(w,1); long_be(w,masks[i]); }
  for (int kind=0;kind<2;kind++) {
    uint32_t mask=masks[kind]; unsigned count=0;
    for (int s=0;s<26;s++) if (mask&(1u<<s)) count++;
    varint(w,count);
    for (int s=0;s<26;s++) if (mask&(1u<<s)) {
      const uint8_t *data=full;
      if (c && s>=5 && s<=20) data=(kind?c->light:c->sky)+(s-5)*2048;
      varint(w,2048); bytes(w,data,2048);
    }
  }
}
const uint8_t *build_chunk_packet(int cx, int cz, size_t *length) {
  *length=0;
  const AnvilChunk *c=anvil_enabled?anvil_chunk(cx,cz):NULL;
  if (anvil_enabled && !c) return NULL;
  anvil_pin(c);
  Writer w={0}; byte(&w,0x27); int_be(&w,cx); int_be(&w,cz); varint(&w,0);
  size_t size_at=w.n; w.n+=3; size_t data_at=w.n;
  for (int sy=-4;sy<20;sy++) {
    uint8_t biome=W_plains;
    if ((c && sy<0) || sy>=16) {
      short_be(&w,0); byte(&w,0); varint(&w,0); byte(&w,0); varint(&w,biome);
      continue;
    }
    if (c) {
      for (int i=0;i<4096;i++) states[i]=anvil_section_state(c,i&15,sy*16+i/256,(i/16)&15);
      for (int i=0;i<state_edit_count;i++) {
        const StateEdit *s=&state_edits[i];
        if (!s->valid || s->x<cx*16 || s->x>=cx*16+16 || s->z<cz*16 || s->z>=cz*16+16 || s->y<sy*16 || s->y>=sy*16+16) continue;
        unsigned index=(s->x-cx*16)+(s->z-cz*16)*16+(s->y-sy*16)*256;
        states[index]=anvil_value_state(c,s->x-cx*16,s->y,s->z-cz*16,s->value);
      }
      for (int i=0;i<block_changes_count;i++) {
        const BlockChange *b=&block_changes[i];
        if (b->block==255) continue;
        if (b->x>=cx*16 && b->x<cx*16+16 && b->z>=cz*16 && b->z<cz*16+16 && b->y>=sy*16 && b->y<sy*16+16) {
          unsigned index=(b->x-cx*16)+(b->z-cz*16)*16+(b->y-sy*16)*256;
          states[index]=block_palette[b->block];
        }
        #ifdef ALLOW_CHESTS
        if (b->block==B_chest) i+=14;
        #endif
      }
    } else if (sy<0) {
      for (int i=0;i<4096;i++) states[i]=block_palette[B_bedrock];
    } else if (sy<16) {
      biome=buildChunkSection(cx*16,sy*16,cz*16);
      for (int i=0;i<4096;i++) states[i]=block_palette[chunk_section[i^7]];
    } else memset(states,0,sizeof(states));
    if(!c)for(int i=0;i<state_edit_count;i++) {
      const StateEdit *s=&state_edits[i];
      if(!s->valid || s->x<cx*16 || s->x>=cx*16+16 || s->z<cz*16 || s->z>=cz*16+16 || s->y<sy*16 || s->y>=sy*16+16)continue;
      unsigned index=(s->x-cx*16)+(s->z-cz*16)*16+(s->y-sy*16)*256;
      states[index]=(s->value>>4)==55?circuit_wire_state(s->x,s->y,s->z,s->value):anvil_state(s->value);
    }
    section(&w,biome);
  }
  if (w.error) { anvil_pin(NULL); return NULL; }
  size_t size=w.n-data_at;
  /* Use a legal fixed-width VarInt to avoid moving the section payload. */
  packet[size_at]=(size&127)|128; packet[size_at+1]=((size>>7)&127)|128; packet[size_at+2]=size>>14;
  varint(&w,c?c->sign_count:0);
  if (c) for (int i=0;i<c->sign_count;i++) {
    const AnvilSign *s=&c->signs[i];
    byte(&w,(s->x<<4)|s->z); short_be(&w,s->y); varint(&w,7);
    byte(&w,10); /* Anonymous network NBT compound. */
    sign_text(&w,"front_text",s); sign_text(&w,"back_text",NULL);
    tag(&w,1,"is_waxed"); byte(&w,0); byte(&w,0);
  }
  lighting(&w,c);
  anvil_pin(NULL);
  if (w.error) return NULL;
  *length=w.n; return packet;
}
