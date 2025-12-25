#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>

#define SCREEN_W 960
#define SCREEN_H 720
#define FOV 90.0
#define MAX_SECTORS 32
#define MAX_WALLS 256
#define MAX_RECURSION 16

#define MINIMAP_X (SCREEN_W - 200 - 10)
#define MINIMAP_Y 10
#define MINIMAP_SIZE 200
#define MINIMAP_SCALE 10.0

typedef struct { double x, y; } Vec2;

typedef struct {
    Vec2 a, b;
    int portal_sector;
    Uint32 color;
} Wall;

typedef struct {
    int start_wall;
    int num_walls;
    float floor_h;
    float ceil_h;
    Uint32 floor_color;
    Uint32 ceil_color;
} Sector;

Sector sectors[MAX_SECTORS];
Wall walls[MAX_WALLS];
int num_sectors = 0;
int num_walls = 0;

typedef struct {
    double x, y, z;
    double angle;
    int current_sector;
} Player;

Player player;

typedef struct {
    int top, bottom;
} ClipRange;

void draw_minimap(SDL_Renderer *renderer);

void init_map() {
    sectors[0] = (Sector){0, 4, 0.0f, 3.0f, 0x808080FFU, 0x404060FFU};

    walls[0] = (Wall){{5,5}, {15,5}, -1, 0xFF0000FFU};   // Red front
    walls[1] = (Wall){{15,5}, {15,15}, -1, 0x00FF00FFU};  // Green right
    walls[2] = (Wall){{15,15}, {5,15}, -1, 0x0000FFFFU};  // Blue back
    walls[3] = (Wall){{5,15}, {5,5}, 1, 0xFFFF00FFU};     // Yellow portal left

    sectors[1] = (Sector){4, 4, 0.0f, 1.5f, 0x606000FFU, 0xA0A040FFU};

    walls[4] = (Wall){{5,15}, {5,5}, 0, 0xDDDD00FFU};
    walls[5] = (Wall){{5,5}, {10,5}, -1, 0xDD0000FFU};
    walls[6] = (Wall){{10,5}, {10,15}, -1, 0x00DD00FFU};
    walls[7] = (Wall){{10,15}, {5,15}, -1, 0x0000DDFFU};

    num_sectors = 2;
    num_walls = 8;

    player.x = 10.0;
    player.y = 13.0;
    player.z = 1.0;
    // Fixed: now angle 0 faces +X (right on minimap). To face the red wall (at y=5, i.e. negative Y direction),
    // we need to look down (negative Y) → angle = 3π/2 = 270°
    player.angle = 4.71238898038469;  // 270° — facing negative Y → toward red wall
    player.current_sector = 0;
}

void render_sector(SDL_Renderer *renderer, int sector_id, ClipRange clip[SCREEN_W], int recursion) {
    if (recursion > MAX_RECURSION) return;

    Sector *s = &sectors[sector_id];
    const double scale_factor = (SCREEN_W / 2.0) / tan(FOV * M_PI / 360.0);

    for (int i = 0; i < s->num_walls; i++) {
        Wall *w = &walls[s->start_wall + i];

        double dx1 = w->a.x - player.x;
        double dy1 = w->a.y - player.y;
        double dx2 = w->b.x - player.x;
        double dy2 = w->b.y - player.y;

        // FIXED ROTATION MATRIX — now matches movement and minimap perfectly
        // Angle 0 → facing positive X, increasing counterclockwise
        double vx1 = -dx1 * sin(player.angle) + dy1 * cos(player.angle);  // lateral (right positive)
        double vz1 =  dx1 * cos(player.angle) + dy1 * sin(player.angle);  // depth (forward positive)

        double vx2 = -dx2 * sin(player.angle) + dy2 * cos(player.angle);
        double vz2 =  dx2 * cos(player.angle) + dy2 * sin(player.angle);

        if (vz1 < 0.1 && vz2 < 0.1) continue;

        // Clip walls behind the player
        if (vz1 < 0.1) {
            double t = (0.1 - vz1) / (vz2 - vz1);
            if (t >= 0.0 && t <= 1.0) {
                vx1 += t * (vx2 - vx1);
                vz1 = 0.1;
            }
        }
        if (vz2 < 0.1) {
            double t = (0.1 - vz2) / (vz1 - vz2);
            if (t >= 0.0 && t <= 1.0) {
                vx2 += t * (vx1 - vx2);
                vz2 = 0.1;
            }
        }

        int x1 = (int)(SCREEN_W / 2.0 + vx1 * scale_factor / vz1);
        int x2 = (int)(SCREEN_W / 2.0 + vx2 * scale_factor / vz2);

        if (x1 == x2) continue;
        if (x2 < x1) {
            int tmp = x1; x1 = x2; x2 = tmp;
            double tmpv = vx1; vx1 = vx2; vx2 = tmpv;
            tmpv = vz1; vz1 = vz2; vz2 = tmpv;
        }

        if (x2 < 0 || x1 >= SCREEN_W) continue;
        x1 = x1 < 0 ? 0 : x1;
        x2 = x2 >= SCREEN_W ? SCREEN_W - 1 : x2;

        double inv_vz1 = 1.0 / vz1;
        double inv_vz2 = 1.0 / vz2;

        for (int x = x1; x <= x2; x++) {
            double screen_u = (double)(x - SCREEN_W / 2.0) / scale_factor;
            double denom = vx2 * inv_vz2 - vx1 * inv_vz1;
            if (fabs(denom) < 1e-8) continue;
            double u = (screen_u - vx1 * inv_vz1) / denom;
            u = fmax(0.0, fmin(1.0, u));

            double inv_vz = inv_vz1 * (1.0 - u) + inv_vz2 * u;
            double proj = scale_factor * inv_vz;

            int cy = (int)(SCREEN_H / 2.0 - (s->ceil_h - player.z) * proj);
            int fy = (int)(SCREEN_H / 2.0 - (s->floor_h - player.z) * proj);

            // Ceiling
            int ceil_bot = cy > clip[x].top ? cy : clip[x].top;
            if (ceil_bot > 0) {
                SDL_SetRenderDrawColor(renderer,
                    (s->ceil_color >> 24) & 0xFF,
                    (s->ceil_color >> 16) & 0xFF,
                    (s->ceil_color >> 8) & 0xFF, 255);
                SDL_RenderDrawLine(renderer, x, 0, x, ceil_bot - 1);
            }

            // Floor
            int floor_top = fy < clip[x].bottom ? fy : clip[x].bottom;
            if (floor_top < SCREEN_H - 1) {
                SDL_SetRenderDrawColor(renderer,
                    (s->floor_color >> 24) & 0xFF,
                    (s->floor_color >> 16) & 0xFF,
                    (s->floor_color >> 8) & 0xFF, 255);
                SDL_RenderDrawLine(renderer, x, floor_top + 1, x, SCREEN_H - 1);
            }

            // Wall
            int wall_top = cy > clip[x].top ? cy : clip[x].top;
            int wall_bot = fy < clip[x].bottom ? fy : clip[x].bottom;
            if (wall_top < wall_bot) {
                SDL_SetRenderDrawColor(renderer,
                    (w->color >> 24) & 0xFF,
                    (w->color >> 16) & 0xFF,
                    (w->color >> 8) & 0xFF, 255);
                SDL_RenderDrawLine(renderer, x, wall_top, x, wall_bot);
            }

            if (w->portal_sector == -1) {
                clip[x].top = cy > clip[x].top ? cy : clip[x].top;
                clip[x].bottom = fy < clip[x].bottom ? fy : clip[x].bottom;
            }
        }

        // Portal recursion
        if (w->portal_sector != -1) {
            ClipRange newclip[SCREEN_W];
            for (int j = 0; j < SCREEN_W; j++) newclip[j] = clip[j];

            for (int x = x1; x <= x2; x++) {
                double screen_u = (double)(x - SCREEN_W / 2.0) / scale_factor;
                double denom = vx2 * inv_vz2 - vx1 * inv_vz1;
                if (fabs(denom) < 1e-8) continue;
                double u = (screen_u - vx1 * inv_vz1) / denom;
                u = fmax(0.0, fmin(1.0, u));

                double inv_vz = inv_vz1 * (1.0 - u) + inv_vz2 * u;
                double proj = scale_factor * inv_vz;

                int cy = (int)(SCREEN_H / 2.0 - (s->ceil_h - player.z) * proj);
                int fy = (int)(SCREEN_H / 2.0 - (s->floor_h - player.z) * proj);

                newclip[x].top = cy > newclip[x].top ? cy : newclip[x].top;
                newclip[x].bottom = fy < newclip[x].bottom ? fy : newclip[x].bottom;
            }

            render_sector(renderer, w->portal_sector, newclip, recursion + 1);
        }
    }
}

