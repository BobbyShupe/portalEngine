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

// ------------------------------------------------------------

double point_side(double px, double py, Vec2 a, Vec2 b) {
    return (b.x - a.x)*(py - a.y) - (b.y - a.y)*(px - a.x);
}

void draw_vline(SDL_Renderer *r, int x, int y1, int y2, Uint32 c) {
    if (y1 >= y2) return;
    SDL_SetRenderDrawColor(r, (c>>24)&255, (c>>16)&255, (c>>8)&255, 255);
    SDL_RenderDrawLine(r, x, y1, x, y2);
}

// ------------------------------------------------------------
// Map / player

void init_map(void) {
    int wi = 0;

    sectors[0] = (Sector){0,4,0,4,0x444444FF,0x222222FF};
    walls[wi++] = (Wall){{-5,-5},{ 5,-5},-1,0xFF0000FF};
    walls[wi++] = (Wall){{ 5,-5},{ 5, 5}, 1,0x00FF00FF};
    walls[wi++] = (Wall){{ 5, 5},{-5, 5},-1,0x0000FFFF};
    walls[wi++] = (Wall){{-5, 5},{-5,-5},-1,0xFFFF00FF};

    sectors[1] = (Sector){4,4,0,5,0x555555FF,0x333333FF};
    walls[wi++] = (Wall){{ 5,-5},{15,-5},-1,0xFF00FFFF};
    walls[wi++] = (Wall){{15,-5},{15, 5},-1,0x00FFFFFF};
    walls[wi++] = (Wall){{15, 5},{ 5, 5},-1,0xFF8800FF};
    walls[wi++] = (Wall){{ 5, 5},{ 5,-5}, 0,0x00FF00FF};

    player = (Player){0,0,1.6,0,0,0};
}

void update_player_sector(void) {
    Sector *s = &sectors[player.sector_id];
    for (int i = 0; i < s->wall_count; i++) {
        Wall *w = &walls[s->first_wall + i];
        if (w->portal_to >= 0 &&
            point_side(player.x, player.y, w->p1, w->p2) < 0) {
            player.sector_id = w->portal_to;
            return;
        }
    }
}

// ------------------------------------------------------------
// Renderer

void render_sector(SDL_Renderer *r, int sid, ClipRange *clip,
                   int depth, int sw, int sh)
{
    if (depth > MAX_RECURSION_DEPTH) return;

    Sector *s = &sectors[sid];
    double proj = (sw/2.0)/tan(FOV_DEG*M_PI/360.0);
    double yoff = sh/2.0 + player.pitch*(sh/2.0);

    for (int i=0;i<s->wall_count;i++) {
        Wall *w = &walls[s->first_wall+i];

        // Player-relative coordinates
        double dx1=w->p1.x-player.x, dy1=w->p1.y-player.y;
        double dx2=w->p2.x-player.x, dy2=w->p2.y-player.y;

        double vx1=dx1*sin(player.angle)-dy1*cos(player.angle);
        double vz1=dx1*cos(player.angle)+dy1*sin(player.angle);
        double vx2=dx2*sin(player.angle)-dy2*cos(player.angle);
        double vz2=dx2*cos(player.angle)+dy2*sin(player.angle);

        if (vz1<NEAR_PLANE && vz2<NEAR_PLANE) continue;
        if (vz1<NEAR_PLANE) { double t=(NEAR_PLANE-vz1)/(vz2-vz1); vx1+=t*(vx2-vx1); vz1=NEAR_PLANE; }
        if (vz2<NEAR_PLANE) { double t=(NEAR_PLANE-vz2)/(vz1-vz2); vx2+=t*(vx1-vx2); vz2=NEAR_PLANE; }

        int x1=sw/2-(int)(vx1*proj/vz1);
        int x2=sw/2-(int)(vx2*proj/vz2);
        if (x1>=x2) continue;

        int sx=fmax(0,x1), ex=fmin(sw-1,x2);
        double iz1=1.0/vz1, iz2=1.0/vz2;

        if (w->portal_to>=0) {
            ClipRange child[sw];
            memcpy(child, clip, sizeof(ClipRange)*sw);
            bool vis=false;
            Sector *ns=&sectors[w->portal_to];

            for (int x=sx;x<=ex;x++) {
                double t=(double)(x-x1)/(x2-x1);
                double iz=iz1+t*(iz2-iz1);
                double sc=proj*iz;

                int yc=yoff-(s->ceil_height-player.z)*sc;
                int yf=yoff-(s->floor_height-player.z)*sc;
                int nyc=yoff-(ns->ceil_height-player.z)*sc;
                int nyf=yoff-(ns->floor_height-player.z)*sc;

                int pt=fmax(clip[x].top,fmax(yc,nyc));
                int pb=fmin(clip[x].bottom,fmin(yf,nyf));

                // Ceiling above wall
                draw_vline(r,x,clip[x].top,pt,s->ceil_color);
                // Wall itself
                draw_vline(r,x,pt,pb,w->color);
                // Floor below wall
                draw_vline(r,x,pb,clip[x].bottom,s->floor_color);

                child[x].top=pt;
                child[x].bottom=pb;
                if (pt<pb) vis=true;
            }
            if(vis) render_sector(r,w->portal_to,child,depth+1,sw,sh);
        }
        else {
            for (int x=sx;x<=ex;x++) {
                double t=(double)(x-x1)/(x2-x1);
                double sc=proj*(iz1+t*(iz2-iz1));

                int yc=yoff-(s->ceil_height-player.z)*sc;
                int yf=yoff-(s->floor_height-player.z)*sc;

                // Ceiling above wall
                draw_vline(r,x,clip[x].top,yc,s->ceil_color);
                // Wall
                draw_vline(r,x,yc,yf,w->color);
                // Floor below wall
                draw_vline(r,x,yf,clip[x].bottom,s->floor_color);
            }
        }
    }
}

// ------------------------------------------------------------

int main(void) {
    SDL_Init(SDL_INIT_VIDEO);
    int w=1024,h=768;
    SDL_Window *win=SDL_CreateWindow("Portal Engine",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,w,h,0);
    SDL_Renderer *ren=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED);

    SDL_SetRelativeMouseMode(SDL_TRUE);
    init_map();

    ClipRange *root=malloc(sizeof(ClipRange)*w);

    while(1){
        SDL_Event e;
        while(SDL_PollEvent(&e))
            if(e.type==SDL_QUIT) exit(0);

        int mx,my;
        SDL_GetRelativeMouseState(&mx,&my);
        player.angle+=mx*0.003;
        player.pitch-=my*0.003;

        const Uint8 *k=SDL_GetKeyboardState(NULL);
        double s=0.1;
        if(k[SDL_SCANCODE_W]) {player.x+=cos(player.angle)*s; player.y+=sin(player.angle)*s;}
        if(k[SDL_SCANCODE_S]) {player.x-=cos(player.angle)*s; player.y-=sin(player.angle)*s;}
        if(k[SDL_SCANCODE_A]) {player.x+=sin(player.angle)*s; player.y-=cos(player.angle)*s;}
        if(k[SDL_SCANCODE_D]) {player.x-=sin(player.angle)*s; player.y+=cos(player.angle)*s;}

        update_player_sector();

        SDL_SetRenderDrawColor(ren,0,0,0,255);
        SDL_RenderClear(ren);

        for(int i=0;i<w;i++){root[i].top=0; root[i].bottom=h;}
        render_sector(ren,player.sector_id,root,0,w,h);

        SDL_RenderPresent(ren);
        SDL_Delay(10);
    }
}
