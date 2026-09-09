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

typedef struct Line_s {
    Point *point;
    Sector *sector;

    unsigned char texture[2];
    short shade[2];
    Point *texture_bias[2];
    Matrix3x2 *texture_transform[2];
} Line;

typedef struct Sector_s {
    Line *firstline;
    unsigned char lines;

    float height[2];

    unsigned char texture[2];
    short shade[2];
    Point *texture_bias[2];
    Matrix2x2 *texture_transform[2];

    unsigned int action;
} Sector;

typedef struct {
    Sector *start;
    Point pos;
    float angle;
    float startheight;
    float height;

    float fov;
} View;

typedef int (* open_map_t)(unsigned char number);
typedef int (* read_map_t)(off_t offset, size_t length, void *data);
typedef void (* close_map_t)();

extern open_map_t open_map_p;
extern read_map_t read_map_p;
extern close_map_t close_map_p;

int engine_load(unsigned char number, unsigned char view);
void engine_render(unsigned char *pixels, int w, int h, int pitch);
void engine_move(float x, float y);

float sin_lookup_wrapper(float angle);
float cos_lookup_wrapper(float angle);
float tanf_custom(float angle);
float atan2approx(float y,float x);
float fast_sqrt(float number);
float fmodf_custom(float x, float y);

extern View v;
