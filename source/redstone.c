#include "redstone.h"
#include <string.h>
#include <stdlib.h>
#define QUEUE 8192
#define HASH 16384
#define TIMERS 1024
typedef struct { int16_t x,z,y; } Position;
typedef struct { Position p; uint32_t due; uint16_t expected,value; uint8_t used; } Timer;
static Position queue[QUEUE], keys[HASH];
static uint8_t occupied[HASH];
static Timer timers[TIMERS];
static unsigned head,count,overflow_count;
static uint32_t current_tick;
static uint32_t timer_tick=UINT32_MAX;
static struct {Position p; uint8_t used,power;} traps[512];
static CircuitRead read_block;
static CircuitWrite write_blocks;
static CircuitPush push_players;
/* Down, up, north, south, west, east: legacy piston facing values. */
static const int dirs[6][3]={{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
static const int horizontal[4][3]={{0,0,-1},{1,0,0},{0,0,1},{-1,0,0}};
static int valid(int x,int y,int z) {return x>=-32768 && x<=32767 && z>=-32768 && z<=32767 && y>=0 && y<=255;}
static uint16_t get(int x,int y,int z) {return valid(x,y,z)?read_block(x,y,z):7*16;}
static unsigned hash(int x,int y,int z) {return ((uint32_t)x*73856093u^(uint32_t)y*19349663u^(uint32_t)z*83492791u)&(HASH-1);}
static int same(Position p,int x,int y,int z) {return p.x==x && p.y==y && p.z==z;}
static int find(int x,int y,int z,int insert) {
  unsigned h=hash(x,y,z);int free_slot=-1;
  for(unsigned n=0;n<HASH;n++,h=(h+1)&(HASH-1)) {
    if(occupied[h]==1 && same(keys[h],x,y,z))return h;
    if(occupied[h]!=1 && free_slot<0)free_slot=h;
    if(!occupied[h])break;
  }
  if(!insert || free_slot<0)return -1;
  keys[free_slot]=(Position){x,z,y};occupied[free_slot]=1;return free_slot;
}
void circuit_seed(int x,int y,int z) {
  if(!read_block || !valid(x,y,z) || find(x,y,z,0)>=0)return;
  if(count==QUEUE){overflow_count++;return;}
  if(find(x,y,z,1)<0){overflow_count++;return;}
  queue[(head+count)%QUEUE]=(Position){x,z,y};count++;
}
void circuit_notify(int x,int y,int z) {
  circuit_seed(x,y,z);
  for(int d=0;d<6;d++)circuit_seed(x+dirs[d][0],y+dirs[d][1],z+dirs[d][2]);
  /* Dust can climb one block. Strongly powered supports affect neighbors. */
  for(int d=0;d<6;d++)for(int e=d;e<6;e++)
    circuit_seed(x+dirs[d][0]+dirs[e][0],y+dirs[d][1]+dirs[e][1],z+dirs[d][2]+dirs[e][2]);
}
void circuit_init(CircuitRead read,CircuitWrite write,CircuitPush push) {
  read_block=read;write_blocks=write;push_players=push;head=count=overflow_count=current_tick=0;
  memset(occupied,0,sizeof(occupied));memset(timers,0,sizeof(timers));
  memset(traps,0,sizeof(traps));timer_tick=UINT32_MAX;
}
int circuit_component(unsigned id) {
  return id==55 || id==69 || id==77 || id==70 || id==72 || id==75 || id==76 || id==93 || id==94 ||
    id==29 || id==33 || id==64 || id==71 || id==96 || id==107 || id==123 || id==124;
}
int circuit_solid(uint16_t value) {
  unsigned id=value>>4;
  switch(id) {
    case 0:case 6:case 8:case 9:case 10:case 11:case 18:case 20:case 26:case 27:case 28:case 30:
    case 31:case 32:case 34:case 37:case 38:case 39:case 40:case 44:case 50:case 51:case 55:
    case 59:case 60:case 63:case 64:case 65:case 66:case 68:case 69:case 70:case 71:case 72:
    case 75:case 76:case 77:case 78:case 81:case 83:case 85:case 90:case 92:case 93:case 94:
    case 96:case 101:case 102:case 104:case 105:case 106:case 107:case 111:case 113:case 115:
    case 117:case 118:case 119:case 127:case 131:case 132: return 0;
    default:return 1;
  }
}
static int apply(CircuitChange *changes,int n) {
  if(write_blocks(changes,n))return -1;
  for(int i=0;i<n;i++)circuit_notify(changes[i].x,changes[i].y,changes[i].z);
  return 0;
}
static int change(int x,int y,int z,uint16_t v) {
  if(get(x,y,z)==v)return 0;
  CircuitChange c={x,y,z,v};return apply(&c,1);
}
static void attachment(int x,int y,int z,unsigned id,unsigned meta,int *sx,int *sy,int *sz) {
  *sx=x;*sy=y;*sz=z;
  switch(meta&7) {
    case 1:--*sx;break;case 2:++*sx;break;case 3:--*sz;break;case 4:++*sz;break;
    case 0:case 7: if(id==69 || id==77)++*sy;else --*sy;break;
    default:--*sy;break;
  }
}
/* Power emitted by a component towards the target cell. */
static int output(int x,int y,int z,int tx,int ty,int tz,int strong,int include_wire) {
  uint16_t v=get(x,y,z);unsigned id=v>>4,m=v&15;
  if(id==152)return 15;
  if(id==69 || id==77) {
    if(!(m&8))return 0;
    if(!strong)return 15;
    int sx,sy,sz;attachment(x,y,z,id,m,&sx,&sy,&sz);return same((Position){sx,sz,sy},tx,ty,tz)?15:0;
  }
  if(id==70 || id==72)return m && (!strong || ty==y-1)?15:0;
  if(id==76) {
    int sx,sy,sz;attachment(x,y,z,id,m,&sx,&sy,&sz);
    if(sx==tx && sy==ty && sz==tz)return 0;
    return !strong || ty==y+1?15:0;
  }
  if(id==94) {
    const int *d=horizontal[m&3];return tx==x+d[0] && ty==y && tz==z+d[2]?15:0;
  }
  if(id==55 && include_wire)return !strong || ty==y-1?m:0;
  return 0;
}
static int strong_power(int x,int y,int z,int include_wire) {
  int power=0;
  for(int d=0;d<6;d++) {
    int p=output(x+dirs[d][0],y+dirs[d][1],z+dirs[d][2],x,y,z,1,include_wire);
    if(p>power)power=p;
  }
  return power;
}
static int input_power(int x,int y,int z,int include_wire,int exclude) {
  int power=0;
  for(int d=0;d<6;d++) {
    if(d==exclude)continue;
    int nx=x+dirs[d][0],ny=y+dirs[d][1],nz=z+dirs[d][2];
    int p=output(nx,ny,nz,x,y,z,0,include_wire);
    if(circuit_solid(get(nx,ny,nz))) {int through=strong_power(nx,ny,nz,include_wire);if(through>p)p=through;}
    if(p>power)power=p;
  }
  return power;
}
static int wire_neighbor(int x,int y,int z,int dx,int dz,int *ny) {
  int nx=x+dx,nz=z+dz;unsigned id=get(nx,y,nz)>>4;
  *ny=y;
  if(id==55)return 1;
  if(circuit_solid(get(nx,y,nz))) {
    if(!circuit_solid(get(x,y+1,z)) && (get(nx,y+1,nz)>>4)==55){*ny=y+1;return 1;}
  } else if((get(nx,y-1,nz)>>4)==55){*ny=y-1;return 1;}
  return 0;
}
uint16_t circuit_wire_state(int x,int y,int z,uint16_t value) {
  if(!read_block)return 4202+(value&15)*9;
  /* Registry order: east, north, power, south, west; up=0, side=1, none=2. */
  const int dx[4]={1,0,0,-1},dz[4]={0,-1,1,0};int side[4],connected=0;
  for(int d=0;d<4;d++) {
    int ny;unsigned id=get(x+dx[d],y,z+dz[d])>>4;
    int wire=wire_neighbor(x,y,z,dx[d],dz[d],&ny);
    int component=id==69 || id==77 || id==75 || id==76 || id==70 || id==72;
    if(id==93 || id==94) {unsigned facing=get(x+dx[d],y,z+dz[d])&3;component=(horizontal[facing][0]!=0)==(dx[d]!=0);}
    side[d]=wire?(ny>y?0:1):component?1:2;
    if(side[d]!=2)connected++;
  }
  if(!connected)for(int i=0;i<4;i++)side[i]=1;
  if(connected==1) {
    if(side[0]!=2)side[3]=1;else if(side[3]!=2)side[0]=1;
    else if(side[1]!=2)side[2]=1;else side[1]=1;
  }
  return 3042+side[0]*432+side[1]*144+(value&15)*9+side[2]*3+side[3];
}
static void schedule(int x,int y,int z,uint16_t expected,uint16_t value,unsigned delay) {
  if(!delay){change(x,y,z,value);return;}
  int free_slot=-1;
  for(int i=0;i<TIMERS;i++) {
    if(timers[i].used && same(timers[i].p,x,y,z)) {
      if(timers[i].value==value && timers[i].expected==expected)return;
      timers[i].used=0;free_slot=i;break;
    }
    if(!timers[i].used)free_slot=i;
  }
  if(expected==value)return;
  if(free_slot<0){overflow_count++;return;}
  timers[free_slot]=(Timer){.p={x,z,y},.due=current_tick+delay,.expected=expected,.value=value,.used=1};
}
static int movable(uint16_t v) {
  unsigned id=v>>4;
  if(id>124 || id==7 || id==49 || id==23 || id==25 || id==26 || id==34 || id==36 || id==52 ||
     id==54 || id==61 || id==62 || id==63 || id==64 || id==68 || id==71 || id==84 ||
     id==90 || id==95 || id==116 || id==117 || id==119 || id==120 || id==122)return 0;
  if((id==29 || id==33) && (v&8))return 0;
  return 1;
}
static void piston(int x,int y,int z,uint16_t v,int power) {
  unsigned face=v&7;if(face>5 || !!(v&8)==!!power)return;
  const int *d=dirs[face];int fx=x+d[0],fy=y+d[1],fz=z+d[2];
  if(!valid(fx,fy,fz))return;
  CircuitChange changes[15];int n=0,moved=0;
  if(power) {
    for(;moved<=12;moved++) {
      int nx=fx+d[0]*moved,ny=fy+d[1]*moved,nz=fz+d[2]*moved;
      if(!valid(nx,ny,nz))return;
      uint16_t next=get(nx,ny,nz);if(!next)break;
      if(moved==12 || !movable(next))return;
    }
    for(int i=moved;i>0;i--)changes[n++]=(CircuitChange){fx+d[0]*i,fy+d[1]*i,fz+d[2]*i,get(fx+d[0]*(i-1),fy+d[1]*(i-1),fz+d[2]*(i-1))};
    changes[n++]=(CircuitChange){fx,fy,fz,(34<<4)|face|((v>>4)==29?8:0)};
    changes[n++]=(CircuitChange){x,y,z,v|8};
  } else {
    uint16_t front=get(fx,fy,fz);
    if((front>>4)==34 && (front&7)==face)changes[n++]=(CircuitChange){fx,fy,fz,0};
    if((v>>4)==29 && (front>>4)==34 && (front&7)==face) {
      int sx=fx+d[0],sy=fy+d[1],sz=fz+d[2];uint16_t pull=get(sx,sy,sz);
      if(pull && movable(pull) && valid(sx,sy,sz)) {
        changes[0].value=pull;changes[n++]=(CircuitChange){sx,sy,sz,0};
      }
    }
    changes[n++]=(CircuitChange){x,y,z,v&~8u};
  }
  if(!apply(changes,n) && power && push_players)push_players(fx,fy,fz,d[0],d[1],d[2],moved+1);
}
static void update(int x,int y,int z) {
  uint16_t v=get(x,y,z);unsigned id=v>>4,m=v&15;
  if(id==55) {
    int power=input_power(x,y,z,0,-1);
    for(int d=0;d<4;d++) {
      int ny;if(!wire_neighbor(x,y,z,horizontal[d][0],horizontal[d][2],&ny))continue;
      int neighbor=(get(x+horizontal[d][0],ny,z+horizontal[d][2])&15)-1;
      if(neighbor>power)power=neighbor;
    }
    change(x,y,z,(55<<4)|power);
  } else if(id==93 || id==94) {
    const int *d=horizontal[m&3];int bx=x-d[0],bz=z-d[2];
    int power=output(bx,y,bz,x,y,z,0,1);
    if(circuit_solid(get(bx,y,bz)) && strong_power(bx,y,bz,1))power=15;
    int locked=0;
    for(int side=-1;side<=1;side+=2) {
      int sx=x+d[2]*side,sz=z-d[0]*side;
      if((get(sx,y,sz)>>4)==94 && output(sx,y,sz,x,y,z,0,1))locked=1;
    }
    if(!locked)schedule(x,y,z,v,((power?94:93)<<4)|m,2*((m>>2)+1));
  } else if(id==75 || id==76) {
    int sx,sy,sz;attachment(x,y,z,id,m,&sx,&sy,&sz);
    int power=strong_power(sx,sy,sz,1);
    schedule(x,y,z,v,((power?75:76)<<4)|m,2);
  } else if(id==29 || id==33)piston(x,y,z,v,input_power(x,y,z,1,m&7));
  else if((id==64 || id==71) && !(m&8)) {
    uint16_t upper=get(x,y+1,z);if((upper>>4)!=id || !(upper&8))return;
    int power=input_power(x,y,z,1,-1)||input_power(x,y+1,z,1,-1);
    /* Manual wooden-door state survives until its powered state changes. */
    if(!!(upper&2)!=!!power) {
      CircuitChange c[2]={{x,y,z,(v&~4u)|(power?4:0)},{x,y+1,z,(upper&~2u)|(power?2:0)}};apply(c,2);
    }
  } else if(id==123 || id==124) {
    int power=input_power(x,y,z,1,-1);schedule(x,y,z,v,(power?124:123)<<4,power?0:4);
  } else if(id==107) {
    int power=input_power(x,y,z,1,-1);
    if(!!(m&8)!=!!power)change(x,y,z,(v&~12u)|(power?12:0));
  } else if(id==96) {
    int power=!!input_power(x,y,z,1,-1),slot=-1;
    for(int i=0;i<512;i++) {
      if(traps[i].used && same(traps[i].p,x,y,z)){slot=i;break;}
      if(!traps[i].used && slot<0)slot=i;
    }
    if(slot>=0) {
      if(!traps[slot].used){traps[slot].used=1;traps[slot].p=(Position){x,z,y};traps[slot].power=0;}
      if(traps[slot].power!=power){traps[slot].power=power;change(x,y,z,(v&~4u)|(power?4:0));}
    }
  }
}
int circuit_use(int x,int y,int z) {
  uint16_t v=get(x,y,z);unsigned id=v>>4;
  if(id==69){change(x,y,z,v^8);return 1;}
  if(id==77) {if(!(v&8))change(x,y,z,v|8);schedule(x,y,z,v|8,v&~8u,20);return 1;}
  if(id==93 || id==94){change(x,y,z,(v&~12u)|(((v&12)+4)&12));return 1;}
  if(id==71)return 1;
  if(id==64) {
    int lower=y-((v&8)?1:0);uint16_t low=get(x,lower,z),high=get(x,lower+1,z);
    if((low>>4)==64 && (high>>4)==64 && (high&8)) {
      CircuitChange c[2]={{x,lower,z,low^4},{x,lower+1,z,high}};apply(c,2);
    }
    return 1;
  }
  if(id==96 || id==107){change(x,y,z,v^4);return 1;}
  return 0;
}
int circuit_step(uint32_t tick,int budget) {
  if(!read_block)return 0;
  current_tick=tick;
  if(timer_tick!=tick)for(int i=0;i<TIMERS;i++)if(timers[i].used && (int32_t)(tick-timers[i].due)>=0) {
    Timer t=timers[i];timers[i].used=0;
    if(get(t.p.x,t.p.y,t.p.z)==t.expected)change(t.p.x,t.p.y,t.p.z,t.value);
  }
  timer_tick=tick;
  int processed=0;
  while(count && processed<budget) {
    Position p=queue[head];head=(head+1)%QUEUE;count--;
    int index=find(p.x,p.y,p.z,0);if(index>=0)occupied[index]=2;
    update(p.x,p.y,p.z);processed++;
  }
  if(!count)memset(occupied,0,sizeof(occupied));
  return processed;
}
int circuit_pending(void) {return count;}
unsigned circuit_overflows(void) {return overflow_count;}
