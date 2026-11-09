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
#include <unistd.h>
#include <fcntl.h>

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "engine.h"
#include "log.h"
#include "cache.h"

extern unsigned char mapnum;

/* screen size of the watch minus the bar showing time */
#define SCREEN_WIDTH (200)
#define SCREEN_HEIGHT (202)
/* imprecise, but this is fine */
#define FRAME_WAIT (20)
#define MOUSE_SENSITIVITY (0.4)

typedef enum {
    SEL_POINT = 0,
    SEL_LINE,
    SEL_SECTOR
} SelectionTarget;

const SelectionTarget NEXT_TARGET[] = {
    SEL_LINE,
    SEL_SECTOR,
    SEL_POINT
};

const char *SELECTION_TARGET[] = {
    "Point",
    "Line",
    "Sector"
};

const char *FLAT_NAMES[] = {
    "Ceiling",
    "Floor"
};

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

int map_exists(unsigned char number) {
    if(open_map(number) < 0) {
        return(0);
    }

    fclose(mapfile);

    return(1);
}

int read_map(off_t offset, size_t length, void *data) {
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

typedef void (* prompt_cb_t)(char *str);

prompt_cb_t prompt_cb = NULL;

void start_prompt(prompt_cb_t callback) {
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    prompt_cb = callback;
}

int check_prompt() {
    char str[256];
    prompt_cb_t orig_cb;
    ssize_t ret;

    if(prompt_cb == NULL) {
        return(0);
    }

    ret = read(0, str, sizeof(str));
    if(ret > 0) {
        str[ret - 1] = '\0';
        /* let the callback set a next callback */
        orig_cb = prompt_cb;
        prompt_cb = NULL;
        orig_cb(str);
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK);
    }

    return(prompt_cb != NULL);
}

/* point 0 is the no-bias wall scroll */
int s_point = 1;
int s_line = 0;
int s_sector = 0;

typedef void (* select_cb_t)(int sindex, int lindex, int pindex);

select_cb_t select_cb = NULL;

void start_select(select_cb_t callback) {
    select_cb = callback;
}

int check_select(int sindex, int lindex, int pindex) {
    select_cb_t orig_cb;

    if(select_cb != NULL && sindex >= 0) {
        /* let the callback set a next callback */
        orig_cb = select_cb;
        select_cb = NULL;
        orig_cb(sindex, lindex, pindex);
    }

    return(select_cb != NULL);
}

void cancel_input_callbacks() {
    select_cb = NULL;
    prompt_cb = NULL;
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK);
}

typedef void (* point_cb_t)(float x, float y);

point_cb_t point_cb;

float prompt_x;

void prompt_point_y_cb(char *str) {
    char *endptr;
    float prompt_y;

    prompt_y = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    point_cb(prompt_x, prompt_y);
}

void prompt_point_x_cb(char *str) {
    char *endptr;

    prompt_x = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Point Y: ");
    start_prompt(prompt_point_y_cb);
}

void prompt_point(point_cb_t callback) {
    point_cb = callback;
    fprintf(stderr, "Point X: ");
    start_prompt(prompt_point_x_cb);
}

typedef void (* matrix2x2_cb_t)(float xx, float xy,
                                float yz, float yy);

matrix2x2_cb_t matrix2x2_cb;

float prompt_xx, prompt_xy, prompt_xz,
      prompt_yx, prompt_yy;

void prompt_matrix2x2_yy_cb(char *str) {
    char *endptr;

    prompt_yy = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    matrix2x2_cb(prompt_xx, prompt_xy,
                 prompt_yx, prompt_yy);
}

void prompt_matrix2x2_yx_cb(char *str) {
    char *endptr;

    prompt_yx = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix2x2 YY: ");
    start_prompt(prompt_matrix2x2_yy_cb);
}

void prompt_matrix2x2_xy_cb(char *str) {
    char *endptr;

    prompt_xy = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix2x2 YX: ");
    start_prompt(prompt_matrix2x2_yx_cb);
}

void prompt_matrix2x2_xx_cb(char *str) {
    char *endptr;

    prompt_xx = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix2x2 XY: ");
    start_prompt(prompt_matrix2x2_xy_cb);
}

