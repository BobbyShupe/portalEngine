#include <SDL2/SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FOV_DEG              90.0
#define MAX_RECURSION_DEPTH  16
#define NEAR_PLANE           0.01

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

// Now includes horizontal clipping
typedef struct {
    int top, bottom;
    int left, right;        // remaining renderable columns
} ClipRange;

Sector sectors[32];
Wall walls[1024];
Player player;

// ────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────

double point_side(double px, double py, Vec2 a, Vec2 b) {
    return (b.x - a.x)*(py - a.y) - (b.y - a.y)*(px - a.x);
}

void draw_vline(SDL_Renderer *r, int x, int y1, int y2, Uint32 c) {
    if (y1 >= y2) return;
    SDL_SetRenderDrawColor(r, (c>>24)&255, (c>>16)&255, (c>>8)&255, 255);
    SDL_RenderDrawLine(r, x, y1, x, y2);
}

// ────────────────────────────────────────────────
// Map
// ────────────────────────────────────────────────

void init_map(void) {
    int wi = 0;

    // Sector 0: Hub
    sectors[0] = (Sector){0, 5, 0.0f, 3.0f, 0x333333FF, 0x111111FF};
    walls[wi++] = (Wall){{-5,-5}, { 5,-5}, -1, 0xFF0000FF};
    walls[wi++] = (Wall){{ 5,-5}, { 5, 5},  1, 0x00FF00FF};
    walls[wi++] = (Wall){{ 5, 5}, {-5, 5},  2, 0x0000FFFF};
    walls[wi++] = (Wall){{-5, 5}, {-5,-2}, -1, 0xFFFF00FF};
    walls[wi++] = (Wall){{-5,-2}, {-5,-5},  4, 0xAA00AAFF};

    // Sector 1: Sunken Pit
    sectors[1] = (Sector){5, 4, -1.0f, 2.0f, 0x222244FF, 0x111122FF};
    walls[wi++] = (Wall){{ 5,-5}, {15,-5}, -1, 0x00FFFFFF};
    walls[wi++] = (Wall){{15,-5}, {15, 5}, -1, 0xFF8800FF};
    walls[wi++] = (Wall){{15, 5}, { 5, 5}, -1, 0xFFFFFFFF};
    walls[wi++] = (Wall){{ 5, 5}, { 5,-5},  0, 0x00FF00FF};

    // Sector 2: Corridor
    sectors[2] = (Sector){9, 4, 0.0f, 2.5f, 0x444444FF, 0x222222FF};
    walls[wi++] = (Wall){{ 5, 5}, { 5,15}, -1, 0x888888FF};
    walls[wi++] = (Wall){{ 5,15}, {-5,15},  3, 0x00FF00FF};
    walls[wi++] = (Wall){{-5,15}, {-5, 5}, -1, 0x888888FF};
    walls[wi++] = (Wall){{-5, 5}, { 5, 5},  0, 0x0000FFFF};

    // Sector 3: Crawlspace
    sectors[3] = (Sector){13, 4, 0.5f, 1.2f, 0x111111FF, 0x050505FF};
    walls[wi++] = (Wall){{-5,15}, { 5,15},  2, 0x00FF00FF};
    walls[wi++] = (Wall){{ 5,15}, { 5,20}, -1, 0x440000FF};
    walls[wi++] = (Wall){{ 5,20}, {-5,20}, -1, 0x440000FF};
    walls[wi++] = (Wall){{-5,20}, {-5,15}, -1, 0x440000FF};

    // Sector 4: Raised Deck
    sectors[4] = (Sector){17, 4, 1.0f, 5.0f, 0x664422FF, 0x221100FF};
    walls[wi++] = (Wall){{-5,-5}, {-5,-2},  0, 0xAA00AAFF};
    walls[wi++] = (Wall){{-5,-2}, {-10,-2}, -1, 0x00FF00FF};
    walls[wi++] = (Wall){{-10,-2},{-10,-5}, -1, 0x00FF00FF};
    walls[wi++] = (Wall){{-10,-5},{-5, -5}, -1, 0x00FF00FF};

    player = (Player){0.0, 0.0, 1.5, 0.0, 0.0, 0};
}

