#include "commands.h"
#include "packets.h"
#include "tools.h"
#include "varnum.h"
#include "chunkstream.h"
#include "procedures.h"
#include "transport.h"
#include "redstone.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static unsigned char modes[MAX_PLAYERS], flight_allowed[MAX_PLAYERS], flying[MAX_PLAYERS];
static double measured_tps,window_us;
static unsigned window_ticks;
int player_mode(PlayerData *p) { return modes[p-player_data]; }
int player_mode_fd(int fd) {PlayerData *p;return getPlayerData(fd,&p)?GAMEMODE:player_mode(p);}
int player_can_fly(PlayerData *p) {int i=p-player_data;return modes[i]==1 || modes[i]==3 || flight_allowed[i];}
int player_is_flying(PlayerData *p) {return flying[p-player_data] || player_mode(p)==3;}
void command_tick(double elapsed) {
  if(elapsed<=0)return;
  window_us+=elapsed;window_ticks++;
  measured_tps=fmin(20,window_ticks*1000000.0/window_us);
  if(window_us>=5000000){window_us=0;window_ticks=0;}
}
static void reply(PlayerData *p,const char *s) {sc_systemChat(p->client_fd,(char*)s,strlen(s));}
static uint8_t abilities(PlayerData *p) {
  int m=player_mode(p);return (m==1?9:0)|(m==3?1:0)|(player_can_fly(p)?4:0)|(player_is_flying(p)?2:0);
}
void command_sync(PlayerData *p) {
  int m=player_mode(p);
  writeVarInt(p->client_fd,6);writeByte(p->client_fd,0x22);writeByte(p->client_fd,3);writeFloat(p->client_fd,m);
  sc_playerAbilities(p->client_fd,abilities(p));
  for(int i=0;i<MAX_PLAYERS;i++)if(player_data[i].client_fd>=0) {
    int fd=player_data[i].client_fd;writeVarInt(fd,20);writeByte(fd,0x3f);writeByte(fd,4);writeByte(fd,1);
    send_all(fd,p->uuid,16);writeVarInt(fd,m);
  }
  broadcastPlayerMetadata(p);
}
void command_flight(PlayerData *p,int enabled) {
  flying[p-player_data]=player_can_fly(p) && enabled;
  p->grounded_y=p->y;sc_playerAbilities(p->client_fd,abilities(p));
}
int command_teleport(PlayerData *p,double x,double y,double z) {
  if(!isfinite(x)||!isfinite(y)||!isfinite(z)||x< -32768||x>=32768||z< -32768||z>=32768||y<0||y>255)return -1;
  p->x=(short)floor(x);p->y=y;p->z=(short)floor(z);p->grounded_y=p->y;
  chunk_stream_request(p,div_floor(p->x,16),div_floor(p->z,16));
  sc_synchronizePlayerPosition(p->client_fd,x,y,z,p->yaw*180.0f/127,p->pitch*90.0f/127);
  for(int i=0;i<MAX_PLAYERS;i++)if(player_data[i].client_fd>=0 && &player_data[i]!=p)
    sc_teleportEntity(player_data[i].client_fd,p->client_fd,x,y,z,p->yaw*180.0f/127,p->pitch*90.0f/127);
  return 0;
}
int server_command(PlayerData *p,const char *text) {
  int slot=p-player_data;char extra;
  if(!strcmp(text,"!tps")) {
    char b[160];double ratio=transport_raw_bytes?100.0*transport_wire_bytes/transport_raw_bytes:0;
    snprintf(b,sizeof(b),"TPS %.1f/20 | network %.1f%% of raw | circuits %d pending, %u overflows",measured_tps,ratio,circuit_pending(),circuit_overflows());reply(p,b);return 1;
  }
  if(!strncmp(text,"!gamemode",9) && (!text[9] || text[9]==' ')) {
    char mode[32];int m=-1;
    if(sscanf(text,"!gamemode %31s %c",mode,&extra)==1) {
      if(!strcmp(mode,"0")||!strcmp(mode,"survival"))m=0;
      if(!strcmp(mode,"1")||!strcmp(mode,"creative"))m=1;
      if(!strcmp(mode,"2")||!strcmp(mode,"adventure"))m=2;
      if(!strcmp(mode,"3")||!strcmp(mode,"spectator"))m=3;
    }
    if(m<0){reply(p,"Usage: !gamemode survival|creative|adventure|spectator (0..3)");return 1;}
    modes[slot]=m;flight_allowed[slot]=0;flying[slot]=m==3;p->grounded_y=p->y;
    if(m==1 || m==3){p->hunger=20;p->health=20;sc_setHealth(p->client_fd,20,20,p->saturation);}
    command_sync(p);reply(p,"Game mode updated.");return 1;
  }
  if(!strncmp(text,"!fly",4) && (!text[4] || text[4]==' ')) {
    if(modes[slot]==3){reply(p,"Flight is always enabled in spectator mode.");return 1;}
    int enable=!player_is_flying(p);
    if(text[4] && strcmp(text+4," on") && strcmp(text+4," off")){reply(p,"Usage: !fly [on|off]");return 1;}
    if(text[4])enable=!strcmp(text+4," on");
    flight_allowed[slot]=enable;flying[slot]=enable;p->grounded_y=p->y;
    sc_playerAbilities(p->client_fd,abilities(p));reply(p,enable?"Flight enabled.":"Flight disabled.");return 1;
  }
  if(!strncmp(text,"!tp",3) && (!text[3] || text[3]==' ')) {
    double x,y,z;char name[17];
    if(sscanf(text,"!tp %lf %lf %lf %c",&x,&y,&z,&extra)==3 && !command_teleport(p,x,y,z))return 1;
    if(sscanf(text,"!tp %16s %c",name,&extra)==1)for(int i=0;i<MAX_PLAYERS;i++)if(player_data[i].client_fd>=0 && !strcmp(name,player_data[i].name)) {
      command_teleport(p,player_data[i].x+0.5,player_data[i].y,player_data[i].z+0.5);return 1;
    }
    reply(p,"Usage: !tp X Y Z (Y: 0..255), or !tp player");return 1;
  }
  return 0;
}
void command_creative_slot(int fd) {
  int slot=readInt16(fd),count=readVarInt(fd),item=0,added=0,removed=0;
  PlayerData *p;if(getPlayerData(fd,&p))return;
  if(count>0){item=readVarInt(fd);added=readVarInt(fd);removed=readVarInt(fd);}
  if(player_mode(p)!=1 || slot<1 || slot>45)return;
  uint8_t internal=clientSlotToServerSlot(0,slot);if(internal>40)return;
  if(count<0 || count>64 || (count && (item<=0 || item>1415)) || added<0 || removed<0)return;
  /* BareCTR stores plain stacks. Unsupported item components are stripped. */
  p->inventory_items[internal]=count?item:0;p->inventory_count[internal]=count;
  sc_setContainerSlot(fd,0,slot,count,count?item:0);
}
