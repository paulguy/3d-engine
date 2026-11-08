#ifdef PBL_SDK_3
/* pebble SDK doesn't define this by default */
/* copied from math.h */
# define M_PI           3.14159265358979323846  /* pi */
# define M_PI_2         1.57079632679489661923  /* pi/2 */

/* pebble SDK has a broken math functions */
#define sin(angle) sin_lookup_wrapper(angle)
#define cos(angle) cos_lookup_wrapper(angle)
#define tanf(angle) tanf_custom(angle)
#define atan2f(y, x) atan2approx(y, x)
#define sqrtf(number) fast_sqrt(number)
#define fmodf(x, y) fmodf_custom(x, y)
/* unmask the ifdef to compile these functions */
#define BROKEN_MATH

/* this contains off_t and size_t */
#include <stdio.h>
#else
/* the sys/types.h from pebble SDK causes a compile error */
#include <sys/types.h>
#endif

#define CEILING (0)
#define FLOOR (1)

#define SHADE_SUB (0x100)
#define SHADE_PARALLAX (0x200)

#define ACTION_NONE (0x00)
#define ACTION_WARP (0x01)
#define ACTION_WAYPOINT (0x02)

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    unsigned short points;
    unsigned short matrix2x2s;
    unsigned short matrix3x2s;
    unsigned short walls;
    unsigned short lines;
    unsigned short flats;
    unsigned short sectors;
    unsigned short links;
    unsigned short views;
} Header;

typedef struct {
    float x, y;
} Point;

typedef struct {
    float xx, xy;
    float yx, yy;
} Matrix2x2;

typedef struct {
    float xx, xy, xz;
    float yx, yy, yz;
} Matrix3x2;

typedef struct Sector_s Sector;
typedef struct Line_s Line;

typedef struct {
    unsigned char texture;
    unsigned short shade;
    unsigned short bias;
    unsigned short transform;
} Wall;

typedef struct Line_s {
    unsigned short point;
    unsigned short sector;

    unsigned short wall[2];
} Line;

typedef struct {
    unsigned char texture;
    unsigned short shade;
    unsigned short bias;
    unsigned short transform;
} Flat;

typedef struct Sector_s {
    unsigned short firstline;
    unsigned char lines;

    float height[2];

    unsigned short flat[2];

    unsigned int action;
} Sector;

typedef struct {
    unsigned short start;
    Point pos;
    float angle;
    float startheight;
    float height;

    float fov;
} View;

typedef int (* map_exists_t)(unsigned char number);
typedef int (* open_map_t)(unsigned char number);
typedef int (* read_map_t)(off_t offset, size_t length, void *data);
typedef void (* close_map_t)();

extern map_exists_t map_exists_p;
extern open_map_t open_map_p;
extern read_map_t read_map_p;
extern close_map_t close_map_p;

int engine_load(unsigned char number, unsigned char view);
void engine_render(unsigned char *pixels, int width, int height, int pitch
#ifdef EDITOR
                   , int hl_point, int hl_line, int hl_sector
                   , int sel_point, int sel_line, int sel_sector
#endif
        );
void engine_move(float x, float y,
                 int action);

#ifdef EDITOR
void print_data(Header *h);

int get_sector(unsigned int x, unsigned int y,
               unsigned int w, unsigned int h);
int get_line(unsigned int x, unsigned int y,
             unsigned int w, unsigned int h);
int get_point(unsigned int x, unsigned int y,
              unsigned int w, unsigned int h);

/* these shouldn't need to be accessible outside of editor mode */
extern Header h;
extern Point (*p)[];
extern Matrix2x2 (*m22)[];
extern Matrix3x2 (*m32)[];
extern Wall (*w)[];
extern Line (*l)[];
extern Flat (*f)[];
extern Sector (*s)[];

int split_line(int sindex, int lindex);
int join_lines(int sindex, int pindex);
int set_point(int sindex, int pindex,
              float x, float y);
void move_point(int pindex,
                float x, float y);
int set_line_texture(int lindex, int which,
                     unsigned char texture);
int set_line_shade(int lindex, int which,
                   unsigned short shade);
int set_line_bias(int lindex, int which,
                  float x, float y);
int set_line_transform(int lindex, int which,
                       float xx, float xy, float xz,
                       float yx, float yy, float yz);
int split_sector(int sindex, int pindex1, int pindex2);
int join_sectors(int sindex, int lindex);
int link_sectors(int sindex, int lindex);
int unlink_sectors(int sindex, int lindex);
int solidify_sector(int sindex);
int set_sector_texture(int sindex, int which,
                       unsigned char texture);
int set_sector_shade(int sindex, int which,
                     unsigned short shade);
int set_sector_bias(int sindex, int which,
                    float x, float y);
int set_sector_transform(int sindex, int which,
                         float xx, float xy,
                         float yx, float yy);
#endif
 
float sin_lookup_wrapper(float angle);
float cos_lookup_wrapper(float angle);
float tanf_custom(float angle);
float atan2approx(float y,float x);
float fast_sqrt(float number);
float fmodf_custom(float x, float y);

extern View v;
