#include "commands.h"
#include "interactions.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include "dashboard.h"

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#include "globals.h"
#include "tools.h"
#include "varnum.h"
#include "packets.h"
#include "worldgen.h"
#include "registries.h"
#include "procedures.h"
#include "serialize.h"
#include "anvil.h"
#include "chunkview.h"
#include "chunkstream.h"
#include "runtime.h"
#include "transport.h"
#include "blockstates.h"

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000
static u32 *soc_sharedmem = NULL;
// ----------------------------------------------

/**
 * Routes an incoming packet to its packet handler or procedure.
 */
void handlePacket (int client_fd, int length, int packet_id, int state) {

  // Count the amount of bytes received to catch length discrepancies
  uint64_t bytes_received_start = total_bytes_received;

  switch (packet_id) {

    case 0x00:
      if (state == STATE_NONE) {
        if (cs_handshake(client_fd)) break;
      } else if (state == STATE_STATUS) {
        if (sc_statusResponse(client_fd)) break;
      } if (state == STATE_LOGIN) {
        uint8_t uuid[16];
        char name[16];
        if (cs_loginStart(client_fd, uuid, name)) break;
        if (reservePlayerData(client_fd, uuid, name)) {
          recv_count = 0;
          return;
        }
        if (transport_compress(client_fd)) break;
        if (sc_loginSuccess(client_fd, uuid, name)) break;
      } else if (state == STATE_CONFIGURATION) {
        if (cs_clientInformation(client_fd)) break;
        if (sc_knownPacks(client_fd)) break;
        if (sc_registries(client_fd)) break;

        #ifdef SEND_BRAND
        if (sc_sendPluginMessage(client_fd, "minecraft:brand", (uint8_t *)brand, brand_len)) break;
        #endif
      }
      break;

    case 0x01:
      if (state == STATE_STATUS) {
        writeByte(client_fd, 9);
        writeByte(client_fd, 0x01);
        writeUint64(client_fd, readUint64(client_fd));
        recv_count = 0;
        return;
      }
      break;

    case 0x02:
      if (state == STATE_CONFIGURATION) cs_pluginMessage(client_fd);
      break;

    case 0x03:
      if (state == STATE_LOGIN) {
        setClientState(client_fd, STATE_CONFIGURATION);
      } else if (state == STATE_CONFIGURATION) {
        setClientState(client_fd, STATE_PLAY);
        sc_loginPlay(client_fd);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;
        spawnPlayer(player);

        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          if (player_data[i].flags & 0x20) continue;
          sc_playerInfoUpdateAddPlayer(client_fd, player_data[i]);
          sc_spawnEntityPlayer(client_fd, player_data[i]);
        }

        uint8_t uuid[16];
        uint32_t r = fast_rand();
        memcpy(uuid, &r, 4);
        for (int i = 0; i < MAX_MOBS; i ++) {
          if (mob_data[i].type == 0) continue;
          if ((mob_data[i].data & 31) == 0) continue;
          memcpy(uuid + 4, &i, 4);
          sc_spawnEntity(
            client_fd, -2 - i, uuid,
            mob_data[i].type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
            0, 0
          );
          broadcastMobMetadata(client_fd, -2 - i);
        }
      }
      break;

    case 0x07:
      if (state == STATE_CONFIGURATION) {
        sc_finishConfiguration(client_fd);
      }
      break;

    case 0x08:
      if (state == STATE_PLAY) cs_chat(client_fd);
      break;

    case 0x0B:
      if (state == STATE_PLAY) cs_clientStatus(client_fd);
      break;

    case 0x0C: // Client tick (ignored)
      break;

    case 0x11:
      if (state == STATE_PLAY) cs_clickContainer(client_fd);
      break;

    case 0x12:
      if (state == STATE_PLAY) cs_closeContainer(client_fd);
      break;

    case 0x1B:
      if (state == STATE_PLAY) discard_all(client_fd, length, false);
      break;

    case 0x19:
      if (state == STATE_PLAY) { PlayerData *p; if (!getPlayerData(client_fd, &p) && player_mode(p) != 3) cs_interact(client_fd); }
      break;

    case 0x1D:
    case 0x1E:
    case 0x1F:
    case 0x20:
      if (state == STATE_PLAY) {

        double x, y, z;
        float yaw, pitch;
        uint8_t on_ground;

        if (packet_id == 0x1D) cs_setPlayerPosition(client_fd, &x, &y, &z, &on_ground);
        else if (packet_id == 0x1F) cs_setPlayerRotation (client_fd, &yaw, &pitch, &on_ground);
        else if (packet_id == 0x20) cs_setPlayerMovementFlags (client_fd, &on_ground);
        else cs_setPlayerPositionAndRotation(client_fd, &x, &y, &z, &yaw, &pitch, &on_ground);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        int feet_x = player->x, feet_y = player->y, feet_z = player->z;
        if (packet_id == 0x1D || packet_id == 0x1E) {
          if (!isfinite(x) || !isfinite(y) || !isfinite(z) || x < -32768 || x >= 32768 || z < -32768 || z >= 32768) break;
          feet_x = (int)floor(x);
          feet_y = y < 0 ? 0 : y > 255 ? 255 : (int)y;
          feet_z = (int)floor(z);
        }
        uint8_t block_feet = getBlockAt(feet_x, feet_y, feet_z);
        uint8_t swimming = block_feet >= B_water && block_feet < B_water + 8;

        if (player_is_flying(player)) player->grounded_y = feet_y;
        if (on_ground) {
          int16_t damage = player->grounded_y - feet_y - 3;
          if (damage > 0 && (player_mode(player) == 0 || player_mode(player) == 2) && !swimming) {
            hurtEntity(client_fd, -1, D_fall, damage);
          }
          player->grounded_y = feet_y;
        } else if (swimming || block_feet == B_ladder) {
          player->grounded_y = feet_y;
        }

        if (packet_id == 0x20) break;

        if (packet_id != 0x1D) {
          player->yaw = ((short)(yaw + 540) % 360 - 180) * 127 / 180;
          player->pitch = pitch / 90.0f * 127.0f;
        }

        uint8_t should_broadcast = true;

        #ifndef BROADCAST_ALL_MOVEMENT
          should_broadcast = !(player->flags & 0x40);
          if (should_broadcast) player->flags |= 0x40;
        #endif

        if (should_broadcast) {
          if (packet_id == 0x1D) {
            yaw = player->yaw * 180 / 127;
            pitch = player->pitch * 90 / 127;
          }
          for (int i = 0; i < MAX_PLAYERS; i ++) {
            if (player_data[i].client_fd == -1) continue;
            if (player_data[i].flags & 0x20) continue;
            if (player_data[i].client_fd == client_fd) continue;
            if (packet_id == 0x1F) {
              sc_updateEntityRotation(player_data[i].client_fd, client_fd, player->yaw, player->pitch);
            } else {
              sc_teleportEntity(player_data[i].client_fd, client_fd, x, y, z, yaw, pitch);
            }
            sc_setHeadRotation(player_data[i].client_fd, client_fd, player->yaw);
          }
        }

        if (packet_id == 0x1F) break;

        if (player_mode(player) == 1 || player_mode(player) == 3) {
          player->hunger = 20;
        } else if (player->saturation == 0) {
          if (player->hunger > 0) player->hunger--;
          player->saturation = 200;
          sc_setHealth(client_fd, player->health, player->hunger, player->saturation);
        } else if (player->flags & 0x08) {
          player->saturation -= 1;
        }

        short cx = (short)floor(x), cz = (short)floor(z);
        int cy = y < 0 ? -1 : y > 255 ? 256 : (int)y;
        short _x = div_floor(cx, 16), _z = div_floor(cz, 16);
        short old_x = div_floor(player->x, 16), old_z = div_floor(player->z, 16);
        short dx = _x - old_x, dz = _z - old_z;

        if (cy < 0) {
          cy = 0;
          player->grounded_y = 0;
          sc_synchronizePlayerPosition(client_fd, cx, 0, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        } else if (cy > 255) {
          cy = 255;
          sc_synchronizePlayerPosition(client_fd, cx, 255, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        }

        player->x = cx;
        player->y = cy;
        player->z = cz;

        if (dx == 0 && dz == 0) break;

        uint32_t r = fast_rand();
        if (!anvil_enabled && (r & 3) == 0) {
          short mob_x = (_x + dx * VIEW_DISTANCE) * 16 + ((r >> 4) & 15);
          short mob_z = (_z + dz * VIEW_DISTANCE) * 16 + ((r >> 8) & 15);
          uint8_t mob_y = cy - 8;
          uint8_t b_low = getBlockAt(mob_x, mob_y - 1, mob_z);
          uint8_t b_mid = getBlockAt(mob_x, mob_y, mob_z);
          uint8_t b_top = getBlockAt(mob_x, mob_y + 1, mob_z);
          while (mob_y < 255) {
            if (!isPassableBlock(b_low) && isPassableSpawnBlock(b_mid) && isPassableSpawnBlock(b_top)) break;
            b_low = b_mid;
            b_mid = b_top;
            b_top = getBlockAt(mob_x, mob_y + 2, mob_z);
            mob_y ++;
          }
          if (mob_y != 255) {
            if ((world_time < 13000 || world_time > 23460) && mob_y > 48) {
              uint32_t mob_choice = (r >> 12) & 3;
              if (mob_choice == 0) spawnMob(25, mob_x, mob_y, mob_z, 4);
              else if (mob_choice == 1) spawnMob(28, mob_x, mob_y, mob_z, 10);
              else if (mob_choice == 2) spawnMob(95, mob_x, mob_y, mob_z, 10);
              else if (mob_choice == 3) spawnMob(106, mob_x, mob_y, mob_z, 8);
            } else {
              spawnMob(145, mob_x, mob_y, mob_z, 20);
            }
          }
        }

        chunk_stream_request(player, _x, _z);
      }
      break;

    case 0x37:
      if (state == STATE_PLAY) command_creative_slot(client_fd);
      break;
    case 0x27:
      if (state == STATE_PLAY) {
        PlayerData *p; uint8_t flags = readByte(client_fd);
        if (!getPlayerData(client_fd, &p)) command_flight(p, (flags & 2) != 0);
      }
      break;
    case 0x29:
      if (state == STATE_PLAY) cs_playerCommand(client_fd);
      break;

    case 0x2A:
      if (state == STATE_PLAY) cs_playerInput(client_fd);
      break;

    case 0x2B:
      if (state == STATE_PLAY) cs_playerLoaded(client_fd);
      break;

    case 0x34:
      if (state == STATE_PLAY) cs_setHeldItem(client_fd);
      break;
  
    case 0x3C:
      if (state == STATE_PLAY) cs_swingArm(client_fd);
      break;

    case 0x28:
      if (state == STATE_PLAY) cs_playerAction(client_fd);
      break;

    case 0x3F:
      if (state == STATE_PLAY) cs_useItemOn(client_fd);
      break;

    case 0x40:
      if (state == STATE_PLAY) cs_useItem(client_fd);
      break;

    default:
      discard_all(client_fd, length, false);
      break;
  }

  int processed_length = total_bytes_received - bytes_received_start;
  if (processed_length == length) return;

  if (length > processed_length) {
    discard_all(client_fd, length - processed_length, false);
  }
}

int main () {
  gfxInitDefault();
  dashboard_init();

  bool server_ready = false;

  bool is_new_3ds = false;
  APT_CheckNew3DS(&is_new_3ds);
  osSetSpeedupEnable(is_new_3ds);
  runtime_configure(is_new_3ds);

  DashboardStats initial_stats = {0};

  initial_stats.max_players = server_max_clients;
  initial_stats.view_distance = server_view_distance;

  dashboard_render(&initial_stats);

  gfxFlushBuffers();
  gfxSwapBuffers();
  
  printf("%s profile: view %d, clients %d\n", is_new_3ds ? "New 3DS" : "Old 3DS", server_view_distance, server_max_clients);
  // -------------------------------------

  printf("--- Serveur Bareiron 3DS ---\n\n");

  soc_sharedmem = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
  if(soc_sharedmem == NULL) {
      printf("Erreur: Impossible d'allouer la memoire reseau.\n");
      goto wait_and_exit;
  }
  Result ret = socInit(soc_sharedmem, SOC_BUFFERSIZE);
  if(R_FAILED(ret)) {
      printf("Erreur socInit: %08lx\n", ret);
      goto wait_and_exit;
  }

  world_seed = splitmix64(world_seed);
  rng_seed = splitmix64(rng_seed);

  for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
    block_changes[i].block = 0xFF;
  }

  #ifdef __3DS__
  if (mkdir("sdmc:/3ds", 0777) && errno != EEXIST) goto wait_and_exit;
  if (mkdir("sdmc:/3ds/BareCTR", 0777) && errno != EEXIST) goto wait_and_exit;
  if (chdir("sdmc:/3ds/BareCTR")) goto wait_and_exit;
  #endif
  int imported = anvil_open("world");
  if (imported < 0) { printf("Invalid world/level.dat or spawn chunk.\n"); goto wait_and_exit; }
  if (imported) printf("Anvil spawn: %d, %d, %d\n", world_spawn_x, world_spawn_y, world_spawn_z);
  else printf("No world/level.dat: using generated terrain.\n");
  if (initSerializer()) goto wait_and_exit;
  if (imported && state_edits_open("world/barectr-states.bin")) { printf("Invalid block state save.\n"); goto wait_and_exit; }

  legacy_init();
  int clients[MAX_PLAYERS];
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    clients[i] = -1;
    client_states[i * 2] = -1;
    player_data[i].client_fd = -1;
  }

  int server_fd, opt = 1;
  struct sockaddr_in server_addr, client_addr;
  socklen_t addr_len = sizeof(client_addr);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) goto wait_and_exit;

  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) goto wait_and_exit;

  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);
