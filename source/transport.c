#include "transport.h"
#include "globals.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <zlib.h>
#define TX_SIZE (512*1024)
#define FRAME_SIZE (384*1024)
#define RX_SIZE (64*1024)
#define THRESHOLD 256
typedef struct {
  int fd, compressed, error;
  uint8_t tx[TX_SIZE], frame[FRAME_SIZE], rx[RX_SIZE], input[RX_SIZE];
  size_t head, queued, frame_n, frame_size, rx_n, input_n, input_p;
  unsigned prefix_n, prefix_shift;
} Connection;
static Connection *connections[MAX_PLAYERS];
static uint8_t encoded[FRAME_SIZE+1024];
uint64_t transport_raw_bytes, transport_wire_bytes;
static Connection *get(int fd) {
  for (int i=0;i<MAX_PLAYERS;i++) if (connections[i] && connections[i]->fd==fd) return connections[i];
  return NULL;
}
int transport_active(int fd) { return get(fd)!=NULL; }
int transport_open(int fd) {
  if (get(fd)) return -1;
  for (int i=0;i<MAX_PLAYERS;i++) if (!connections[i]) {
    connections[i]=calloc(1,sizeof(Connection));
    if (!connections[i]) return -1;
    connections[i]->fd=fd; return 0;
  }
  return -1;
}
void transport_close(int fd) {
  for (int i=0;i<MAX_PLAYERS;i++) if (connections[i] && connections[i]->fd==fd) { free(connections[i]); connections[i]=NULL; }
}
static unsigned put_var(uint8_t *p, size_t v) {
  unsigned n=0; do { p[n++]=(v&127)|(v>127?128:0); v>>=7; } while(v); return n;
}
static int get_var(const uint8_t *p,size_t size,uint32_t *value) {
  *value=0;
  for(unsigned i=0;i<5;i++) {
    if (i>=size) return 0;
    if (i==4 && (p[i]&0xf0)) return -1;
    *value|=(uint32_t)(p[i]&127)<<(i*7);
    if (!(p[i]&128)) return i+1;
  }
  return -1;
}
static int enqueue(Connection *c,const uint8_t *p,size_t size) {
  if(size>TX_SIZE-c->queued) { c->error=1; return -1; }
  size_t tail=(c->head+c->queued)%TX_SIZE, first=TX_SIZE-tail;
  if(first>size)first=size;
  memcpy(c->tx+tail,p,first); memcpy(c->tx,p+first,size-first); c->queued+=size; return 0;
}
static int finish(Connection *c) {
  uint8_t prefix[10]; unsigned n; size_t size=c->frame_size;
  transport_raw_bytes+=size;
  if(c->compressed && size>=THRESHOLD) {
    uLongf compressed_size=sizeof(encoded);
    if(compress2(encoded,&compressed_size,c->frame,size,1)!=Z_OK) { c->error=1; return -1; }
    uint8_t inner[5]; unsigned in=put_var(inner,size);
    n=put_var(prefix,compressed_size+in);
    if(enqueue(c,prefix,n) || enqueue(c,inner,in) || enqueue(c,encoded,compressed_size))return -1;
    transport_wire_bytes+=n+in+compressed_size;
  } else {
    n=put_var(prefix,size+(c->compressed?1:0));
    if(c->compressed)prefix[n++]=0;
    if(enqueue(c,prefix,n)||enqueue(c,c->frame,size))return -1;
    transport_wire_bytes+=n+size;
  }
  c->frame_n=c->frame_size=c->prefix_n=c->prefix_shift=0;return 0;
}
ssize_t transport_write(int fd,const void *data,size_t size) {
  Connection *c=get(fd); if(!c || c->error)return -1;
  const uint8_t *p=data; size_t total=size;
  while(size) {
    if(!c->frame_size) {
      unsigned v=*p++; size--;
      if(c->prefix_n==4 && (v&0xf0)) { c->error=1; return -1; }
      c->frame_n|=(size_t)(v&127)<<c->prefix_shift;c->prefix_shift+=7;c->prefix_n++;
      if(v&128) { if(c->prefix_n>=5){c->error=1;return -1;} continue; }
      c->frame_size=c->frame_n;c->frame_n=0;
      if(!c->frame_size || c->frame_size>FRAME_SIZE) { c->error=1; return -1; }
    }
    size_t n=c->frame_size-c->frame_n;if(n>size)n=size;
    memcpy(c->frame+c->frame_n,p,n);c->frame_n+=n;p+=n;size-=n;
    if(c->frame_n==c->frame_size && finish(c))return -1;
  }
  return total;
}
int transport_compress(int fd) {
  Connection *c=get(fd);if(!c || c->compressed || c->frame_size || c->prefix_n)return -1;
  const uint8_t packet[]={3,3,0x80,2};
  if(transport_write(fd,packet,sizeof(packet))<0)return -1;
  c->compressed=1;return 0;
}
int transport_flush(int fd,size_t budget) {
  Connection *c=get(fd);if(!c || c->error)return -1;
  while(c->queued && budget) {
    size_t n=TX_SIZE-c->head;if(n>c->queued)n=c->queued;if(n>budget)n=budget;
    ssize_t sent=send(fd,c->tx+c->head,n,0);
    if(sent<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    if(sent<=0){c->error=1;return -1;}
    c->head=(c->head+sent)%TX_SIZE;c->queued-=sent;budget-=sent;
  }
  return 0;
}
size_t transport_pending(int fd) { Connection *c=get(fd);return c?c->queued:0; }
int transport_failed(int fd) { Connection *c=get(fd);return !c || c->error; }
int transport_poll(int fd) {
  Connection *c=get(fd);if(!c || c->error)return -1;
  if(c->input_p<c->input_n) { c->error=1;return -1; }
  uint32_t length;int prefix=get_var(c->rx,c->rx_n,&length);
  if(prefix<0 || (prefix && (!length || length>RX_SIZE-5))){c->error=1;return -1;}
  if(!prefix || c->rx_n<(size_t)prefix+length) {
    ssize_t n=recv(fd,c->rx+c->rx_n,RX_SIZE-c->rx_n,0);
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))return 0;
    if(n<=0){c->error=1;return -1;}
    c->rx_n+=n;prefix=get_var(c->rx,c->rx_n,&length);
    if(prefix<0 || (prefix && (!length || length>RX_SIZE-5))){c->error=1;return -1;}
    if(!prefix || c->rx_n<(size_t)prefix+length)return 0;
  }
  const uint8_t *payload=c->rx+prefix;size_t size=length;
  if(c->compressed) {
    uint32_t unpacked;int n=get_var(payload,size,&unpacked);
    if(n<=0 || unpacked>RX_SIZE-5){c->error=1;return -1;}
    payload+=n;size-=n;
    if(unpacked) {
      if(unpacked<THRESHOLD){c->error=1;return -1;}
      unsigned header=put_var(c->input,unpacked);
      z_stream z={0};z.next_in=(Bytef*)payload;z.avail_in=size;z.next_out=c->input+header;z.avail_out=unpacked;
      if(inflateInit(&z)!=Z_OK){c->error=1;return -1;}
      int result=inflate(&z,Z_FINISH),good=result==Z_STREAM_END && z.total_out==unpacked && z.avail_in==0;
      inflateEnd(&z);if(!good){c->error=1;return -1;}
      c->input_n=header+unpacked;
    } else {
      if(!size || size>=THRESHOLD){c->error=1;return -1;}
      unsigned header=put_var(c->input,size);memcpy(c->input+header,payload,size);c->input_n=header+size;
    }
  } else {
    unsigned header=put_var(c->input,size);memcpy(c->input+header,payload,size);c->input_n=header+size;
  }
  size_t consumed=prefix+length;c->rx_n-=consumed;memmove(c->rx,c->rx+consumed,c->rx_n);c->input_p=0;return 1;
}
ssize_t transport_read(int fd,void *data,size_t size) {
  Connection *c=get(fd);if(!c || c->error || size>c->input_n-c->input_p) {if(c)c->error=1;return -1;}
  memcpy(data,c->input+c->input_p,size);c->input_p+=size;return size;
}
