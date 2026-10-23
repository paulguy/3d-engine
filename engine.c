/* slow, but small raycaster 3D engine with non-axis aligned walls and
 * differing height walls and ceilings.
 * Need to keep it stupid to avoid needing stacks or heaps or gathering/sorting
 *
 * for each column
 *     scan all sector walls until a wall collides with the current ray
 *     draw floor up to that distance
 *     draw a wall or any visible upper or lower wall
 *     if no other sector or max distance is reached
 *         next column
 *     if there's a portal, go around again but don't check portal wall just passed through
 *
 * determining columns:
 *     calculate "axis" and "rise" and "run" from projected angle based on how far off center it is
 *         these will be used for the wall hit tests
 *         Maybe store wall data this way?  more data but faster?
 *     distances should all be parallel to the camera plane since these calculations will be cartesian?
 *     this table can be built once per frame, probably
 *
 * collision check
 *     if wall is axis aligned, find the position it passes through at
 *     calculate rise and run for the wall along the same axis
 *     find position they intersect at
 *     find the wall's start and end points on this axis and determine if the intersection is between them
 *
 * drawing flats
 *     for each pixel in the column
 *         sample position based on column rise/run value to see where/if it hits the floor
 *         if the distance goes past the sector
 *             end drawing
 *         lookup pixel value in texture and draw it
 *
 *     rise / run values per row can probably be calculated once at start
 *
 * drawing walls
 *     find X position where collision hit
 *     start at Y height
 *     calculate distance
 *     determine scale based on distance
 *     sample each pixel iterating Y position based on scale
 *
 * on movement:
 *     cast ray from center to distance traveling through sectors similarly to line drawing
 *     update view sector
 *     update view position
 *     if there was no sector to travel through, error
 *
 * structures:
 *     point:
 *         x, y
 *     array of points
 *
 *     line:
 *         start point index
 *         sector1 index
 *         sector2 index
 *         lower texture
 *         upper texture
 *         other parameters about texture display
 *     array of lines
 *
 *     sector:
 *         array of line indexes
 *         floor height
 *         ceiling height
 *         floor texture
 *         ceiling texture
 *     array of sectors
 *
 *     view:
 *         sector index
 *         position point
 *         angle
 *
 *     array of per unit float deltas for vertical rays (calculated once)
 *     array of per unit float deltas for horizontal rays (calculated per frame)
 */

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#ifdef EDITOR
#include <string.h>
#endif

#ifdef BROKEN_MATH
/* needed for heap_bytes_free() */
#include <pebble.h>
#endif

#include "engine.h"
#include "log.h"
#include "cache.h"

#define FOV (60.0 / 360.0 * (M_PI * 2.0))

#define CEILING (0)
#define FLOOR (1)

#define ACTION_NONE (0x00)
#define ACTION_WARP (0x01)
#define ACTION_WAYPOINT (0x02)

#define SHADE_SUB (0x100)
#define SHADE_PARALLAX (0x200)

#define HIGHLIGHT_DENSITY (4)
#define HIGHLIGHT_LINE_THICKNESS (2.0)
#define HIGHLIGHT_PHASE (0)
#define SELECTION_PHASE (2)

#define NO_SECTOR (0xFFFF)

/* these are all packed in the file, so very efficient access isn't necessary */

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

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    float x, y;
} Point_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    float xx, xy;
    float yx, yy;
} Matrix2x2_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    float xx, xy, xz;
    float yx, yy, yz;
} Matrix3x2_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    unsigned char texture;
    unsigned short shade;
    unsigned short bias;
    unsigned short transform;
} Wall_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    unsigned short point;
    /* sectors are lined by a separate link table */

    unsigned short wall[2];
} Line_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    unsigned char texture;
    unsigned short shade;
    unsigned short bias;
    unsigned short transform;
} Flat_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    float height[2];

    unsigned short flat[2];

    unsigned short firstline;
    unsigned char lines;

    unsigned int action;
} Sector_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    unsigned short sector;
    unsigned short line;
    unsigned short lsector;
    unsigned short lline;
} Link_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    unsigned short start;
    unsigned short pos;
    float angle;
    float startheight;

    float fov;
} View_data;

typedef union {
    Point_data p;
    Matrix2x2_data m22;
    Matrix3x2_data m32;
    Wall_data w;
    Line_data l;
    Flat_data f;
    Sector_data s;
    Link_data li;
    View_data v;
} Data;

unsigned char mapnum;
View v;
Point waypoint;
Point (*p)[] = NULL;
Matrix2x2 (*m22)[] = NULL;
Matrix3x2 (*m32)[] = NULL;
Wall (*w)[] = NULL;
Line (*l)[] = NULL;
Flat (*f)[] = NULL;
Sector (*s)[] = NULL;

#ifdef EDITOR
int *point_refs = NULL;
int *m22_refs = NULL;
int *m32_refs = NULL;
int *wall_refs = NULL;
int *flat_refs = NULL;
Header h;
#endif

map_exists_t map_exists_p;
open_map_t open_map_p;
read_map_t read_map_p;
close_map_t close_map_p;

static void print_data(Header *h) {
    int i;

    LOG("%hu %hu %hu %hu %hu %hu %hu\n", h->points, h->matrix2x2s, h->matrix3x2s, h->lines, h->sectors, h->links, h->views);

    for(i = 0; i < h->points; i++) {
        LOG("%d Point %f %f\n", i, (*p)[i].x, (*p)[i].y);
    }

    for(i = 0; i < h->matrix2x2s; i++) {
        LOG("%d Matrix2x2 %f %f %f %f\n", i, (*m22)[i].xx, (*m22)[i].xy, (*m22)[i].yx, (*m22)[i].yy);
    }

    for(i = 0; i < h->matrix3x2s; i++) {
        LOG("%d Matrix3x2 %f %f %f %f %f %f\n", i, (*m32)[i].xx, (*m32)[i].xy, (*m32)[i].xz, (*m32)[i].yx, (*m32)[i].yy, (*m32)[i].yz);
    }

    for(i = 0; i < h->walls; i++) {
        LOG("%d Wall %hhu %02hX %hu %hu\n", i,
            (*w)[i].texture, (*w)[i].shade, (*w)[i].bias, (*w)[i].transform);
    }

    for(i = 0; i < h->lines; i++) {
        LOG("%d Line %hu %hu %hu %hu\n", i,
            (*l)[i].point, (*l)[i].sector,
            (*l)[i].wall[CEILING], (*l)[i].wall[FLOOR]);
    }

    for(i = 0; i < h->flats; i++) {
        LOG("%d Flat %hhu %02hX %hu %hu\n", i,
            (*f)[i].texture, (*f)[i].shade, (*f)[i].bias, (*f)[i].transform);
    }

    for(i = 0; i < h->sectors; i++) {
        LOG("%d Sector %f %f %hu %hu %hu %hhu ", i,
            (*s)[i].height[CEILING], (*s)[i].height[FLOOR],
            (*s)[i].flat[CEILING], (*s)[i].flat[FLOOR],
            (*s)[i].firstline, (*s)[i].lines);
        switch((*s)[i].action & 0xFF) {
            case ACTION_NONE:
                LOG("None\n");
                break;
            case ACTION_WARP:
                LOG("Warp %hhu %hhu\n", ((*s)[i].action & 0xFF00) >> 8, ((*s)[i].action & 0xFF0000) >> 16);
                break;
            case ACTION_WAYPOINT:
                LOG("Waypoint %hu %hhu\n", ((*s)[i].action & 0xFFFF00) >> 8, ((*s)[i].action & 0xFF000000) >> 24);
                break;
            default:
                LOG("Unknown\n");
        }
    }

    LOG("View %hu %f %f %f %f %f %f\n",
        v.start, v.pos.x, v.pos.y,
        v.angle, v.startheight, v.height, v.fov);
}

void apply_view_data(View_data *view) {
    v.start = view->start;
    v.pos.x = (*p)[view->pos].x;
    v.pos.y = (*p)[view->pos].y;
    v.angle = view->angle;
    v.startheight = view->startheight;
    v.fov = view->fov;

    /* set the player's world height */
    v.height = (*s)[v.start].height[FLOOR] + v.startheight;
}

int init_map();

