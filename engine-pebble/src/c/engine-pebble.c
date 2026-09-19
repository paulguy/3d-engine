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

#include <pebble.h>

#include "engine.h"
#include "cache.h"

#define GFX_READ_BUF (32)

static Window *s_window;
static GFont s_font;
static Layer *s_text_layer;
static Layer *s_engine_layer;

static ResHandle map;

const unsigned char TEX_IDS[] = {
    RESOURCE_ID_TEX_0,
    RESOURCE_ID_TEX_1,
    RESOURCE_ID_TEX_2,
    RESOURCE_ID_TEX_3
};
#define MAX_TEX_ID (sizeof(TEX_IDS) - 1)

const unsigned char MAP_IDS[] = {
    RESOURCE_ID_MAP_0,
    RESOURCE_ID_MAP_1
};
#define MAX_MAP_ID (sizeof(MAP_IDS) - 1)

#define SMALL_TEX_RES_SIZE (SMALL_TEX_SIZE / 4 * 3)
#define LARGE_TEX_RES_SIZE (LARGE_TEX_SIZE / 4 * 3)

int open_map(unsigned char number) {
    if(number > MAX_MAP_ID) {
        return(-1);
    }

    map = resource_get_handle(MAP_IDS[number]);

    return(0);
}

int read_map(off_t offset, size_t length, void *data) {
    if(resource_load_byte_range(map, offset, data, length) < length) {
        return(-1);
    }

    return(0);
}

void close_map() {
    /* nothing to do */
}

/*
# image parameters
# edge dimension (32, 64) (1b)
# storage direction (horizontal/vertical) (1b)
# palette size (2, 4, 8, 16, 32, 64) (3b)
# RLE repeat word size (uncompressed, 2, 3, 4, 5, 6, 7, 8) (3b)
# 64 color palette = direct color
*/

int get_graphic_dim(unsigned char number) {
    ResHandle handle;
    unsigned char header;

    if(number > MAX_TEX_ID) {
        return(-1);
    }

    handle = resource_get_handle(TEX_IDS[number]);
    resource_load_byte_range(handle, 0, &header, 1);

    if(header & 0x80) {
        /* bit set = 64 */
        return(64);
    }

    return(32);
}

/* avoid passing these along on the stack a bunch or weird silent stack/heap overflows */
static ResHandle gfx;
static unsigned int gfx_filepos;
static unsigned char gfx_bufpos;
static unsigned char gfx_byte;
static unsigned char gfx_bit;
static unsigned char gfx_palette[32];
static unsigned char gfx_buf[GFX_READ_BUF];

static void read_byte() {
    if(gfx_bufpos == GFX_READ_BUF) {
        resource_load_byte_range(gfx, gfx_filepos + GFX_READ_BUF, gfx_buf, GFX_READ_BUF);
        gfx_filepos += GFX_READ_BUF;
        gfx_bufpos = 0;
    }

    gfx_byte = gfx_buf[gfx_bufpos];
    gfx_bufpos += 1;
}

static unsigned char read_bits(unsigned char count) {
    /* simple bit unpacking function that can only unpack up to 8 bits! */
    unsigned char val;

    /* mask out leading bits */
    val = gfx_byte & (0xFF >> gfx_bit);
    /* advance bit position */
    gfx_bit += count;
    if(gfx_bit < 8) {
        /* not aligned */

        /* align the value to the least significant bit and return it */
        return(val >> (8 - gfx_bit));
    } else if(gfx_bit > 8) {
        /* more to copy */
        read_byte(gfx_buf);

        /* bring bit back in to the next byte */
        gfx_bit -= 8; /* bit is now the remaining bits needed */
        /* shift what's been copied to make room and OR in the rest */
        return((val << gfx_bit) | ((gfx_byte & (0xFF << (8 - gfx_bit))) >> (8 - gfx_bit)));
    }

    /* value's all read in and aligned so just return it */
    gfx_bit = 0;
    read_byte(gfx_buf);
    return(val);
}

