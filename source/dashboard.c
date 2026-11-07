
#include <stdio.h>
#include <string.h>
#include "dashboard.h"

#define SCREEN_W 400
#define SCREEN_H 240

// Couleurs de l'interface
#define BG_R       10
#define BG_G       13
#define BG_B       17

#define PANEL_R    24
#define PANEL_G    34
#define PANEL_B    46

#define CARD_R     38
#define CARD_G     55
#define CARD_B     72

#define GREEN_R    78
#define GREEN_G    205
#define GREEN_B    139


/*
 * Police bitmap 3x5.
 * Chaque ligne utilise les 3 bits de poids faible.
 */
static const u8 *get_glyph(char c) {
    static const u8 space[5] = {0,0,0,0,0};
    static const u8 letters[26][5] = {
        {2,5,7,5,5}, // A
        {6,5,6,5,6}, // B
        {3,4,4,4,3}, // C
        {6,5,5,5,6}, // D
        {7,4,6,4,7}, // E
        {7,4,6,4,4}, // F
        {3,4,5,5,3}, // G
        {5,5,7,5,5}, // H
        {7,2,2,2,7}, // I
        {1,1,1,5,2}, // J
        {5,5,6,5,5}, // K
        {4,4,4,4,7}, // L
        {5,7,7,5,5}, // M
        {5,7,7,7,5}, // N
        {2,5,5,5,2}, // O
        {6,5,6,4,4}, // P
        {2,5,5,7,3}, // Q
        {6,5,6,5,5}, // R
        {3,4,2,1,6}, // S
        {7,2,2,2,2}, // T
        {5,5,5,5,7}, // U
        {5,5,5,5,2}, // V
        {5,5,7,7,5}, // W
        {5,5,2,5,5}, // X
        {5,5,2,2,2}, // Y
        {7,1,2,4,7}  // Z
    };

    static const u8 digits[10][5] = {
        {7,5,5,5,7}, // 0
        {2,6,2,2,7}, // 1
        {6,1,7,4,7}, // 2
        {6,1,3,1,6}, // 3
        {5,5,7,1,1}, // 4
        {7,4,6,1,6}, // 5
        {3,4,7,5,7}, // 6
        {7,1,2,2,2}, // 7
        {7,5,7,5,7}, // 8
        {7,5,7,1,6}  // 9
    };

    c = (c >= 'a' && c <= 'z') ? c - 32 : c;

    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];

    static const u8 dash[5]  = {0,0,7,0,0};
    static const u8 slash[5] = {1,1,2,4,4};
    static const u8 colon[5] = {0,2,0,2,0};
    static const u8 dot[5]   = {0,0,0,0,2};
    static const u8 percent[5] = {5,1,2,4,5};

    switch (c) {
        case '-': return dash;
        case '/': return slash;
        case ':': return colon;
        case '.': return dot;
        case '%': return percent;
        default: return space;
    }
}

static void draw_round_rect(
    u8 *fb,
    int x, int y,
    int w, int h,
    int radius,
    u8 r, u8 g, u8 b
);

static void draw_char(
    u8 *fb, int x, int y, char c,
    int scale, u8 r, u8 g, u8 b
) {
    const u8 *glyph = get_glyph(c);

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (glyph[row] & (1 << (2 - col))) {
                draw_round_rect(
                    fb,
                    x + col * scale,
                    y + row * scale,
                    scale, scale, 0,
                    r, g, b
                );
            }
        }
    }
}

static void draw_text(
    u8 *fb, int x, int y,
    const char *text, int scale,
    u8 r, u8 g, u8 b
) {
    while (*text) {
        draw_char(fb, x, y, *text, scale, r, g, b);
        x += 4 * scale;
        text++;
    }
}

static PrintConsole bottom_console;

static void put_pixel(
    u8 *fb, int x, int y,
    u8 r, u8 g, u8 b
) {
    if (!fb || x < 0 || x >= SCREEN_W ||
        y < 0 || y >= SCREEN_H) {
        return;
    }

    // Le framebuffer 3DS est stocké sur le côté :
    // 240 pixels de large et 400 pixels de haut.
    int offset = (x * 240 + (239 - y)) * 3;

    fb[offset]     = r;
    fb[offset + 1] = g;
    fb[offset + 2] = b;
}

static void draw_round_rect(
    u8 *fb,
    int x, int y,
    int w, int h,
    int radius,
    u8 r, u8 g, u8 b
) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            int cx = px;
            int cy = py;

            if (px < x + radius) cx = x + radius;
            if (px >= x + w - radius) cx = x + w - radius - 1;

            if (py < y + radius) cy = y + radius;
            if (py >= y + h - radius) cy = y + h - radius - 1;

            int dx = px - cx;
            int dy = py - cy;

            if (dx * dx + dy * dy <= radius * radius) {
                put_pixel(fb, px, py, r, g, b);
            }
        }
    }
}

