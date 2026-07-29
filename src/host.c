#include "host.h"

#include <SDL.h>

#include "cia.h"

// This module is a deliberately untested thin I/O boundary. It adapts SDL to the
// emulator (window, texture upload, event pump, audio queue) and contains no
// emulation logic, so a defect here shows up immediately as a black window, a
// dead key, or silence, not as a subtle wrong result. The headless path never
// enters it, so it reads 0% under the coverage tools by design. test/host_smoke.c
// exercises init/present/poll/shutdown under SDL's dummy drivers so a crash or
// leak in the adapter itself is still caught under the sanitizers.

// Integer upscale from the VIC framebuffer to the window; SDL's logical size
// keeps the aspect ratio and lets the window be resized freely.

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int fb_pitch;  // bytes per framebuffer row
static SDL_AudioDeviceID audio_dev;
static SDL_GameController *pad;

// Audio ring: single producer (main loop via host_audio_push), single consumer
// (SDL's audio callback thread). It decouples sample delivery from main-loop timing
// so a render or scheduling hitch cannot starve the device into a click. Free-running
// head/tail counters (masked only when indexing) make empty vs full unambiguous. On
// underrun the callback holds the last sample rather than stepping to silence.
#define AUDIO_RING 32768u            // power of two; ~0.74 s at 44.1 kHz
#define AUDIO_RING_MASK (AUDIO_RING - 1u)
static int16_t audio_ring[AUDIO_RING];
static SDL_atomic_t ring_head;       // producer write count
static SDL_atomic_t ring_tail;       // consumer read count
static int16_t audio_last;           // last sample emitted, held across an underrun