void prompt_matrix2x2(matrix2x2_cb_t callback) {
    matrix2x2_cb = callback;
    fprintf(stderr, "Matrix2x2 XX: ");
    start_prompt(prompt_matrix2x2_xx_cb);
}

typedef void (* matrix3x2_cb_t)(float xx, float xy, float xz,
                                float yx, float yy, float yz);

matrix3x2_cb_t matrix3x2_cb;

void prompt_matrix3x2_yz_cb(char *str) {
    char *endptr;
    float prompt_yz;

    prompt_yz = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    matrix3x2_cb(prompt_xx, prompt_xy, prompt_xz,
                 prompt_yx, prompt_yy, prompt_yz);
}

void prompt_matrix3x2_yy_cb(char *str) {
    char *endptr;

    prompt_yy = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix3x2 YZ: ");
    start_prompt(prompt_matrix3x2_yz_cb);
}

void prompt_matrix3x2_yx_cb(char *str) {
    char *endptr;

    prompt_yx = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix3x2 YY: ");
    start_prompt(prompt_matrix3x2_yy_cb);
}

void prompt_matrix3x2_xz_cb(char *str) {
    char *endptr;

    prompt_xz = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix3x2 YX: ");
    start_prompt(prompt_matrix3x2_yx_cb);
}

void prompt_matrix3x2_xy_cb(char *str) {
    char *endptr;

    prompt_xy = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix3x2 XZ: ");
    start_prompt(prompt_matrix3x2_xz_cb);
}

void prompt_matrix3x2_xx_cb(char *str) {
    char *endptr;

    prompt_xx = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    fprintf(stderr, "Matrix3x2 XY: ");
    start_prompt(prompt_matrix3x2_xy_cb);
}

void prompt_matrix3x2(matrix3x2_cb_t callback) {
    matrix3x2_cb = callback;
    fprintf(stderr, "Matrix3x2 XX: ");
    start_prompt(prompt_matrix3x2_xx_cb);
}

void split_sector_cb(int sindex, int lindex, int pindex) {
    if(split_sector(s_sector, s_point, pindex) < 0) {
        fprintf(stderr, "Split sector failed.\n");
    }
}

void set_point_cb(float x, float y) {
    if(set_point(s_sector, s_point, x, y) < 0) {
        fprintf(stderr, "Set point failed.\n");
    }
}

void move_point_cb(float x, float y) {
    move_point(s_point, x, y);
}

int action_which;

void prompt_line_texture_cb(char *str) {
    char *endptr;
    int texture;

    texture = strtol(str, &endptr, 10);
    if(endptr == str) {
        return;
    }

    if(set_line_texture(s_line, action_which,
                        texture) < 0) {
        fprintf(stderr, "Set line texture failed.\n");
    }
}

void prompt_sector_texture_cb(char *str) {
    char *endptr;
    int texture;

    texture = strtol(str, &endptr, 10);
    if(endptr == str) {
        return;
    }

    if(set_sector_texture(s_sector, action_which,
                        texture) < 0) {
        fprintf(stderr, "Set sector texture failed.\n");
    }
}

int parse_shade(const char *str, int *shade) {
    int color;
    *shade = 0;

    char *endptr;
    const char *num = str;

    if(num[0] == '\n') {
        return(-1);
    } else if(num[0] == 'p' || num[0] == 'P') {
        *shade |= SHADE_PARALLAX;
        num = &num[1];
    }

    if(num[0] == '\n') {
        return(-1);
    } else if(num[0] == '-') {
        *shade |= SHADE_SUB;
        num = &num[1];
    }

    /* shades are 0-3, so base 4 */
    color = strtol(num, &endptr, 4);
    if(endptr == num) {
        return(-1);
    }
    *shade |= (color & 0x03) | ((color & 0x0C) << 1) | ((color & 0x30) << 2);

    return(0);
}