void update_player_sector(void) {
    Sector *s = &sectors[player.sector_id];
    for (int i = 0; i < s->wall_count; i++) {
        Wall *w = &walls[s->first_wall + i];
        if (w->portal_to >= 0 && point_side(player.x, player.y, w->p1, w->p2) < 0) {
            player.sector_id = w->portal_to;
            return;  // note: in real engines you'd loop until stable
        }
    }
}

// ────────────────────────────────────────────────
// Renderer (with horizontal clipping + sorting)
// ────────────────────────────────────────────────

void render_sector(SDL_Renderer *r, int sid, ClipRange *clip, int depth, int sw, int sh) {
    if (depth > MAX_RECURSION_DEPTH) return;
    Sector *s = &sectors[sid];
    double proj = (sw / 2.0) / tan(FOV_DEG * M_PI / 360.0);
    double yoff = sh / 2.0 + player.pitch * (sh / 2.0);

    // Collect + sort walls back-to-front (simple painter)
    typedef struct { int idx; double depth; } WallSort;
    WallSort tosort[64];
    int nsort = 0;

    for (int i = 0; i < s->wall_count; i++) {
        Wall *w = &walls[s->first_wall + i];
        double mx = (w->p1.x + w->p2.x) * 0.5 - player.x;
        double my = (w->p1.y + w->p2.y) * 0.5 - player.y;
        double dz = mx * cos(player.angle) + my * sin(player.angle);
        if (dz > NEAR_PLANE / 2) {
            tosort[nsort++] = (WallSort){i, dz};
        }
    }

    // Bubble sort (good enough for <64 items)
    for (int i = 0; i < nsort - 1; i++)
        for (int j = i + 1; j < nsort; j++)
            if (tosort[i].depth < tosort[j].depth) {
                WallSort tmp = tosort[i]; tosort[i] = tosort[j]; tosort[j] = tmp;
            }

    for (int si = 0; si < nsort; si++) {
        int i = tosort[si].idx;
        Wall *w = &walls[s->first_wall + i];

        double dx1 = w->p1.x - player.x, dy1 = w->p1.y - player.y;
        double dx2 = w->p2.x - player.x, dy2 = w->p2.y - player.y;

        double vx1 = dx1 * sin(player.angle) - dy1 * cos(player.angle);
        double vz1 = dx1 * cos(player.angle) + dy1 * sin(player.angle);
        double vx2 = dx2 * sin(player.angle) - dy2 * cos(player.angle);
        double vz2 = dx2 * cos(player.angle) + dy2 * sin(player.angle);

        if (vz1 <= 0 && vz2 <= 0) continue;

        if (vz1 < NEAR_PLANE) { double t = (NEAR_PLANE - vz1)/(vz2-vz1); vx1 += t*(vx2-vx1); vz1 = NEAR_PLANE; }
        if (vz2 < NEAR_PLANE) { double t = (NEAR_PLANE - vz2)/(vz1-vz2); vx2 += t*(vx1-vx2); vz2 = NEAR_PLANE; }

        int x1 = (int)(sw / 2.0 - vx1 * proj / vz1);
        int x2 = (int)(sw / 2.0 - vx2 * proj / vz2);

        // Make sure range is ordered
        if (x1 > x2) { int tmp = x1; x1 = x2; x2 = tmp; }

        // Expand slightly to cover rounding gaps
        int sx = fmax(0, x1 - 1);
        int ex = fmin(sw - 1, x2 + 1);

        double iz1 = 1.0 / vz1;
        double iz2 = 1.0 / vz2;

        if (w->portal_to >= 0) {
            Sector *ns = &sectors[w->portal_to];
            ClipRange child[1024];
            bool opening_visible = false;

            // Copy parent clip to child
            for (int x = 0; x < sw; x++) child[x] = clip[x];

            for (int x = sx; x <= ex; x++) {
                if (x < clip[x].left || x > clip[x].right) continue;
                if (clip[x].top >= clip[x].bottom) continue;

                double t = (double)(x - x1) / (x2 - x1);
                double sc = proj * (iz1 + t * (iz2 - iz1));

                int yc_this = (int)(yoff - (s->ceil_height - player.z) * sc);
                int yf_this = (int)(yoff - (s->floor_height - player.z) * sc);
                int yc_next = (int)(yoff - (ns->ceil_height - player.z) * sc);
                int yf_next = (int)(yoff - (ns->floor_height - player.z) * sc);

                // Draw current sector ceiling & floor outside portal
                draw_vline(r, x, clip[x].top,    fmax(clip[x].top,    yc_this), s->ceil_color);
                draw_vline(r, x, fmin(clip[x].bottom, yf_this), clip[x].bottom, s->floor_color);

                // Draw portal frame / step
                if (yc_next > yc_this)
                    draw_vline(r, x, yc_this, yc_next, w->color);
                if (yf_next < yf_this)
                    draw_vline(r, x, yf_next, yf_this, w->color);

                // Portal opening = max(ceil), min(floor)
                int ptop = fmax(yc_this, yc_next);
                int pbot = fmin(yf_this, yf_next);

                child[x].top    = fmax(clip[x].top,    ptop);
                child[x].bottom = fmin(clip[x].bottom, pbot);

                if (child[x].top < child[x].bottom)
                    opening_visible = true;
            }

            if (opening_visible)
                render_sector(r, w->portal_to, child, depth + 1, sw, sh);

            // After portal recursion: block portal columns in parent
            for (int x = sx; x <= ex; x++) {
                if (x < clip[x].left || x > clip[x].right) continue;
                clip[x].top = clip[x].bottom;  // fully consumed
            }
        }
        else {
            // Solid wall
            for (int x = sx; x <= ex; x++) {
                if (x < clip[x].left || x > clip[x].right) continue;
                if (clip[x].top >= clip[x].bottom) continue;

                double t = (double)(x - x1) / (x2 - x1);
                double sc = proj * (iz1 + t * (iz2 - iz1));

                int yc = (int)(yoff - (s->ceil_height - player.z) * sc);
                int yf = (int)(yoff - (s->floor_height - player.z) * sc);

                draw_vline(r, x, clip[x].top,    yc,               s->ceil_color);
                draw_vline(r, x, fmax(clip[x].top, yc), fmin(clip[x].bottom, yf), w->color);
                draw_vline(r, x, yf,             clip[x].bottom,   s->floor_color);

                // Consume column
                clip[x].top = clip[x].bottom;
            }
        }
    }
}