// Two host-key -> C64 matrix maps for the character keys, selectable at runtime
// (F11 toggles). Symbolic (the default) keys off the SDL keycode, so the host
// key that produces a letter/digit maps to that C64 letter/digit regardless of
// the host layout: the friendly everyday typing mode. Positional keys off the
// physical scancode, so the key at a position maps to the same C64 position: an
// authentic C64 layout where symbols follow the C64 legends. Modifiers, cursor
// keys, RESTORE, and the special keys are position-based in both modes and are
// handled directly in apply_key; joystick keys (numpad) are polled separately.
// invariant: the symbolic map identifies keys by keycode (layout-aware) but
// keeps C64 shift semantics; it does not remap shifted symbols to the host's
// character (host Shift+2 yields the C64 '"', not '@'). Letters, digits, and
// unshifted symbols produce the expected C64 character; a full character-level
// symbolic map (via SDL_TEXTINPUT) is a possible later refinement.
typedef struct {
    int32_t key;  // SDL_Keycode (symbolic) or SDL_Scancode (positional)
    uint8_t row, col;
} KeyMap;
static const KeyMap KEY_SYM[] = {
    {SDLK_a, 1, 2}, {SDLK_b, 3, 4}, {SDLK_c, 2, 4}, {SDLK_d, 2, 2},
    {SDLK_e, 1, 6}, {SDLK_f, 2, 5}, {SDLK_g, 3, 2}, {SDLK_h, 3, 5},
    {SDLK_i, 4, 1}, {SDLK_j, 4, 2}, {SDLK_k, 4, 5}, {SDLK_l, 5, 2},
    {SDLK_m, 4, 4}, {SDLK_n, 4, 7}, {SDLK_o, 4, 6}, {SDLK_p, 5, 1},
    {SDLK_q, 7, 6}, {SDLK_r, 2, 1}, {SDLK_s, 1, 5}, {SDLK_t, 2, 6},
    {SDLK_u, 3, 6}, {SDLK_v, 3, 7}, {SDLK_w, 1, 1}, {SDLK_x, 2, 7},
    {SDLK_y, 3, 1}, {SDLK_z, 1, 4},
    {SDLK_0, 4, 3}, {SDLK_1, 7, 0}, {SDLK_2, 7, 3}, {SDLK_3, 1, 0},
    {SDLK_4, 1, 3}, {SDLK_5, 2, 0}, {SDLK_6, 2, 3}, {SDLK_7, 3, 0},
    {SDLK_8, 3, 3}, {SDLK_9, 4, 0},
    {SDLK_COMMA, 5, 7}, {SDLK_PERIOD, 5, 4}, {SDLK_SLASH, 6, 7},
    {SDLK_SEMICOLON, 6, 2}, {SDLK_QUOTE, 5, 5}, {SDLK_MINUS, 5, 3},
    {SDLK_EQUALS, 6, 5}, {SDLK_LEFTBRACKET, 5, 6}, {SDLK_RIGHTBRACKET, 6, 1},
    {SDLK_BACKQUOTE, 7, 1},
};
static const KeyMap KEY_POS[] = {
    {SDL_SCANCODE_A, 1, 2}, {SDL_SCANCODE_B, 3, 4}, {SDL_SCANCODE_C, 2, 4},
    {SDL_SCANCODE_D, 2, 2}, {SDL_SCANCODE_E, 1, 6}, {SDL_SCANCODE_F, 2, 5},
    {SDL_SCANCODE_G, 3, 2}, {SDL_SCANCODE_H, 3, 5}, {SDL_SCANCODE_I, 4, 1},
    {SDL_SCANCODE_J, 4, 2}, {SDL_SCANCODE_K, 4, 5}, {SDL_SCANCODE_L, 5, 2},
    {SDL_SCANCODE_M, 4, 4}, {SDL_SCANCODE_N, 4, 7}, {SDL_SCANCODE_O, 4, 6},
    {SDL_SCANCODE_P, 5, 1}, {SDL_SCANCODE_Q, 7, 6}, {SDL_SCANCODE_R, 2, 1},
    {SDL_SCANCODE_S, 1, 5}, {SDL_SCANCODE_T, 2, 6}, {SDL_SCANCODE_U, 3, 6},
    {SDL_SCANCODE_V, 3, 7}, {SDL_SCANCODE_W, 1, 1}, {SDL_SCANCODE_X, 2, 7},
    {SDL_SCANCODE_Y, 3, 1}, {SDL_SCANCODE_Z, 1, 4},
    {SDL_SCANCODE_1, 7, 0}, {SDL_SCANCODE_2, 7, 3}, {SDL_SCANCODE_3, 1, 0},
    {SDL_SCANCODE_4, 1, 3}, {SDL_SCANCODE_5, 2, 0}, {SDL_SCANCODE_6, 2, 3},
    {SDL_SCANCODE_7, 3, 0}, {SDL_SCANCODE_8, 3, 3}, {SDL_SCANCODE_9, 4, 0},
    {SDL_SCANCODE_0, 4, 3},
    {SDL_SCANCODE_COMMA, 5, 7}, {SDL_SCANCODE_PERIOD, 5, 4},
    {SDL_SCANCODE_SLASH, 6, 7}, {SDL_SCANCODE_SEMICOLON, 6, 2},
    {SDL_SCANCODE_APOSTROPHE, 5, 5}, {SDL_SCANCODE_MINUS, 5, 3},
    {SDL_SCANCODE_EQUALS, 6, 5}, {SDL_SCANCODE_LEFTBRACKET, 5, 6},
    {SDL_SCANCODE_RIGHTBRACKET, 6, 1}, {SDL_SCANCODE_GRAVE, 7, 1},
};
static bool symbolic_mode = true;  // symbolic is the friendly default; F11 toggles
static bool warp_mode;             // F10 toggles unthrottled (turbo) emulation
static bool joy_mode;              // F9 routes the cursor keys to the joystick instead
static unsigned joy_port = 1;      // F8 swaps the port: 1 = joystick 2 (Port A, default), 0 = joystick 1 (Port B)
static char base_title[64];        // window title, for appending the [WARP]/[JOY] tags

#define LSHIFT_ROW 1u
#define LSHIFT_COL 7u