void prompt_line_shade_cb(char *str) {
    int shade;

    if(parse_shade(str, &shade) < 0) {
        return;
    }

    if(set_line_shade(s_line, action_which,
                      shade) < 0) {
        fprintf(stderr, "Set line shade failed.\n");
    }
}

void prompt_sector_shade_cb(char *str) {
    int shade;

    if(parse_shade(str, &shade) < 0) {
        return;
    }

    if(set_sector_shade(s_sector, action_which,
                        shade) < 0) {
        fprintf(stderr, "Set sector shade failed.\n");
    }
}

void prompt_sector_height_cb(char *str) {
    char *endptr;
    float height;

    height = strtof(str, &endptr);
    if(endptr == str) {
        return;
    }

    (*s)[s_sector].height[action_which] = height;
}

void set_line_bias_cb(float x, float y) {
    if(set_line_bias(s_line, action_which, x, y) < 0) {
        fprintf(stderr, "Set line bias failed.\n");
    }
}

void set_sector_bias_cb(float x, float y) {
    if(set_sector_bias(s_sector, action_which, x, y) < 0) {
        fprintf(stderr, "Set sector bias failed.\n");
    }
}

void set_line_transform_cb(float xx, float xy, float xz,
                           float yx, float yy, float yz) {
    if(set_line_transform(s_line, action_which,
                          xx, xy, xz,
                          yx, yy, yz) < 0) {
        fprintf(stderr, "Set line transform failed.\n");
    }
}

void set_sector_transform_cb(float xx, float xy,
                             float yx, float yy) {
    if(set_sector_transform(s_sector, action_which,
                            xx, xy,
                            yx, yy) < 0) {
        fprintf(stderr, "Set sector transform failed.\n");
    }
}

void prompt_mapnum_cb(char *str) {
    char *endptr;

    mapnum = strtol(str, &endptr, 10);
    fprintf(stderr, "Mapnum set to %hhu.\n", mapnum);
}

void print_view() {
    fprintf(stderr, "View is at %f, %f looking at %f ", v.pos.x, v.pos.y, v.angle);
    if(v.angle > (M_PI / 16.0) && v.angle <= (M_PI / 16.0 * 3.0)) {
        fprintf(stderr, "(between +Y and +X)\n");
    } else if(v.angle > (M_PI / 8.0 * 3.0) && v.angle <= (M_PI / 8.0 * 5.0)) {
        fprintf(stderr, "(towards +X)\n");
    } else if(v.angle > (M_PI / 8.0 * 5.0) && v.angle <= (M_PI / 8.0 * 7.0)) {
        fprintf(stderr, "(between +X and -Y)\n");
    } else if(v.angle > (M_PI / 8.0 * 7.0) && v.angle <= (M_PI / 8.0 * 9.0)) {
        fprintf(stderr, "(towards -Y)\n");
    } else if(v.angle > (M_PI / 8.0 * 9.0) && v.angle <= (M_PI / 8.0 * 11.0)) {
        fprintf(stderr, "(between -Y and -X)\n");
    } else if(v.angle > (M_PI / 8.0 * 11.0) && v.angle <= (M_PI / 8.0 * 13.0)) {
        fprintf(stderr, "(towards -X)\n");
    } else if(v.angle > (M_PI / 8.0 * 13.0) && v.angle <= (M_PI / 8.0 * 15.0)) {
        fprintf(stderr, "(between -X and +Y)\n");
    } else {
        fprintf(stderr, "(towards +Y)\n");
    }
}

void print_shade(unsigned short shade) {
    if(shade & SHADE_PARALLAX) {
        fputc('p', stderr);
    }
    if(shade & SHADE_SUB) {
        fputc('-', stderr);
    }
    fprintf(stderr, "%hu%hu%hu", (shade & 0xC0) >> 6,
                                 (shade & 0x18) >> 3,
                                 (shade & 0x03));
}

void print_point(unsigned short pindex) {
    fprintf(stderr, "Point %hu (%f, %f)\n", pindex, (*p)[pindex].x, (*p)[pindex].y);
}

