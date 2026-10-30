#ifndef TRANSPORT_H
#define TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
int transport_open(int fd);
void transport_close(int fd);
int transport_active(int fd);
int transport_compress(int fd);
ssize_t transport_write(int fd, const void *data, size_t size);
ssize_t transport_read(int fd, void *data, size_t size);
int transport_poll(int fd);
int transport_flush(int fd, size_t budget);
size_t transport_pending(int fd);
int transport_failed(int fd);
extern uint64_t transport_raw_bytes, transport_wire_bytes;
#endif