bool host_init(int width, int height, int scale, const char *title) {
    if (scale < HOST_SCALE_MIN || scale > HOST_SCALE_MAX) {
        return false;  // caller validates and reports; never silently clamped
    }
    // We own main() (SDL_MAIN_HANDLED); tell SDL the entry point is ready so
    // SDL_Init succeeds on macOS and Windows without linking SDL2main. No-op on Linux.
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        return false;
    }
    // Log every joystick index, including those SDL_IsGameController rejects: an
    // unmapped pad enumerates fine but is skipped here, so its name/GUID is what we
    // need to add a mapping. Diagnostic only; the open logic below is unchanged.
    int njoy = SDL_NumJoysticks();
    SDL_Log("host: %d joystick device(s) present", njoy);
    for (int i = 0; i < njoy; i++) {
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        const char *name = SDL_JoystickNameForIndex(i);
        SDL_Log("host: joy %d name=\"%s\" guid=%s gamecontroller=%s", i,
                name ? name : "(null)", guid, SDL_IsGameController(i) ? "yes" : "no");
    }
    for (int i = 0; i < njoy && !pad; i++) {
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (!pad) {
                SDL_Log("host: SDL_GameControllerOpen(%d) failed: %s", i, SDL_GetError());
            }
        }
    }
    fb_pitch = width * (int)sizeof(uint32_t);
    SDL_snprintf(base_title, sizeof(base_title), "%s", title);
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              width * scale, height * scale,
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

static void update_title(void) {
    char t[96];
    SDL_snprintf(t, sizeof(t), "%s%s%s%s", base_title, warp_mode ? " [WARP]" : "",
                 joy_port == 0 ? " [JOY1]" : " [JOY2]", joy_mode ? " CRSR" : "");
    SDL_SetWindowTitle(window, t);
}

// Apply a host key press/release to the C64 matrix. Modifiers, cursor keys,
// RESTORE and the special keys are position-based in both modes; the character
// keys use the symbolic map (by keycode) or the positional map (by scancode).
// Cursor up/left are the C64 down/right keys with SHIFT auto-applied.
static void apply_key(SDL_Scancode sc, SDL_Keycode kc, bool down) {
    if (joy_mode) {
        switch (sc) {  // claimed by poll_joystick; must not also reach the matrix
            case SDL_SCANCODE_UP:
            case SDL_SCANCODE_DOWN:
            case SDL_SCANCODE_LEFT:
            case SDL_SCANCODE_RIGHT:
            case SDL_SCANCODE_RALT:
            case SDL_SCANCODE_LCTRL:
                return;
            default: break;
        }
    }
    switch (sc) {
        case SDL_SCANCODE_PAGEUP: cia_restore_set(down); return;
        case SDL_SCANCODE_UP:
            cia_key_set(LSHIFT_ROW, LSHIFT_COL, down);
            cia_key_set(0, 7, down);  // shift + CRSR up/down
            return;
        case SDL_SCANCODE_LEFT:
            cia_key_set(LSHIFT_ROW, LSHIFT_COL, down);
            cia_key_set(0, 2, down);  // shift + CRSR left/right
            return;
        case SDL_SCANCODE_RIGHT: cia_key_set(0, 2, down); return;
        case SDL_SCANCODE_DOWN: cia_key_set(0, 7, down); return;
        case SDL_SCANCODE_RETURN: cia_key_set(0, 1, down); return;
        case SDL_SCANCODE_F1: cia_key_set(0, 4, down); return;  // C64 F1 (Shift = F2)
        case SDL_SCANCODE_F3: cia_key_set(0, 5, down); return;  // C64 F3 (Shift = F4)
        case SDL_SCANCODE_F5: cia_key_set(0, 6, down); return;  // C64 F5 (Shift = F6)
        case SDL_SCANCODE_F7: cia_key_set(0, 3, down); return;  // C64 F7 (Shift = F8)
        case SDL_SCANCODE_SPACE: cia_key_set(7, 4, down); return;
        case SDL_SCANCODE_LSHIFT: cia_key_set(1, 7, down); return;
        case SDL_SCANCODE_RSHIFT: cia_key_set(6, 4, down); return;
        case SDL_SCANCODE_TAB: cia_key_set(7, 2, down); return;    // CTRL
        case SDL_SCANCODE_ESCAPE: cia_key_set(7, 7, down); return;  // RUN/STOP
        case SDL_SCANCODE_LGUI:
        case SDL_SCANCODE_LALT: cia_key_set(7, 5, down); return;    // Commodore
        case SDL_SCANCODE_BACKSPACE: cia_key_set(0, 0, down); return;  // INS/DEL
        case SDL_SCANCODE_HOME: cia_key_set(6, 3, down); return;
        default: break;
    }
    if (symbolic_mode) {
        for (size_t i = 0; i < sizeof(KEY_SYM) / sizeof(KEY_SYM[0]); i++) {
            if (KEY_SYM[i].key == (int32_t)kc) {
                cia_key_set(KEY_SYM[i].row, KEY_SYM[i].col, down);
                return;
            }
        }
    } else {
        for (size_t i = 0; i < sizeof(KEY_POS) / sizeof(KEY_POS[0]); i++) {
            if (KEY_POS[i].key == (int32_t)sc) {
                cia_key_set(KEY_POS[i].row, KEY_POS[i].col, down);
                return;
            }
        }
    }
}

