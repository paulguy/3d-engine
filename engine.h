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

typedef struct Sector_s Sector;
typedef struct Line_s Line;

typedef struct Line_s {
    Point *point;
    Sector *sector;
    int texture[2];
} Line;

typedef struct Sector_s {
    Line (*line)[];
    int numlines;

    float floor_h;
    float ceiling_h;
    int texture[2];
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