void print_line(unsigned short lindex) {
    unsigned short windex;
    unsigned short bindex;
    unsigned short rindex;

    fprintf(stderr, "Line %hu Point: %hu Sector: %hu Ceiling Wall: %hu Floor Wall: %hu\n", lindex,
            (*l)[lindex].point, (*l)[lindex].sector,
            (*l)[lindex].wall[CEILING], (*l)[lindex].wall[FLOOR]);
    windex = (*l)[lindex].wall[CEILING];
    bindex = (*w)[windex].bias;
    rindex = (*w)[windex].transform;
    fprintf(stderr, "Ceiling Wall %hu Texture: %hhu Shade: ", windex,
            (*w)[windex].texture);
    print_shade((*w)[windex].shade);
    fprintf(stderr, " Bias: %hu Transform: %hu\n", bindex, rindex);
    fprintf(stderr, "Ceiling Bias %hu (%f, %f)\n", bindex, (*p)[bindex].x, (*p)[bindex].y);
    fprintf(stderr, "Ceiling Matrix3x2 %hu [%f %f %f] [%f %f %f]\n", rindex,
            (*m32)[rindex].xx, (*m32)[rindex].xy, (*m32)[rindex].xz,
            (*m32)[rindex].yx, (*m32)[rindex].yy, (*m32)[rindex].yz);
    windex = (*l)[lindex].wall[FLOOR];
    bindex = (*w)[windex].bias;
    rindex = (*w)[windex].transform;
    fprintf(stderr, "Floor Wall %hu Texture: %hhu Shade: ", windex,
            (*w)[windex].texture);
    print_shade((*w)[windex].shade);
    fprintf(stderr, " Bias: %hu Transform: %hu\n", bindex, rindex);
    fprintf(stderr, "Floor Bias %hu (%f, %f)\n", bindex, (*p)[bindex].x, (*p)[bindex].y);
    fprintf(stderr, "Floor Matrix3x2 %hu [%f %f %f] [%f %f %f]\n", rindex,
            (*m32)[rindex].xx, (*m32)[rindex].xy, (*m32)[rindex].xz,
            (*m32)[rindex].yx, (*m32)[rindex].yy, (*m32)[rindex].yz);
}

void print_sector(unsigned short sindex) {
    unsigned short findex;
    unsigned short bindex;
    unsigned short rindex;

    fprintf(stderr, "Sector %hu Ceiling height: %f Floor Height: %f Ceiling Flat: %hu Floor Flat: %hu First Line: %hu Lines: %hhu ", sindex,
        (*s)[sindex].height[CEILING], (*s)[sindex].height[FLOOR],
        (*s)[sindex].flat[CEILING], (*s)[sindex].flat[FLOOR],
        (*s)[sindex].firstline, (*s)[sindex].lines);
    switch((*s)[sindex].action & 0xFF) {
        case ACTION_NONE:
            fprintf(stderr, "None\n");
            break;
        case ACTION_WARP:
            fprintf(stderr, "Warp Map: %hhu View: %hhu\n", ((*s)[sindex].action & 0xFF00) >> 8, ((*s)[sindex].action & 0xFF0000) >> 16);
            break;
        case ACTION_WAYPOINT:
            fprintf(stderr, "Waypoint First Point: %hu Count: %hhu\n", ((*s)[sindex].action & 0xFFFF00) >> 8, ((*s)[sindex].action & 0xFF000000) >> 24);
            break;
        default:
            fprintf(stderr, "Unknown\n");
    }
    findex = (*s)[sindex].flat[CEILING];
    bindex = (*f)[findex].bias;
    rindex = (*f)[findex].transform;
    fprintf(stderr, "Ceiling Flat %hu Texture: %hhu Shade: ", findex,
        (*f)[findex].texture);
    print_shade((*f)[findex].shade);
    fprintf(stderr, " Bias: %hu Transform: %hu\n", bindex, rindex);
    fprintf(stderr, "Ceiling Bias %hu (%f, %f)\n", bindex, (*p)[bindex].x, (*p)[bindex].y);
    fprintf(stderr, "Ceiling Matrix2x2 %hu [%f %f] [%f %f]\n", rindex,
            (*m22)[rindex].xx, (*m22)[rindex].xy,
            (*m22)[rindex].yx, (*m22)[rindex].yy);
    findex = (*s)[sindex].flat[FLOOR];
    bindex = (*f)[findex].bias;
    rindex = (*f)[findex].transform;
    fprintf(stderr, "Floor Flat %hu Texture: %hhu Shade: ", findex,
        (*f)[findex].texture);
    print_shade((*f)[findex].shade);
    fprintf(stderr, " Bias: %hu Transform: %hu\n", bindex, rindex);
    fprintf(stderr, "Floor Bias %hu (%f, %f)\n", bindex, (*p)[bindex].x, (*p)[bindex].y);
    fprintf(stderr, "Floor Matrix2x2 %hu [%f %f] [%f %f]\n", rindex,
            (*m22)[rindex].xx, (*m22)[rindex].xy,
            (*m22)[rindex].yx, (*m22)[rindex].yy);
}

