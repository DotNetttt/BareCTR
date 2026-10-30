#ifndef ANVIL_H
#define ANVIL_H
#include <stdint.h>
#define ANVIL_SIGNS 32
typedef struct {
  uint8_t x, z;
  int16_t y;
  char text[4][256];
} AnvilSign;
typedef struct {
  int x, z, valid, activated;
  uint32_t used;
  uint16_t blocks[65536];
  uint8_t sky[32768], light[32768];
  uint16_t sections;
  int sign_count;
  AnvilSign signs[ANVIL_SIGNS];
} AnvilChunk;
extern int anvil_enabled;
extern int world_spawn_x, world_spawn_y, world_spawn_z;
extern char world_save_path[512];
/* Returns 1 for Anvil, 0 if absent, and -1 for an invalid world. */
int anvil_open(const char *directory);
void anvil_cache_configure(int count);
void anvil_pin(const AnvilChunk *chunk);
uint8_t anvil_physics(uint16_t legacy);
const AnvilChunk *anvil_chunk(int x, int z);
uint16_t anvil_state(uint16_t legacy);
uint16_t anvil_state_at(int x, int y, int z);
uint16_t anvil_legacy_at(int x, int y, int z);
uint16_t anvil_value_state(const AnvilChunk *chunk, int x, int y, int z, uint16_t value);
uint8_t anvil_block(int x, int y, int z);
uint16_t anvil_section_state(const AnvilChunk *chunk, int x, int y, int z);
#endif