void draw_minimap(SDL_Renderer *renderer) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_Rect bg = { MINIMAP_X - 5, MINIMAP_Y - 5, MINIMAP_SIZE + 10, MINIMAP_SIZE + 10 };
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &bg);

    for (int i = 0; i < num_walls; i++) {
        Wall *w = &walls[i];
        int x1 = MINIMAP_X + (int)(w->a.x * MINIMAP_SCALE);
        int y1 = MINIMAP_Y + (int)(w->a.y * MINIMAP_SCALE);
        int x2 = MINIMAP_X + (int)(w->b.x * MINIMAP_SCALE);
        int y2 = MINIMAP_Y + (int)(w->b.y * MINIMAP_SCALE);

        SDL_SetRenderDrawColor(renderer,
            w->portal_sector != -1 ? 255 : 100,
            w->portal_sector != -1 ? 255 : 100,
            w->portal_sector != -1 ? 0 : 100, 255);
        SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
    }

    int px = MINIMAP_X + (int)(player.x * MINIMAP_SCALE);
    int py = MINIMAP_Y + (int)(player.y * MINIMAP_SCALE);

    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    for (int dx = -3; dx <= 3; dx++) {
        for (int dy = -3; dy <= 3; dy++) {
            if (dx*dx + dy*dy <= 9) SDL_RenderDrawPoint(renderer, px + dx, py + dy);
        }
    }

    int dir_len = 15;
    int dx = (int)(cos(player.angle) * dir_len);
    int dy = (int)(sin(player.angle) * dir_len);
    SDL_RenderDrawLine(renderer, px, py, px + dx, py + dy);
}

int main(int argc, char *argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Portal Engine - FIXED ORIENTATION",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window) {
        printf("Window Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        printf("Renderer Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    init_map();

    ClipRange clip[SCREEN_W];
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
        }

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        double move_speed = 0.3;
        double rot_speed = 0.08;

        if (keys[SDL_SCANCODE_W]) {
            player.x += cos(player.angle) * move_speed;
            player.y += sin(player.angle) * move_speed;
        }
        if (keys[SDL_SCANCODE_S]) {
            player.x -= cos(player.angle) * move_speed;
            player.y -= sin(player.angle) * move_speed;
        }
        if (keys[SDL_SCANCODE_A]) player.angle -= rot_speed;
        if (keys[SDL_SCANCODE_D]) player.angle += rot_speed;

        for (int i = 0; i < SCREEN_W; i++) {
            clip[i].top = 0;
            clip[i].bottom = SCREEN_H - 1;
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        render_sector(renderer, player.current_sector, clip, 0);

        draw_minimap(renderer);

        SDL_RenderPresent(renderer);

        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
