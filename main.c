/*
 * pebble SDK notes
 *
 * figure out how to create a viewport window and another window to show time and maybe battery charge or BPM or whatever
 *
 * get the root layer
 * struct Layer *window_get_root_layer(const Window * window)
 *
 * set the update procedure
 * void layer_set_update_proc(Layer *layer, LayerUpdateProc update_proc)
 *
 * do this once per minute to make the layer update eventually
 * void layer_mark_dirty(Layer *layer)
 *
 * layer update procedure, all the next functions should be called here
 * typedef void(* LayerUpdateProc)(struct Layer *layer, GContext *ctx)
 *
 * get the frame buffer
 * GBitmap *graphics_capture_frame_buffer(GContext *ctx)
 *
 * get the data pointer
 * uint8_t gbitmap_get_data(const GBitmap *bitmap)
 *
 * get the width and height
 * GRect layer_get_bounds(const Layer *layer)
 * or this?  from the doc
 * layer_get_frame(window_get_root_layer(window)).origin
 * 
 * get the row pitch
 * uint16_t gbitmap_get_bytes_per_row(const GBitmap *bitmap)
 *
 * ... draw in to it ...
 *
 * release the frame buffer
 * bool graphics_release_frame_buffer(GContext *ctx, GBitmap *bitmap)
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "engine.h"
#include "log.h"
#include "cache.h"

SDL_Surface *load_png_checked(unsigned char number) {
    char name[16];
    snprintf(name, sizeof(name), "tex%hhd.png", number);

    SDL_Surface *png = SDL_LoadPNG(name);
    if(png == NULL) {
        LOG("Couldn't open file: %s, %s\n", name, SDL_GetError());
        return(NULL);
    }

    if(png->format != SDL_PIXELFORMAT_INDEX8) {
        LOG("Image must be 8BPP paletted!");
        SDL_DestroySurface(png);
        return(NULL);
    }

    if(png->w != png->h) {
        LOG("Image must be square!");
        SDL_DestroySurface(png);
        return(NULL);
    }

    return(png);
}

int get_graphic_dim(unsigned char number) {
    LOG("%hhd\n", number);
    SDL_Surface *png = load_png_checked(number);
    if(png == NULL) {
        return(-1);
    }

    int width = png->w;
    SDL_DestroySurface(png);

    return(width);
}

int load_graphic(unsigned char number, unsigned char *data) {
    int y, x;
    unsigned char pixel;

    SDL_Surface *png = load_png_checked(number);
    if(png == NULL) {
        return(-1);
    }

    for(y = 0; y < png->h; y++) {
        for(x = 0; x < png->w; x++) {
            pixel = ((unsigned char *)png->pixels)[y * png->pitch + x];
            /* store pixel in "spread out" format */
            data[y * png->h + x] = ((pixel & 0x30) << 2) | ((pixel & 0x0C) << 1) | (pixel & 0x03);
        }
    }

    SDL_DestroySurface(png);

    return(0);
}

FILE *mapfile = NULL;

int open_map(unsigned char number) {
    char name[16];
    snprintf(name, sizeof(name), "map%hhd.bin", number);

    mapfile = fopen(name, "rb");
    if(mapfile == NULL) {
        return(-1);
    }

    return(0);
}

int read_map(off_t offset, ssize_t length, void *data) {
    if(fseek(mapfile, offset, SEEK_SET) < 0) {
        return(-1);
    }

    if(fread(data, 1, length, mapfile) < length) {
        return(-1);
    }

    return(0);
}

void close_map() {
    fclose(mapfile);
}

