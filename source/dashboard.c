#include <stdio.h>
#include "dashboard.h"

static PrintConsole top_console;
static PrintConsole bottom_console;

void dashboard_init(void)
{
    consoleInit(GFX_TOP, &top_console);
    consoleInit(GFX_BOTTOM, &bottom_console);

    // Les logs du serveur seront affichés en bas.
    consoleSelect(&bottom_console);
    consoleClear();

    printf("\x1b[32;1m");
    printf("BARECTR SERVER LOG\n");
    printf("==============================\n");
    printf("\x1b[0m");
}

void dashboard_render(const DashboardStats *stats)
{
    consoleSelect(&top_console);
    consoleClear();

    printf("\x1b[32;1m");
    printf("  +--------------------------------------+\n");
    printf("  |          BARECTR DASHBOARD           |\n");
    printf("  +--------------------------------------+\n");
    printf("\x1b[0m");

    printf("\n");

    printf("  SERVER : ");

    if (stats->server_ready) {
        printf("\x1b[32;1mONLINE\x1b[0m\n");
    } else {
        printf("\x1b[33;1mSTARTING\x1b[0m\n");
    }

    printf("\n");

    printf("  PLAYERS: %d / %d\n",
           stats->players,
           stats->max_players);

    printf("  VIEW   : %d chunks\n",
           stats->view_distance);

    printf("\n");
    printf("  --------------------------------------\n");
    printf("  START : SAVE AND EXIT\n");
    printf("\n");
    printf("  VERSION 0.1.0\n");

    // Les prochains printf() iront dans les logs du bas.
    consoleSelect(&bottom_console);
}