static void draw_background(u8 *fb) {
    draw_round_rect(
        fb, 0, 0, SCREEN_W, SCREEN_H, 0,
        BG_R, BG_G, BG_B
    );

    // Panneau principal
    draw_round_rect(
        fb, 8, 8, 384, 224, 8,
        PANEL_R, PANEL_G, PANEL_B
    );

    // Cartes statistiques
    draw_round_rect(fb, 16, 55, 178, 43, 5,
                    CARD_R, CARD_G, CARD_B);
    draw_round_rect(fb, 206, 55, 178, 43, 5,
                    CARD_R, CARD_G, CARD_B);
    draw_round_rect(fb, 16, 104, 178, 43, 5,
                    CARD_R, CARD_G, CARD_B);
    draw_round_rect(fb, 206, 104, 178, 43, 5,
                    CARD_R, CARD_G, CARD_B);

    // Panneaux Connection et Performance
    draw_round_rect(fb, 16, 153, 178, 65, 5,
                    CARD_R, CARD_G, CARD_B);
    draw_round_rect(fb, 206, 153, 178, 65, 5,
                    CARD_R, CARD_G, CARD_B);

    // Séparateur de l'en-tête
    for (int x = 16; x < 384; x++) {
        put_pixel(fb, x, 46, 105, 126, 145);
    }

    // Barres d'historique TPS : hauteur provisoire uniforme
    for (int i = 0; i < 12; i++) {
        draw_round_rect(
            fb,
            212 + i * 14,
            185,
            12,
            16,
            2,
            GREEN_R, GREEN_G, GREEN_B
        );
    }
}

void dashboard_init(void) {
    // Console uniquement sur l'écran inférieur.
    consoleInit(GFX_BOTTOM, &bottom_console);
    consoleSelect(&bottom_console);
    consoleClear();

    printf("\x1b[32;1m");
    printf("BARECTR SERVER LOG\n");
    printf("==============================\n");
    printf("\x1b[0m");
}


void dashboard_render(const DashboardStats *stats) {
    if (!stats) return;

    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) return;

    draw_background(fb);

    const u8 white_r = 235;
    const u8 white_g = 240;
    const u8 white_b = 245;

    const u8 muted_r = 155;
    const u8 muted_g = 174;
    const u8 muted_b = 190;

    // HEADER
    draw_text(fb, 20, 17, "BARECTR", 2,
              white_r, white_g, white_b);

    draw_text(fb, 20, 35, "SERVER DASHBOARD", 1,
              muted_r, muted_g, muted_b);

    if (stats->server_ready) {
        draw_text(fb, 330, 20, "ONLINE", 1,
                  GREEN_R, GREEN_G, GREEN_B);
    } else {
        draw_text(fb, 322, 20, "OFFLINE", 1,
                  235, 180, 80);
    }

    // PLAYERS
    draw_text(fb, 25, 61, "PLAYERS", 1,
              muted_r, muted_g, muted_b);

    char players[16];
    snprintf(players, sizeof(players), "%d / %d",
             stats->players, stats->max_players);

    draw_text(fb, 25, 77, players, 2,
              white_r, white_g, white_b);

    // UPTIME
    draw_text(fb, 215, 61, "UPTIME", 1,
              muted_r, muted_g, muted_b);

    draw_text(fb, 215, 77, "--:--:--", 2,
              white_r, white_g, white_b);

    // TPS
    draw_text(fb, 25, 110, "TPS", 1,
              muted_r, muted_g, muted_b);

    draw_text(fb, 25, 126, "--.-", 2,
              white_r, white_g, white_b);

    // RAM
    draw_text(fb, 215, 110, "RAM USED", 1,
              muted_r, muted_g, muted_b);

    draw_text(fb, 215, 126, "-- MB", 2,
              white_r, white_g, white_b);

    // CONNECTION
    draw_text(fb, 25, 160, "CONNECTION", 1,
              white_r, white_g, white_b);

    draw_text(fb, 25, 177, "IP: --.--.--.--", 1,
              muted_r, muted_g, muted_b);

    draw_text(fb, 25, 195, "PORT: 25565", 1,
              muted_r, muted_g, muted_b);

    // PERFORMANCE
    draw_text(fb, 215, 160, "PERFORMANCE", 1,
              white_r, white_g, white_b);

    draw_text(fb, 215, 177, "TPS HISTORY", 1,
              muted_r, muted_g, muted_b);

    draw_text(fb, 215, 202, "WAITING", 1,
              GREEN_R, GREEN_G, GREEN_B);

    // FOOTER
    draw_text(fb, 20, 222, "WI-FI", 1,
              muted_r, muted_g, muted_b);

    draw_text(fb, 340, 222, "--", 1,
              muted_r, muted_g, muted_b);
}