// The keyboard/pad joystick drives the port selected by joy_port (F8 swaps it,
// default port 2 / Port A): a game controller if present, else the numpad as a
// keyboard fallback (8/2/4/6 = up/down/left/right, KP_0 or Right-Ctrl = fire).
static void poll_joystick(void) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint8_t mask = 0;
    if (ks[SDL_SCANCODE_KP_8]) mask |= 0x01;
    if (ks[SDL_SCANCODE_KP_2]) mask |= 0x02;
    if (ks[SDL_SCANCODE_KP_4]) mask |= 0x04;
    if (ks[SDL_SCANCODE_KP_6]) mask |= 0x08;
    if (ks[SDL_SCANCODE_KP_0] || ks[SDL_SCANCODE_RCTRL]) mask |= 0x10;
    if (joy_mode) {  // F9: cursor keys drive joystick 2 on keyboards without a numpad
        if (ks[SDL_SCANCODE_UP]) mask |= 0x01;
        if (ks[SDL_SCANCODE_DOWN]) mask |= 0x02;
        if (ks[SDL_SCANCODE_LEFT]) mask |= 0x04;
        if (ks[SDL_SCANCODE_RIGHT]) mask |= 0x08;
        if (ks[SDL_SCANCODE_RALT] || ks[SDL_SCANCODE_LCTRL]) mask |= 0x10;
    }
    if (pad) {
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) mask |= 0x01;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) mask |= 0x02;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mask |= 0x04;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mask |= 0x08;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A)) mask |= 0x10;
        Sint16 ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (ay < -8000) mask |= 0x01;
        if (ay > 8000) mask |= 0x02;
        if (ax < -8000) mask |= 0x04;
        if (ax > 8000) mask |= 0x08;
    }
    cia_joy_set(joy_port, mask);  // the selected port (F8 swaps port 2 / port 1)
}

bool host_poll(void) {
    SDL_Event e;
    bool quit = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            quit = true;
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F12) {
            quit = true;  // host control (F12 = quit), not C64 keyboard input
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F11 &&
                   !e.key.repeat) {
            symbolic_mode = !symbolic_mode;  // toggle symbolic/positional keyboard
            cia_key_reset();                 // drop held keys so none stick across modes
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F10 &&
                   !e.key.repeat) {
            warp_mode = !warp_mode;  // host control (F10 = warp/turbo), not C64 input
            update_title();
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F9 &&
                   !e.key.repeat) {
            joy_mode = !joy_mode;  // host control (F9 = cursor keys as the joystick)
            cia_key_reset();       // drop held keys so none stick across modes
            cia_joy_set(0, 0); cia_joy_set(1, 0);  // release both so nothing sticks
            update_title();
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_F8 &&
                   !e.key.repeat) {
            cia_joy_set(joy_port, 0);  // release the port we are leaving
            joy_port ^= 1u;            // host control (F8 = swap joystick port 2 <-> 1)
            update_title();
        } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            apply_key(e.key.keysym.scancode, e.key.keysym.sym, true);
        } else if (e.type == SDL_KEYUP) {
            apply_key(e.key.keysym.scancode, e.key.keysym.sym, false);
        } else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) {
            pad = SDL_GameControllerOpen(e.cdevice.which);
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad) {
            SDL_GameControllerClose(pad);
            pad = NULL;
        }
    }
    poll_joystick();
    return quit;
}

