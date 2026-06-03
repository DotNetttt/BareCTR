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
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

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

// --- Configuration de la memoire reseau 3DS ---
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
      if (state == STATE_PLAY) cs_interact(client_fd);
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

        uint8_t block_feet = getBlockAt(player->x, player->y, player->z);
        uint8_t swimming = block_feet >= B_water && block_feet < B_water + 8;

        if (on_ground) {
          int16_t damage = player->grounded_y - player->y - 3;
          if (damage > 0 && (GAMEMODE == 0 || GAMEMODE == 2) && !swimming) {
            hurtEntity(client_fd, -1, D_fall, damage);
          }
          player->grounded_y = player->y;
        } else if (swimming) {
          player->grounded_y = player->y;
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

        if (player->saturation == 0) {
          if (player->hunger > 0) player->hunger--;
          player->saturation = 200;
          sc_setHealth(client_fd, player->health, player->hunger, player->saturation);
        } else if (player->flags & 0x08) {
          player->saturation -= 1;
        }

        short cx = x, cy = y, cz = z;
        if (x < 0) cx -= 1;
        if (z < 0) cz -= 1;
        short _x = (cx < 0 ? cx - 16 : cx) / 16, _z = (cz < 0 ? cz - 16 : cz) / 16;
        short dx = _x - (player->x < 0 ? player->x - 16 : player->x) / 16;
        short dz = _z - (player->z < 0 ? player->z - 16 : player->z) / 16;

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

        int found = false;
        for (int i = 0; i < VISITED_HISTORY; i ++) {
          if (player->visited_x[i] == _x && player->visited_z[i] == _z) {
            found = true;
            break;
          }
        }
        if (found) break;

        for (int i = 0; i < VISITED_HISTORY - 1; i ++) {
          player->visited_x[i] = player->visited_x[i + 1];
          player->visited_z[i] = player->visited_z[i + 1];
        }
        player->visited_x[VISITED_HISTORY - 1] = _x;
        player->visited_z[VISITED_HISTORY - 1] = _z;

        uint32_t r = fast_rand();
        if ((r & 3) == 0) {
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

        // --- GÉNÉRATION DE CHUNKS ---
        sc_setCenterChunk(client_fd, _x, _z);
        while (dx != 0) {
          sc_chunkDataAndUpdateLight(client_fd, _x + dx * VIEW_DISTANCE, _z);
          for (int i = 1; i <= VIEW_DISTANCE; i ++) {
            sc_chunkDataAndUpdateLight(client_fd, _x + dx * VIEW_DISTANCE, _z - i);
            sc_chunkDataAndUpdateLight(client_fd, _x + dx * VIEW_DISTANCE, _z + i);
          }
          dx += dx > 0 ? -1 : 1;
        }
        while (dz != 0) {
          sc_chunkDataAndUpdateLight(client_fd, _x, _z + dz * VIEW_DISTANCE);
          for (int i = 1; i <= VIEW_DISTANCE; i ++) {
            sc_chunkDataAndUpdateLight(client_fd, _x - i, _z + dz * VIEW_DISTANCE);
            sc_chunkDataAndUpdateLight(client_fd, _x + i, _z + dz * VIEW_DISTANCE);
          }
          dz += dz > 0 ? -1 : 1;
        }
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
  // 1. Initialisation de l'écran
  gfxInitDefault();
  consoleInit(GFX_TOP, NULL);

  // --- OPTIMISATION CRITIQUE NEW 3DS ---
  // Débride le processeur à 804MHz et active le cache L2 (2Mo)
  osSetSpeedupEnable(true);
  printf("New 3DS Speedup (804MHz + L2 Cache) ACTIVE.\n\n");
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

  if (initSerializer()) exit(EXIT_FAILURE);

  int clients[MAX_PLAYERS], client_index = 0;
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

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    close(server_fd);
    goto wait_and_exit;
  }

  if (listen(server_fd, 5) < 0) {
    close(server_fd);
    goto wait_and_exit;
  }
  printf("Serveur ecoute sur le port %d...\n", PORT);

  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

  int64_t last_tick_time = get_program_time();
  
  // Timer pour rafraichir l'écran sans ralentir le serveur
  int64_t last_screen_update = get_program_time();

  // Boucle principale
  while (aptMainLoop()) {
    
    // --- OPTIMISATION DU VBLANK ---
    // Ne rafraichit l'écran qu'une fois toutes les 500ms (2 fois par seconde)
    // Ça permet au CPU de tourner à 100% sur le réseau et la génération !
    if (get_program_time() - last_screen_update > 500) {
      hidScanInput();
      u32 kDown = hidKeysDown();
      if (kDown & KEY_START) break; 

      gfxFlushBuffers();
      gfxSwapBuffers();
      last_screen_update = get_program_time();
    }

    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (clients[i] != -1) continue;
      clients[i] = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
      if (clients[i] != -1) {
        printf("Nouveau joueur connecte, fd: %d\n", clients[i]);
        int client_flags = fcntl(clients[i], F_GETFL, 0);
        fcntl(clients[i], F_SETFL, client_flags | O_NONBLOCK);
        client_count ++;
      }
      break;
    }

    client_index ++;
    if (client_index >= MAX_PLAYERS) client_index = 0;
    
    if (clients[client_index] == -1) {
        // Si personne n'est co, on dort 1 milliseconde pour ne pas faire fondre la batterie
        svcSleepThread(1000000); 
        continue;
    }

    int64_t time_since_last_tick = get_program_time() - last_tick_time;
    if (time_since_last_tick > TIME_BETWEEN_TICKS) {
      handleServerTick(time_since_last_tick);
      last_tick_time = get_program_time();
    }

    int client_fd = clients[client_index];

    recv_count = recv(client_fd, &recv_buffer, 2, MSG_PEEK);
    if (recv_count < 2) {
      if (recv_count == 0 || (recv_count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        disconnectClient(&clients[client_index], 1);
      }
      continue;
    }

    int length = readVarInt(client_fd);
    if (length == VARNUM_ERROR) {
      disconnectClient(&clients[client_index], 2);
      continue;
    }
    int packet_id = readVarInt(client_fd);
    if (packet_id == VARNUM_ERROR) {
      disconnectClient(&clients[client_index], 3);
      continue;
    }
    int state = getClientState(client_fd);
    if (state == STATE_NONE && length == 254 && packet_id == 122) {
      disconnectClient(&clients[client_index], 5);
      continue;
    }
    
    handlePacket(client_fd, length - sizeVarInt(packet_id), packet_id, state);
    
    if (recv_count == 0 || (recv_count == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      disconnectClient(&clients[client_index], 4);
      continue;
    }
  }

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