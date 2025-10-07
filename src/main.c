#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <dlfcn.h>
#endif

#define SCREEN_WIDTH 900
#define SCREEN_HEIGHT 800

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
Uint64 sdl_frame_start = 0;
bool should_quit = false;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    (void)appstate; (void)argc; (void)argv; 

    if (!SDL_Init(SDL_INIT_VIDEO)) exit(1);

    if (!SDL_CreateWindowAndRenderer("main", 0, 0, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer)) {
        printf("[SDL] Could not create window: %s\n", SDL_GetError());
        exit(3);
    }

    SDL_SetWindowSize(window, SCREEN_WIDTH, SCREEN_HEIGHT);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    (void)appstate;
    if (should_quit) return SDL_APP_SUCCESS;

    SDL_RenderPresent(renderer);
    Uint64 sdl_frame_end = SDL_GetTicksNS();

    const int target_fps = 60;
    const int nsec_per_usec = 1000ull;
    Uint64 frame_time_target = nsec_per_usec / target_fps;
    Uint64 frame_time = sdl_frame_end - sdl_frame_start;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    SDL_FRect frame = { 250, 250, 200, 200 };
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
    SDL_RenderFillRect(renderer, &frame);

    if (frame_time < frame_time_target) {
        Uint64 frame_delay = frame_time_target - frame_time;
        SDL_DelayNS(frame_delay);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    (void)appstate; (void)event;
    if (event->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)appstate; (void)result;
}