// ────────────────────────────────────────────────
// Main
// ────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    SDL_Init(SDL_INIT_VIDEO);
    int w = 1024, h = 768;
    SDL_Window *win = SDL_CreateWindow("Portal Engine - Occlusion Improved", 100, 100, w, h, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetRelativeMouseMode(SDL_TRUE);

    init_map();

    ClipRange *root = malloc(sizeof(ClipRange) * w);
    for (int i = 0; i < w; i++) {
        root[i].top   = 0;
        root[i].bottom = h;
        root[i].left  = 0;
        root[i].right = w - 1;
    }

    while (1) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) goto cleanup;

        int mx, my;
        SDL_GetRelativeMouseState(&mx, &my);
        player.angle += mx * 0.002;
        player.pitch -= my * 0.002;
        player.pitch = fmax(-1.4, fmin(1.4, player.pitch)); // optional: limit pitch

        const Uint8 *k = SDL_GetKeyboardState(NULL);
        double speed = 0.12;
        double rot = player.angle;
        if (k[SDL_SCANCODE_W]) { player.x += cos(rot)*speed; player.y += sin(rot)*speed; }
        if (k[SDL_SCANCODE_S]) { player.x -= cos(rot)*speed; player.y -= sin(rot)*speed; }
        if (k[SDL_SCANCODE_A]) { player.x += sin(rot)*speed; player.y -= cos(rot)*speed; }
        if (k[SDL_SCANCODE_D]) { player.x -= sin(rot)*speed; player.y += cos(rot)*speed; }

        update_player_sector();

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        // Reset root clip every frame
        for (int i = 0; i < w; i++) {
            root[i].top    = 0;
            root[i].bottom = h;
            root[i].left   = 0;
            root[i].right  = w - 1;
        }

        render_sector(ren, player.sector_id, root, 0, w, h);

        SDL_RenderPresent(ren);
        SDL_Delay(10);
    }

cleanup:
    free(root);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
