#ifndef COMMANDS_H
#define COMMANDS_H
#include "globals.h"
int player_mode(PlayerData *p);
int player_mode_fd(int fd);
int player_can_fly(PlayerData *p);
int player_is_flying(PlayerData *p);
void command_sync(PlayerData *p);
void command_flight(PlayerData *p,int enabled);
int command_teleport(PlayerData *p,double x,double y,double z);
void command_creative_slot(int fd);
int server_command(PlayerData *p,const char *text);
void command_tick(double elapsed);
#endif
