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

static Window *s_window;
static GFont s_font;
static Layer *s_text_layer;
static Layer *s_engine_layer;

static ResHandle map;

const unsigned char TEX_IDS[] = {
    RESOURCE_ID_TEX_0
};
#define MAX_TEX_ID (sizeof(TEX_IDS) - 1)

const unsigned char MAP_IDS[] = {
    RESOURCE_ID_MAP_0
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

int get_graphic_dim(unsigned char number) {
    ResHandle handle;
    size_t res_size;

    if(number > MAX_TEX_ID) {
        return(-1);
    }

    handle = resource_get_handle(TEX_IDS[number]);

    res_size = resource_size(handle);

    if(res_size == SMALL_TEX_RES_SIZE) {
        return(SMALL_TEX_DIM);
    } else if(res_size == LARGE_TEX_RES_SIZE) {
        return(LARGE_TEX_DIM);
    }

    /* return invalid size */
    return(0);
}

int load_graphic(unsigned char number, unsigned char *data) {
    unsigned int in, out;
    ResHandle handle;
    size_t res_size;
    size_t tex_size;

    if(number > MAX_TEX_ID) {
        return(-1);
    }

    handle = resource_get_handle(TEX_IDS[number]);

    res_size = resource_size(handle);

    if(res_size == SMALL_TEX_RES_SIZE) {
        tex_size = SMALL_TEX_SIZE;
    } else { /* LARGE_TEX_RES_SIZE */
        tex_size = LARGE_TEX_SIZE;
    }

    /* load in and unpack in place */
    resource_load(handle, &(data[tex_size - res_size]), res_size);
    out = 0;
    for(in = tex_size - res_size; in < tex_size; in += 3) {
        /* ######.. -> ##.##.## */
        data[out] = (data[in] & 0xC0) | ((data[in] & 0x30) >> 1) | ((data[in] & 0x0C) >> 2);
        /* ......## -> ##. ##.## <- ####.... */
        data[out+1] = ((data[in] & 0x03) << 6) | ((data[in+1] & 0xC0) >> 3) | ((data[in+1] & 0x30) >> 4);
        /* ....#### -> ##.## .## <- ##...... */
        data[out+2] = ((data[in+1] & 0x0C) << 4) | ((data[in+1] & 0x03) << 3) | ((data[in+2] & 0xC0) >> 6);
        /* ..###### -> ##.##.## */
        data[out+3] = ((data[in+2] & 0x30) << 2) | ((data[in+2] & 0x0C) << 1) | (data[in+2] & 0x03);
        out += 4;
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
    engine_move(v.pos.x + (sin(v.angle) * 10.0), v.pos.y + (cos(v.angle) * 10.0));
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
