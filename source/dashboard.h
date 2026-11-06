#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <3ds.h>
#include <stdbool.h>

typedef struct {
    bool server_ready;
    int players;
    int max_players;
    int view_distance;
} DashboardStats;

void dashboard_init(void);
void dashboard_render(const DashboardStats *stats);

#endif