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

#ifdef BROKEN_MATH
/* needed for heap_bytes_free() */
#include <pebble.h>
#endif

#include "engine.h"
#include "log.h"
#include "cache.h"

#define CEILING (0)
#define FLOOR (1)

#define ACTION_NONE (0x00)
#define ACTION_WARP (0x01)
#define ACTION_WAYPOINT (0x02)

#define SHADE_SUB (0x100)
#define SHADE_PARALLAX (0x200)

unsigned char mapnum;
View v;
Point waypoint;
Point (*p)[] = NULL;
Matrix2x2 (*m22)[] = NULL;
Matrix3x2 (*m32)[] = NULL;
Line (*l)[] = NULL;
Sector (*s)[] = NULL;

open_map_t open_map_p;
read_map_t read_map_p;
close_map_t close_map_p;

/* these are all packed in the file, so very efficient access isn't necessary */

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    unsigned short points;
    unsigned short matrix2x2s;
    unsigned short matrix3x2s;
    unsigned short lines;
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
    unsigned short point;
    /* sectors are lined by a separate link table */

    unsigned char texture[2];
    unsigned short shade[2];
    unsigned short texture_bias[2];
    unsigned short texture_transform[2];
} Line_data;

typedef struct __attribute__((packed)) __attribute__((aligned(1))) {
    float height[2];

    unsigned char texture[2];
    unsigned short shade[2];
    unsigned short texture_bias[2];
    unsigned short texture_transform[2];

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
    Line_data l;
    Sector_data s;
    Link_data li;
    View_data v;
} Data;

