/* pebble SDK doesn't define this by default */
#if !defined(M_PI)
#define BROKEN_MATH
/* copied from math.h */
# define M_PI           3.14159265358979323846  /* pi */
# define M_PI_2         1.57079632679489661923  /* pi/2 */
/* pebble SDK has a broken math functions */
#define sin(angle) sin_lookup_wrapper(angle)
#define cos(angle) cos_lookup_wrapper(angle)
#define atan2f(y, x) atan2approx(y, x)
#define sqrtf(number) fast_sqrt(number)
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
    Point texture_bias[2];
    Matrix3x2 texture_transform[2];
} Line;

typedef struct Sector_s {
    Line (*line)[];
    unsigned char numlines;

    float height[2];

    unsigned char texture[2];
    short shade[2];
    Point texture_bias[2];
    Matrix2x2 texture_transform[2];
} Sector;

typedef struct {
    Sector *start;
    Point pos;
    float angle;
    float height;

    float fov;
} View;

void engine_load();
void engine_render(unsigned char *pixels, int w, int h, int pitch);
void engine_move(float x, float y);

float sin_lookup_wrapper(float angle);
float cos_lookup_wrapper(float angle);
float atan2approx(float y,float x);
float fast_sqrt(float number);

extern View v;
