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
void engine_render(char *pixels, int w, int h, int pitch);
void engine_move(float x, float y);

extern View v;
