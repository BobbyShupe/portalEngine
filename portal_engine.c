#include <SDL2/SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FOV_DEG 90.0
#define MAX_RECURSION_DEPTH 8
#define NEAR_PLANE 0.01

typedef struct { double x, y; } Vec2;

typedef struct {
    Vec2 p1, p2;
    int portal_to;
    Uint32 color;
} Wall;

typedef struct {
    int first_wall;
    int wall_count;
    float floor_height, ceil_height;
    Uint32 floor_color, ceil_color;
} Sector;

typedef struct {
    double x, y, z;
    double angle, pitch;
    int sector_id;
} Player;

typedef struct { int top, bottom; } ClipRange;

Sector sectors[32];
Wall walls[1024];
Player player;

// ------------------- Helpers -------------------

double point_side(double px, double py, Vec2 a, Vec2 b) {
    return (b.x - a.x)*(py - a.y) - (b.y - a.y)*(px - a.x);
}

void draw_vline(SDL_Renderer *r, int x, int y1, int y2, Uint32 c) {
    if (y1 >= y2) return;
    SDL_SetRenderDrawColor(r, (c>>24)&255, (c>>16)&255, (c>>8)&255, 255);
    SDL_RenderDrawLine(r, x, y1, x, y2);
}

// ------------------- Map -------------------

void init_map(void) {
    int wi = 0;
    // Sector 0: Hub
    sectors[0] = (Sector){0, 5, 0.0, 3.0, 0x333333FF, 0x111111FF};
    walls[wi++] = (Wall){{-5, -5}, { 5, -5}, -1, 0xFF0000FF};
    walls[wi++] = (Wall){{ 5, -5}, { 5,  5},  1, 0x00FF00FF};
    walls[wi++] = (Wall){{ 5,  5}, {-5,  5},  2, 0x0000FFFF};
    walls[wi++] = (Wall){{-5,  5}, {-5, -2}, -1, 0xFFFF00FF};
    walls[wi++] = (Wall){{-5, -2}, {-5, -5},  4, 0xAA00AAFF};

    // Sector 1: Sunken Pit
    sectors[1] = (Sector){wi-wi+5, 4, -1.0, 2.0, 0x222244FF, 0x111122FF};
    walls[wi++] = (Wall){{ 5, -5}, {15, -5}, -1, 0x00FFFFFF};
    walls[wi++] = (Wall){{15, -5}, {15,  5}, -1, 0xFF8800FF};
    walls[wi++] = (Wall){{15,  5}, { 5,  5}, -1, 0xFFFFFFFF};
    walls[wi++] = (Wall){{ 5,  5}, { 5, -5},  0, 0x00FF00FF};

    // Sector 2: Corridor
    sectors[2] = (Sector){wi-wi+9, 4, 0.0, 2.5, 0x444444FF, 0x222222FF};
    walls[wi++] = (Wall){{ 5,  5}, { 5, 15}, -1, 0x888888FF};
    walls[wi++] = (Wall){{ 5, 15}, {-5, 15},  3, 0x00FF00FF};
    walls[wi++] = (Wall){{-5, 15}, {-5,  5}, -1, 0x888888FF};
    walls[wi++] = (Wall){{-5,  5}, { 5,  5},  0, 0x0000FFFF};

    // Sector 3: Crawlspace
    sectors[3] = (Sector){wi-wi+13, 4, 0.5, 1.2, 0x111111FF, 0x050505FF};
    walls[wi++] = (Wall){{-5, 15}, { 5, 15},  2, 0x00FF00FF};
    walls[wi++] = (Wall){{ 5, 15}, { 5, 20}, -1, 0x440000FF};
    walls[wi++] = (Wall){{ 5, 20}, {-5, 20}, -1, 0x440000FF};
    walls[wi++] = (Wall){{-5, 20}, {-5, 15}, -1, 0x440000FF};

    // Sector 4: Raised Deck
    sectors[4] = (Sector){wi-wi+17, 4, 1.0, 5.0, 0x664422FF, 0x221100FF};
    walls[wi++] = (Wall){{-5, -5}, {-5, -2},  0, 0xAA00AAFF};
    walls[wi++] = (Wall){{-5, -2}, {-10,-2}, -1, 0x00FF00FF};
    walls[wi++] = (Wall){{-10,-2}, {-10,-5}, -1, 0x00FF00FF};
    walls[wi++] = (Wall){{-10,-5}, {-5, -5}, -1, 0x00FF00FF};

    player = (Player){0.0, 0.0, 1.5, 0, 0, 0};
}