int load_graphic(unsigned char number,
                 unsigned char *data) {
    unsigned short i, j;
    unsigned char x, y;
    unsigned char dimension;
    char vertical;
    unsigned char palette_size, color_bits;
    unsigned char repeat_bits;
    unsigned char repeats, color;

    if(number > MAX_TEX_ID) {
        return(-1);
    }

    gfx = resource_get_handle(TEX_IDS[number]);
    /* initial buffer fill */
    resource_load_byte_range(gfx, 0, gfx_buf, GFX_READ_BUF);
    /* set up buffer and read first byte */
    gfx_filepos = 0;
    gfx_bufpos = 0;
    gfx_bit = 0;
    read_byte();

    if(read_bits(1)) {
        dimension = 64;
    } else {
        dimension = 32;
    }
    vertical = read_bits(1);
    color_bits = read_bits(3); /* 0-7, 0 and 7 are invalid... but this doesn't chack for that */
    repeat_bits = read_bits(3);
    repeat_bits = repeat_bits > 0 ? repeat_bits + 1 : 0; /* 0 (uncompressed), 2-8 */

    if(color_bits < 6) {
        /* 6 bits is direct color, 5 and below is paletted */
        /* BUG yeah yeah i know, but this isn't user-provided data... */
        /* palette size is 1-32 */
        palette_size = read_bits(5) + 1;
        for(i = 0; i < palette_size; i++) {
            color = read_bits(6);
            gfx_palette[i] = ((color & 0x30) << 2) | ((color & 0x0C) << 1) | (color & 0x03);
        }
    }

    i = 0;
    if(repeat_bits == 0) {
        /* uncompressed */
        for(i = 0; i < dimension * dimension; i++) {
            color = read_bits(color_bits);
            if(color_bits < 6) {
                /* paletted */
                color = gfx_palette[color];
            } else {
                color = ((color & 0x30) << 2) | ((color & 0x0C) << 1) | (color & 0x03);
            }
            data[i] = color;
        }
    } else {
        while(i < dimension * dimension) {
            /* repeats starts at 1 */
            repeats = read_bits(repeat_bits) + 1;
            /* clamp repeats to data size */
            repeats = i + repeats > dimension * dimension ? (dimension * dimension) - i : repeats;
            color = read_bits(color_bits);
            if(color_bits < 6) {
                /* paletted */
                color = gfx_palette[color];
            } else {
                color = ((color & 0x30) << 2) | ((color & 0x0C) << 1) | (color & 0x03);
            }
            if(vertical) {
                for(j = 0; j < repeats; j++) {
                    y = (i+j) / dimension;
                    x = (i+j) % dimension;
                    data[x * dimension + y] = color;
                }
            } else {
                for(j = 0; j < repeats; j++) {
                    data[i+j] = color;
                }
            }
            i += repeats;
        }
    }

    return(0);
}

static void engine_layer_update(struct Layer *layer, GContext *ctx) {
    GBitmap *fb = graphics_capture_frame_buffer(ctx);
    uint8_t *pixels = gbitmap_get_data(fb);
    GRect bounds = layer_get_bounds(layer);
    uint16_t pitch = gbitmap_get_bytes_per_row(fb);

    engine_render(&(pixels[pitch * bounds.origin.y + bounds.origin.x]),
                  bounds.size.w, bounds.size.h,
                  pitch);

    graphics_release_frame_buffer(ctx, fb);
}

static void text_layer_update(struct Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    char timebuffer[16];

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, bounds, 0, 0);
    clock_copy_time_string(timebuffer, sizeof(timebuffer));
    /* jank bounding box to make it center correctly */
    graphics_draw_text(ctx, timebuffer, s_font, GRect(3, -4, bounds.size.w - 3, bounds.size.h), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    /* signal engine view to update after updating the time display to ensure the time displayed is most current */
    layer_mark_dirty(s_engine_layer);
}

void select_click_handler(ClickRecognizerRef recognizer, void *context) {
    engine_move(sin(v.angle) * 10.0, cos(v.angle) * 10.0);
    layer_mark_dirty(s_engine_layer);
}

void up_click_handler(ClickRecognizerRef recognizer, void *context) {
    v.angle -= M_PI * 0.1;
    layer_mark_dirty(s_engine_layer);
}

void down_click_handler(ClickRecognizerRef recognizer, void *context) {
    v.angle += M_PI * 0.1;
    layer_mark_dirty(s_engine_layer);
}

void config_provider(Window *window) {
    window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_click_handler);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_click_handler);
    window_single_repeating_click_subscribe(BUTTON_ID_SELECT, 100, select_click_handler);
}

static void prv_window_load(Window *window) {
    window_set_background_color(window, GColorBlack);
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    window_set_click_config_provider(window, (ClickConfigProvider) config_provider);

    s_text_layer = layer_create(GRect(0, bounds.size.h - 26, bounds.size.w, 26));
    layer_set_update_proc(s_text_layer, text_layer_update);

    layer_add_child(window_layer, s_text_layer);

    s_engine_layer = layer_create(GRect(0, 0, bounds.size.w, bounds.size.h - 26));
    layer_set_update_proc(s_engine_layer, engine_layer_update);

    layer_add_child(window_layer, s_engine_layer);
}

static void prv_window_unload(Window *window) {
    layer_destroy(s_engine_layer);
    layer_destroy(s_text_layer);
}

void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    /* signal to redraw the layers, engine view will be marked dirty after time is updated */
    layer_mark_dirty(s_text_layer);
}

static void prv_init(void) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers) {
        .load = prv_window_load,
        .unload = prv_window_unload,
    });
    const bool animated = true;
    window_stack_push(s_window, animated);

    s_font = fonts_get_system_font(FONT_KEY_LECO_26_BOLD_NUMBERS_AM_PM);

    /* setup wrapper function pointers */
    open_map_p = open_map;
    read_map_p = read_map;
    close_map_p = close_map;
    get_graphic_dim_p = get_graphic_dim;
    load_graphic_p = load_graphic;
    /* allocate texture memory */
    texmem = malloc(TEXMEM);
    if(texmem == NULL) {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Failed to allocate texture memory, probably going to crash.\n", s_window);
    }
    /* initial map and view */
    engine_load(0, 0);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
}

static void prv_deinit(void) {
    window_destroy(s_window);
}

int main(void) {
    prv_init();

    app_event_loop();
    prv_deinit();
}
