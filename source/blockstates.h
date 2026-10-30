#ifndef BLOCKSTATES_H
#define BLOCKSTATES_H
#include <stdint.h>
#define STATE_LIMIT 32768
typedef struct { int16_t x,z; uint16_t value; uint8_t y,valid; } StateEdit;
extern StateEdit state_edits[STATE_LIMIT];
extern int state_edit_count;
int state_edits_open(const char *path);
int state_edits_flush(void);
int state_edit_get(int x,int y,int z,uint16_t *value);
int state_edit_set(int x,int y,int z,uint16_t value);
int state_edit_reserve(int x,int y,int z);
void state_edit_clear(int x,int y,int z);
#endif