int main(int argc, char **argv) {
    SDL_Window *window;
    SDL_Surface *win_surf;
    SDL_Surface *surface;
    SDL_Palette *palette;
    int r, g, b;
    int running = 1;
    int redraw = 1;
    int turning = 0;
    int move = 0;
    int strafe = 0;
    Point total_move;
    SDL_Event event;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if(!SDL_Init(SDL_INIT_VIDEO)) {
        LOG("Couldn't initialize SDL.\n");
        goto error;
    }

    window = SDL_CreateWindow("engine", 200, 202, 0);
    if(window == NULL) {
        LOG("Failed to create window: %s\n", SDL_GetError());
        goto error_init;
    }

    win_surf = SDL_GetWindowSurface(window);
    if(win_surf == NULL) {
        LOG("Failed to get window surface: %s\n", SDL_GetError());
        goto error_init;
    }

    surface = SDL_CreateSurface(win_surf->w, win_surf->h, SDL_PIXELFORMAT_INDEX8);
    if(surface == NULL) {
        LOG("Failed to create surface: %s\n", SDL_GetError());
        goto error_init;
    }

    palette = SDL_CreateSurfacePalette(surface);
    /*
    for(b = 0; b < 256; b++) {
        palette.colors[b].r = 255;
        palette.colors[b].g = 0;
        palette.colors[b].b = 255;
        palette.colors[b].a = 255;
    }
    */

    /* don't know what the alpha does yet when writing to the framebuffer so treat
     * any as valid for now.  If it's ignored, it can save a few cycles not having
     * to mask out bits */
    for(b = 0; b < 4; b++) {
        for(g = 0; g < 4; g++) {
            for(r = 0; r < 4; r++) {
                palette->colors[0x00 | (r << 4) | (g << 2) | b].r = r * (255 / 3);
                palette->colors[0x00 | (r << 4) | (g << 2) | b].g = g * (255 / 3);
                palette->colors[0x00 | (r << 4) | (g << 2) | b].b = b * (255 / 3);
                palette->colors[0x40 | (r << 4) | (g << 2) | b].r = r * (255 / 3);
                palette->colors[0x40 | (r << 4) | (g << 2) | b].g = g * (255 / 3);
                palette->colors[0x40 | (r << 4) | (g << 2) | b].b = b * (255 / 3);
                palette->colors[0x80 | (r << 4) | (g << 2) | b].r = r * (255 / 3);
                palette->colors[0x80 | (r << 4) | (g << 2) | b].g = g * (255 / 3);
                palette->colors[0x80 | (r << 4) | (g << 2) | b].b = b * (255 / 3);
                palette->colors[0xC0 | (r << 4) | (g << 2) | b].r = r * (255 / 3);
                palette->colors[0xC0 | (r << 4) | (g << 2) | b].g = g * (255 / 3);
                palette->colors[0xC0 | (r << 4) | (g << 2) | b].b = b * (255 / 3);
            }
        }
    }

    /* setup wrapper function pointers */
    open_map_p = open_map;
    read_map_p = read_map;
    close_map_p = close_map;
    get_graphic_dim_p = get_graphic_dim;
    load_graphic_p = load_graphic;
    /* allocate texture memory */
    texmem = malloc(TEXMEM);
    if(texmem == NULL) {
        LOG("Failed to allocate texture memory.\n");
        goto error_init;
    }
    /* map 0 view 0 */
    engine_load(0, 0);

    log_quiet = 0;

    while(running) {
        if(redraw) {
            SDL_ClearSurface(surface, 0, 0, 0, 0);

            if(!SDL_LockSurface(surface)) {
                LOG("Failed to lock surface: %s\n", SDL_GetError());
            }

            engine_render((unsigned char *)(surface->pixels), surface->w, surface->h, surface->pitch);

            SDL_UnlockSurface(surface);

            redraw = 0;
        }

        if(!SDL_BlitSurface(surface, NULL, win_surf, NULL)) {
            LOG("Failed to blit surface: %s\n", SDL_GetError());
        }

        if(!SDL_UpdateWindowSurface(window)) {
            LOG("Failed to update window surface: %s\n", SDL_GetError());
        }

        SDL_Delay(20);

        while(SDL_PollEvent(&event)) {
            switch(event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_KEY_DOWN:
                    switch(((SDL_KeyboardEvent *)&event)->key) {
                        case SDLK_LEFT:
                            turning = -1;
                            break;
                        case SDLK_RIGHT:
                            turning = 1;
                            break;
                        case SDLK_W:
                            move = 1;
                            break;
                        case SDLK_S:
                            move = -1;
                            break;
                        case SDLK_A:
                            strafe = -1;
                            break;
                        case SDLK_D:
                            strafe = 1;
                            break;
                        case SDLK_L:
                            log_quiet = !log_quiet;
                            if(!log_quiet) {
                                fprintf(stderr, "\n");
                                redraw = 1;
                            }
                            break;
                        case SDLK_ESCAPE:
                        case SDLK_Q:
                            running = 0;
                            break;
                        default:
                            continue;
                    }
                    break;
                case SDL_EVENT_KEY_UP:
                    switch(((SDL_KeyboardEvent *)&event)->key) {
                        case SDLK_LEFT:
                        case SDLK_RIGHT:
                            turning = 0;
                            break;
                        case SDLK_W:
                        case SDLK_S:
                            move = 0;
                            break;
                        case SDLK_A:
                        case SDLK_D:
                            strafe = 0;
                            break;
                        default:
                            continue;
                    }
                    break;
                default:
                    continue;
            }
        }

        if(turning) {
            v.angle = fmodf(v.angle + ((float)turning * M_PI * 0.01), M_PI * 2.0);
            if(v.angle < 0.0) {
                v.angle += M_PI * 2.0;
            }
            redraw = 1;
        }

        total_move.x = 0.0;
        total_move.y = 0.0;
        if(move) {
            total_move.x += sin(v.angle) * move;
            total_move.y += cos(v.angle) * move;
        }

        if(strafe) {
            total_move.x += sin(v.angle + (M_PI / 2.0)) * strafe;
            total_move.y += cos(v.angle + (M_PI / 2.0)) * strafe;
        }

        if(total_move.x != 0.0 || total_move.y != 0.0) {
            engine_move(v.pos.x + total_move.x, v.pos.y + total_move.y);
            fprintf(stderr, "\r%f %f %f", v.pos.x, v.pos.y, v.angle);
            redraw = 1;
        }
    }

error_init:
    SDL_Quit();
error:
    exit(EXIT_SUCCESS);
}

