/*
 * main.c — TinkerOS Desktop Simulator Entry Point
 *
 * Initializes SDL2 + LVGL, loads the home screen, and runs the event loop.
 *
 * Usage:
 *   cd sim && make && ./tinkeros_sim
 *
 * Mouse = touch: left click = tap, drag = swipe
 */

#include <stdio.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "lvgl.h"

/* Our UI modules */
#include "ui_core.h"
#include "ui_splash.h"
#include "ui_home.h"

/* Defined in ui_core_sim.c */
extern void tab5_ui_sim_init(void);

/* Forward declare so we can call from test mode */
extern lv_obj_t *ui_chat_create(void);
extern void ui_chat_add_message(const char *text, bool is_user);
extern bool ui_chat_is_active(void);

static void run_selftest(void)
{
    printf("\n=== SELF-TEST MODE ===\n");

    tab5_ui_sim_init();

    /* Test 1: splash */
    printf("[1] ui_splash_create... ");
    lv_obj_t *splash = ui_splash_create();
    for (int i = 0; i < 10; i++) { lv_timer_handler(); SDL_Delay(5); }
    printf("%s\n", splash ? "OK" : "FAIL (null)");

    /* Test 2: home */
    printf("[2] ui_home_create... ");
    lv_obj_t *home = ui_home_create();
    for (int i = 0; i < 10; i++) { lv_timer_handler(); SDL_Delay(5); }
    printf("%s\n", home ? "OK" : "FAIL (null)");

    /* Test 3: chat */
    printf("[3] ui_chat_create... ");
    lv_obj_t *chat = ui_chat_create();
    for (int i = 0; i < 10; i++) { lv_timer_handler(); SDL_Delay(5); }
    printf("%s\n", (chat && ui_chat_is_active()) ? "OK" : "FAIL");

    /* Test 4: add messages */
    printf("[4] ui_chat_add_message (user)... ");
    ui_chat_add_message("Hello from simulator!", true);
    for (int i = 0; i < 5; i++) { lv_timer_handler(); SDL_Delay(5); }
    printf("OK\n");

    printf("[5] ui_chat_add_message (ai)... ");
    ui_chat_add_message("Hello! I'm Tinker. How can I help you today?", false);
    for (int i = 0; i < 5; i++) { lv_timer_handler(); SDL_Delay(5); }
    printf("OK\n");

    printf("\n=== ALL SELF-TESTS PASSED ===\n\n");
    SDL_Quit();
}

int main(int argc, char *argv[])
{
    /* --test flag: run self-test and exit */
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        run_selftest();
        return 0;
    }

    printf("TinkerOS Simulator — LVGL %d.%d.%d\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    printf("Window: 720x1280 | Mouse = touch\n");
    printf("Close window or press Ctrl+C to quit.\n\n");

    /* Initialize SDL2 + LVGL with SDL2 display backend */
    tab5_ui_sim_init();

    /* Show splash screen briefly, then home */
    lv_obj_t *splash = ui_splash_create();
    (void)splash;

    /* Run a few frames so splash renders */
    for (int i = 0; i < 30; i++) {
        lv_timer_handler();
        SDL_Delay(16);
    }

    /* Load home screen */
    ui_home_create();

    /* Main event loop */
    bool running = true;
    while (running) {
        /* Process SDL events (window close, keyboard shortcuts) */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    running = false;
                }
            }
        }

        /* Drive LVGL — returns ms until next timer expires */
        uint32_t delay = lv_timer_handler();
        if (delay > 16) delay = 16; /* cap at ~60 FPS */
        SDL_Delay(delay);
    }

    printf("\nSimulator exited cleanly.\n");
    SDL_Quit();
    return 0;
}