static void print_data(Header *h) {
    int i;

    LOG("%hu %hu %hu %hu %hu %hu %hu\n", h->points, h->matrix2x2s, h->matrix3x2s, h->lines, h->sectors, h->links, h->views);

    for(i = 0; i < h->points; i++) {
        LOG("%d Point %p %f %f\n", i, &(*p)[i], (*p)[i].x, (*p)[i].y);
    }

    for(i = 0; i < h->matrix2x2s; i++) {
        LOG("%d Matrix2x2 %p %f %f %f %f\n", i, &(*m22)[i], (*m22)[i].xx, (*m22)[i].xy, (*m22)[i].yx, (*m22)[i].yy);
    }

    for(i = 0; i < h->matrix3x2s; i++) {
        LOG("%d Matrix3x2 %p %f %f %f %f %f %f\n", i, &(*m32)[i], (*m32)[i].xx, (*m32)[i].xy, (*m32)[i].xz, (*m32)[i].yx, (*m32)[i].yy, (*m32)[i].yz);
    }

    for(i = 0; i < h->lines; i++) {
        LOG("%d Line %p %p %p %hu %hu %hX %hX %p %p %p %p\n", i,
            &(*l)[i], (*l)[i].point, (*l)[i].sector,
            (*l)[i].texture[CEILING], (*l)[i].texture[FLOOR],
            (*l)[i].shade[CEILING], (*l)[i].shade[FLOOR],
            (*l)[i].texture_bias[CEILING], (*l)[i].texture_bias[FLOOR],
            (*l)[i].texture_transform[CEILING], (*l)[i].texture_transform[FLOOR]);
    }

    for(i = 0; i < h->sectors; i++) {
        LOG("%d Sector %p %f %f %hu %hu %hX %hX %p %p %p %p %p %hhu ", i,
            &(*s)[i], (*s)[i].height[CEILING], (*s)[i].height[FLOOR],
            (*s)[i].texture[CEILING], (*s)[i].texture[FLOOR],
            (*s)[i].shade[CEILING], (*s)[i].shade[FLOOR],
            (*s)[i].texture_bias[CEILING], (*s)[i].texture_bias[FLOOR],
            (*s)[i].texture_transform[CEILING], (*s)[i].texture_transform[FLOOR],
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

    LOG("View %p %f %f %f %f %f %f\n",
        v.start, v.pos.x, v.pos.y,
        v.angle, v.startheight, v.height, v.fov);
}

void apply_view_data(View_data *vd) {
    v.start = &(*s)[vd->start];
    v.pos.x = (*p)[vd->pos].x;
    v.pos.y = (*p)[vd->pos].y;
    v.angle = vd->angle;
    v.startheight = vd->startheight;
    v.fov = vd->fov;

    /* set the player's world height */
    v.height = v.start->height[FLOOR] + v.startheight;
}

int engine_load(unsigned char number, unsigned char view) {
    Header h;
    Data d;
    int i;
    off_t start = 0;

#ifdef BROKEN_MATH
    /* pebble */
    LOG("Pre-free %d bytes\n", heap_bytes_free());
#endif

    if(p != NULL) {
        free(s);
        free(l);
        free(m32);
        free(m22);
        free(p);
    }

#ifdef BROKEN_MATH
    LOG("Post-free %d bytes\n", heap_bytes_free());
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

    l = malloc(h.lines * sizeof(Line));
    if(l == NULL) {
        goto error_m32s;
    }

    s = malloc(h.sectors * sizeof(Sector));
    if(s == NULL) {
        goto error_lines;
    }

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
    for(i = 0; i < h.lines; i++) {
        read_map_p(start + (i * sizeof(Line_data)), sizeof(Line_data), &d.l);
        (*l)[i].point = &(*p)[d.l.point];
        /* sector will be filled when links are read */
        (*l)[i].sector = NULL;
        (*l)[i].texture[CEILING] = d.l.texture[CEILING];
        (*l)[i].texture[FLOOR] = d.l.texture[FLOOR];
        (*l)[i].shade[CEILING] = d.l.shade[CEILING];
        (*l)[i].shade[FLOOR] = d.l.shade[FLOOR];
        (*l)[i].texture_bias[CEILING] = &(*p)[d.l.texture_bias[CEILING]];
        (*l)[i].texture_bias[FLOOR] = &(*p)[d.l.texture_bias[FLOOR]];
        (*l)[i].texture_transform[CEILING] = &(*m32)[d.l.texture_transform[CEILING]];
        (*l)[i].texture_transform[FLOOR] = &(*m32)[d.l.texture_transform[FLOOR]];
    }

    start += sizeof(Line_data) * h.lines;
    for(i = 0; i < h.sectors; i++) {
        read_map_p(start + (i * sizeof(Sector_data)), sizeof(Sector_data), &d.s);
        d.s.lines += 3; /* 3 lines minimum so 0 is 3 mines */
        (*s)[i].height[CEILING] = d.s.height[CEILING];
        (*s)[i].height[FLOOR] = d.s.height[FLOOR];
        (*s)[i].texture[CEILING] = d.s.texture[CEILING];
        (*s)[i].texture[FLOOR] = d.s.texture[FLOOR];
        (*s)[i].shade[CEILING] = d.s.shade[CEILING];
        (*s)[i].shade[FLOOR] = d.s.shade[FLOOR];
        (*s)[i].texture_bias[CEILING] = &(*p)[d.s.texture_bias[CEILING]];
        (*s)[i].texture_bias[FLOOR] = &(*p)[d.s.texture_bias[FLOOR]];
        (*s)[i].texture_transform[CEILING] = &(*m22)[d.s.texture_transform[CEILING]];
        (*s)[i].texture_transform[FLOOR] = &(*m22)[d.s.texture_transform[FLOOR]];
        (*s)[i].firstline = &(*l)[d.s.firstline];
        (*s)[i].lines = d.s.lines;
        (*s)[i].action = d.s.action;
    }

    start += sizeof(Sector_data) * h.sectors;
    for(i = 0; i < h.links; i++) {
        read_map_p(start + (i * sizeof(Link_data)), sizeof(Link_data), &d.li);
        /* link first sector to second */
        (*s)[d.li.sector].firstline[d.li.line].sector = &(*s)[d.li.lsector];
        /* link second sector to first */
        (*s)[d.li.lsector].firstline[d.li.lline].sector = &(*s)[d.li.sector];
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
    LOG("Memory in bytes - Points %d Matrix2x2s %d Matrix3x2s %d Lines %d Sectors %d Total %d\n",
         h.points * sizeof(Point), h.matrix2x2s * sizeof(Matrix2x2), h.matrix3x2s * sizeof(Matrix3x2), h.lines * sizeof(Line), h.sectors * sizeof(Sector),
        (h.points * sizeof(Point)) + (h.matrix2x2s * sizeof(Matrix2x2)) + (h.matrix3x2s * sizeof(Matrix3x2)) + (h.lines * sizeof(Line)) + (h.sectors * sizeof(Sector)));
#endif

    mapnum = number;

    return(0);

error_lines:
    free(l);
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
 
Line *scan_sector(Sector *last_s, Sector *s,
                  Point *pos, Point *slope,
                  int vvertical, float vslope,
                  Point *hit) {
    Point *point;
    Point *nextpoint;
    Line *line;
    int i;

    point = s->firstline->point;
    for(i = 0; i < s->lines; i++) {
        line = &s->firstline[i];

        /* current line spans from the current point to the next line point */
        nextpoint = s->firstline[(i + 1) % s->lines].point;

        /* don't check for lines that would look back towards the previous sector */
        if(last_s != NULL && line->sector == last_s) {
            /* make sure the point used in the next iteration is updated */
            point = nextpoint;
            continue;
        }

        if(line_hit(point, nextpoint,
                    pos, slope,
                    vvertical, vslope,
                    hit)) {
            return line;
        }

        point = nextpoint;
    }

    return NULL;
}

float get_distance(Point *pos, Point *hit) {
    return(sqrtf(fabs(powf(hit->y - pos->y, 2.0)) + fabs(powf(hit->x - pos->x, 2.0))));
}

float get_distance2(Point *pos, Point *hit, Point *slope) {
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

void engine_render(unsigned char *pixels, int w, int h, int pitch) {
    Sector *s, *last_s;
    float offset;
    Point slope;
    int vvertical;
    float vslope;
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
    Point *texture_bias;
    Matrix2x2 *texture_transform_22;
    Matrix3x2 *texture_transform_32;

    float w_2 = (float)w / 2.0;
    float h_2 = (float)h / 2.0;
    float fov_2 = v.fov / 2.0;
    float sin_angle = sin(v.angle);
    float cos_angle = cos(v.angle);
    float max_offset = atan2(fov_2, 1.0) * 2.0;

    age_slots();

    /* for each column */
    for(x = 0; x < w; x++) {
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
 
        s = v.start;
        last_s = NULL;
        top = 0;
        bottom = h - 1;
        pos.x = v.pos.x;
        pos.y = v.pos.y;
        total_distance = 0.0;
        while (true) {
            line = scan_sector(last_s, s, &pos,
                               &slope, vvertical, vslope,
                               &hit);

            /* shouldn't happen, but in case the ray misses for some reason */
            if(line == NULL) {
                break;
            }

            /* get the distance between the view and hit coordinates */
            total_distance += get_distance2(&pos, &hit, &slope);

            /* iterate y (angle transformed with FOV to get an angle from screen Y) with height
             * from bottom solving for distance until total distance is reached.
             * Texture X and Y lookup from offset from view given ray angle */

            /* draw ceiling */
            ceilingdiff = s->height[0] - v.height;
            if(ceilingdiff > 0.0) {
                /* ceiling is above the view line */

                /* get ceiling texture */
                if(s->texture[CEILING] != ceiling_tex) {
                    ceiling_tex = s->texture[CEILING];
                    set_tex(ceiling_tex, &ceiling_data, &ceiling_mask, &ceiling_shift, &ceiling_dim);
                }
 
                texture_bias = s->texture_bias[CEILING];
                texture_transform_22 = s->texture_transform[CEILING];
                for(y = top; y < h_2 && y < bottom; y++) {
                    z = ceilingdiff / ((h_2 - y) / h_2) * max_offset;

                    if(z > total_distance) {
                        break;
                    }
                    if(ceiling_dim == 0) {
                        color = 0x00;
                    } else {
                        if(s->shade[CEILING] & SHADE_PARALLAX) {
                            wx = (float)x / w * ceiling_dim;
                            wy = (float)y / h * ceiling_dim;
                        } else {
                            wx = v.pos.x + (slope.x * z);
                            wy = v.pos.y + (slope.y * z);
                        }
                        tx = (wx * texture_transform_22->xx) +
                             (wy * texture_transform_22->xy) +
                             texture_bias->x;
                        ty = (wx * texture_transform_22->yx) +
                             (wy * texture_transform_22->yy) +
                             texture_bias->y;
                        color = ceiling_data[(ty & ceiling_mask) << ceiling_shift | (tx & ceiling_mask)];
                    }

                    if(s->shade[CEILING] & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += s->shade[CEILING] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += s->shade[CEILING] & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }

                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                top = y;
            }

            /* draw floor */
            floordiff = s->height[FLOOR] - v.height;
            if(floordiff < 0.0) {
                if(s->texture[FLOOR] != floor_tex) {
                    floor_tex = s->texture[FLOOR];
                    set_tex(floor_tex, &floor_data, &floor_mask, &floor_shift, &floor_dim);
                }

                texture_bias = s->texture_bias[FLOOR];
                texture_transform_22 = s->texture_transform[FLOOR];
                for(y = bottom; y >= 0 && y >= top; y--) {
                    z = floordiff / ((h_2 - y) / h_2) * max_offset;

                    if(z >= total_distance) {
                        break;
                    }
                    if(floor_dim == 0) {
                        color = 0x00;
                    } else {
                        if(s->shade[FLOOR] & SHADE_PARALLAX) {
                            wx = (float)x / w * floor_dim;
                            wy = (float)y / h * floor_dim;
                        } else {
                            wx = v.pos.x + (slope.x * z);
                            wy = v.pos.y + (slope.y * z);
                        }
                        tx = (wx * texture_transform_22->xx) +
                             (wy * texture_transform_22->xy) +
                             texture_bias->x;
                        ty = (wx * texture_transform_22->yx) +
                             (wy * texture_transform_22->yy) +
                             texture_bias->y;
                        color = floor_data[(ty & floor_mask) << floor_shift | (tx & floor_mask)];
                    }
                    if(s->shade[FLOOR] & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += s->shade[FLOOR] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += s->shade[FLOOR] & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                bottom = y;
            }

            /* get top wall texture as this'll most likely be needed */
            if(line->texture[CEILING] != top_line_tex) {
                top_line_tex = line->texture[CEILING];
                set_tex(top_line_tex, &top_line_data, &top_line_mask, &top_line_shift, &top_line_dim);
            }

            if(line->sector == NULL) {
                /* solid wall, no sector on the other side */
    
                /* draw wall */
                if(line->shade[CEILING] & SHADE_PARALLAX) {
                    /* calculate screen X position relative to texture res */
                    /* TODO: try to calculate based on screen aspect */
                    wx = (float)x / w * top_line_dim;
                } else {
                    /* calculate world coordinates */
                    wx = v.pos.x + (slope.x * total_distance);
                    wy = v.pos.y + (slope.y * total_distance);
                }

                texture_bias = line->texture_bias[CEILING];
                texture_transform_32 = line->texture_transform[CEILING];
                for(y = top;
                    y <= bottom && y < h;
                    y++) {
                    if(line->shade[CEILING] & SHADE_PARALLAX) {
                        /* calculate screen Y position */
                        wy = (float)y / h * top_line_dim;
                    }

                    if(top_line_dim == 0) {
                        color = 0x00;
                    } else {
                        if(line->shade[CEILING] & SHADE_PARALLAX) {
                            wz = 0.0;
                        } else {
                            /* calculate height */
                            wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                        }

                        /* calculate translated texcoords */
                        tx = (wx * texture_transform_32->xx) +
                             (wy * texture_transform_32->xy) +
                             (wz * texture_transform_32->xz) +
                             texture_bias->x;
                        ty = (wx * texture_transform_32->yx) +
                             (wy * texture_transform_32->yy) +
                             (wz * texture_transform_32->yz) +
                             texture_bias->y;

                        /* fetch tex color */
                        color = top_line_data[(ty & top_line_mask) << top_line_shift | (tx & top_line_mask)];
                    }

                    if(line->shade[CEILING] & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += line->shade[CEILING] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += line->shade[CEILING] & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                break;
            }

            /* get sector on other side of the line while keeping note of the last sector so raycasting won't traverse back through it */
            last_s = s;
            s = line->sector;

            ceilingdiff = s->height[0] - v.height;
            next_y = h_2 - (ceilingdiff / total_distance * max_offset * h_2);
            if(next_y > top) {
                /* draw top wall */
                if(line->shade[CEILING] & SHADE_PARALLAX) {
                    wx = (float)x / w * top_line_dim;
                } else {
                    wx = v.pos.x + (slope.x * total_distance);
                    wy = v.pos.y + (slope.y * total_distance);
                }

                texture_bias = line->texture_bias[0];
                texture_transform_32 = line->texture_transform[0];
                for(y = top;
                    y <= next_y && y < h;
                    y++) {
                    if(line->shade[CEILING] & SHADE_PARALLAX) {
                        wy = (float)y / h * top_line_dim;
                    }

                    if(top_line_dim == 0) {
                        color = 0x00;
                    } else {
                        if(line->shade[CEILING] & SHADE_PARALLAX) {
                            wz = 0.0;
                        } else {
                            wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                        }

                        tx = (wx * texture_transform_32->xx) +
                             (wy * texture_transform_32->xy) +
                             (wz * texture_transform_32->xz) +
                             texture_bias->x;
                        ty = (wx * texture_transform_32->yx) +
                             (wy * texture_transform_32->yy) +
                             (wz * texture_transform_32->yz) +
                             texture_bias->y;
                        color = top_line_data[(ty & top_line_mask) << top_line_shift | (tx & top_line_mask)];
                    }

                    if(line->shade[CEILING] & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += line->shade[CEILING] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += line->shade[CEILING] & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                top = y;
            }

            /* draw bottom wall */
            floordiff = s->height[FLOOR] - v.height;
            next_y = h_2 - (floordiff / total_distance * max_offset * h_2);
            if(next_y < bottom) {
                /* bottom wall texture will be needed */
                if(line->texture[FLOOR] != bottom_line_tex) {
                    bottom_line_tex = line->texture[FLOOR];
                    set_tex(bottom_line_tex, &bottom_line_data, &bottom_line_mask, &bottom_line_shift, &bottom_line_dim);
                }

                if(line->shade[FLOOR] & SHADE_PARALLAX) {
                    wx = (float)x / w * bottom_line_dim;
                } else {
                    wx = v.pos.x + (slope.x * total_distance);
                    wy = v.pos.y + (slope.y * total_distance);
                }

                texture_bias = line->texture_bias[FLOOR];
                texture_transform_32 = line->texture_transform[FLOOR];
                for(y = bottom;
                    y >= next_y && y >= 0;
                    y--) {
                    if(line->shade[FLOOR] & SHADE_PARALLAX) {
                        wy = (float)y / h * bottom_line_dim;
                    }

                    if(bottom_line_dim == 0) {
                        color = 0x00;
                    } else {
                        if(line->shade[FLOOR] & SHADE_PARALLAX) {
                            wz = 0.0;
                        } else {
                            wz = ((y - h_2) / h_2) / max_offset * total_distance - v.height;
                        }

                        tx = (wx * texture_transform_32->xx) +
                             (wy * texture_transform_32->xy) +
                             (wz * texture_transform_32->xz) +
                             texture_bias->x;
                        ty = (wx * texture_transform_32->yx) +
                             (wy * texture_transform_32->yy) +
                             (wz * texture_transform_32->yz) +
                             texture_bias->y;
                        color = bottom_line_data[(ty & bottom_line_mask) << bottom_line_shift | (tx & bottom_line_mask)];
                    }

                    if(line->shade[FLOOR] & SHADE_SUB) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += line->shade[FLOOR] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += line->shade[FLOOR] & 0xFF; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
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

int do_action(Sector *s) {
    switch(s->action & 0xFF) {
        case ACTION_WARP:
            unsigned char map = (s->action & 0xFF00) >> 8;
            unsigned char view = (s->action & 0xFF0000) >> 16;
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
            unsigned short firstpoint = (s->action & 0xFFFF00) >> 8;
            unsigned short count = (s->action & 0xFF000000) >> 24;
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

void engine_move(float x, float y) {
    Point slope;
    Point dest;
    int vvertical;
    float vslope;
    Point pos;
    Point hit;
    Line *line;
    Sector *s;
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
    s = v.start;
    while(true) {
        line = scan_sector(last_s, s, &pos,
                           &slope, vvertical, vslope,
                           &hit);

        /* shouldn't happen, but in case the ray misses for some reason */
        if(line == NULL) {
            return;
        }

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
        if(line->sector == NULL) {
            return;
        }

        /* update working position and sector */
        pos.x = hit.x;
        pos.y = hit.y;
        last_s = s;
        s = line->sector;

        if(do_action(s)) {
            /* do_action indicated that it already updated the view state */
            return;
        };
    }

    /* finally, update player position and sector */
    v.pos.x = dest.x;
    v.pos.y = dest.y;
    v.start = s;
    v.height = s->height[1] + v.startheight;
}

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
