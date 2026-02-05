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

void render_sector(SDL_Renderer *r, int sid, ClipRange *clip, int depth, int sw, int sh) {
    if (depth > MAX_RECURSION_DEPTH) return;

    Sector *s = &sectors[sid];
    double proj = (sw / 2.0) / tan(FOV_DEG * M_PI / 360.0);
    double yoff = sh / 2.0 + player.pitch * (sh / 2.0);

    for (int i = 0; i < s->wall_count; i++) {
        Wall *w = &walls[s->first_wall + i];

        // View transformation
        double dx1 = w->p1.x - player.x, dy1 = w->p1.y - player.y;
        double dx2 = w->p2.x - player.x, dy2 = w->p2.y - player.y;

        double vx1 = dx1 * sin(player.angle) - dy1 * cos(player.angle);
        double vz1 = dx1 * cos(player.angle) + dy1 * sin(player.angle);
        double vx2 = dx2 * sin(player.angle) - dy2 * cos(player.angle);
        double vz2 = dx2 * cos(player.angle) + dy2 * sin(player.angle);

        if (vz1 <= 0 && vz2 <= 0) continue;

        // Clip to NEAR_PLANE
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
        if (x1 >= x2) continue;

        int sx = fmax(0, x1), ex = fmin(sw - 1, x2);
        double iz1 = 1.0 / vz1, iz2 = 1.0 / vz2;

        // Prepare child clip for portal recursion
        ClipRange child_clip[1024];
        bool portal_visible = false;

        for (int x = sx; x <= ex; x++) {
            if (clip[x].top >= clip[x].bottom) continue;

            double t = (double)(x - x1) / (x2 - x1);
            double sc = proj * (iz1 + t * (iz2 - iz1));

            int yc = (int)(yoff - (s->ceil_height - player.z) * sc);
            int yf = (int)(yoff - (s->floor_height - player.z) * sc);

            if (w->portal_to >= 0) {
                Sector *ns = &sectors[w->portal_to];
                int nyc = (int)(yoff - (ns->ceil_height - player.z) * sc);
                int nyf = (int)(yoff - (ns->floor_height - player.z) * sc);

                // Current sector ceiling/floor
                draw_vline(r, x, clip[x].top, fmax(clip[x].top, yc), s->ceil_color);
                draw_vline(r, x, fmin(clip[x].bottom, yf), clip[x].bottom, s->floor_color);

                // Portal lips (wall between height differences)
                draw_vline(r, x, fmax(clip[x].top, yc), fmin(clip[x].bottom, nyc), w->color);
                draw_vline(r, x, fmax(clip[x].top, nyf), fmin(clip[x].bottom, yf), w->color);

                // Shrink child window
                child_clip[x].top = fmax(clip[x].top, fmax(yc, nyc));
                child_clip[x].bottom = fmin(clip[x].bottom, fmin(yf, nyf));
                if (child_clip[x].top < child_clip[x].bottom) portal_visible = true;
            } else {
                // Solid wall
                draw_vline(r, x, clip[x].top, fmax(clip[x].top, yc), s->ceil_color);
                draw_vline(r, x, fmin(clip[x].bottom, yf), clip[x].bottom, s->floor_color);
                draw_vline(r, x, fmax(clip[x].top, yc), fmin(clip[x].bottom, yf), w->color);
                
                // Solid wall consumes the vertical space
                clip[x].top = clip[x].bottom;
            }
        }

        if (w->portal_to >= 0 && portal_visible) {
            render_sector(r, w->portal_to, child_clip, depth + 1, sw, sh);
            // After returning from recursion, we must update the current clip 
            // so subsequent walls in THIS sector don't overdraw the portal
            for(int x = sx; x <= ex; x++) {
                // If a portal was here, the "available" space for the rest of the 
                // walls in this sector is now blocked by whatever was in the portal.
                // However, in a standard portal engine, we usually just update clip 
                // based on the portal boundaries.
                clip[x].top = fmax(clip[x].top, child_clip[x].top); // This is simplified
                clip[x].bottom = fmin(clip[x].bottom, child_clip[x].bottom);
                // But for perfect occlusion, we mark the portal area as used:
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
