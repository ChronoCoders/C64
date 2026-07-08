#include "host.h"

#include <SDL.h>

// Integer upscale from the VIC framebuffer to the window; SDL's logical size
// keeps the aspect ratio and lets the window be resized freely.
#define HOST_SCALE 3

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int fb_pitch;  // bytes per framebuffer row

bool host_init(int width, int height, const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return false;
    }
    fb_pitch = width * (int)sizeof(uint32_t);
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              width * HOST_SCALE, height * HOST_SCALE,
                              SDL_WINDOW_RESIZABLE);
    if (!window) {
        return false;
    }
    // Prefer a vsync-throttled renderer (SDL picks accelerated when available);
    // fall back to any available driver so this works across WSLg, X11, and the
    // kmsdrm kiosk without hardcoding a driver.
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (!renderer) {
        return false;
    }
    SDL_RenderSetLogicalSize(renderer, width, height);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture) {
        return false;
    }
    return true;
}

void host_present(const uint32_t *framebuffer) {
    SDL_UpdateTexture(texture, NULL, framebuffer, fb_pitch);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

bool host_poll(void) {
    SDL_Event e;
    bool quit = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            quit = true;
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
            quit = true;  // host control, not C64 keyboard input
        }
    }
    return quit;
}

const char *host_error(void) { return SDL_GetError(); }

void host_shutdown(void) {
    if (texture) {
        SDL_DestroyTexture(texture);
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}
