#ifndef INTERACTIONS_H
#define INTERACTIONS_H
#include "globals.h"
#include "redstone.h"
void legacy_init(void);
uint16_t legacy_get(int x,int y,int z);
uint16_t legacy_render(int x,int y,int z);
int legacy_write(const CircuitChange *changes,int count);
void legacy_seed_chunk(int cx,int cz);
int legacy_place(PlayerData *player,int x,int y,int z,int face,uint16_t item);
void legacy_pressure_tick(void);
int legacy_interact(PlayerData *player,int x,int y,int z);
void legacy_break_pair(int x,int y,int z);
void legacy_refresh_power(int x,int y,int z);
#endif
