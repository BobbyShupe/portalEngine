#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define FOV_DEG          90.0
#define MAX_RECURSION_DEPTH 8
#define NEAR_PLANE       0.01
#define MAX_SECTORS      64
#define MAX_WALLS       2048
#define TEX_SCALE        0.25  // Adjust as needed: higher value = denser tiling

typedef struct { double x, y; } Vec2;

typedef struct {
    Vec2 p1, p2;
    int portal_to;
    Uint32 color;
    SDL_Surface *tex;
    double length;
} Wall;

typedef struct {
    int first_wall;
    int wall_count;
    float floor_height, ceil_height;
    Uint32 floor_color, ceil_color;
    SDL_Surface *floor_tex;
    SDL_Surface *ceil_tex;
} Sector;

typedef struct {
    double x, y, z;
    double angle, pitch;
    int sector_id;
} Player;

typedef struct {
    int top;
    int bottom;
    int highest_ceil_y;
    int lowest_floor_y;
} ColumnClip;

typedef struct {
    SDL_Surface **textures;
    int count;
    int capacity;
} TextureList;

// Globals
Sector sectors[MAX_SECTORS];
Wall   walls[MAX_WALLS];
int    sector_count = 0;
int    wall_count   = 0;
Player player;

// ---------------------------------------------------
// Helpers
// ---------------------------------------------------
double point_side(double px, double py, Vec2 a, Vec2 b) {
    return (b.x - a.x)*(py - a.y) - (b.y - a.y)*(px - a.x);
}

static inline Uint32 get_tex_pixel(SDL_Surface *tex, int tx, int ty) {
    int w = tex->w;
    int h = tex->h;
    tx %= w;
    if (tx < 0) tx += w;
    ty %= h;
    if (ty < 0) ty += h;
    Uint32 *pixels = (Uint32 *)tex->pixels;
    return pixels[ty * (tex->pitch / 4) + tx];
}

static inline void draw_solid_vline(Uint32 *pixels, int pitch, int x, int y1, int y2, Uint32 color, int height) {
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

void draw_wall_vline(Uint32 *pixels, int pitch, int x, int y1, int y2, SDL_Surface *tex, double along_dist, double sc, double yoff, double player_z, double floor_h, double ceil_h, int height) {
    if (y1 >= y2) return;
    if (y1 < 0) y1 = 0;
    if (y2 > height) y2 = height;
    if (y1 >= y2) return;
    int tx = (int)(along_dist * TEX_SCALE * tex->w) % tex->w;
    if (tx < 0) tx += tex->w;
    Uint32 *dst = pixels + y1 * pitch + x;
    for (int y = y1; y < y2; y++) {
        double hit_h = player_z + (yoff - y) / sc;
        int ty = (int)((hit_h - floor_h) * TEX_SCALE * tex->h) % tex->h;
        if (ty < 0) ty += tex->h;
        *dst = get_tex_pixel(tex, tx, ty);
        dst += pitch;
    }
}

void draw_plane_vline(Uint32 *pixels, int pitch, int x, int y1, int y2,
                      SDL_Surface *tex,
                      double proj,
                      double yoff,
                      double player_x, double player_y, double player_angle,
                      double player_z, double plane_h, int height)
{
    if (y1 >= y2) return;
    if (y1 < 0) y1 = 0;
    if (y2 > height) y2 = height;
    if (y1 >= y2) return;

    // ────────────────────────────────────────────────
    // Fixed: compute view-space ray direction **once**
    //        (screen column → world direction)
    // ────────────────────────────────────────────────
    double screen_center_x = height / 2.0;           // normally width/2, typo in original!
	double ray_dx_screen = (x - screen_center_x) / proj;
    double ray_dz_screen   = 1.0;                             // forward

    // Rotate to world space — do this **once per column**, not per pixel
    double dir_x = ray_dx_screen * (-sin(player_angle)) + cos(player_angle);
                 
    double dir_y = ray_dx_screen * ( cos(player_angle)) + sin(player_angle);
    // Optional: normalize if you want constant texel density (usually not needed)
    // double len = hypot(dir_x, dir_y); if (len > 0) { dir_x /= len; dir_y /= len; }

    Uint32 *dst = pixels + y1 * pitch + x;

    for (int y = y1; y < y2; y++)
    {
        double div = (y - yoff);
        if (fabs(div) < 1e-6) {
            *dst = 0; // or some fallback color
            dst += pitch;
            continue;
        }

        // Distance along ray to plane
        double dist = (player_z - plane_h) / div * proj;

        if (dist <= 0) {    // behind camera or exactly on plane
            *dst = 0xFF000000; // black / transparent fallback
            dst += pitch;
            continue;
        }

        double hit_x = player_x + dist * dir_x;
        double hit_y = player_y + dist * dir_y;

        // Texture coordinates — now stable regardless of rotation
        int tx = (int)(hit_x * TEX_SCALE * tex->w) % tex->w;
        int ty = (int)(hit_y * TEX_SCALE * tex->h) % tex->h;

        if (tx < 0) tx += tex->w;
        if (ty < 0) ty += tex->h;

        *dst = get_tex_pixel(tex, tx, ty);
        dst += pitch;
    }
}

void init_texture_list(TextureList *list) {
    list->textures = NULL;
    list->count = 0;
    list->capacity = 0;
}

void add_texture(TextureList *list, SDL_Surface *tex) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 16;
        list->textures = realloc(list->textures, sizeof(SDL_Surface*) * list->capacity);
    }
    list->textures[list->count++] = tex;
}

