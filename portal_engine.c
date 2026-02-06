#include <SDL2/SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FOV_DEG          90.0
#define MAX_RECURSION_DEPTH 8
#define NEAR_PLANE       0.01

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

typedef struct {
    int top;             // Nothing above this is visible (standard clip)
    int bottom;          // Nothing below this is visible (standard clip)
    int highest_ceil_y;  // Ceiling should not be drawn above this y
    int lowest_floor_y;  // Floor should not be drawn below this y
} ColumnClip;

// Globals
Sector sectors[32];
Wall walls[1024];
Player player;

// ---------------------------------------------------
// Helpers
// ---------------------------------------------------
double point_side(double px, double py, Vec2 a, Vec2 b) {
    return (b.x - a.x)*(py - a.y) - (b.y - a.y)*(px - a.x);
}

static inline void draw_vline(Uint32 *pixels, int pitch, int x, int y1, int y2, Uint32 color, int height) {
    if (y1 >= y2) return;
    if (y1 < 0) y1 = 0;
    if (y2 > height) y2 = height;
    if (y1 >= y2) return;
    Uint32 *dst = pixels + y1 * pitch + x;
    for (int y = y1; y < y2; y++) {
        *dst = color;
        dst += pitch;
    }
}

// ---------------------------------------------------
// Map
// ---------------------------------------------------
void init_map(void) {
    int wi = 0;

    // Sector 0: Hub
    sectors[0] = (Sector){0, 5, 0.0f, 3.0f, 0x333333FF, 0x111111FF};
    walls[wi++] = (Wall){{-5, -5}, { 5, -5}, -1, 0xFF0000FF};
    walls[wi++] = (Wall){{ 5, -5}, { 5, 5}, 1, 0x00FF00FF};
    walls[wi++] = (Wall){{ 5, 5}, {-5, 5}, 2, 0x0000FFFF};
    walls[wi++] = (Wall){{-5, 5}, {-5, -2}, -1, 0xFFFF00FF};
    walls[wi++] = (Wall){{-5, -2}, {-5, -5}, 4, 0xAA00AAFF};

    // Sector 1: Sunken Pit
    sectors[1] = (Sector){wi, 4, -1.0f, 2.0f, 0x222244FF, 0x111122FF};
    walls[wi++] = (Wall){{ 5, -5}, {15, -5}, -1, 0x00FFFFFF};
    walls[wi++] = (Wall){{15, -5}, {15, 5}, -1, 0xFF8800FF};
    walls[wi++] = (Wall){{15, 5}, { 5, 5}, -1, 0xFFFFFFFF};
    walls[wi++] = (Wall){{ 5, 5}, { 5, -5}, 0, 0x00FF00FF};

    // Sector 2: Corridor
    sectors[2] = (Sector){wi, 4, 0.0f, 2.5f, 0x444444FF, 0x222222FF};
    walls[wi++] = (Wall){{ 5, 5}, { 5, 15}, -1, 0x888888FF};
    walls[wi++] = (Wall){{ 5, 15}, {-5, 15}, 3, 0x00FF00FF};
    walls[wi++] = (Wall){{-5, 15}, {-5, 5}, -1, 0x888888FF};
    walls[wi++] = (Wall){{-5, 5}, { 5, 5}, 0, 0x0000FFFF};

    // Sector 3: Crawlspace
    sectors[3] = (Sector){wi, 4, 0.5f, 1.2f, 0x111111FF, 0x050505FF};
    walls[wi++] = (Wall){{-5, 15}, { 5, 15}, 2, 0x00FF00FF};
    walls[wi++] = (Wall){{ 5, 15}, { 5, 20}, -1, 0x440000FF};
    walls[wi++] = (Wall){{ 5, 20}, {-5, 20}, -1, 0x440000FF};
    walls[wi++] = (Wall){{-5, 20}, {-5, 15}, -1, 0x440000FF};

    // Sector 4: Raised Deck
    sectors[4] = (Sector){wi, 4, 1.0f, 5.0f, 0x664422FF, 0x221100FF};
    walls[wi++] = (Wall){{-5, -5}, {-5, -2}, 0, 0xAA00AAFF};
    walls[wi++] = (Wall){{-5, -2}, {-10, -2}, -1, 0x00FF00FF};
    walls[wi++] = (Wall){{-10,-2}, {-10, -5}, -1, 0x00FF00FF};
    walls[wi++] = (Wall){{-10,-5}, { -5, -5}, -1, 0x00FF00FF};

    player = (Player){0.0, 0.0, 1.5, 0.0, 0.0, 0};
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

// ---------------------------------------------------
// Renderer
// ---------------------------------------------------
void render_sector(Uint32 *pixels, int pitch,
                   int sid,
                   ColumnClip *clip,        // modified in place
                   ColumnClip *portal_clip, // scratch buffer
                   int depth,
                   int sw, int sh)
{
    if (depth > MAX_RECURSION_DEPTH) return;

    Sector *s = &sectors[sid];
    const double proj = (sw / 2.0) / tan(FOV_DEG * M_PI / 360.0);
    const double yoff = sh / 2.0 + player.pitch * (sh / 2.0);

    for (int i = 0; i < s->wall_count; i++)
    {
        Wall *w = &walls[s->first_wall + i];

        // View space transformation
        double dx1 = w->p1.x - player.x, dy1 = w->p1.y - player.y;
        double dx2 = w->p2.x - player.x, dy2 = w->p2.y - player.y;

        double vx1 =  dx1 * cos(player.angle) + dy1 * sin(player.angle);
        double vz1 = -dx1 * sin(player.angle) + dy1 * cos(player.angle);
        double vx2 =  dx2 * cos(player.angle) + dy2 * sin(player.angle);
        double vz2 = -dx2 * sin(player.angle) + dy2 * cos(player.angle);

        if (vz1 <= 0 && vz2 <= 0) continue;

        // Near plane clip
        if (vz1 < NEAR_PLANE) {
            double t = (NEAR_PLANE - vz1) / (vz2 - vz1);
            vx1 += t * (vx2 - vx1);
            vz1 = NEAR_PLANE;
        }
        if (vz2 < NEAR_PLANE) {
            double t = (NEAR_PLANE - vz2) / (vz1 - vz2);
            vx2 += t * (vx1 - vx2);
            vz2 = NEAR_PLANE;
        }

        // Screen space projection
        int x1 = (int)(sw / 2.0 - vx1 * proj / vz1);
        int x2 = (int)(sw / 2.0 - vx2 * proj / vz2);
        if (x1 >= x2) continue;

        int sx = (x1 > 0) ? x1 : 0;
        int ex = (x2 < sw) ? x2 : (sw - 1);

        double iz1 = 1.0 / vz1;
        double iz2 = 1.0 / vz2;

        bool portal_visible = false;

        for (int x = sx; x <= ex; x++)
        {
            ColumnClip *c = &clip[x];
            if (c->top >= c->bottom) continue;

            double t = (double)(x - x1) / (x2 - x1);
            double iz = iz1 + t * (iz2 - iz1);
            double sc = proj * iz;

            int yc = (int)(yoff - (s->ceil_height - player.z) * sc);
            int yf = (int)(yoff - (s->floor_height - player.z) * sc);

            // Respect accumulated occlusion
            yc = fmax(yc, c->highest_ceil_y);
            yf = fmin(yf, c->lowest_floor_y);

            if (w->portal_to >= 0)
            {
                Sector *ns = &sectors[w->portal_to];
                int nyc = (int)(yoff - (ns->ceil_height - player.z) * sc);
                int nyf = (int)(yoff - (ns->floor_height - player.z) * sc);

                // Current sector ceiling
                draw_vline(pixels, pitch, x, c->top, fmin(c->bottom, yc), s->ceil_color, sh);

                // Current sector floor
                draw_vline(pixels, pitch, x, fmax(c->top, yf), c->bottom, s->floor_color, sh);

                // Upper step / lip (between ceilings)
                int upper_top    = fmax(c->top,   yc);
                int upper_bottom = fmin(c->bottom, nyc);
                draw_vline(pixels, pitch, x, upper_top, upper_bottom, w->color, sh);

                // Lower step / lip (between floors)
                int lower_top    = fmax(c->top,   nyf);
                int lower_bottom = fmin(c->bottom, yf);
                draw_vline(pixels, pitch, x, lower_top, lower_bottom, w->color, sh);

                // Prepare child clipping
                portal_clip[x] = *c; // inherit parent limits

                // Tighten vertical occlusion for child sector
                portal_clip[x].highest_ceil_y = fmax(c->highest_ceil_y, fmax(yc, nyc));
                portal_clip[x].lowest_floor_y = fmin(c->lowest_floor_y, fmin(yf, nyf));

                // Horizontal portal window (same as before)
                portal_clip[x].top    = fmax(c->top,    fmax(yc, nyc));
                portal_clip[x].bottom = fmin(c->bottom, fmin(yf, nyf));

                if (portal_clip[x].top < portal_clip[x].bottom)
                    portal_visible = true;
            }
            else // solid wall
            {
                // Ceiling
                draw_vline(pixels, pitch, x, c->top, fmin(c->bottom, yc), s->ceil_color, sh);
                // Floor
                draw_vline(pixels, pitch, x, fmax(c->top, yf), c->bottom, s->floor_color, sh);
                // Wall itself
                draw_vline(pixels, pitch, x, fmax(c->top, yc), fmin(c->bottom, yf), w->color, sh);

                // Solid wall fully occludes the column
                c->top = c->bottom;
            }
        }

        // Recurse into portal if visible
        if (w->portal_to >= 0 && portal_visible)
        {
            render_sector(pixels, pitch, w->portal_to,
                          portal_clip, portal_clip,
                          depth + 1, sw, sh);

            // After child render — update parent occlusion with what was seen through portal
            for (int x = sx; x <= ex; x++) {
                if (portal_clip[x].top < portal_clip[x].bottom) {
                    clip[x].highest_ceil_y = fmax(clip[x].highest_ceil_y, portal_clip[x].highest_ceil_y);
                    clip[x].lowest_floor_y = fmin(clip[x].lowest_floor_y, portal_clip[x].lowest_floor_y);
                }
            }
        }
    }
}

// ---------------------------------------------------
// Main
// ---------------------------------------------------
int main(int argc, char *argv[])
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int w, h;
    SDL_Window *win = SDL_CreateWindow(
        "Portal Engine – Fixed Vertical Occlusion",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        800, 600,
        SDL_WINDOW_FULLSCREEN_DESKTOP
    );
    if (!win) goto cleanup;

    SDL_GetWindowSize(win, &w, &h);

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) goto cleanup;

    SDL_Texture *target = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING, w, h
    );
    if (!target) goto cleanup;

    // Allocate clip buffers
    ColumnClip *root_clip   = malloc(sizeof(ColumnClip) * (size_t)w);
    ColumnClip *portal_clip = malloc(sizeof(ColumnClip) * (size_t)w);
    if (!root_clip || !portal_clip) {
        fprintf(stderr, "Failed to allocate clip buffers\n");
        goto cleanup;
    }

    SDL_SetRelativeMouseMode(SDL_TRUE);
    init_map();

    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN &&
                (e.key.keysym.mod & KMOD_ALT))
            {
                Uint32 flags = SDL_GetWindowFlags(win);
                SDL_SetWindowFullscreen(
                    win,
                    (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP
                );
            }
        }

        int mx, my;
        SDL_GetRelativeMouseState(&mx, &my);
        player.angle += mx * 0.002;
        player.pitch -= my * 0.002;
        player.pitch = fmax(-1.4, fmin(1.4, player.pitch));

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        double speed = 0.12;
        double rot = player.angle;

        if (keys[SDL_SCANCODE_E]) player.z += speed;
        if (keys[SDL_SCANCODE_Q]) player.z -= speed;
        if (keys[SDL_SCANCODE_W]) {
            player.x -= sin(rot) * speed;
            player.y += cos(rot) * speed;
        }
        if (keys[SDL_SCANCODE_S]) {
            player.x += sin(rot) * speed;
            player.y -= cos(rot) * speed;
        }
        if (keys[SDL_SCANCODE_A]) {
            player.x += cos(rot) * speed;
            player.y += sin(rot) * speed;
        }
        if (keys[SDL_SCANCODE_D]) {
            player.x -= cos(rot) * speed;
            player.y -= sin(rot) * speed;
        }

        update_player_sector();

        // Lock texture
        void *pixels_raw;
        int pitch;
        if (SDL_LockTexture(target, NULL, &pixels_raw, &pitch) < 0) {
            fprintf(stderr, "Lock failed: %s\n", SDL_GetError());
            break;
        }

        Uint32 *pixels = (Uint32 *)pixels_raw;
        int pixel_pitch = pitch / 4;

        // Clear to black
        memset(pixels, 0, (size_t)h * (size_t)pitch);

        // Initialize root clipping
        for (int i = 0; i < w; i++) {
            root_clip[i].top           = 0;
            root_clip[i].bottom        = h;
            root_clip[i].highest_ceil_y = 0;     // can draw ceiling to top
            root_clip[i].lowest_floor_y = h;     // can draw floor to bottom
        }

        // Render from current sector
        render_sector(pixels, pixel_pitch,
                      player.sector_id,
                      root_clip,
                      portal_clip,
                      0, w, h);

        SDL_UnlockTexture(target);

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, target, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    free(root_clip);
    free(portal_clip);

cleanup:
    if (target) SDL_DestroyTexture(target);
    if (ren)    SDL_DestroyRenderer(ren);
    if (win)    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
