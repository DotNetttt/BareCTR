#include "blockstates.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
StateEdit state_edits[STATE_LIMIT];
int state_edit_count;
static int16_t slots[STATE_LIMIT*2];
static int initialized,dirty;
static char save_path[512];
static unsigned hash(int x,int y,int z) {return ((uint32_t)x*73856093u^(uint32_t)y*19349663u^(uint32_t)z*83492791u)&(STATE_LIMIT*2-1);}
static int locate(int x,int y,int z,int create) {
  if(!initialized){memset(slots,255,sizeof(slots));initialized=1;}
  unsigned h=hash(x,y,z);
  for(unsigned n=0;n<STATE_LIMIT*2;n++,h=(h+1)&(STATE_LIMIT*2-1)) {
    int index=slots[h];
    if(index<0) {
      if(!create || state_edit_count==STATE_LIMIT)return -1;
      index=state_edit_count++;slots[h]=index;state_edits[index]=(StateEdit){.x=x,.y=y,.z=z};return index;
    }
    StateEdit *s=&state_edits[index];if(s->x==x && s->y==y && s->z==z)return index;
  }
  return -1;
}
int state_edit_get(int x,int y,int z,uint16_t *value) {
  if(y<0 || y>255)return 0;
  int i=locate(x,y,z,0);if(i<0 || !state_edits[i].valid)return 0;*value=state_edits[i].value;return 1;
}
int state_edit_set(int x,int y,int z,uint16_t value) {
  if(x<-32768 || x>32767 || z<-32768 || z>32767 || y<0 || y>255)return -1;
  int i=locate(x,y,z,1);if(i<0)return -1;
  if(state_edits[i].valid && state_edits[i].value==value)return 0;
  state_edits[i].value=value;state_edits[i].valid=1;dirty=1;return 0;
}
int state_edit_reserve(int x,int y,int z) {
  if(x<-32768 || x>32767 || z<-32768 || z>32767 || y<0 || y>255)return -1;
  return locate(x,y,z,1)<0?-1:0;
}
void state_edit_clear(int x,int y,int z) {int i=locate(x,y,z,0);if(i>=0 && state_edits[i].valid){state_edits[i].valid=0;dirty=1;}}
int state_edits_open(const char *path) {
  if(strlen(path)>=sizeof(save_path))return -1;
  strcpy(save_path,path);memset(slots,255,sizeof(slots));memset(state_edits,0,sizeof(state_edits));state_edit_count=0;initialized=1;dirty=0;
  FILE *f=fopen(path,"rb");if(!f)return errno==ENOENT?0:-1;
  char magic[8];if(fread(magic,1,8,f)!=8 || memcmp(magic,"BCTS0001",8)){fclose(f);return -1;}
  uint8_t b[8];size_t n;
  while((n=fread(b,1,8,f))==8) {
    if(b[7]!=1 || state_edit_set((int16_t)(b[0]|b[1]<<8),b[4],(int16_t)(b[2]|b[3]<<8),b[5]|b[6]<<8)) {fclose(f);return -1;}
  }
  int bad=n!=0 || ferror(f);fclose(f);dirty=0;return bad?-1:0;
}
int state_edits_flush(void) {
  if(!dirty || !save_path[0])return 0;
  char temporary[520];snprintf(temporary,sizeof(temporary),"%s.tmp",save_path);
  FILE *f=fopen(temporary,"wb");if(!f)return -1;
  int bad=fwrite("BCTS0001",1,8,f)!=8;
  for(int i=0;i<state_edit_count && !bad;i++) {
    StateEdit *s=&state_edits[i];if(!s->valid)continue;
    uint8_t b[8]={s->x,s->x>>8,s->z,s->z>>8,s->y,s->value,s->value>>8,1};
    if(fwrite(b,1,8,f)!=8)bad=1;
  }
  if(fclose(f))bad=1;
  if(bad)return -1;
  if(rename(temporary,save_path))return -1;
  dirty=0;return 0;
}