int engine_load(unsigned char number, unsigned char view) {
#ifndef EDITOR
    Header h;
#endif
    Data d;
    int i;
    off_t start = 0;

#ifdef BROKEN_MATH
    /* pebble */
    LOG("Pre-free %d bytes\n", heap_bytes_free());
#endif

    if(p != NULL) {
        free(s);
        free(f);
        free(l);
        free(w);
        free(m32);
        free(m22);
        free(p);
#ifdef EDITOR
        free(point_refs);
        free(m22_refs);
        free(m32_refs);
        free(wall_refs);
        free(flat_refs);
#endif
    }

#ifdef BROKEN_MATH
    LOG("Post-free %d bytes\n", heap_bytes_free());
#endif

#ifdef EDITOR
    /* just let it error out on pebble rather than use the memory on loading a blank map */
    if(!map_exists_p(number)) {
        init_map();
        print_data(&h);
        return(0);
    }
#endif

    if(open_map_p(number) < 0) {
        goto error;
    }

    /* don't bother checking return values because
     * there isn't much that can be done on a read failure and it
     * won't work and won't be published */
    read_map_p(0, sizeof(Header), &h);

    p = malloc(h.points * sizeof(Point));
    if(p == NULL) {
        goto error_open;
    }

    m22 = malloc(h.matrix2x2s * sizeof(Matrix2x2));
    if(m22 == NULL) {
        goto error_points;
    }

    m32 = malloc(h.matrix3x2s * sizeof(Matrix3x2));
    if(m32 == NULL) {
        goto error_m22s;
    }

    w = malloc(h.walls * sizeof(Wall));
    if(w == NULL) {
        goto error_m32s;
    }

    l = malloc(h.lines * sizeof(Line));
    if(l == NULL) {
        goto error_walls;
    }

    f = malloc(h.flats * sizeof(Flat));
    if(f == NULL) {
        goto error_lines;
    }

    s = malloc(h.sectors * sizeof(Sector));
    if(s == NULL) {
        goto error_flats;
    }

#ifdef EDITOR
    /* not too worried about freeing these right since it'll just be crashing out after anyway and this isn't for end users anyway */
    point_refs = malloc(h.points * sizeof(int));
    if(point_refs == NULL) {
        goto error_open;
    }

    m22_refs = malloc(h.matrix2x2s * sizeof(int));
    if(m22_refs == NULL) {
        goto error_open;
    }

    m32_refs = malloc(h.matrix3x2s * sizeof(int));
    if(m32_refs == NULL) {
        goto error_open;
    }

    wall_refs = malloc(h.walls * sizeof(int));
    if(wall_refs == NULL) {
        goto error_open;
    }

    flat_refs = malloc(h.flats * sizeof(int));
    if(flat_refs == NULL) {
        goto error_open;
    }
#endif

    start += sizeof(Header);
    for(i = 0; i < h.points; i++) {
        read_map_p(start + (i * sizeof(Point_data)), sizeof(Point_data), &d.p);
        (*p)[i].x = d.p.x;
        (*p)[i].y = d.p.y;
    }

    start += sizeof(Point_data) * h.points;
    for(i = 0; i < h.matrix2x2s; i++) {
        read_map_p(start + (i * sizeof(Matrix2x2_data)), sizeof(Matrix2x2_data), &d.m22);
        (*m22)[i].xx = d.m22.xx;
        (*m22)[i].xy = d.m22.xy;
        (*m22)[i].yx = d.m22.yx;
        (*m22)[i].yy = d.m22.yy;
    }

    start += sizeof(Matrix2x2_data) * h.matrix2x2s;
    for(i = 0; i < h.matrix3x2s; i++) {
        read_map_p(start + (i * sizeof(Matrix3x2_data)), sizeof(Matrix3x2_data), &d.m32);
        (*m32)[i].xx = d.m32.xx;
        (*m32)[i].xy = d.m32.xy;
        (*m32)[i].xz = d.m32.xz;
        (*m32)[i].yx = d.m32.yx;
        (*m32)[i].yy = d.m32.yy;
        (*m32)[i].yz = d.m32.yz;
    }

    start += sizeof(Matrix3x2_data) * h.matrix3x2s;
    for(i = 0; i < h.walls; i++) {
        read_map_p(start + (i * sizeof(Wall_data)), sizeof(Wall_data), &d.w);
        (*w)[i].texture = d.w.texture;
        (*w)[i].shade = d.w.shade;
        (*w)[i].bias = d.w.bias;
        (*w)[i].transform = d.w.transform;
#ifdef EDITOR
        point_refs[d.w.bias]++;
        m32_refs[d.w.transform]++;
#endif
    }

    start += sizeof(Wall_data) * h.walls;
    for(i = 0; i < h.lines; i++) {
        read_map_p(start + (i * sizeof(Line_data)), sizeof(Line_data), &d.l);
        (*l)[i].point = d.l.point;
        /* sector will be filled when links are read */
        (*l)[i].sector = NO_SECTOR;
        (*l)[i].wall[CEILING] = d.l.wall[CEILING];
        (*l)[i].wall[FLOOR] = d.l.wall[FLOOR];
#ifdef EDITOR
        point_refs[d.l.point]++;
        wall_refs[d.l.wall[CEILING]]++;
        wall_refs[d.l.wall[FLOOR]]++;
#endif
    }

    start += sizeof(Line_data) * h.lines;
    for(i = 0; i < h.flats; i++) {
        read_map_p(start + (i * sizeof(Flat_data)), sizeof(Flat_data), &d.f);
        (*f)[i].texture = d.f.texture;
        (*f)[i].shade = d.f.shade;
        (*f)[i].bias = d.f.bias;
        (*f)[i].transform = d.f.transform;
#ifdef EDITOR
        point_refs[d.w.bias]++;
        m22_refs[d.w.transform]++;
#endif
    }

    start += sizeof(Flat_data) * h.flats;
    for(i = 0; i < h.sectors; i++) {
        read_map_p(start + (i * sizeof(Sector_data)), sizeof(Sector_data), &d.s);
        d.s.lines += 3; /* 3 lines minimum so 0 is 3 mines */
        (*s)[i].height[CEILING] = d.s.height[CEILING];
        (*s)[i].height[FLOOR] = d.s.height[FLOOR];
        (*s)[i].flat[CEILING] = d.s.flat[CEILING];
        (*s)[i].flat[FLOOR] = d.s.flat[FLOOR];
        (*s)[i].firstline = d.s.firstline;
        (*s)[i].lines = d.s.lines;
        (*s)[i].action = d.s.action;
#ifdef EDITOR
        flat_refs[d.s.flat[CEILING]]++;
        flat_refs[d.s.flat[FLOOR]]++;
#endif
    }

    start += sizeof(Sector_data) * h.sectors;
    for(i = 0; i < h.links; i++) {
        read_map_p(start + (i * sizeof(Link_data)), sizeof(Link_data), &d.li);
        /* link first sector to second */
        (*l)[(*s)[d.li.sector].firstline + d.li.line].sector = d.li.lsector;
        /* link second sector to first */
        (*l)[(*s)[d.li.lsector].firstline + d.li.lline].sector = d.li.sector;
    }

    start += sizeof(Link_data) * h.links;
    /* read the selected view */
    read_map_p(start + (view * sizeof(View_data)), sizeof(View_data), &d.v);
    apply_view_data(&d.v);

    close_map_p();

#ifdef BROKEN_MATH
    LOG("Post-load %d bytes\n", heap_bytes_free());
#else
    print_data(&h);
    LOG("Memory in bytes - Points %d Matrix2x2s %d Matrix3x2s %d Walls %d Lines %d Flats %d Sectors %d Total %d\n",
         h.points * sizeof(Point), h.matrix2x2s * sizeof(Matrix2x2), h.matrix3x2s * sizeof(Matrix3x2), h.walls * sizeof(Wall), h.lines * sizeof(Line),h.flats * sizeof(Flat), h.sectors * sizeof(Sector),
        (h.points * sizeof(Point)) + (h.matrix2x2s * sizeof(Matrix2x2)) + (h.matrix3x2s * sizeof(Matrix3x2)) + (h.walls * sizeof(Wall)) + (h.lines * sizeof(Line)) + (h.flats * sizeof(Flat)) + (h.sectors * sizeof(Sector)));
#endif

    mapnum = number;

    return(0);

error_flats:
    free(f);
error_lines:
    free(l);
error_walls:
    free(w);
error_m32s:
    free(m32);
error_m22s:
    free(m22);
error_points:
    free(p);
error_open:
    close_map_p();
error:
    return(-1);
}

int line_hit(Point *p1, Point *p2,
             Point *pos, Point *slope,
             int vvertical, float vslope,
             Point *hit) {
    float x, y;
    float lslope;
    /* view is determined as a general axis facing direction and a slope representing the line
     * firing out from the view position out of a range of 1/4 pi from both directions
     * of the axis.
     *
     * The slopes of the wall line need to be figured out along the same axis as the view axis.
     *
     * the 
     * equations for view ray
     * NX, PX
     * y = (slope * x) + pos->y - (slope * pos->x)
     * NY, PY
     * x = (slope * y) + pos->x - (slope * pos->y)
     *
     * equation for wall line
     * y = (lslope * x) + p1->y - (lslope * p1->x)
     *
     * (slope * x) + pos->y - (slope * pos->x) = (lslope * x) + p1->y - (lslope * p1->x)
     * (slope * x) - (lslope * x) = p1->y - (lslope * p1->x) - pos->y + (slope * pos->x)
     * x = (-(lslope * p1->x) + (slope * pos->x) + p1->y - pos->y) / (slope - lslope)
    */

    if(vvertical) {
        /* view ray about Y axis */
        if(p1->y == p2->y) {
            /* horizontal line, Y is Y */
            y = p1->y;
        } else {
            /* calculate line slope */
            lslope = (p2->x - p1->x) / (p2->y - p1->y);

            /* find Y intersection */
            y = (-(lslope * p1->y) + (vslope * pos->y) + p1->x - pos->x) / (vslope - lslope);

            /* if intercept is outside of the line's bounds, it's not eligible */
            if(!((y >= p1->y && y <= p2->y) ||
                 (y >= p2->y && y <= p1->y))) {
                return(0);
            }
        }

        /* calculate X */
        if(p1->x == p2->x) {
            x = p1->x;
        } else {
            x = (vslope * y) + pos->x - (vslope * pos->y);
        }

        if(((x >= p1->x && x <= p2->x) ||
            (x >= p2->x && x <= p1->x)) &&
           ((slope->y > 0.0 && y >= pos->y) ||
            (slope->y < 0.0 && y <= pos->y))) {
            /* X intercept is between line points and in front of view */
            hit->x = x;
            hit->y = y;
            return(1);
        }
    } else {
        /* view ray about X axis */
        if(p1->x == p2->x) {
            /* vertical line, X is just X */
            x = p1->x;
        } else {
            /* calculate line slope */
            lslope = (p2->y - p1->y) / (p2->x - p1->x);

            /* find X intersection */
            x = (-(lslope * p1->x) + (vslope * pos->x) + p1->y - pos->y) / (vslope - lslope);

            /* if intercept is outside of the line's bounds, it's not eligible */
            if(!((x >= p1->x && x <= p2->x) ||
                 (x >= p2->x && x <= p1->x))) {
                return(0);
            }
        }
 
        /* calculate Y */
        if(p1->y == p2->y) {
            y = p1->y;
        } else {
            y = (vslope * x) + pos->y - (vslope * pos->x);
        }

        if(((y <= p1->y && y >= p2->y) ||
            (y <= p2->y && y >= p1->y)) &&
           ((slope->x > 0.0 && x >= pos->x) ||
            (slope->x < 0.0 && x <= pos->x))) {
            /* X intercept is between line points and in front of view */
            hit->x = x;
            hit->y = y;
            return(1);
        }
    }

    /* missed */

    return(0);
}
 
int scan_sector(Sector *last_s, Sector *sector,
                Point *pos, Point *slope,
                int vvertical, float vslope,
                Point *hit) {
    Point *point, *nextpoint;
    unsigned short lindex;
    Line *line;
    int i;

    point = &(*p)[(*l)[sector->firstline].point];
    for(i = 0; i < sector->lines; i++) {
        lindex = sector->firstline + i;
        line = &(*l)[lindex];

        /* current line spans from the current point to the next line point */
        nextpoint = &(*p)[(*l)[sector->firstline + ((i + 1) % sector->lines)].point];

        /* don't check for lines that would look back towards the previous sector */
        if(last_s != NULL && &(*s)[line->sector] == last_s) {
            /* make sure the point used in the next iteration is updated */
            point = nextpoint;
            continue;
        }

        if(line_hit(point, nextpoint,
                    pos, slope,
                    vvertical, vslope,
                    hit)) {
            return(lindex);
        }

        point = nextpoint;
    }

    return(-1);
}

float get_distance(Point *pos, Point *hit, Point *slope) {
    if(fabs(slope->x) > fabs(slope->y)) {
        return(fabs((pos->x - hit->x) / slope->x));
    }

    return(fabs((pos->y - hit->y) / slope->y));
}

void set_tex(unsigned char num,
             unsigned char **data,
             unsigned char *mask,
             unsigned char *shift,
             unsigned char *dim) {
    *dim = load_tex(num, data);
    if(*dim == 64) {
        *mask = 0x3F;
        *shift = 6;
    } else if(*dim == 32) {
        *mask = 0x1F;
        *shift = 5;
    }
}