void free_texture_list(TextureList *list) {
    for (int i = 0; i < list->count; i++) {
        if (list->textures[i]) SDL_FreeSurface(list->textures[i]);
    }
    free(list->textures);
}

void load_textures_recursive(const char *dir, TextureList *list) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            load_textures_recursive(path, list);
        } else {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".bmp") == 0)) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
                SDL_Surface *tex = IMG_Load(path);
                if (tex) {
                    add_texture(list, tex);
                } else {
                    fprintf(stderr, "Failed to load texture: %s - %s\n", path, IMG_GetError());
                }
            }
        }
    }
    closedir(d);
}

bool load_map(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Cannot open map file: %s\n", filename);
        return false;
    }

    // Reset globals
    sector_count = 0;
    wall_count   = 0;

    // Track which sectors actually exist
    bool sector_defined[MAX_SECTORS] = {0};

    // Initialize sectors safely
    for (int i = 0; i < MAX_SECTORS; i++) {
        sectors[i].first_wall = -1;
        sectors[i].wall_count = 0;
        sectors[i].floor_tex = NULL;
        sectors[i].ceil_tex = NULL;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == '#')
            continue;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* ---------------- SECTOR ---------------- */
        if (strncmp(p, "sector", 6) == 0) {
            int id;
            float fh, ch;
            unsigned long long fc, cc;

            if (sscanf(p, "sector %d %f %f 0x%llx 0x%llx",
                       &id, &fh, &ch, &fc, &cc) != 5)
            {
                fprintf(stderr, "Bad sector line: %s", line);
                fclose(f);
                return false;
            }

            if (id < 0 || id >= MAX_SECTORS) {
                fprintf(stderr, "Invalid sector id %d\n", id);
                fclose(f);
                return false;
            }

            sectors[id].floor_height = fh;
            sectors[id].ceil_height  = ch;
            sectors[id].floor_color  = (Uint32)fc;
            sectors[id].ceil_color   = (Uint32)cc;

            sector_defined[id] = true;
            if (id + 1 > sector_count)
                sector_count = id + 1;
        }

        /* ---------------- WALL ---------------- */
        else if (strncmp(p, "wall", 4) == 0) {
            int sec, portal;
            double x1, y1, x2, y2;
            unsigned long long color;

            if (sscanf(p, "wall %d %lf %lf %lf %lf %d 0x%llx",
                       &sec, &x1, &y1, &x2, &y2, &portal, &color) != 7)
            {
                fprintf(stderr, "Bad wall line: %s", line);
                fclose(f);
                return false;
            }

            if (sec < 0 || sec >= MAX_SECTORS || !sector_defined[sec]) {
                fprintf(stderr, "Wall references undefined sector %d\n", sec);
                fclose(f);
                return false;
            }

            if (portal >= 0 &&
                (portal >= MAX_SECTORS || !sector_defined[portal]))
            {
                fprintf(stderr, "Invalid portal target %d\n", portal);
                fclose(f);
                return false;
            }

            if (wall_count >= MAX_WALLS) {
                fprintf(stderr, "Too many walls\n");
                fclose(f);
                return false;
            }

            // First wall for this sector?
            if (sectors[sec].first_wall < 0)
                sectors[sec].first_wall = wall_count;

            walls[wall_count] = (Wall){
                {x1, y1}, {x2, y2},
                portal,
                (Uint32)color,
                NULL,
                hypot(x2 - x1, y2 - y1)
            };

            wall_count++;
            sectors[sec].wall_count++;
        }

        /* ---------------- PLAYER ---------------- */
        else if (strncmp(p, "player", 6) == 0) {
            double x, y, z, ang, pit;
            int sec;

            if (sscanf(p, "player %lf %lf %lf %lf %lf %d",
                       &x, &y, &z, &ang, &pit, &sec) != 6)
            {
                fprintf(stderr, "Bad player line: %s", line);
                fclose(f);
                return false;
            }

            player.x = x;
            player.y = y;
            player.z = z;
            player.angle = ang;
            player.pitch = pit;
            player.sector_id = sec;
        }
    }

    fclose(f);

    /* --------- FINAL VALIDATION --------- */

    if (sector_count == 0) {
        fprintf(stderr, "No sectors defined\n");
        return false;
    }

    if (player.sector_id < 0 ||
        player.sector_id >= sector_count ||
        !sector_defined[player.sector_id])
    {
        fprintf(stderr, "Player starts in invalid sector %d\n",
                player.sector_id);
        return false;
    }

    printf("Loaded %d sectors, %d walls\n", sector_count, wall_count);
    return true;
}

