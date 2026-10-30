#ifndef REDSTONE_H
#define REDSTONE_H
#include <stdint.h>
typedef struct { int x,y,z; uint16_t value; } CircuitChange;
typedef uint16_t (*CircuitRead)(int,int,int);
typedef int (*CircuitWrite)(const CircuitChange *,int);
typedef void (*CircuitPush)(int,int,int,int,int,int,int);
void circuit_init(CircuitRead read,CircuitWrite write,CircuitPush push);
void circuit_notify(int x,int y,int z);
void circuit_seed(int x,int y,int z);
int circuit_step(uint32_t tick,int budget);
int circuit_pending(void);
unsigned circuit_overflows(void);
int circuit_component(unsigned id);
int circuit_solid(uint16_t value);
int circuit_use(int x,int y,int z);
uint16_t circuit_wire_state(int x,int y,int z,uint16_t value);
#endif