void update_player_sector(void) {
    Sector *s = &sectors[player.sector_id];
    for (int i = 0; i < s->wall_count; i++) {
        Wall *w = &walls[s->first_wall + i];
        if (w->portal_to >= 0 && point_side(player.x, player.y, w->p1, w->p2) < 0) {
            player.sector_id = w->portal_to;
            return;
        }
    }
}

// ------------------- Renderer -------------------

// Replace the entire render_sector function with this version

void render_sector(SDL_Renderer *r, int sid, ClipRange *clip, int depth, int sw, int sh) {
    if (depth > MAX_RECURSION_DEPTH) return;
    Sector *s = &sectors[sid];
    double proj = (sw / 2.0) / tan(FOV_DEG * M_PI / 360.0);
    double yoff = sh / 2.0 + player.pitch * (sh / 2.0);

    // Collect walls + approximate depth for rough back-to-front sort
    typedef struct { int idx; double avg_vz; } WallEntry;
    WallEntry entries[64];
    int n = 0;

    for (int i = 0; i < s->wall_count; i++) {
        Wall *w = &walls[s->first_wall + i];
        double dx1 = w->p1.x - player.x, dy1 = w->p1.y - player.y;
        double dx2 = w->p2.x - player.x, dy2 = w->p2.y - player.y;
        double vz1 = dx1 * cos(player.angle) + dy1 * sin(player.angle);
        double vz2 = dx2 * cos(player.angle) + dy2 * sin(player.angle);
        if (vz1 > NEAR_PLANE || vz2 > NEAR_PLANE) {  // at least partially in front
            double avg_vz = (fmax(vz1, NEAR_PLANE) + fmax(vz2, NEAR_PLANE)) * 0.5;
            entries[n++] = (WallEntry){i, avg_vz};
        }
    }

    // Sort back-to-front (higher vz = farther)
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (entries[i].avg_vz < entries[j].avg_vz) {
                WallEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    for (int ei = 0; ei < n; ei++) {
        int i = entries[ei].idx;
        Wall *w = &walls[s->first_wall + i];

        double dx1 = w->p1.x - player.x, dy1 = w->p1.y - player.y;
        double dx2 = w->p2.x - player.x, dy2 = w->p2.y - player.y;

        double vx1 = dx1 * sin(player.angle) - dy1 * cos(player.angle);
        double vz1 = dx1 * cos(player.angle) + dy1 * sin(player.angle);
        double vx2 = dx2 * sin(player.angle) - dy2 * cos(player.angle);
        double vz2 = dx2 * cos(player.angle) + dy2 * sin(player.angle);

        if (vz1 <= 0 && vz2 <= 0) continue;

        if (vz1 < NEAR_PLANE) {
            double t = (NEAR_PLANE - vz1) / (vz2 - vz1);
            vx1 += t * (vx2 - vx1); vz1 = NEAR_PLANE;
        }
        if (vz2 < NEAR_PLANE) {
            double t = (NEAR_PLANE - vz2) / (vz1 - vz2);
            vx2 += t * (vx1 - vx2); vz2 = NEAR_PLANE;
        }

        int x1 = (int)(sw / 2.0 - vx1 * proj / vz1);
        int x2 = (int)(sw / 2.0 - vx2 * proj / vz2);

        // Make range inclusive and robust against rounding
        if (x1 > x2) { int tmp = x1; x1 = x2; x2 = tmp; }
        int sx = fmax(0, x1);
        int ex = fmin(sw - 1, x2);
        if (sx > ex) continue;

        double iz1 = 1.0 / vz1;
        double iz2 = 1.0 / vz2;

        ClipRange child[1024];  // assuming sw <= 1024
        bool has_visible_portal = false;

        if (w->portal_to >= 0) {
            Sector *ns = &sectors[w->portal_to];

            // Initialize child with current clip
            for (int x = sx; x <= ex; x++) child[x] = clip[x];

            for (int x = sx; x <= ex; x++) {
                if (clip[x].top >= clip[x].bottom) continue;

                double t = (double)(x - x1) / (x2 - x1);
                double sc = proj * (iz1 + t * (iz2 - iz1));

                int yc_this = (int)(yoff - (s->ceil_height - player.z) * sc);
                int yf_this = (int)(yoff - (s->floor_height - player.z) * sc);
                int yc_next = (int)(yoff - (ns->ceil_height - player.z) * sc);
                int yf_next = (int)(yoff - (ns->floor_height - player.z) * sc);

                // Draw ceiling/floor of CURRENT sector outside portal
                int portal_top    = fmax(yc_this, yc_next);
                int portal_bot    = fmin(yf_this, yf_next);

                // Ceiling above portal opening
                if (clip[x].top < portal_top) {
                    draw_vline(r, x, clip[x].top, portal_top, s->ceil_color);
                }
                // Floor below portal opening
                if (portal_bot < clip[x].bottom) {
                    draw_vline(r, x, portal_bot, clip[x].bottom, s->floor_color);
                }

                // Draw portal frame / step / lip if heights differ
                if (yc_next > yc_this) draw_vline(r, x, yc_this, yc_next, w->color);
                if (yf_next < yf_this) draw_vline(r, x, yf_next, yf_this, w->color);

                // Tight child clip = only the actual portal opening
                child[x].top    = fmax(clip[x].top,    portal_top);
                child[x].bottom = fmin(clip[x].bottom, portal_bot);

                if (child[x].top < child[x].bottom) has_visible_portal = true;
            }

            if (has_visible_portal) {
                render_sector(r, w->portal_to, child, depth + 1, sw, sh);
            }

            // After recursion: block portal columns completely in parent clip
            // (prevents later walls in same sector from leaking into portal area)
            for (int x = sx; x <= ex; x++) {
                if (clip[x].top < clip[x].bottom) {
                    clip[x].top = clip[x].bottom;  // fully consumed
                }
            }
        } else {
            // Solid wall — draw everything and fully consume column
            for (int x = sx; x <= ex; x++) {
                if (clip[x].top >= clip[x].bottom) continue;

                double t = (double)(x - x1) / (x2 - x1);
                double sc = proj * (iz1 + t * (iz2 - iz1));

                int yc = (int)(yoff - (s->ceil_height - player.z) * sc);
                int yf = (int)(yoff - (s->floor_height - player.z) * sc);

                // Ceiling
                if (clip[x].top < yc)
                    draw_vline(r, x, clip[x].top, yc, s->ceil_color);

                // Wall
                int wall_top = fmax(clip[x].top, yc);
                int wall_bot = fmin(clip[x].bottom, yf);
                if (wall_top < wall_bot)
                    draw_vline(r, x, wall_top, wall_bot, w->color);

                // Floor
                if (yf < clip[x].bottom)
                    draw_vline(r, x, yf, clip[x].bottom, s->floor_color);

                // Consume entire column
                clip[x].top = clip[x].bottom;
            }
        }
    }
}
// ------------------- Main -------------------

int main(int argc, char *argv[]) {
    SDL_Init(SDL_INIT_VIDEO);
    int w = 1024, h = 768;
    SDL_Window *win = SDL_CreateWindow("Portal Engine Fixed v2.0", 100, 100, w, h, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetRelativeMouseMode(SDL_TRUE);
    init_map();

    ClipRange *root_clip = malloc(sizeof(ClipRange) * w);

    while (1) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) exit(0);

        int mx, my;
        SDL_GetRelativeMouseState(&mx, &my);
        player.angle += mx * 0.002;
        player.pitch -= my * 0.002;

        const Uint8 *k = SDL_GetKeyboardState(NULL);
        double speed = 0.1, rot = player.angle;
        if (k[SDL_SCANCODE_W]) { player.x += cos(rot)*speed; player.y += sin(rot)*speed; }
        if (k[SDL_SCANCODE_S]) { player.x -= cos(rot)*speed; player.y -= sin(rot)*speed; }
        if (k[SDL_SCANCODE_A]) { player.x += sin(rot)*speed; player.y -= cos(rot)*speed; }
        if (k[SDL_SCANCODE_D]) { player.x -= sin(rot)*speed; player.y += cos(rot)*speed; }

        update_player_sector();

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        for (int i = 0; i < w; i++) { root_clip[i].top = 0; root_clip[i].bottom = h; }
        render_sector(ren, player.sector_id, root_clip, 0, w, h);

        SDL_RenderPresent(ren);
        SDL_Delay(10);
    }
}