bool host_warp(void) { return warp_mode; }

const char *host_error(void) { return SDL_GetError(); }

// SDL audio thread: drains the ring into the device buffer. On underrun (ring
// empty before the request is filled) it holds the last sample, so a shortfall is a
// brief flat hold instead of a hard step to zero that would click.
static void audio_callback(void *userdata, Uint8 *stream, int len) {
    (void)userdata;
    int16_t *out = (int16_t *)stream;
    int want = len / (int)sizeof(int16_t);
    Uint32 head = (Uint32)SDL_AtomicGet(&ring_head);
    Uint32 tail = (Uint32)SDL_AtomicGet(&ring_tail);
    Uint32 avail = head - tail;
    int n = (avail < (Uint32)want) ? (int)avail : want;
    for (int i = 0; i < n; i++) {
        audio_last = audio_ring[tail & AUDIO_RING_MASK];
        out[i] = audio_last;
        tail++;
    }
    for (int i = n; i < want; i++) {
        out[i] = audio_last;  // underrun: hold, do not click to silence
    }
    SDL_AtomicSet(&ring_tail, (int)tail);
}

bool host_audio_init(int rate) {
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;             // SID is mono
    want.samples = 2048;           // device buffer; larger tolerates transport jitter
    const char *buf_env = SDL_getenv("C64_AUDIO_BUF");  // device buffer override
    if (buf_env) {
        int v = SDL_atoi(buf_env);
        if (v >= 512 && v <= 16384) { want.samples = (Uint16)v; }
    }
    want.callback = audio_callback;  // pull model: the audio thread drains the ring
    SDL_AtomicSet(&ring_head, 0);
    SDL_AtomicSet(&ring_tail, 0);
    audio_last = 0;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev == 0) {
        return false;
    }
    SDL_PauseAudioDevice(audio_dev, 0);  // begin playback
    return true;
}

// Producer: append into the ring. Drops the overflow if the ring is full, which the
// pacer below keeps from happening in steady state.
void host_audio_push(const int16_t *samples, int count) {
    if (audio_dev == 0 || count <= 0) {
        return;
    }
    Uint32 head = (Uint32)SDL_AtomicGet(&ring_head);
    Uint32 tail = (Uint32)SDL_AtomicGet(&ring_tail);
    Uint32 space = AUDIO_RING - (head - tail);
    Uint32 n = ((Uint32)count < space) ? (Uint32)count : space;
    for (Uint32 i = 0; i < n; i++) {
        audio_ring[head & AUDIO_RING_MASK] = samples[i];
        head++;
    }
    SDL_AtomicSet(&ring_head, (int)head);
}

// Realtime clock: block the main loop while the ring holds more than the target, so
// the loop runs exactly as fast as the audio thread consumes (the callback drains at
// the device rate).
void host_audio_pace(unsigned target_samples) {
    if (audio_dev == 0) {
        return;
    }
    while ((Uint32)(SDL_AtomicGet(&ring_head) - SDL_AtomicGet(&ring_tail)) > target_samples) {
        SDL_Delay(1);
    }
}

void host_audio_shutdown(void) {
    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
        audio_dev = 0;
    }
}

void host_shutdown(void) {
    if (pad) {
        SDL_GameControllerClose(pad);
        pad = NULL;
    }
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