void update_player_sector(void) {
    Sector *s = &sectors[player.sector_id];
    for (int i = 0; i < s->wall_count; i++) {
        Wall *w = &walls[s->first_wall + i];
        if (w->portal_to >= 0 && point_side(player.x, player.y, w->p1, w->p2) < 0) {
            player.sector_id = w->portal_to;
            return;  // simple — can be improved to handle stacked sectors later
        }
    }
}

// ---------------------------------------------------
// Renderer
// ---------------------------------------------------
void render_sector(Uint32 *pixels, int pitch,
                   int sid,
                   ColumnClip *clip,
                   ColumnClip *portal_clip,
                   int depth,
                   int sw, int sh)
{
    if (depth > MAX_RECURSION_DEPTH) return;
    if (sid < 0 || sid >= sector_count) return;

    Sector *s = &sectors[sid];
    const double proj = (sw / 2.0) / tan(FOV_DEG * M_PI / 360.0);
    const double yoff = sh / 2.0 + player.pitch * (sh / 2.0);

    for (int i = 0; i < s->wall_count; i++)
    {
        Wall *w = &walls[s->first_wall + i];

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

            yc = fmax(yc, c->highest_ceil_y);
            yf = fmin(yf, c->lowest_floor_y);

            if (w->portal_to >= 0)
            {
                Sector *ns = &sectors[w->portal_to];
                int nyc = (int)(yoff - (ns->ceil_height - player.z) * sc);
                int nyf = (int)(yoff - (ns->floor_height - player.z) * sc);

                // Draw ceiling
                if (s->ceil_tex)
                    draw_plane_vline(pixels, pitch, x, c->top, fmin(c->bottom, yc), s->ceil_tex, proj, yoff, player.x, player.y, player.angle, player.z, s->ceil_height, sh);
                else
                    draw_solid_vline(pixels, pitch, x, c->top, fmin(c->bottom, yc), s->ceil_color, sh);

                // Draw floor
                if (s->floor_tex)
                    draw_plane_vline(pixels, pitch, x, fmax(c->top, yf), c->bottom, s->floor_tex, proj, yoff, player.x, player.y, player.angle, player.z, s->floor_height, sh);
                else
                    draw_solid_vline(pixels, pitch, x, fmax(c->top, yf), c->bottom, s->floor_color, sh);

                // Draw upper wall
                int upper_top    = fmax(c->top,   yc);
                int upper_bottom = fmin(c->bottom, nyc);
                if (w->tex)
                    draw_wall_vline(pixels, pitch, x, upper_top, upper_bottom, w->tex, t * w->length, sc, yoff, player.z, s->floor_height, s->ceil_height, sh);
                else
                    draw_solid_vline(pixels, pitch, x, upper_top, upper_bottom, w->color, sh);

                // Draw lower wall
                int lower_top    = fmax(c->top,   nyf);
                int lower_bottom = fmin(c->bottom, yf);
                if (w->tex)
                    draw_wall_vline(pixels, pitch, x, lower_top, lower_bottom, w->tex, t * w->length, sc, yoff, player.z, s->floor_height, s->ceil_height, sh);
                else
                    draw_solid_vline(pixels, pitch, x, lower_top, lower_bottom, w->color, sh);

                portal_clip[x] = *c;

                portal_clip[x].highest_ceil_y = fmax(c->highest_ceil_y, fmax(yc, nyc));
                portal_clip[x].lowest_floor_y = fmin(c->lowest_floor_y, fmin(yf, nyf));

                portal_clip[x].top    = fmax(c->top,    fmax(yc, nyc));
                portal_clip[x].bottom = fmin(c->bottom, fmin(yf, nyf));

                if (portal_clip[x].top < portal_clip[x].bottom)
                    portal_visible = true;
            }
            else
            {
                // Draw ceiling
                if (s->ceil_tex)
                    draw_plane_vline(pixels, pitch, x, c->top, fmin(c->bottom, yc), s->ceil_tex, proj, yoff, player.x, player.y, player.angle, player.z, s->ceil_height, sh);
                else
                    draw_solid_vline(pixels, pitch, x, c->top, fmin(c->bottom, yc), s->ceil_color, sh);

                // Draw floor
                if (s->floor_tex)
                    draw_plane_vline(pixels, pitch, x, fmax(c->top, yf), c->bottom, s->floor_tex, proj, yoff, player.x, player.y, player.angle, player.z, s->floor_height, sh);
                else
                    draw_solid_vline(pixels, pitch, x, fmax(c->top, yf), c->bottom, s->floor_color, sh);

                // Draw wall
                int wall_top = fmax(c->top, yc);
                int wall_bottom = fmin(c->bottom, yf);
                if (w->tex)
                    draw_wall_vline(pixels, pitch, x, wall_top, wall_bottom, w->tex, t * w->length, sc, yoff, player.z, s->floor_height, s->ceil_height, sh);
                else
                    draw_solid_vline(pixels, pitch, x, wall_top, wall_bottom, w->color, sh);

                c->top = c->bottom;  // occlude
            }
        }

        if (w->portal_to >= 0 && portal_visible)
        {
            render_sector(pixels, pitch, w->portal_to,
                          portal_clip, portal_clip,
                          depth + 1, sw, sh);

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

    if (!(IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG))) {
        fprintf(stderr, "IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return 1;
    }

    int w, h;
    SDL_Window *win = SDL_CreateWindow(
        "Portal Engine – Map from file",
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
        ren, SDL_PIXELFORMAT_ABGR8888,           // ← most common fix on little-endian systems
        SDL_TEXTUREACCESS_STREAMING, w, h
    );
    if (!target) goto cleanup;

    ColumnClip *root_clip   = malloc(sizeof(ColumnClip) * (size_t)w);
    ColumnClip *portal_clip = malloc(sizeof(ColumnClip) * (size_t)w);
    if (!root_clip || !portal_clip) goto cleanup;

    const char *mapfile = (argc > 1) ? argv[1] : "map.txt";

    if (!load_map(mapfile)) {
        fprintf(stderr, "Failed to load map\n");
        goto cleanup;
    }

    TextureList textures;
    init_texture_list(&textures);
    load_textures_recursive("Textures", &textures);
    printf("Loaded %d textures\n", textures.count);

    if (textures.count > 0) {
        srand((unsigned int)time(NULL));
        for (int i = 0; i < sector_count; i++) {
            sectors[i].floor_tex = textures.textures[rand() % textures.count];
            sectors[i].ceil_tex = textures.textures[rand() % textures.count];
        }
        for (int i = 0; i < wall_count; i++) {
            walls[i].tex = textures.textures[rand() % textures.count];
        }
    }

    SDL_SetRelativeMouseMode(SDL_TRUE);

    bool running = true;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
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

        void *pixels_raw;
        int pixel_pitch;
        if (SDL_LockTexture(target, NULL, &pixels_raw, &pixel_pitch) < 0) break;

        Uint32 *pixels = (Uint32 *)pixels_raw;
        pixel_pitch /= 4;

        memset(pixels, 0, (size_t)h * (size_t)(pixel_pitch * 4));

        for (int i = 0; i < w; i++) {
            root_clip[i].top           = 0;
            root_clip[i].bottom        = h;
            root_clip[i].highest_ceil_y = 0;
            root_clip[i].lowest_floor_y = h;
        }

        render_sector(pixels, pixel_pitch,
                      player.sector_id,
                      root_clip, portal_clip,
                      0, w, h);

        SDL_UnlockTexture(target);

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, target, NULL, NULL);
        SDL_RenderPresent(ren);
    }

    free_texture_list(&textures);
    free(root_clip);
    free(portal_clip);

cleanup:
    if (target) SDL_DestroyTexture(target);
    if (ren)    SDL_DestroyRenderer(ren);
    if (win)    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