void engine_render(unsigned char *pixels, int width, int height, int pitch
#ifdef EDITOR
                   , int hl_point, int hl_line, int hl_sector
                   , int sel_point, int sel_line, int sel_sector
#endif
        ) {
#ifdef EDITOR
    Point *h_point = hl_point < 0 ? NULL : &(*p)[hl_point];
    Line *h_line = hl_line < 0 ? NULL : &(*l)[hl_line];
    Sector *h_sector = hl_sector < 0 ? NULL : &(*s)[hl_sector];
    Point *s_point = sel_point < 0 ? NULL : &(*p)[sel_point];
    Line *s_line = sel_line < 0 ? NULL : &(*l)[sel_line];
    Sector *s_sector = sel_sector < 0 ? NULL : &(*s)[sel_sector];
#endif
    Sector *sector, *last_s;
    float offset;
    Point slope;
    int vvertical;
    float vslope;
    int lindex;
    Line *line;
    Point hit;
    float total_distance;
    int x, y;
    float z;
    float wx, wy, wz;
    int tx, ty;
    int top, bottom;
    Point pos;
    float ceilingdiff;
    float floordiff;
    float next_y;
    unsigned char floor_tex = 0;
    unsigned char floor_dim = 0;
    unsigned char floor_shift = 0;
    unsigned char floor_mask = 0;
    unsigned char *floor_data = NULL;
    unsigned char ceiling_tex = 0;
    unsigned char ceiling_dim = 0;
    unsigned char ceiling_shift = 0;
    unsigned char ceiling_mask = 0;
    unsigned char *ceiling_data = NULL;
    unsigned char top_line_tex = 0;
    unsigned char top_line_dim = 0;
    unsigned char top_line_shift = 0;
    unsigned char top_line_mask = 0;
    unsigned char *top_line_data = NULL;
    unsigned char bottom_line_tex = 0;
    unsigned char bottom_line_dim = 0;
    unsigned char bottom_line_shift = 0;
    unsigned char bottom_line_mask = 0;
    unsigned char *bottom_line_data = NULL;
    unsigned short color;
    Point bias;
    Matrix2x2 *transform22;
    Matrix3x2 *transform32;
    unsigned short shade;
    float x_ratio;

    float w_2 = (float)width / 2.0;
    float h_2 = (float)height / 2.0;
    float fov_2 = v.fov / 2.0;
    float sin_angle = sin(v.angle);
    float cos_angle = cos(v.angle);
    float max_offset = atan2f(fov_2, 1.0) * 2.0;
    float bg_pan = v.angle / (M_PI * 2.0);

    age_slots();

    /* scale horizontally to keep parallax texture pixels square */
    x_ratio = (float)width / height;

    /* for each column */
    for(x = 0; x < width; x++) {
        /* this is probably not right ... */
        offset = (float)(x - w_2) / w_2 * max_offset;
        slope.x = sin_angle + (cos_angle * offset);
        slope.y = cos_angle - (sin_angle * offset);

        if(fabs(slope.y) > fabs(slope.x)) {
            /* view ray about Y axis */
            vvertical = 1;
            vslope = slope.x / slope.y;
        } else {
            /* view ray about X axis */
            vvertical = 0;
            vslope = slope.y / slope.x;
        }
 
        sector = &(*s)[v.start];
        last_s = NULL;
        top = 0;
        bottom = height - 1;
        pos.x = v.pos.x;
        pos.y = v.pos.y;
        total_distance = 0.0;
        while (true) {
            lindex = scan_sector(last_s, sector, &pos,
                                 &slope, vvertical, vslope,
                                 &hit);

            /* shouldn't happen, but in case the ray misses for some reason */
            if(lindex < 0) {
                break;
            }
            line = &(*l)[lindex];

            /* get the distance between the view and hit coordinates */
            total_distance += get_distance(&pos, &hit, &slope);

            /* iterate y (angle transformed with FOV to get an angle from screen Y) with height
             * from bottom solving for distance until total distance is reached.
             * Texture X and Y lookup from offset from view given ray angle */

            /* draw ceiling */
            ceilingdiff = sector->height[CEILING] - v.height;
            if(ceilingdiff > 0.0) {
                /* ceiling is above the view line */

                /* get ceiling texture */
                if((*f)[sector->flat[CEILING]].texture != ceiling_tex) {
                    ceiling_tex = (*f)[sector->flat[CEILING]].texture;
                    set_tex(ceiling_tex, &ceiling_data, &ceiling_mask, &ceiling_shift, &ceiling_dim);
                }
 
                bias.x = (*p)[(*f)[sector->flat[CEILING]].bias].x;
                bias.y = (*p)[(*f)[sector->flat[CEILING]].bias].y;
                transform22 = &(*m22)[(*f)[sector->flat[CEILING]].transform];
                shade = (*f)[sector->flat[CEILING]].shade;
                /* silence a warning */
                wy = 0.0;
                if(shade & SHADE_PARALLAX) {
                    wx = (float)x / width * x_ratio * ceiling_dim;
                    /* apply panning */
                    bias.x += bg_pan * ceiling_dim;
                }
                for(y = top; y < h_2 && y < bottom; y++) {
                    z = ceilingdiff / ((h_2 - y) / h_2) * max_offset;

                    if(z > total_distance) {
                        break;
                    }
                    if(ceiling_dim == 0) {
                        color = 0x00;
                    } else {
                        if(shade & SHADE_PARALLAX) {
                            wy = (float)y / height * ceiling_dim;
                        } else {
                            wx = v.pos.x + (slope.x * z);
                            wy = v.pos.y + (slope.y * z);
                        }
                        tx = (wx * transform22->xx) +
                             (wy * transform22->xy) +
                             bias.x;
                        ty = (wx * transform22->yx) +
                             (wy * transform22->yy) +
                             bias.y;
                        color = ceiling_data[(ty & ceiling_mask) << ceiling_shift | (tx & ceiling_mask)];
                    }

                    if(shade & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += shade & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += shade & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
#ifdef EDITOR
                    if(h_point != NULL && fabsf(h_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(h_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(s_point != NULL && fabsf(s_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(s_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }

                    if(sector == h_sector) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(sector == s_sector) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }
#endif
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                top = y;
            }

            /* draw floor */
            floordiff = sector->height[FLOOR] - v.height;
            if(floordiff < 0.0) {
                if((*f)[sector->flat[FLOOR]].texture != floor_tex) {
                    floor_tex = (*f)[sector->flat[FLOOR]].texture;
                    set_tex(floor_tex, &floor_data, &floor_mask, &floor_shift, &floor_dim);
                }

                bias.x = (*p)[(*f)[sector->flat[FLOOR]].bias].x;
                bias.y = (*p)[(*f)[sector->flat[FLOOR]].bias].y;
                transform22 = &(*m22)[(*f)[sector->flat[FLOOR]].transform];
                shade = (*f)[sector->flat[FLOOR]].shade;
                /* silence a warning */
                wy = 0.0;
                if(shade & SHADE_PARALLAX) {
                    wx = (float)x / width * x_ratio * floor_dim;
                    bias.x += bg_pan * floor_dim;
                }
                for(y = bottom; y >= 0 && y >= top; y--) {
                    z = floordiff / ((h_2 - y) / h_2) * max_offset;

                    if(z >= total_distance) {
                        break;
                    }
                    if(floor_dim == 0) {
                        color = 0x00;
                    } else {
                        if(shade & SHADE_PARALLAX) {
                            wy = (float)y / height * floor_dim;
                        } else {
                            wx = v.pos.x + (slope.x * z);
                            wy = v.pos.y + (slope.y * z);
                        }
                        tx = (wx * transform22->xx) +
                             (wy * transform22->xy) +
                             bias.x;
                        ty = (wx * transform22->yx) +
                             (wy * transform22->yy) +
                             bias.y;
                        color = floor_data[(ty & floor_mask) << floor_shift | (tx & floor_mask)];
                    }
                    if(shade & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += shade & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += shade & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
#ifdef EDITOR
                    if(h_point != NULL && fabsf(h_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(h_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(s_point != NULL && fabsf(s_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(s_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }

                    if(sector == h_sector) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(sector == s_sector) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }
#endif
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                bottom = y;
            }

            /* get top wall texture parameters as these'll most likely be needed */
            if((*w)[line->wall[CEILING]].texture != top_line_tex) {
                top_line_tex = (*w)[line->wall[CEILING]].texture;
                set_tex(top_line_tex, &top_line_data, &top_line_mask, &top_line_shift, &top_line_dim);
            }
            bias.x = (*p)[(*w)[line->wall[CEILING]].bias].x;
            bias.y = (*p)[(*w)[line->wall[CEILING]].bias].y;
            transform32 = &(*m32)[(*w)[line->wall[CEILING]].transform];
            shade = (*w)[line->wall[CEILING]].shade;
            if(shade & SHADE_PARALLAX) {
                /* calculate screen X position relative to texture res */
                wx = (float)x / width * x_ratio * top_line_dim;
                /* silence a warning */
                wy = 0.0;
                /* Z is unused */
                wz = 0.0;
                bias.x += bg_pan * top_line_dim;
            } else {
                /* calculate world coordinates */
                wx = v.pos.x + (slope.x * total_distance);
                wy = v.pos.y + (slope.y * total_distance);
                /* silence a warning */
                wz = 0.0;
            }

            if(line->sector == NO_SECTOR) {
                /* solid wall, no sector on the other side */
                for(y = top;
                    y <= bottom && y < height;
                    y++) {
                    if(top_line_dim == 0) {
                        color = 0x00;
                    } else {
                        if(shade & SHADE_PARALLAX) {
                            /* calculate screen y position */
                            wy = (float)y / height * top_line_dim;
                        } else {
                            /* calculate height */
                            wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                        }

                        /* calculate translated texcoords */
                        tx = (wx * transform32->xx) +
                             (wy * transform32->xy) +
                             (wz * transform32->xz) +
                             bias.x;
                        ty = (wx * transform32->yx) +
                             (wy * transform32->yy) +
                             (wz * transform32->yz) +
                             bias.y;

                        /* fetch tex color */
                        color = top_line_data[(ty & top_line_mask) << top_line_shift | (tx & top_line_mask)];
                    }

                    if(shade & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += shade & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += shade & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
#ifdef EDITOR
                    if(h_point != NULL && fabsf(h_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(h_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(s_point != NULL && fabsf(s_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(s_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }

                    if(line == h_line) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(line == s_line) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }
#endif
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                break;
            }

            /* get sector on other side of the line while keeping note of the last sector so raycasting won't traverse back through it */
            last_s = sector;
            sector = &(*s)[line->sector];

            ceilingdiff = sector->height[CEILING] - v.height;
            next_y = h_2 - (ceilingdiff / total_distance * max_offset * h_2);
            if(next_y > top) {
                /* draw top wall */
                /* wx and wy should already be set */
                for(y = top;
                    y <= next_y && y < height;
                    y++) {
                    if(top_line_dim == 0) {
                        color = 0x00;
                    } else {
                        if(shade & SHADE_PARALLAX) {
                            wy = (float)y / height * top_line_dim;
                        } else {
                            wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                        }

                        tx = (wx * transform32->xx) +
                             (wy * transform32->xy) +
                             (wz * transform32->xz) +
                             bias.x;
                        ty = (wx * transform32->yx) +
                             (wy * transform32->yy) +
                             (wz * transform32->yz) +
                             bias.y;
                        color = top_line_data[(ty & top_line_mask) << top_line_shift | (tx & top_line_mask)];
                    }

                    if(shade & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += shade & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += shade & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
#ifdef EDITOR
                    if(h_point != NULL && fabsf(h_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(h_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(s_point != NULL && fabsf(s_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(s_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }

                    if(line == h_line) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(line == s_line) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }
#endif
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                top = y;
            }

            /* draw bottom wall */
            floordiff = sector->height[FLOOR] - v.height;
            next_y = h_2 - (floordiff / total_distance * max_offset * h_2);
            if(next_y < bottom) {
                /* bottom wall texture will be needed */
                if((*w)[line->wall[FLOOR]].texture != bottom_line_tex) {
                    bottom_line_tex = (*w)[line->wall[FLOOR]].texture;
                    set_tex(bottom_line_tex, &bottom_line_data, &bottom_line_mask, &bottom_line_shift, &bottom_line_dim);
                }
                bias.x = (*p)[(*w)[line->wall[CEILING]].bias].x;
                bias.y = (*p)[(*w)[line->wall[CEILING]].bias].y;
                transform32 = &(*m32)[(*w)[line->wall[FLOOR]].transform];
                shade = (*w)[line->wall[FLOOR]].shade;

                /* shade can be different, so recalculate */
                if(shade & SHADE_PARALLAX) {
                    wx = (float)x / width * x_ratio * top_line_dim;
                    wz = 0.0;
                    bias.x += bg_pan * bottom_line_dim;
                } else {
                    wx = v.pos.x + (slope.x * total_distance);
                    wy = v.pos.y + (slope.y * total_distance);
                }

                for(y = bottom;
                    y >= next_y && y >= 0;
                    y--) {
                    if(bottom_line_dim == 0) {
                        color = 0x00;
                    } else {
                        if(shade & SHADE_PARALLAX) {
                            wy = (float)y / height * top_line_dim;
                        } else {
                            wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                        }

                        tx = (wx * transform32->xx) +
                             (wy * transform32->xy) +
                             (wz * transform32->xz) +
                             bias.x;
                        ty = (wx * transform32->yx) +
                             (wy * transform32->yy) +
                             (wz * transform32->yz) +
                             bias.y;
                        color = bottom_line_data[(ty & bottom_line_mask) << bottom_line_shift | (tx & bottom_line_mask)];
                    }

                    if(shade & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += shade & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += shade & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
#ifdef EDITOR
                    if(h_point != NULL && fabsf(h_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(h_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(s_point != NULL && fabsf(s_point->x - wx) < HIGHLIGHT_LINE_THICKNESS && fabsf(s_point->y - wy) < HIGHLIGHT_LINE_THICKNESS) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }

                    if(line == h_line) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == HIGHLIGHT_PHASE) {
                            color = ~color;
                        }
                    }

                    if(line == s_line) {
                        if((x ^ y) % HIGHLIGHT_DENSITY == SELECTION_PHASE) {
                            color = ~color;
                        }
                    }
#endif
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                bottom = y;
            }

            /* column fully drawn */
            if(top >= bottom) {
                break;
            }

            pos.x = hit.x;
            pos.y = hit.y;
        }
    }
}

int do_action(Sector *sector) {
    switch(sector->action & 0xFF) {
        case ACTION_WARP:
            unsigned char map = (sector->action & 0xFF00) >> 8;
            unsigned char view = (sector->action & 0xFF0000) >> 16;
            if(map == mapnum) {
                /* views aren't stored, so load the view from file */

                Header h;
                View_data vd;
                off_t start = 0;

                /* don't bother chacking for error since this map had to have loaded before already.. */
                open_map_p(mapnum);

                read_map_p(0, sizeof(Header), &h);

                if(view >= h.views) {
                    return(0);
                }

                start = sizeof(Header) +
                        (h.points * sizeof(Point_data)) +
                        (h.matrix2x2s * sizeof(Matrix2x2_data)) +
                        (h.matrix3x2s * sizeof(Matrix3x2_data)) +
                        (h.lines * sizeof(Line_data)) +
                        (h.sectors * sizeof(Sector_data)) +
                        (h.links * sizeof(Link_data));

                read_map_p(start + (view * sizeof(View_data)), sizeof(View_data), &vd);
                apply_view_data(&vd);

                close_map_p();
            } else {
                engine_load(map, view);
            }
            return(1);
        case ACTION_WAYPOINT:
            unsigned short firstpoint = (sector->action & 0xFFFF00) >> 8;
            unsigned short count = (sector->action & 0xFF000000) >> 24;
            /* count is mapped to start at 1, avoids division by 0 */
            count++;
            /* pick a waypoint by random */
            unsigned short selection = firstpoint + (rand() % count);
            waypoint.x = (*p)[selection].x;
            waypoint.y = (*p)[selection].y;
            break;
    }

    return(0);
}

void engine_move(float x, float y,
                 int action) {
    Point slope;
    Point dest;
    int vvertical;
    float vslope;
    Point pos;
    Point hit;
    int lindex;
    Line *line;
    int sindex;
    Sector *sector;
    Sector *last_s;

    /* calculate slope values, this doesn't need a planar projection, so just divide rise/run */
    if(fabs(y) > fabs(x)) {
        /* ray about Y axis */
        vvertical = 1;
        vslope = x / y;
    } else {
        /* ray about X axis */
        vvertical = 0;
        vslope = y / x;
    }
    slope.x = x;
    slope.y = y;
 
    /* get destination */
    dest.x = v.pos.x + x;
    dest.y = v.pos.y + y;

    pos.x = v.pos.x;
    pos.y = v.pos.y;
    last_s = NULL;
    sindex = v.start;
    sector = &(*s)[sindex];
    while(true) {
        lindex = scan_sector(last_s, sector, &pos,
                             &slope, vvertical, vslope,
                             &hit);

        /* shouldn't happen, but in case the ray misses for some reason */
        if(lindex < 0) {
            return;
        }
        line = &(*l)[lindex];

        /* check if the hit is further than traveled, if it is, a new sector won't be reached, so stop,
         * otherwise, continue to iterate */
        if(fabs(slope.x) > fabs(slope.y)) {
            if(slope.x > 0.0 && hit.x >= dest.x) {
                break;
            } else if(slope.x < 0.0 && hit.x <= dest.x) {
                break;
            }
        } else {
            if(slope.y > 0.0 && hit.y >= dest.y) {
                break;
            } else if(slope.y < 0.0 && hit.y <= dest.y) {
                break;
            }
        }

        /* if wall collided, don't continue to move
         * this isn't intended to be very interactive, so for now, don't bother with real collision
         * detection, just make sure the view can't leave where there're no sectors */
        if(line->sector == NO_SECTOR) {
            return;
        }

        /* update working position and sector */
        pos.x = hit.x;
        pos.y = hit.y;
        last_s = sector;
        sindex = line->sector;
        sector = &(*s)[sindex];

        if(action) {
            if(do_action(sector)) {
                /* do_action indicated that it already updated the view state */
                return;
            }
        }
    }

    /* finally, update player position and sector */
    v.pos.x = dest.x;
    v.pos.y = dest.y;
    v.start = sindex;
    v.height = sector->height[FLOOR] + v.startheight;
}

#ifdef EDITOR
int get_prev_lindex(Sector *sector, int lindex) {
    int prev_lindex;

    prev_lindex = lindex - sector->firstline - 1;
    if(prev_lindex < 0) {
        prev_lindex += sector->lines;
    }

    return(prev_lindex + sector->firstline);
}

int get_next_lindex(Sector *sector, int lindex) {
    return(sector->firstline + ((lindex - sector->firstline + 1) % sector->lines));
}

int get_hit(int mx, int my,
            int width, int height,
            int *sindex, int *lindex,
            float *wx, float *wy, float *wz) {
    Sector *sector, *last_s;
    Line *line;
    float offset;
    Point slope;
    int vvertical;
    float vslope;
    Point hit;
    float total_distance;
    int y;
    float z;
    int top, bottom;
    Point pos;
    float ceilingdiff;
    float floordiff;
    float next_y;

    float w_2 = (float)width / 2.0;
    float h_2 = (float)height / 2.0;
    float fov_2 = v.fov / 2.0;
    float sin_angle = sin(v.angle);
    float cos_angle = cos(v.angle);
    float max_offset = atan2f(fov_2, 1.0) * 2.0;
 
    offset = (float)(mx - w_2) / w_2 * max_offset;
    slope.x = sin_angle + (cos_angle * offset);
    slope.y = cos_angle - (sin_angle * offset);

    if(fabs(slope.y) > fabs(slope.x)) {
        /* view ray about Y axis */
        vvertical = 1;
        vslope = slope.x / slope.y;
    } else {
        /* view ray about X axis */
        vvertical = 0;
        vslope = slope.y / slope.x;
    }

    line = NULL;
    *sindex = v.start;
    sector = &(*s)[*sindex];
    last_s = NULL;
    top = 0;
    bottom = height - 1;
    pos.x = v.pos.x;
    pos.y = v.pos.y;
    total_distance = 0.0;
    while (true) {
        *lindex = scan_sector(last_s, sector, &pos,
                              &slope, vvertical, vslope,
                              &hit);

        /* shouldn't happen, but in case the ray misses for some reason */
        if(*lindex < 0) {
            break;
        }
        line = &(*l)[*lindex];

        /* get the distance between the view and hit coordinates */
        total_distance += get_distance(&pos, &hit, &slope);

        /* iterate y (angle transformed with FOV to get an angle from screen Y) with height
         * from bottom solving for distance until total distance is reached.
         * Texture X and Y lookup from offset from view given ray angle */

        /* draw ceiling */
        ceilingdiff = sector->height[CEILING] - v.height;
        if(ceilingdiff > 0.0) {
            /* ceiling is above the view line */

            for(y = top; y < h_2 && y < bottom; y++) {
                z = ceilingdiff / ((h_2 - y) / h_2) * max_offset;

                if(z > total_distance) {
                    break;
                }
                *wx = v.pos.x + (slope.x * z);
                *wy = v.pos.y + (slope.y * z);
                if(y == my) {
                    *wz = sector->height[CEILING];
                    return(0);
                }
            }
            top = y;
        }

        /* draw floor */
        floordiff = sector->height[FLOOR] - v.height;
        if(floordiff < 0.0) {
            for(y = bottom; y >= 0 && y >= top; y--) {
                z = floordiff / ((h_2 - y) / h_2) * max_offset;

                if(z >= total_distance) {
                    break;
                }
                *wx = v.pos.x + (slope.x * z);
                *wy = v.pos.y + (slope.y * z);
                if(y == my) {
                    *wz = sector->height[FLOOR];
                    return(0);
                }
            }
            bottom = y;
        }

        *wx = v.pos.x + (slope.x * total_distance);
        *wy = v.pos.y + (slope.y * total_distance);

        if(line->sector == NO_SECTOR) {
            /* solid wall, no sector on the other side */
            for(y = top;
                y <= bottom && y < height;
                y++) {
                *wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                if(y == my) {
                    return(0);
                }
            }
            break;
        }

        /* get sector on other side of the line while keeping note of the last sector so raycasting won't traverse back through it */
        last_s = sector;
        sector = &(*s)[line->sector];

        ceilingdiff = sector->height[CEILING] - v.height;
        next_y = h_2 - (ceilingdiff / total_distance * max_offset * h_2);
        if(next_y > top) {
            /* draw top wall */
            /* wx and wy should already be set */
            for(y = top;
                y <= next_y && y < height;
                y++) {
                *wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                if(y == my) {
                    return(0);
                }
            }
            top = y;
        }

        /* draw bottom wall */
        floordiff = sector->height[FLOOR] - v.height;
        next_y = h_2 - (floordiff / total_distance * max_offset * h_2);
        if(next_y < bottom) {
            /* bottom wall texture will be needed */
            *wx = v.pos.x + (slope.x * total_distance);
            *wy = v.pos.y + (slope.y * total_distance);

            for(y = bottom;
                y >= next_y && y >= 0;
                y--) {
                *wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                if(y == my) {
                    return(0);
                }
            }
            bottom = y;
        }

        /* column fully drawn */
        if(top >= bottom) {
            break;
        }

        pos.x = hit.x;
        pos.y = hit.y;
    }

    return(-1);
}

int get_sector(unsigned int x, unsigned int y,
               unsigned int w, unsigned int h) {
    int sindex, lindex;
    float wx;
    float wy;
    float wz;

    if(get_hit(x, y, w, h, &sindex, &lindex, &wx, &wy, &wz) != 0) {
        return(-1);
    }

    /* sector will be the sector pointed by the cursor or the sector attached to the wall pointed at */
    return(sindex);
}

int get_line(unsigned int x, unsigned int y,
               unsigned int w, unsigned int h) {
    int sindex, lindex;
    float wx;
    float wy;
    float wz;

    if(get_hit(x, y, w, h, &sindex, &lindex, &wx, &wy, &wz) != 0) {
        return(-1);
    }

    /* line will be the wall pointed at or the wall at the far end of the sector on the column pointed at */
    return(lindex);
}

int get_point(unsigned int x, unsigned int y,
              unsigned int w, unsigned int h) {
    int sindex, lindex;
    Sector *sector;
    Line *line;
    unsigned short pindex1, pindex2;
    Point *point1 = NULL, *point2;
    float wx;
    float wy;
    float wz;
    int i;

    if(get_hit(x, y, w, h, &sindex, &lindex, &wx, &wy, &wz) != 0) {
        return(-1);
    }
    sector = &(*s)[sindex];
    line = &(*l)[lindex];

    /* find nearest point */
    /* find point and adjacent point on line in sector */
    for(i = 0; i < sector->lines; i++) {
        pindex1 = (*l)[sector->firstline + i].point;
        if(line == &(*l)[sector->firstline + i]) {
            pindex2 = (*l)[get_next_lindex(sector, sector->firstline + i)].point;
            point1 = &(*p)[pindex1];
            point2 = &(*p)[pindex2];
            break;
        }
    }

    if(point1 == NULL) {
        /* no reason this should happen but prevent a potential crash due to bug */
        return(-1);
    }

    /* compare distances */
    if(sqrt(pow(wx - point1->x, 2.0) + pow(wy - point1->y, 2.0)) <
       sqrt(pow(wx - point2->x, 2.0) + pow(wy - point2->y, 2.0))) {
        /* first point is closer */
        return(pindex1);
    }

    /* second point is closer */
    return(pindex2);
}

/* Map structure modification functions */

int add_point(float x, float y) {
    Point (*new_p)[];
    int *new_point_refs;
    unsigned int i;

    if(h.points == 0) {
        /* allocate initial arrays */
        p = malloc(sizeof(Point));
        if(p == NULL) {
            return(-1);
        }
        point_refs = malloc(sizeof(int));
        if(point_refs == NULL) {
            return(-1);
        }
        (*p)[0].x = x;
        (*p)[0].y = y;
        h.points = 1;
        point_refs[0] = 1;
        return(0);
    }

    /* try to find the point */
    for(i = 0; i < h.points; i++) {
        if((*p)[i].x == x && (*p)[i].y == y) {
            point_refs[i]++;
            return(i);
        }
    }

    /* resize array */
    new_p = realloc(p, (h.points + 1) * sizeof(Point));
    if(new_p == NULL) {
        return(-1);
    }
    p = new_p;
    new_point_refs = realloc(point_refs, (h.points + 1) * sizeof(int));
    if(new_point_refs == NULL) {
        return(-1);
    }
    point_refs = new_point_refs;

    (*p)[h.points].x = x;
    (*p)[h.points].y = y;
    point_refs[h.points] = 1;
    h.points++;
    return(h.points-1);
}

int deref_point(int pindex) {
    Point (*new_p)[];
    int *new_point_refs;
    int i;

    point_refs[pindex]--;
    if(point_refs[pindex] == 0) {
        if(pindex < h.points - 1) {
            /* if this isn't the last entry, shift everything back and repoint everything */
            memmove(&(*p)[pindex], &(*p)[pindex+1], sizeof(Point) * (h.points - (pindex + 1)));
            memmove(&point_refs[pindex], &point_refs[pindex+1], sizeof(int) * (h.points - (pindex + 1)));
            /* wall, line, flat */
            for(i = 0; i < h.walls; i++) {
                if((*w)[i].bias > pindex) {
                    (*w)[i].bias--;
                }
            }

            for(i = 0; i < h.lines; i++) {
                if((*l)[i].point > pindex) {
                    (*l)[i].point--;
                }
            }

            for(i = 0; i < h.flats; i++) {
                if((*f)[i].bias > pindex) {
                    (*f)[i].bias--;
                }
            }
        }
        /* resize the arrays */
        new_p = realloc(p, (h.points - 1) * sizeof(Point));
        if(new_p == NULL) {
            return(-1);
        }
        p = new_p;
        new_point_refs = realloc(point_refs, (h.points - 1) * sizeof(int));
        if(new_point_refs == NULL) {
            return(-1);
        }
        point_refs = new_point_refs;
        h.points--;
    }

    return(0);
}

int modify_point(int pindex,
                 float x, float y) {
    Point *point = &(*p)[pindex];
    int new_pindex;

    /* if this is the only reference, just modify it */
    if(point_refs[pindex] == 1) {
        point->x = x;
        point->y = y;
        return(pindex);
    }

    /* if this won't change anything, just return it */
    if(point->x == x && point->y == y) {
        return(pindex);
    }

    /* otherwise, try adding a new point with the new value,
     * if it's found, it'll have its refcount increased */
    new_pindex = add_point(x, y);
    /* dereference the old point */
    deref_point(pindex);

    return(new_pindex);
}

int add_matrix2x2(float xx, float xy,
                  float yx, float yy) {
    Matrix2x2 (*new_m22)[];
    int *new_m22_refs;
    unsigned int i;

    if(h.matrix2x2s == 0) {
        /* allocate initial arrays */
        m22 = malloc(sizeof(Matrix2x2));
        if(m22 == NULL) {
            return(-1);
        }
        m22_refs = malloc(sizeof(int));
        if(m22_refs == NULL) {
            return(-1);
        }
        (*m22)[0].xx = xx;
        (*m22)[0].xy = xy;
        (*m22)[0].yx = yx;
        (*m22)[0].yy = yy;
        h.matrix2x2s = 1;
        m22_refs[0] = 1;
        return(0);
    }

    /* try to find the point */
    for(i = 0; i < h.matrix2x2s; i++) {
        if((*m22)[i].xx == xx && (*m22)[i].xy == xy &&
           (*m22)[i].yx == yx && (*m22)[i].yy == yy) {
            m22_refs[i]++;
            return(i);
        }
    }

    /* resize array */
    new_m22 = realloc(m22, (h.matrix2x2s + 1) * sizeof(Matrix2x2));
    if(new_m22 == NULL) {
        return(-1);
    }
    m22 = new_m22;
    new_m22_refs = realloc(m22_refs, (h.matrix2x2s + 1) * sizeof(int));
    if(new_m22_refs == NULL) {
        return(-1);
    }
    m22_refs = new_m22_refs;

    (*m22)[h.matrix2x2s].xx = xx;
    (*m22)[h.matrix2x2s].xy = xy;
    (*m22)[h.matrix2x2s].yx = yx;
    (*m22)[h.matrix2x2s].yy = yy;
    m22_refs[h.matrix2x2s] = 1;
    h.matrix2x2s++;
    return(h.matrix2x2s-1);
}

int deref_matrix2x2(int m22index) {
    Matrix2x2 (*new_m22)[];
    int *new_m22_refs;
    int i;

    m22_refs[m22index]--;
    if(m22_refs[m22index] == 0) {
        if(m22index < h.matrix2x2s - 1) {
            /* if this isn't the last entry, shift everything back and repoint everything */
            memmove(&(*m22)[m22index], &(*m22)[m22index+1], sizeof(Matrix2x2) * (h.matrix2x2s - (m22index + 1)));
            memmove(&m22_refs[m22index], &m22_refs[m22index+1], sizeof(int) * (h.matrix2x2s - (m22index + 1)));
            /* flat */
            for(i = 0; i < h.flats; i++) {
                if((*f)[i].transform > m22index) {
                    (*f)[i].transform--;
                }
            }
        }
        /* resize the arrays */
        new_m22 = realloc(m22, (h.matrix2x2s - 1) * sizeof(Matrix2x2));
        if(new_m22 == NULL) {
            return(-1);
        }
        m22 = new_m22;
        new_m22_refs = realloc(m22_refs, (h.matrix2x2s - 1) * sizeof(int));
        if(new_m22_refs == NULL) {
            return(-1);
        }
        m22_refs = new_m22_refs;
        h.matrix2x2s--;
    }

    return(0);
}

int modify_matrix2x2(int m22index,
                     float xx, float xy,
                     float yx, float yy) {
    Matrix2x2 *matrix2x2 = &(*m22)[m22index];
    int new_m22index;

    /* if this is the only reference, just modify it */
    if(m22_refs[m22index] == 1) {
        matrix2x2->xx = xx;
        matrix2x2->xy = xy;
        matrix2x2->yx = yx;
        matrix2x2->yy = yy;
        return(m22index);
    }

    /* if this won't change anything, just return it */
    if(matrix2x2->xx == xx && matrix2x2->xy == xy &&
       matrix2x2->yx == yx && matrix2x2->yy == yy) {
        return(m22index);
    }

    /* otherwise, try adding a new point with the new value,
     * if it's found, it'll have its refcount increased */
    new_m22index = add_matrix2x2(xx, xy,
                                 yx, yy);
    /* dereference the old matrix2x2 */
    deref_matrix2x2(m22index);

    return(new_m22index);
}

int add_matrix3x2(float xx, float xy, float xz,
                  float yx, float yy, float yz) {
    Matrix3x2 (*new_m32)[];
    int *new_m32_refs;
    unsigned int i;

    if(h.matrix3x2s == 0) {
        /* allocate initial arrays */
        m32 = malloc(sizeof(Matrix3x2));
        if(m32 == NULL) {
            return(-1);
        }
        m32_refs = malloc(sizeof(int));
        if(m32_refs == NULL) {
            return(-1);
        }
        (*m32)[0].xx = xx;
        (*m32)[0].xy = xy;
        (*m32)[0].xz = xz;
        (*m32)[0].yx = yx;
        (*m32)[0].yy = yy;
        (*m32)[0].yz = yz;
        h.matrix3x2s = 1;
        m32_refs[0] = 1;
        return(0);
    }

    /* try to find the point */
    for(i = 0; i < h.matrix3x2s; i++) {
        if((*m32)[i].xx == xx && (*m32)[i].xy == xy && (*m32)[i].xz == xz &&
           (*m32)[i].yx == yx && (*m32)[i].yy == yy && (*m32)[i].yz == yz) {
            m32_refs[i]++;
            return(i);
        }
    }

    /* resize array */
    new_m32 = realloc(m32, (h.matrix3x2s + 1) * sizeof(Matrix3x2));
    if(new_m32 == NULL) {
        return(-1);
    }
    m32 = new_m32;
    new_m32_refs = realloc(m32_refs, (h.matrix3x2s + 1) * sizeof(int));
    if(new_m32_refs == NULL) {
        return(-1);
    }
    m32_refs = new_m32_refs;

    (*m32)[h.matrix3x2s].xx = xx;
    (*m32)[h.matrix3x2s].xy = xy;
    (*m32)[h.matrix3x2s].xz = xz;
    (*m32)[h.matrix3x2s].yx = yx;
    (*m32)[h.matrix3x2s].yy = yy;
    (*m32)[h.matrix3x2s].yz = yz;
    m32_refs[h.matrix3x2s] = 1;
    h.matrix3x2s++;
    return(h.matrix3x2s-1);
}

int deref_matrix3x2(int m32index) {
    Matrix3x2 (*new_m32)[];
    int *new_m32_refs;
    int i;

    m32_refs[m32index]--;
    if(m32_refs[m32index] == 0) {
        if(m32index < h.matrix3x2s - 1) {
            /* if this isn't the last entry, shift everything back and repoint everything */
            memmove(&(*m32)[m32index], &(*m32)[m32index+1], sizeof(Matrix3x2) * (h.matrix3x2s - (m32index + 1)));
            memmove(&m32_refs[m32index], &m32_refs[m32index+1], sizeof(int) * (h.matrix3x2s - (m32index + 1)));
            /* wall */
            for(i = 0; i < h.walls; i++) {
                if((*w)[i].transform > m32index) {
                    (*w)[i].transform--;
                }
            }
        }
        /* resize the arrays */
        new_m32 = realloc(m32, (h.matrix3x2s - 1) * sizeof(Matrix3x2));
        if(new_m32 == NULL) {
            return(-1);
        }
        m32 = new_m32;
        new_m32_refs = realloc(m32_refs, (h.matrix3x2s - 1) * sizeof(int));
        if(new_m32_refs == NULL) {
            return(-1);
        }
        m32_refs = new_m32_refs;
        h.matrix3x2s--;
    }

    return(0);
}

int modify_matrix3x2(int m32index,
                     float xx, float xy, float xz,
                     float yx, float yy, float yz) {
    Matrix3x2 *matrix3x2 = &(*m32)[m32index];
    int new_m32index;

    /* if this is the only reference, just modify it */
    if(m32_refs[m32index] == 1) {
        matrix3x2->xx = xx;
        matrix3x2->xy = xy;
        matrix3x2->xz = xz;
        matrix3x2->yx = yx;
        matrix3x2->yy = yy;
        matrix3x2->yz = yz;
        return(m32index);
    }

    /* if this won't change anything, just return it */
    if(matrix3x2->xx == xx && matrix3x2->xy == xy && matrix3x2->xy == xz &&
       matrix3x2->yx == yx && matrix3x2->yy == yy && matrix3x2->yy == yz) {
        return(m32index);
    }

    /* otherwise, try adding a new point with the new value,
     * if it's found, it'll have its refcount increased */
    new_m32index = add_matrix3x2(xx, xy, xz,
                                 yx, yy, yz);
    /* dereference the old matrix3x2 */
    deref_matrix3x2(m32index);

    return(new_m32index);
}

int add_wall(unsigned char texture,
             unsigned short shade,
             unsigned short bias,
             unsigned short transform) {
    Wall (*new_w)[];
    int *new_wall_refs;
    unsigned int i;

    if(h.walls == 0) {
        /* allocate initial arrays */
        w = malloc(sizeof(Wall));
        if(w == NULL) {
            return(-1);
        }
        wall_refs = malloc(sizeof(int));
        if(wall_refs == NULL) {
            return(-1);
        }
        (*w)[0].texture = texture;
        (*w)[0].shade = shade;
        (*w)[0].bias = bias;
        (*w)[0].transform = transform;
        h.walls = 1;
        wall_refs[0] = 1;
        return(0);
    }

    /* try to find the point */
    for(i = 0; i < h.walls; i++) {
        if((*w)[i].texture == texture &&
           (*w)[i].shade == shade &&
           (*w)[i].bias == bias &&
           (*w)[i].transform == transform) {
            wall_refs[i]++;
            return(i);
        }
    }

    /* resize array */
    new_w = realloc(w, (h.walls + 1) * sizeof(Wall));
    if(new_w == NULL) {
        return(-1);
    }
    w = new_w;
    new_wall_refs = realloc(wall_refs, (h.walls + 1) * sizeof(int));
    if(new_wall_refs == NULL) {
        return(-1);
    }
    wall_refs = new_wall_refs;
    (*w)[i].texture = texture;
    (*w)[i].shade = shade;
    (*w)[i].bias = bias;
    (*w)[i].transform = transform;
    wall_refs[h.walls] = 1;
    h.walls++;
    return(h.walls-1);
}

int deref_wall(int windex) {
    Wall (*new_w)[];
    int *new_wall_refs;
    int i;

    wall_refs[windex]--;
    if(wall_refs[windex] == 0) {
        if(windex < h.walls - 1) {
            /* if this isn't the last entry, shift everything back and repoint everything */
            memmove(&(*w)[windex], &(*w)[windex+1], sizeof(Wall) * (h.walls - (windex + 1)));
            memmove(&wall_refs[windex], &wall_refs[windex+1], sizeof(int) * (h.walls - (windex + 1)));
            /* line */
            for(i = 0; i < h.lines; i++) {
                if((*l)[i].wall[CEILING] > windex) {
                    (*l)[i].wall[CEILING]--;
                }
                if((*l)[i].wall[FLOOR] > windex) {
                    (*l)[i].wall[FLOOR]--;
                }
            }
        }
        /* resize the arrays */
        new_w = realloc(w, (h.walls - 1) * sizeof(Wall));
        if(new_w == NULL) {
            return(-1);
        }
        w = new_w;
        new_wall_refs = realloc(wall_refs, (h.walls - 1) * sizeof(int));
        if(new_wall_refs == NULL) {
            return(-1);
        }
        wall_refs = new_wall_refs;
        h.walls--;
    }

    return(0);
}

int modify_wall(int windex,
                unsigned char texture,
                unsigned short shade,
                unsigned short bias,
                unsigned short transform) {
    Wall *wall = &(*w)[windex];
    int new_windex;

    /* if this is the only reference, just modify it */
    if(wall_refs[windex] == 1) {
        wall->texture = texture;
        wall->shade = shade;
        wall->bias = bias;
        wall->transform = transform;
        return(windex);
    }

    /* if this won't change anything, just return it */
    if(wall->texture == texture &&
       wall->shade == shade &&
       wall->bias == bias &&
       wall->transform == transform) {
        return(windex);
    }

    /* otherwise, try adding a new point with the new value,
     * if it's found, it'll have its refcount increased */
    new_windex = add_wall(texture, shade, bias, transform);
    /* dereference the old wall */
    deref_wall(windex);

    return(new_windex);
}

int add_flat(unsigned char texture,
             unsigned short shade,
             unsigned short bias,
             unsigned short transform) {
    Flat (*new_f)[];
    int *new_flat_refs;
    unsigned int i;

    if(h.flats == 0) {
        /* allocate initial arrays */
        f = malloc(sizeof(Flat));
        if(f == NULL) {
            return(-1);
        }
        flat_refs = malloc(sizeof(int));
        if(flat_refs == NULL) {
            return(-1);
        }
        (*f)[0].texture = texture;
        (*f)[0].shade = shade;
        (*f)[0].bias = bias;
        (*f)[0].transform = transform;
        h.flats = 1;
        flat_refs[0] = 1;
        return(0);
    }

    /* try to find the point */
    for(i = 0; i < h.flats; i++) {
        if((*f)[i].texture == texture &&
           (*f)[i].shade == shade &&
           (*f)[i].bias == bias &&
           (*f)[i].transform == transform) {
            flat_refs[i]++;
            return(i);
        }
    }

    /* resize array */
    new_f = realloc(f, (h.flats + 1) * sizeof(Flat));
    if(new_f == NULL) {
        return(-1);
    }
    f = new_f;
    new_flat_refs = realloc(flat_refs, (h.flats + 1) * sizeof(int));
    if(new_flat_refs == NULL) {
        return(-1);
    }
    flat_refs = new_flat_refs;
    (*f)[i].texture = texture;
    (*f)[i].shade = shade;
    (*f)[i].bias = bias;
    (*f)[i].transform = transform;
    flat_refs[h.flats] = 1;
    h.flats++;
    return(h.flats-1);
}

int deref_flat(int findex) {
    Flat (*new_f)[];
    int *new_flat_refs;
    int i;

    flat_refs[findex]--;
    if(flat_refs[findex] == 0) {
        if(findex < h.flats - 1) {
            /* if this isn't the last entry, shift everything back and repoint everything */
            memmove(&(*f)[findex], &(*f)[findex+1], sizeof(Flat) * (h.flats - (findex + 1)));
            memmove(&flat_refs[findex], &flat_refs[findex+1], sizeof(int) * (h.flats - (findex + 1)));
            /* sector */
            for(i = 0; i < h.sectors; i++) {
                if((*s)[i].flat[CEILING] > findex) {
                    (*s)[i].flat[CEILING]--;
                }
                if((*s)[i].flat[FLOOR] > findex) {
                    (*s)[i].flat[FLOOR]--;
                }
            }
        }
        /* resize the arrays */
        new_f = realloc(f, (h.flats - 1) * sizeof(Flat));
        if(new_f == NULL) {
            return(-1);
        }
        f = new_f;
        new_flat_refs = realloc(flat_refs, (h.flats - 1) * sizeof(int));
        if(new_flat_refs == NULL) {
            return(-1);
        }
        flat_refs = new_flat_refs;
        h.flats--;
    }

    return(0);
}

int modify_flat(int findex,
                unsigned char texture,
                unsigned short shade,
                unsigned short bias,
                unsigned short transform) {
    Flat *flat = &(*f)[findex];
    int new_findex;

    /* if this is the only reference, just modify it */
    if(flat_refs[findex] == 1) {
        flat->texture = texture;
        flat->shade = shade;
        flat->bias = bias;
        flat->transform = transform;
        return(findex);
    }

    /* if this won't change anything, just return it */
    if(flat->texture == texture &&
       flat->shade == shade &&
       flat->bias == bias &&
       flat->transform == transform) {
        return(findex);
    }

    /* otherwise, try adding a new point with the new value,
     * if it's found, it'll have its refcount increased */
    new_findex = add_flat(texture, shade, bias, transform);
    /* dereference the old flat */
    deref_flat(findex);

    return(new_findex);
}

int add_line(int point, int sector,
             int wall_ceiling, int wall_floor) {
    Line (*new_l)[];
    Line *line;

    /* resize array */
    new_l = realloc(l, (h.lines + 1) * sizeof(Line));
    if(new_l == NULL) {
        return(-1);
    }
    l = new_l;
    h.lines++;

    line = &(*l)[h.lines - 1];
    line->point = point;
    line->sector = sector;
    line->wall[CEILING] = wall_ceiling;
    wall_refs[wall_ceiling]++;
    line->wall[FLOOR] = wall_floor;
    wall_refs[wall_floor]++;

    return(h.lines - 1);
}

int expand_lines(int index, int amount) {
    Line (*new_l)[];
    int i;

    /* resize array */
    new_l = realloc(l, (h.lines + amount) * sizeof(Line));
    if(new_l == NULL) {
        return(-1);
    }
    l = new_l;

    if(index == h.lines) {
        /* make the space */
        memmove(&(*l)[index + amount], &(*l)[index], sizeof(Line) * (h.lines - index));
        /* repoint sectors with firstline after this index */
        for(i = 0; i < h.sectors; i++) {
            if((*s)[i].firstline > index) {
                (*s)[i].firstline++;
            }
        }
    }

    h.lines += amount;

    return(0);
}

int insert_line(int index,
                float x, float y,
                unsigned short sector,
                unsigned short wall_ceiling,
                unsigned short wall_floor) {
    Line *line;
    int point;

    if(expand_lines(index, 1) < 0) {
        return(-1);
    }

    line = &(*l)[index];

    point = add_point(x, y);
    if(point < 0) {
        return(-1);
    }
    line->point = point;
    line->sector = sector;
    wall_refs[wall_ceiling]++;
    line->wall[CEILING] = wall_ceiling;
    wall_refs[wall_floor]++;
    line->wall[FLOOR] = wall_floor;

    return(0);
}

int delete_line(int index) {
    Line *line = &(*l)[index];
    Line (*new_l)[];
    int i;

    if(deref_point(line->point) < 0) {
        return(-1);
    }
    if(deref_wall(line->wall[CEILING]) < 0) {
        return(-1);
    }
    if(deref_wall(line->wall[FLOOR]) < 0) {
        return(-1);
    }

    if(index < h.lines - 1) {
        /* move the rest inward */
        memmove(&(*l)[index], &(*l)[index + 1], sizeof(Line) * (h.lines - (index + 1)));
        /* repoint sectors with firstline after this index */
        for(i = 0; i < h.sectors; i++) {
            if((*s)[i].firstline > index) {
                (*s)[i].firstline--;
            }
        }
    }

    /* resize array */
    new_l = realloc(l, (h.lines - 1) * sizeof(Line));
    if(new_l == NULL) {
        return(-1);
    }
    l = new_l;

    h.lines--;

    return(0);
}

int split_line(int sindex, int lindex) {
    /* add a point to a line's midpoint */
    Sector *sector = &(*s)[sindex];
    Line *line = &(*l)[lindex];
    int next_lindex = get_next_lindex(sector, lindex);
    Line *next_line = &(*l)[next_lindex];
    float x, y;
    Sector *other_sector;
    int other_lindex, other_next_lindex;
    Line *other_line;

    /* calculate midpoint */
    x = ((*p)[line->point].x + (*p)[next_line->point].x) / 2.0;
    y = ((*p)[line->point].y + (*p)[next_line->point].y) / 2.0;

    /* create new line, inserted in to lines list, point it to whatever the original line pointed to, even if nothing */
    if(insert_line(lindex + 1, x, y, line->sector, line->wall[CEILING], line->wall[FLOOR]) < 0) {
        return(-1);
    }
    /* update next index and line since it'll have moved from the insertion */
    next_lindex++;
    next_line = &(*l)[next_lindex];

    if(line->sector != NO_SECTOR) {
        /* if there's a sector on the other side, also split the line in the other sector */
        other_sector = &(*s)[line->sector];
        /* find line in other sector */
        for(other_lindex = 0; other_lindex < other_sector->lines; other_lindex++) {
            other_next_lindex = get_next_lindex(other_sector, other_lindex);;
            /* points will be in opposite order */
            if((*l)[other_sector->firstline + other_lindex].point == next_line->point &&
               (*l)[other_sector->firstline + other_next_lindex].point == line->point) {
                break;
            }
        }
        /* get absolute line index and insert a new line, point it back to original sector */
        other_lindex += other_sector->firstline;
        other_line = &(*l)[other_lindex];
        if(insert_line(other_lindex + 1, x, y, sindex, other_line->wall[CEILING], other_line->wall[FLOOR]) < 0) {
            return(-1);
        }
    }

    return(0);
}

int find_opposite_line(int sindex, int lindex) {
    Sector *sector;
    Line *line = &(*l)[lindex];
    int point;
    int i;

    /* if no opposite sector, there won't be an opposite line */
    if(line->sector == NO_SECTOR) {
        return(-1);
    }

    /* get the point on the next line in this sector, which will be the point of the opposite line in the next */
    sector = &(*s)[sindex];
    point = (*l)[get_next_lindex(sector, lindex)].point;

    /* find the line with the point in the other sector */
    sector = &(*s)[line->sector];
    for(i = 0; i < sector->lines; i++) {
        if((*l)[sector->firstline + i].point == point) {
            return(i);
        }
    }

    return(-1);
}

void unlink_line(int sindex, int lindex) {
    int other_lindex;

    other_lindex = find_opposite_line(sindex, lindex);
    if(other_lindex >= 0) {
        (*l)[other_lindex].sector = NO_SECTOR;
    }

    (*l)[other_lindex].sector = NO_SECTOR;
}

int find_line(int sindex, int pindex) {
    Sector *sector = &(*s)[sindex];
    int i;

    for(i = 0; i < sector->lines; i++) {
        if((*l)[sector->firstline + i].point == pindex) {
            return(sector->firstline + i);
        }
    }

    return(-1);
}

int join_lines(int sindex, int pindex) {
    /* delete a point from a line and connecting the 2 points together, decreasing the point refcount and freeing it if needed */
    int lindex, prev_lindex;

    lindex = find_line(sindex, pindex);

    Sector *sector = &(*s)[sindex];

    /* find the other line this would be joined with */
    prev_lindex = get_prev_lindex(sector, lindex);

    /* unlink lines */
    unlink_line(sindex, lindex);
    unlink_line(sindex, prev_lindex);

    /* delete the line with this point */
    if(delete_line(lindex) < 0) {
        return(-1);
    }

    return(0);
}

int split_point(int sindex, int pindex,
                float x, float y) {
    /* create a new point for a line sharing a point with another line to freely move it away */
    int lindex, prev_lindex;
    int new_point;

    lindex = find_line(sindex, pindex);

    Sector *sector = &(*s)[sindex];

    /* find the other line this would be joined with */
    prev_lindex = get_prev_lindex(sector, lindex);

    /* unlink lines */
    unlink_line(sindex, lindex);
    unlink_line(sindex, prev_lindex);

    new_point = modify_point((*l)[lindex].point, x, y);
    if(new_point < 0) {
        return(-1);
    }
    (*l)[lindex].point = new_point;

    return(0);
}

int set_line_point(int lindex,
                   float x, float y) {
    int new_point = modify_point((*l)[lindex].point, x, y);
    if(new_point < 0) {
        return(-1);
    }
    (*l)[lindex].point = new_point;
    return(0);
}

int set_line_wall(int lindex, int which,
                  unsigned char texture,
                  unsigned short shade,
                  unsigned short bias,
                  unsigned short transform) {
    int new_wall = modify_wall((*l)[lindex].wall[which], texture, shade, bias, transform);
    if(new_wall < 0) {
        return(-1);
    }
    (*l)[lindex].wall[which] = new_wall;

    return(0);
}

int set_line_texture(int lindex, int which,
                     unsigned char texture) {
    Wall *wall = &(*w)[(*l)[lindex].wall[which]];
    if(set_line_wall(lindex, which,
                      texture, wall->shade, wall->bias, wall->transform)) {
        return(-1);
    }
    return(0);
}

int set_line_shade(int lindex, int which,
                   unsigned short shade) {
    Wall *wall = &(*w)[(*l)[lindex].wall[which]];
    if(set_line_wall(lindex, which,
                     wall->texture, shade, wall->bias, wall->transform)) {
        return(-1);
    }
    return(0);
}

int set_line_bias(int lindex, int which,
                  float x, float y) {
    Wall *wall = &(*w)[(*l)[lindex].wall[which]];
    int new_point = modify_point(wall->bias, x, y);
    if(new_point < 0) {
        return(-1);
    }
    if(set_line_wall(lindex, which,
                     wall->texture, wall->shade, new_point, wall->transform) < 0) {
        return(-1);
    }

    return(0);
}

int set_line_transform(int lindex, int which,
                       float xx, float xy, float xz,
                       float yx, float yy, float yz) {
    Wall *wall = &(*w)[(*l)[lindex].wall[which]];
    int new_m32 = modify_matrix3x2(wall->transform, xx, xy, xz,
                                                    yx, yy, yz);
    if(new_m32 < 0) {
        return(-1);
    }
    if(set_line_wall(lindex, which,
                     wall->texture, wall->shade, wall->bias, new_m32) < 0) {
        return(-1);
    }

    return(0);
}

void repoint_lines(int sindex, int lindex1, int lindex2, int new_sindex) {
    int other_lindex;
    int i;

    for(i = lindex1; i < lindex2; i++) {
        other_lindex = find_opposite_line(sindex, i);
        if(other_lindex >= 0) {
            (*l)[other_lindex].sector = new_sindex;
        }
    }
}

int delete_sector(int sindex) {
    Sector *sector = &(*s)[sindex];
    Sector (*new_s)[];
    int i;

    if(deref_flat(sector->flat[CEILING]) < 0) {
        return(-1);
    }
    if(deref_flat(sector->flat[FLOOR]) < 0) {
        return(-1);
    }

    if(sindex < h.sectors - 1) {
        /* move the rest inward */
        memmove(&(*s)[sindex], &(*s)[sindex + 1], sizeof(Sector) * (h.sectors - (sindex + 1)));
        /* repoint lines with sector after this index */
        for(i = 0; i < h.lines; i++) {
            if((*l)[i].sector > sindex) {
                (*l)[i].sector--;
            }
        }
    }

    /* resize array */
    new_s = realloc(s, (h.sectors - 1) * sizeof(Sector));
    if(new_s == NULL) {
        return(-1);
    }
    s = new_s;

    h.sectors--;

    return(0);
}

int add_sector(unsigned short firstline, unsigned char lines,
               float height_ceiling, float height_floor,
               unsigned short flat_ceiling, unsigned short flat_floor,
               unsigned int action) {
    Sector (*new_s)[];
    Sector *sector;

    new_s = realloc(s, (h.sectors + 1) * sizeof(Sector));
    if(new_s == NULL) {
        return(-1);
    }
    s = new_s;
    h.sectors++;

    sector = &(*s)[h.sectors - 1];
    sector->firstline = firstline;
    sector->lines = lines;
    sector->height[CEILING] = height_ceiling;
    sector->height[FLOOR] = height_floor;
    sector->flat[CEILING] = flat_ceiling;
    flat_refs[flat_ceiling]++;
    sector->flat[FLOOR] = flat_floor;
    flat_refs[flat_floor]++;
    sector->action = action;

    return(h.sectors - 1);
}

int split_sector(int sindex, int pindex1, int pindex2) {
    /* create a new sector pinched off from a sector between 2 points */
    Sector *sector = &(*s)[sindex];
    Sector *new_sector;
    int lindex1, lindex2;
    int next_lindex;
    int temp;
    Sector (*new_s)[];
    Line *line, *new_line;

    lindex1 = find_line(sindex, pindex1);
    lindex2 = find_line(sindex, pindex2);
    /* make sure 2 is after 1 */
    if(lindex2 < lindex1) {
        temp = lindex2;
        lindex2 = lindex1;
        lindex1 = temp;

        temp = pindex2;
        pindex2 = pindex1;
        pindex1 = temp;
    }
    /* if either sector would be left with less than 3 lines, don't do anything */
    if(lindex2 - lindex1 < 3 ||
       (lindex1 - sector->firstline) + (sector->firstline + sector->lines - lindex2) < 3) {
        return(-1);
    }

    /* rearrange the line array to make space for the 2 new lines for both sides for both sectors */

    /* make space, this only affects everything after, so indexes should be fine */
    if(expand_lines(sector->firstline + sector->lines, 2) < 0) {
        return(-1);
    }

    /* make room for the new sector */
    new_s = realloc(s, (h.sectors + 1) * sizeof(Sector));
    if(new_s == NULL) {
        return(-1);
    }
    s = new_s;
    h.sectors++;

    /* repoint opposite lines in what will be the new sector to what will be the new sector */
    repoint_lines(sindex, lindex1, lindex2, h.sectors - 1);

    /* rearrange the lines to be contiguous in each sector */
    /* this inefficiently copies some data just to copy it back but it's never going to be much data and it's clearer */
    Line lines[sector->lines];
    memcpy(lines, &(*l)[sector->firstline], sector->lines);

    /* the lines before and after the points will make up the original sector
     * and the lines in the middle will make up the new sector + 1 new line for each */
    /* copy first part before the slice */
    memcpy(&(*l)[sector->firstline], lines, lindex1 - sector->firstline);
    /* copy the end past the second slice in to the first sector, leaving space for the new line */
    memcpy(&(*l)[lindex1 + 1], &lines[lindex2 - sector->firstline], sector->firstline + sector->lines - lindex2);
    /* copy the rest to make up the new sector leaving space at the start for its new line */
    memcpy(&(*l)[lindex2 + 2], &lines[lindex1 - sector->firstline], lindex2 - lindex1);

    /* update line count */
    sector->lines = (lindex1 - sector->firstline) + (sector->firstline + sector->lines - lindex2) + 1;

    new_sector = &(*s)[h.sectors - 1];
    /* make the new sector from the old sector parameters */
    new_sector->firstline = sector->firstline + sector->lines; /* next line after original sector lines */
    new_sector->lines = lindex2 - lindex1 + 1; /* slice + new line */
    new_sector->height[CEILING] = sector->height[CEILING];
    new_sector->height[FLOOR] = sector->height[FLOOR];
    new_sector->flat[CEILING] = sector->flat[CEILING];
    flat_refs[new_sector->flat[CEILING]]++;
    new_sector->flat[FLOOR] = sector->flat[FLOOR];
    flat_refs[new_sector->flat[FLOOR]]++;

    /* make the new lines from a neighboring line's parameters */
    /* original sector's new line */
    new_line = &(*l)[lindex1]; /* new line is in the space between the 2 slices */
    next_lindex = get_next_lindex(sector, lindex1);
    line = &(*l)[next_lindex];
    new_line->point = pindex1; /* original line's point */
    new_line->sector = h.sectors - 1; /* the new sector */
    new_line->wall[CEILING] = line->wall[CEILING];
    wall_refs[new_line->wall[CEILING]]++;
    new_line->wall[FLOOR] = line->wall[FLOOR];
    wall_refs[new_line->wall[FLOOR]]++;

    /* new sector's new line */
    lindex2 = new_sector->firstline; /* new line is first index of new sector */
    new_line = &(*l)[lindex2];
    next_lindex = new_sector->firstline + 1; /* pick a neighboring line */
    line = &(*l)[next_lindex];
    new_line->point = pindex2; /* other side where the sector was pinched off/opposite line's other point */
    new_line->sector = sindex; /* the original sector */
    new_line->wall[CEILING] = line->wall[CEILING];
    wall_refs[new_line->wall[CEILING]]++;
    new_line->wall[FLOOR] = line->wall[FLOOR];
    wall_refs[new_line->wall[FLOOR]]++;

    return(0);
}

int link_sectors(int sindex, int lindex) {
    /* link 2 sectors sharing a common line with the same 2 points */
    Sector *sector = &(*s)[sindex];
    Sector *original_sector = sector; /* for copying parameters */
    Line *line = &(*l)[lindex];
    Line *other_line;
    int other_sindex = line->sector;
    int other_lindex;
    int temp;

    if(other_sindex == NO_SECTOR) {
        /* no other sector */
        return(-1);
    }
    other_lindex = find_opposite_line(sindex, lindex);
    /* make other_lindex after lindex to simplify memory management */
    if(other_lindex < lindex) {
        temp = other_lindex;
        other_lindex = lindex;
        lindex = temp;

        temp = other_sindex;
        other_sindex = sindex;
        sindex = temp;

        line = &(*l)[lindex];
        sector = &(*s)[sindex];
    }
    Sector *other_sector = &(*s)[other_sindex];
    other_line = &(*l)[other_lindex];

    /* insert the slice of 1 sector to another sector, except for the 2 lines between them and make that all 1 sector */

    /* deref walls from joining lines */
    deref_wall(line->wall[CEILING]);
    deref_wall(line->wall[FLOOR]);
    deref_wall(other_line->wall[CEILING]);
    deref_wall(other_line->wall[FLOOR]);

    /* copy other sector lines in to a buffer, excluding the joining line */
    Line lines[other_sector->lines - 1];
    memcpy(lines,
           &(*l)[other_sector->firstline],
           other_lindex - other_sector->firstline);
    memcpy(&lines[other_lindex - other_sector->firstline],
           &(*l)[other_lindex + 1],
           other_sector->firstline + other_sector->lines - other_lindex - 1);

    /* expand space after first sector split line for new lines */
    /* -2 because there's already 1 space to take up where the old line is */
    if(expand_lines(lindex, other_sector->lines - 2) < 0) {
        return(-1);
    }

    /* copy other sector lines in and update sector line count */
    memcpy(&(*l)[lindex], lines, other_sector->lines - 1);
    sector->lines += other_sector->lines - 1;

    /* point other sectors pointing to the other sector to the first sector */
    repoint_lines(sindex, lindex, lindex + other_sector->lines - 1, sindex);

    /* move parameters and delete other sector */
    sector->height[CEILING] = original_sector->height[CEILING];
    sector->height[FLOOR] = original_sector->height[FLOOR];
    sector->flat[CEILING] = original_sector->flat[CEILING];
    sector->flat[FLOOR] = original_sector->flat[FLOOR];
    sector->action = original_sector->action;
    if(delete_sector(other_sindex) < 0) {
        return(-1);
    }

    return(0);
}

int solidify_sector(int sindex) {
    Sector *sector = &(*s)[sindex];

    repoint_lines(sindex, sector->firstline, sector->firstline + sector->lines, NO_SECTOR);

    if(delete_sector(sindex) < 0) {
        return(-1);
    }

    return(0);
}

int set_sector_flat(int sindex, int which,
                    unsigned char texture,
                    unsigned short shade,
                    unsigned short bias,
                    unsigned short transform) {
    int new_flat = modify_flat((*s)[sindex].flat[which], texture, shade, bias, transform);
    if(new_flat < 0) {
        return(-1);
    }
    (*s)[sindex].flat[which] = new_flat;

    return(0);
}

int set_sector_texture(int sindex, int which,
                       unsigned char texture) {
    Flat *flat = &(*f)[(*s)[sindex].flat[which]];
    if(set_sector_flat(sindex, which,
                       texture, flat->shade, flat->bias, flat->transform)) {
        return(-1);
    }
    return(0);
}

int set_sector_shade(int sindex, int which,
                     unsigned short shade) {
    Flat *flat = &(*f)[(*s)[sindex].flat[which]];
    if(set_sector_flat(sindex, which,
                       flat->texture, shade, flat->bias, flat->transform)) {
        return(-1);
    }
    return(0);
}

int set_sector_bias(int sindex, int which,
                    float x, float y) {
    Flat *flat = &(*f)[(*s)[sindex].flat[which]];
    int new_point = modify_point(flat->bias, x, y);
    if(new_point < 0) {
        return(-1);
    }
    if(set_sector_flat(sindex, which,
                       flat->texture, flat->shade, new_point, flat->transform) < 0) {
        return(-1);
    }

    return(0);
}

int set_sector_transform(int sindex, int which,
                         float xx, float xy,
                         float yx, float yy) {
    Flat *flat = &(*f)[(*s)[sindex].flat[which]];
    int new_m22 = modify_matrix2x2(flat->transform, xx, xy,
                                                    yx, yy);
    if(new_m22 < 0) {
        return(-1);
    }
    if(set_sector_flat(sindex, which,
                       flat->texture, flat->shade, flat->bias, new_m22) < 0) {
        return(-1);
    }

    return(0);
}

int init_map() {
    /* set up a square room with no textures and the view in the middle, and initialize point refs */
    int firstpoint;
    int firstline;
    int no_scroll;
    int north, south, west, east;
    int north_wall, south_wall, west_wall, east_wall;
    int flat_1x;
    int top_flat, bottom_flat;

    h.points = 0;
    h.matrix2x2s = 0;
    h.matrix3x2s = 0;
    h.walls = 0;
    h.lines = 0;
    h.flats = 0;
    h.sectors = 0;
    h.links = 0;
    h.views = 0;

    no_scroll = add_point(0.0, 0.0);
    if(no_scroll < 0) {
        return(-1);
    }
    north = add_matrix3x2(1.0, 0.0, 0.0,
                          0.0, 0.0, 1.0);
    if(north < 0) {
        return(-1);
    }
    south = add_matrix3x2(-1.0, 0.0, 0.0,
                           0.0, 0.0, 1.0);
    if(south < 0) {
        return(-1);
    }
    west = add_matrix3x2(0.0, 1.0, 0.0,
                         0.0, 0.0, 1.0);
    if(west < 0) {
        return(-1);
    }
    east = add_matrix3x2(0.0, -1.0, 0.0,
                         0.0,  0.0, 1.0);
    if(east < 0) {
        return(-1);
    }
    flat_1x = add_matrix2x2(1.0, 0.0,
                            0.0, 1.0);
    if(flat_1x < 0) {
        return(-1);
    }

    /* west, north, east, south */
    firstpoint = add_point(-64.0, -64.0);
    if(firstpoint < 0) {
        return(-1);
    }
    if(add_point(-64.0, 64.0) < 0) {
        return(-1);
    }
    if(add_point(64.0, 64.0) < 0) {
        return(-1);
    }
    if(add_point(64.0, -64.0) < 0) {
        return(-1);
    }
    north_wall = add_wall(0, 0x03, no_scroll, north);
    if(north_wall < 0) {
        return(-1);
    }
    south_wall = add_wall(0, 0x18, no_scroll, south);
    if(south_wall < 0) {
        return(-1);
    }
    west_wall = add_wall(0, 0xc0, no_scroll, west);
    if(west_wall < 0) {
        return(-1);
    }
    east_wall = add_wall(0, 0xc3, no_scroll, east);
    if(east_wall < 0) {
        return(-1);
    }
    firstline = add_line(firstpoint, NO_SECTOR, west_wall, west_wall);
    if(firstline < 0) {
        return(-1);
    }
    if(add_line(firstpoint+1, NO_SECTOR, north_wall, north_wall) < 0) {
        return(-1);
    }
    if(add_line(firstpoint+2, NO_SECTOR, east_wall, east_wall) < 0) {
        return(-1);
    }
    if(add_line(firstpoint+3, NO_SECTOR, south_wall, south_wall) < 0) {
        return(-1);
    }
    top_flat = add_flat(0, 0xd8, no_scroll, flat_1x);
    if(top_flat < 0) {
        return(-1);
    }
    bottom_flat = add_flat(0, 0x1b, no_scroll, flat_1x);
    if(bottom_flat < 0) {
        return(-1);
    }
    if(add_sector(firstline, 4, 64.0, 0.0, top_flat, bottom_flat, 0) < 0) {
        return(-1);
    }

    v.pos.x = 0.0;
    v.pos.y = 0.0;
    v.angle = 0.0;
    v.startheight = 32.0;
    v.height = 32.0;
    v.start = 0;
    v.fov = FOV; 

    return(0);
}

int save_file() {
    return(0);
}
#endif

/* only compile these if detected that it's using the broken pebble SDK math */
#ifdef BROKEN_MATH
float sin_lookup_wrapper(float angle) {
    return (float)sin_lookup((int)(angle / (M_PI * 2.0) * 65536.0)) / 65536.0;
}

float cos_lookup_wrapper(float angle) {
    return (float)cos_lookup((int)(angle / (M_PI * 2.0) * 65536.0)) / 65536.0;
}

/* from https://github.com/Jejis06/fsqrt/blob/main/main.cpp
 * based on Quake 3's fast sqrt */

float fast_sqrt(float number) {
    if (number <= 0) return 0;
    
    union {
        float f;
        uint32_t i;
    } conv = {.f = number};
    
    conv.i = 0x1fbb4000 + (conv.i >> 1);  // Magic number derived for sqrt approximation
    
    // Newton-Raphson iteration for refinement
    conv.f = 0.5f * (conv.f + number / conv.f);
    conv.f = 0.5f * (conv.f + number / conv.f);  // Second iteration for better accuracy
    
    return conv.f;
}

/* from wikipedia */
float tanf_custom(float angle) {
    float sinangle = sin_lookup_wrapper(angle);
    return sinangle / fast_sqrt(1.0 - (sinangle * sinangle));
}

/* from https://github.com/ducha-aiki/fast_atan2
 * with New BSD license */

#define M_PI_4_P_0273	1.05839816339744830962 //M_PI/4 + 0.273
float atan2approx(float y,float x) {
  float absx, absy;
  absy = fabs(y);
  absx = fabs(x);
  short octant = ((x<0) << 2) + ((y<0) << 1 ) + (absx <= absy);
  switch (octant) {
    case 0: {
        if (x == 0 && y == 0)
          return 0;
        float val = absy/absx;
        return (M_PI_4_P_0273 - 0.273*val)*val; //1st octant
        break;
      }
    case 1:{
        if (x == 0 && y == 0)
          return 0.0;
        float val = absx/absy;
        return M_PI_2 - (M_PI_4_P_0273 - 0.273*val)*val; //2nd octant
        break;
      }
    case 2: {
        float val =absy/absx;
        return -(M_PI_4_P_0273 - 0.273*val)*val; //8th octant
        break;
      }
    case 3: {
        float val =absx/absy;
        return -M_PI_2 + (M_PI_4_P_0273 - 0.273*val)*val;//7th octant
        break;
      }
    case 4: {
        float val =absy/absx;
        return  M_PI - (M_PI_4_P_0273 - 0.273*val)*val;  //4th octant
      }
    case 5: {
        float val =absx/absy;
        return  M_PI_2 + (M_PI_4_P_0273 - 0.273*val)*val;//3rd octant
        break;
      }
    case 6: {
        float val =absy/absx;
        return -M_PI + (M_PI_4_P_0273 - 0.273*val)*val; //5th octant
        break;
      }
    case 7: {
        float val =absx/absy;
        return -M_PI_2 - (M_PI_4_P_0273 - 0.273*val)*val; //6th octant
        break;
      }
    default:
      return 0.0;
    }
}

float fmodf_custom(float x, float y) {
    return(((x / y) - ((int)x / (int)y)) * y);
}
#endif