#ifndef __3DS__
  server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (getenv("BARECTR_TEST_PORT")) server_addr.sin_port = htons(atoi(getenv("BARECTR_TEST_PORT")));
#endif

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    close(server_fd);
    goto wait_and_exit;
  }

  if (listen(server_fd, 5) < 0) {
    close(server_fd);
    goto wait_and_exit;
  }

  server_ready = true;
  
  printf("Serveur ecoute sur le port %d...\n", PORT);

  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

  int64_t last_tick_time = get_program_time();
  
  int64_t last_screen_update = get_program_time();

  while (aptMainLoop()) {
    
    int64_t now = get_program_time();
    if (now - last_screen_update >= 50000) {
      hidScanInput();
      if (hidKeysDown() & KEY_START) break;
      // gfxFlushBuffers(); gfxSwapBuffers(); last_screen_update = now;

      DashboardStats stats;

      stats.server_ready = server_ready;
      stats.players = client_count;
      stats.max_players = server_max_clients;
      stats.view_distance = server_view_distance;

      dashboard_render(&stats);

      gfxFlushBuffers();
      gfxSwapBuffers();

      last_screen_update = now;
    }
    if (client_count < server_max_clients) for (int i = 0; i < MAX_PLAYERS; i++) {
      if (clients[i] != -1) continue;
      addr_len = sizeof(client_addr);
      int fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
      if (fd >= 0) {
        if (transport_open(fd)) { close(fd); break; }
        clients[i] = fd;
        int client_flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, client_flags | O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        client_count++;
        printf("Connected fd %d\n", fd);
      }
      break;
    }
    int busy = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      int fd = clients[i]; if (fd < 0) continue;
      if (!transport_active(fd)) { clients[i] = -1; continue; }
      if (transport_flush(fd, 16384) < 0) { disconnectClient(&clients[i], 1); continue; }
      for (int packet = 0; packet < 16; packet++) {
        int ready = transport_poll(fd);
        if (ready < 0) { disconnectClient(&clients[i], 2); break; }
        if (!ready) break;
        busy = 1;
        int length = readVarInt(fd), id = readVarInt(fd);
        if (length <= 0 || id < 0 || length < sizeVarInt(id)) { disconnectClient(&clients[i], 3); break; }
        handlePacket(fd, length - sizeVarInt(id), id, getClientState(fd));
        if (transport_failed(fd)) {
          if (transport_active(fd)) disconnectClient(&clients[i], 4);
          else clients[i] = -1;
          break;
        }
      }
    }
    now = get_program_time();
    if (now - last_tick_time >= TIME_BETWEEN_TICKS) {
      command_tick(now - last_tick_time);
      handleServerTick(now - last_tick_time);
      legacy_pressure_tick();
      last_tick_time = now;
    }
    int64_t circuit_start = get_program_time();
    for (int work = 0; work < server_circuit_nodes; work += 8) {
      circuit_step(server_ticks, 8);
      if (!circuit_pending() || get_program_time() - circuit_start >= server_circuit_budget_us) break;
    }
    chunk_stream_tick();
    for (int i = 0; i < MAX_PLAYERS; i++) if (clients[i] >= 0 && transport_active(clients[i])) {
      if (transport_flush(clients[i], 16384) < 0) disconnectClient(&clients[i], 5);
    }
    if (!busy) svcSleepThread(1000000);
  }
  state_edits_flush();
  writePlayerDataToDisk();
  writeBlockChangesToDisk(0, block_changes_count);

  printf("Fermeture du serveur...\n");
  close(server_fd);
  
wait_and_exit:
  socExit();
  if(soc_sharedmem != NULL) {
      free(soc_sharedmem);
  }
  gfxExit();
  return 0;
}