int main(int argc, char **argv) {
    SDL_Window *window;
    SDL_Surface *win_surf;
    SDL_Surface *surface;
    SDL_Palette *palette;
    int r, g, b;
    int running = 1;
    int turning = 0;
    int move = 0;
    int strafe = 0;
    int drag_move = 0;
    int drag_turn = 0;
    int action = 0;
    Point total_move;
    float total_turn;
    SDL_Event event;
    SDL_MouseButtonEvent *mbevent;
    SDL_MouseMotionEvent *mmevent;
    int mapnum = 0;
    int viewnum = 0;
    Point cursor = {0};
    Point *point;
    Matrix2x2 *matrix2x2;
    Matrix3x2 *matrix3x2;
    int ret;

    if(argc > 1) {
        mapnum = strtol(argv[1], NULL, 10);
        if(argc > 2) {
            viewnum = strtol(argv[2], NULL, 10);
        }
    }

    SelectionTarget target = SEL_POINT;
    int h_point = -1;
    int h_line = -1;
    int h_sector = -1;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    if(!SDL_Init(SDL_INIT_VIDEO)) {
        LOG("Couldn't initialize SDL.\n");
        goto error;
    }

    window = SDL_CreateWindow("engine", SCREEN_WIDTH, SCREEN_HEIGHT, 0);
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
    map_exists_p = map_exists;
    close_map_p = close_map;
    get_graphic_dim_p = get_graphic_dim;
    load_graphic_p = load_graphic;
    /* allocate texture memory */
    texmem = malloc(TEXMEM);
    if(texmem == NULL) {
        LOG("Failed to allocate texture memory.\n");
        goto error_init;
    }
    engine_load(mapnum, viewnum);

    log_quiet = 0;

    while(running) {
        SDL_ClearSurface(surface, 0, 0, 0, 0);

        if(!SDL_LockSurface(surface)) {
            LOG("Failed to lock surface: %s\n", SDL_GetError());
        }

        engine_render((unsigned char *)(surface->pixels), surface->w, surface->h, surface->pitch,
                      target == SEL_POINT ? h_point : -1,
                      target == SEL_LINE ? h_line : -1,
                      target == SEL_SECTOR ? h_sector : -1,
                      target == SEL_POINT ? s_point : -1,
                      target == SEL_LINE ? s_line : -1,
                      target == SEL_SECTOR ? s_sector : -1);

        SDL_UnlockSurface(surface);

        if(!SDL_BlitSurface(surface, NULL, win_surf, NULL)) {
            LOG("Failed to blit surface: %s\n", SDL_GetError());
        }

        if(!SDL_UpdateWindowSurface(window)) {
            LOG("Failed to update window surface: %s\n", SDL_GetError());
        }

        SDL_Delay(FRAME_WAIT);

        total_turn = 0.0;
        total_move.x = 0.0;
        total_move.y = 0.0;

        /* do this even if there's no events */
        check_prompt();

        while(SDL_PollEvent(&event)) {
            /* general inputs that always happen */
            switch(event.type) {
                case SDL_EVENT_QUIT:
                    running = 0;
                    break;
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
                        case SDLK_Q:
                            running = 0;
                            break;
                        case SDLK_P:
                            print_data(&h);
                            break;
                        case SDLK_I:
                            if(target == SEL_POINT) {
                                print_point(h_point);
                            } else if(target == SEL_LINE) {
                                print_line(h_line);
                            } else if(target == SEL_SECTOR) {
                                print_sector(h_sector);
                            }
                            break;
                    }

                    if(((SDL_KeyboardEvent *)&event)->mod & SDL_KMOD_SHIFT) {
                        if(move > 0) {
                            move = 10;
                        } else if(move < 0) {
                            move = -10;
                        }
                        if(strafe > 0) {
                            strafe = 10;
                        } else if(strafe < 0) {
                            strafe = -10;
                        }
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
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    mbevent = (SDL_MouseButtonEvent *)(&event);
                    switch(mbevent->button) {
                        case SDL_BUTTON_MIDDLE:
                            drag_move = 1;
                            break;
                        case SDL_BUTTON_RIGHT:
                            drag_turn = 1;
                            break;
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    mbevent = (SDL_MouseButtonEvent *)(&event);
                    switch(mbevent->button) {
                        case SDL_BUTTON_MIDDLE:
                            drag_move = 0;
                            break;
                        case SDL_BUTTON_RIGHT:
                            drag_turn = 0;
                            break;
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    mmevent = (SDL_MouseMotionEvent *)(&event);
                    /* only try to update highlight if the cursor is in the window */
                    if(mmevent->x > 0 && mmevent->x < surface->w &&
                       mmevent->y > 0 && mmevent->y < surface->h) {
                        h_sector = get_sector((unsigned int)mmevent->x, (unsigned int)mmevent->y,
                                              surface->w, surface->h);
                        h_line = get_line((unsigned int)mmevent->x, (unsigned int)mmevent->y,
                                          surface->w, surface->h);
                        h_point = get_point((unsigned int)mmevent->x, (unsigned int)mmevent->y,
                                                surface->w, surface->h);
                    }
                    if(drag_move) {
                        total_move.x = mmevent->xrel;
                        total_move.y = -mmevent->yrel;
                    }
                    if(drag_turn) {
                        total_turn = mmevent->xrel * ((float)(FRAME_WAIT) / 1000.0) * M_PI * 2.0 * MOUSE_SENSITIVITY;
                    }
                    cursor.x = mmevent->x;
                    cursor.y = mmevent->y;
                    break;
            }

            if(!check_select(-1, -1, -1) && !check_prompt()) {
                /* only when not waiting on a specific input */

                switch(event.type) {
                    case SDL_EVENT_KEY_DOWN:
                        switch(((SDL_KeyboardEvent *)&event)->key) {
                            case SDLK_J:
                                /* joining */
                                if(target == SEL_POINT) {
                                    ret = join_lines(s_sector, s_point);
                                    if(ret < 0) {
                                        fprintf(stderr, "Join lines failed.\n");
                                    } else {
                                        h_line = ret;
                                        s_line = ret;
                                        h_point = (*l)[ret].point;
                                        s_point = h_point;
                                    }
                                } else if(target == SEL_LINE) {
                                    if(join_sectors(s_sector, s_line) < 0) {
                                        fprintf(stderr, "Join sectors failed.\n");
                                    }
                                }
                                break;
                            case SDLK_X:
                                /* splitting */
                                if(target == SEL_POINT) {
                                    fprintf(stderr, "Select other point to make line splitting sector.\n");
                                    start_select(split_sector_cb);
                                } else if(target == SEL_LINE) {
                                    if(split_line(s_sector, s_line) < 0) {
                                        fprintf(stderr, "Split line failed.\n");
                                    }
                                }
                                break;
                            case SDLK_L:
                                /* linking */
                                if(target == SEL_LINE) {
                                    if(link_sectors(s_sector, s_line) < 0) {
                                        fprintf(stderr, "Link sectors failed.\n");
                                    }
                                }
                                break;
                            case SDLK_U:
                                /* unlinking */
                                if(target == SEL_LINE) {
                                    if(unlink_sectors(s_sector, s_line) < 0) {
                                        fprintf(stderr, "Unlink sectors failed.\n");
                                    }
                                }
                                break;
                            case SDLK_M:
                                /* moving */
                                if(target == SEL_POINT) {
                                    if(((SDL_KeyboardEvent *)&event)->mod & SDL_KMOD_CTRL) {
                                        fprintf(stderr, "Set Point: Point is at %f, %f.\n", (*p)[s_point].x, (*p)[s_point].y);
                                        print_view();
                                        prompt_point(set_point_cb);
                                    } else {
                                        fprintf(stderr, "Move Point: Point is at %f, %f.\n", (*p)[s_point].x, (*p)[s_point].y);
                                        print_view();
                                        prompt_point(move_point_cb);
                                    }
                                }
                                break;
                            case SDLK_S:
                                /* solidify */
                                if(((SDL_KeyboardEvent *)&event)->mod & SDL_KMOD_CTRL) {
                                    if(target == SEL_SECTOR) {
                                        if(solidify_sector(s_sector) < 0) {
                                            fprintf(stderr, "Solidify sector failed.\n");
                                        }
                                    }
                                }
                                break;
                            case SDLK_T:
                                /* texture */
                                action_which = cursor.y < surface->h / 2 ? CEILING : FLOOR;
                                if(target == SEL_LINE) {
                                    fprintf(stderr, "Set %s line texture: Texture is %hhu.\nTexture: ",
                                            FLAT_NAMES[action_which], (*w)[(*l)[s_line].wall[action_which]].texture);
                                    start_prompt(prompt_line_texture_cb);
                                } else if(target == SEL_SECTOR) {
                                    fprintf(stderr, "Set sector %s texture: Texture is %hhu.\nTexture: ",
                                            FLAT_NAMES[action_which], (*f)[(*s)[s_sector].flat[action_which]].texture);
                                    start_prompt(prompt_sector_texture_cb);
                                }
                                break;
                            case SDLK_H:
                                /* shade */
                                action_which = cursor.y < surface->h / 2 ? CEILING : FLOOR;
                                if(target == SEL_LINE) {
                                    fprintf(stderr, "Set %s line shade: Shade is ", FLAT_NAMES[action_which]);
                                    print_shade((*w)[(*l)[s_line].wall[action_which]].shade);
                                    fprintf(stderr, ".\nShade: ");
                                    start_prompt(prompt_line_shade_cb);
                                } else if(target == SEL_SECTOR) {
                                    if(((SDL_KeyboardEvent *)&event)->mod & SDL_KMOD_CTRL) {
                                        fprintf(stderr, "Set sector %s height: Height is %f.\nHeight: ",
                                                FLAT_NAMES[action_which],
                                                (*s)[s_sector].height[action_which]);
                                        start_prompt(prompt_sector_height_cb);
                                    } else {
                                        fprintf(stderr, "Set sector %s shade: Shade is ", FLAT_NAMES[action_which]);
                                        print_shade((*f)[(*s)[s_sector].flat[action_which]].shade);
                                        fprintf(stderr, ".\nShade: ");
                                        start_prompt(prompt_sector_shade_cb);
                                    }
                                }
                                break;
                            case SDLK_B:
                                /* bias */
                                action_which = cursor.y < surface->h / 2 ? CEILING : FLOOR;
                                if(target == SEL_LINE) {
                                    point = &(*p)[(*w)[(*l)[s_line].wall[action_which]].bias];
                                    fprintf(stderr, "Set %s line bias: Bias is %f, %f.\n", FLAT_NAMES[action_which],
                                            point->x, point->y);
                                    print_view();
                                    prompt_point(set_line_bias_cb);
                                } else if(target == SEL_SECTOR) {
                                    point = &(*p)[(*f)[(*s)[s_sector].flat[action_which]].bias];
                                    fprintf(stderr, "Set sector %s bias: Bias is %f, %f.\n", FLAT_NAMES[action_which],
                                            point->x, point->y);
                                    print_view();
                                    prompt_point(set_sector_bias_cb);
                                }
                                break;
                            case SDLK_R:
                                /* transform */
                                action_which = cursor.y < surface->h / 2 ? CEILING : FLOOR;
                                if(target == SEL_LINE) {
                                    matrix3x2 = &(*m32)[(*w)[(*l)[s_line].wall[action_which]].transform];
                                    fprintf(stderr, "Set %s line transform: Transform is [%f, %f, %f] [%f, %f, %f] .\n",
                                                    FLAT_NAMES[action_which],
                                                    matrix3x2->xx, matrix3x2->xy, matrix3x2->xz,
                                                    matrix3x2->yx, matrix3x2->yy, matrix3x2->yz);
                                    print_view();
                                    prompt_matrix3x2(set_line_transform_cb);
                                } else if(target == SEL_SECTOR) {
                                    matrix2x2 = &(*m22)[(*f)[(*s)[s_sector].flat[action_which]].transform];
                                    fprintf(stderr, "Set sector %s transform: Transform is [%f, %f] [%f, %f] .\n",
                                                    FLAT_NAMES[action_which],
                                                    matrix2x2->xx, matrix2x2->xy,
                                                    matrix2x2->yx, matrix2x2->yy);
                                    print_view();
                                    prompt_matrix2x2(set_sector_transform_cb);
                                }
                                break;
                            case SDLK_N:
                                start_prompt(prompt_mapnum_cb);
                                break;
                            case SDLK_W:
                                if(((SDL_KeyboardEvent *)&event)->mod & SDL_KMOD_CTRL) {
                                    save_file();
                                }
                                break;
                            case SDLK_TAB:
                                target = NEXT_TARGET[target];
                                fprintf(stderr, "Selection target: %s\n", SELECTION_TARGET[target]);
                                break;
                            default:
                                continue;
                        }
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        mbevent = (SDL_MouseButtonEvent *)(&event);
                        switch(mbevent->button) {
                            case SDL_BUTTON_LEFT:
                                s_sector = get_sector((unsigned int)mbevent->x, (unsigned int)mbevent->y,
                                                      surface->w, surface->h);
                                s_line = get_line((unsigned int)mbevent->x, (unsigned int)mbevent->y,
                                                  surface->w, surface->h);
                                s_point = get_point((unsigned int)mbevent->x, (unsigned int)mbevent->y,
                                                    surface->w, surface->h);
                                break;
                        }
                        break;
                }
            } else {
                /* only when waiting on an input */

                switch(event.type) {
                    case SDL_EVENT_KEY_DOWN:
                        switch(((SDL_KeyboardEvent *)&event)->key) {
                            case SDLK_ESCAPE:
                                cancel_input_callbacks();
                                fprintf(stderr, "Input prompts or selections canceled.\n");
                                break;
                        }
                        break;
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                        mbevent = (SDL_MouseButtonEvent *)(&event);
                        switch(mbevent->button) {
                            case SDL_BUTTON_LEFT:
                                check_select(h_sector, h_line, h_point);
                                break;
                        }
                        break;
                }
            }
        }

        if((drag_move == 1 || drag_turn == 1) && !SDL_GetWindowRelativeMouseMode(window)) {
            SDL_SetWindowRelativeMouseMode(window, 1);
        } else if((drag_move == 0 && drag_turn == 0) && SDL_GetWindowRelativeMouseMode(window)) {
            SDL_SetWindowRelativeMouseMode(window, 0);
        }

        if(turning) {
            total_turn += (float)turning * M_PI * 0.01;
        }

        if(move) {
            total_move.y += move;
        }

        if(strafe) {
            total_move.x += strafe;
        }

        if(total_turn != 0.0) {
            v.angle = fmodf(v.angle + total_turn, M_PI * 2.0);
            if(v.angle < 0.0) {
                v.angle += M_PI * 2.0;
            }
        }

        if(total_move.x != 0.0 || total_move.y != 0.0) {
            engine_move(sin(v.angle) * total_move.y + sin(v.angle + M_PI / 2.0) * total_move.x,
                        cos(v.angle) * total_move.y + cos(v.angle + M_PI / 2.0) * total_move.x,
                        action);
        }
    }

error_init:
    SDL_Quit();
error:
    exit(EXIT_SUCCESS);
}

