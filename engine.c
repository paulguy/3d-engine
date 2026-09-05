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

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "engine.h"
#include "log.h"
#include "cache.h"

#define FOV (90.0 / 360.0 * (M_PI * 2.0))
#define VIEW_HEIGHT (50)

typedef enum {
    AXIS_PX,
    AXIS_PY,
    AXIS_NX,
    AXIS_NY
} Axis;

/* something about texture caching
 * loading zones
 */

/* these here temporarily for testing */
Point pdata[] = {
    {-50, 150},
    {50, 150},
    {150, 50},
    {150, -50},
    {50, -150},
    {-50, -150},
    {-150, -50},
    {-150, 50},
    {-50, 300},
    {50, 300}
};
Line ldata[] = {
    {0, 1,
     {1, 1},
     {0, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{1.0, 0.0, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {1, 99999,
     {1, 1},
     {0x192, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.7, -0.7, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {2, 99999,
     {1, 1},
     {0x149, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.0, 0.25, 0.0,
       0.0, 0.0, 0.25},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {3, 99999,
     {1, 1},
     {0, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.7, 0.7, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {4, 99999,
     {1, 1},
     {0x49, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{1.0, 0.0, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {5, 99999,
     {1, 1},
     {0x92, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.7, -0.7, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {6, 99999,
     {1, 1},
     {0x108, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.0, 1.0, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {7, 99999,
     {1, 1},
     {0x08, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.7, 0.7, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},

    {8, 99999,
     {1, 1},
     {0, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{1.0, 0.0, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {9, 99999,
     {1, 1},
     {0, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.0, 1.0, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}},
    {1, 0,
     {1, 1},
     {0, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.0, 0.0, 0.0,
       0.0, 0.0, 0.0},
      {1.0, 0.0, 0.0,
       0.0, 0.0, 1.0}}},
    {0, 99999,
     {1, 1},
     {0, 0},
     {{0.0, 0.0},
      {0.0, 0.0}},
     {{0.0, 1.0, 0.0,
       0.0, 0.0, 1.0},
      {0.0, 0.0, 0.0,
       0.0, 0.0, 0.0}}}
};
Sector sdata[] = {
    {&(ldata[0]), 8, {50, -50}, {1, 1}, {0x92, 0},
        {{0.0, 0.0}, {0.0, 0.0}},
        {{1.0, 0.0,
          0.0, 1.0},
         {1.0, 0.0,
          0.0, 1.0}}},
    {&(ldata[8]), 4, {40, -60}, {0, 0}, {0xC0, 0x03},
        {{0.0, 0.0}, {0.0, 0.0}},
        {{1.0, 0.0,
          0.0, 1.0},
         {1.0, 0.0,
          0.0, 1.0}}}
};

View v;
Point (*p)[] = &pdata;
Line (*l)[] = &ldata;
int numlines = sizeof(ldata) / sizeof(Line);
Sector (*s)[] = &sdata;
int numsectors = sizeof(sdata) / sizeof(Sector);

void print_sectors() {
    int i, j;

    for(i = 0; i < numsectors; i++) {
        LOG("Sector %d\n", i);
        for(j = 0; j < (*s)[i].numlines; j++) {
            LOG("%f, %f  ", (*(*s)[i].line)[j].point->x, (*(*s)[i].line)[j].point->y);
        }
        LOG("\n");
    }
}

void engine_load() {
    int i;
    /* TODO: load from file/resource */

    /* indices to pointers */
    for(i = 0; i < numlines; i++) {
        if((*l)[i].sector > numsectors) {
            (*l)[i].sector = NULL;
        } else {
            (*l)[i].sector = &(*s)[(intptr_t)(*l)[i].sector];
        }
    }

    for(i = 0; i < numlines; i++) {
        (*l)[i].point = &(*p)[(intptr_t)(*l)[i].point];
    }
/*
    print_sectors();
    */

    /* temporarily place view at center of first sector */
    v.start = &(*s)[0];
    v.pos.x = 0.0;
    v.pos.y = 0.0;
    for(i = 0; i < (*s)[0].numlines; i++) {
        v.pos.x += (*(*s)[0].line)[i].point->x;
        v.pos.y += (*(*s)[0].line)[i].point->y;
    }
    v.pos.x /= (*s)[0].numlines;
    v.pos.y /= (*s)[0].numlines;
    v.angle = 0.0;
    v.fov = FOV;
    v.height = v.start->height[1] + VIEW_HEIGHT;
}

Axis find_slope(float angle, float *slope) {
    /* find rise and run of a ray firing from the view such that each unit of slope 
     * given any offset plots a line perpendicular to the view center angle */

    /* avoid division by 0 ... things might still get weird at angles extremely close to the axis
     * so i might still need to return to the -45,+45 degree off axis ranges or return the 
     * slope as a vector and do the transformation in line_hit */
    if(angle >= (M_PI * 2.0 / 8.0) && angle < (M_PI * 2.0 * 3.0 / 8.0)) {
        /* right +x */
        *slope = cos(angle) / sin(angle);
        return(AXIS_PX);
    } else if(angle >= (M_PI * 2.0 * 3.0 / 8.0) && angle < (M_PI * 2.0 * 5.0 / 8.0)) {
        /* down -y */
        *slope = sin(angle) / cos(angle);
        return(AXIS_NY);
    } else if(angle >= (M_PI * 2.0 * 5.0 / 8.0) && angle < (M_PI * 2.0 * 7.0 / 8.0)) {
        /* left -x */
        *slope = cos(angle) / sin(angle);
        return(AXIS_NX);
    }
    /* up +y */
    *slope = sin(angle) / cos(angle);
    return(AXIS_PY);
}

int line_hit(Point *p1, Point *p2,
             Point *pos, Axis axis, float slope,
             Point *hit) {
    float x, y;
    float lslope;
    /* view is given as a general axis facing direction and a slope representing the line
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

    if(axis == AXIS_NX || axis == AXIS_PX) {
        /* view ray about X axis */

        if(p2->x == p1->x) {
            /* vertical line slope can't be calculated */

            /* X is just the line */
            x = p1->x;
        } else {
            /* calculate line slope */
            lslope = (p2->y - p1->y) / (p2->x - p1->x);

            /* calculate line intercept X */
            x = (-(lslope * p1->x) + (slope * pos->x) + p1->y - pos->y) / (slope - lslope);

            /* if intercept is outside of the line's bounds, it's not eligible */
            if(!((x >= p1->x && x <= p2->x) ||
                 (x >= p2->x && x <= p1->x))) {
                return(0);
            }
        }

        /* calculate Y of X intercept from view ray */
        if(p1->y == p2->y) {
            y = p1->y;
        } else {
            y = (slope * x) + pos->y - (slope * pos->x);
        }

        if((axis == AXIS_PX && x >= pos->x && y <= p1->y && y >= p2->y) ||
           (axis == AXIS_NX && x <= pos->x && y <= p2->y && y >= p1->y)) {
            /* X intercept is between line points and in front of view as sector lines are clockwise */

            /* pythagorean theorem solving for hypotenuse */
            hit->x = x;
            hit->y = y;
            return(1);
        }
    } else {
        /* view ray about Y axis */

        if(p2->y == p1->y) {
            /* horizontal line slope can't be calculated */

            /* Y is just the line */
            y = p1->y;
        } else {
            /* calculate line slope */
            lslope = (p2->x - p1->x) / (p2->y - p1->y);

            /* calculate line intercept Y */
            y = (-(lslope * p1->y) + (slope * pos->y) + p1->x - pos->x) / (slope - lslope);

            /* if intercept is outside of the line's bounds, it's not eligible */
            if(!((y >= p1->y && y <= p2->y) ||
                 (y >= p2->y && y <= p1->y))) {
                return(0);
            }
        }

        /* calculate X of Y intercept */
        if(p1->x == p2->x) {
            x = p1->x;
        } else {
            x = (slope * y) + pos->x - (slope * pos->y);
        }

        if((axis == AXIS_PY && y >= pos->y && x >= p1->x && x <= p2->x) ||
           (axis == AXIS_NY && y <= pos->y && x >= p2->x && x <= p1->x)) {
            /* X intercept is between line points and in front of view as sector lines are clockwise */

            /* pythagorean theorem solving for hypotenuse */
            hit->x = x;
            hit->y = y;
            return(1);
        }
    }

    return(0);
}

Line *scan_sector(Sector *s,
                  Point *pos, int axis, float slope,
                  Sector *last_s,
                  Point *hit) {
    Point *point;
    Point *nextpoint;
    Line *line;
    int i;

    point = (*s->line)[0].point;
    for(i = 0; i < s->numlines; i++) {
        line = &(*s->line)[i];

        /* current line spans from the current point to the next line point */
        nextpoint = (*s->line)[(i + 1) % s->numlines].point;

        /* don't check for lines that would look back towards the previous sector */
        if(last_s != NULL && line->sector == last_s) {
            /* make sure the point used in the next iteration is updated */
            point = nextpoint;
            continue;
        }

        if(line_hit(point, nextpoint,
                    pos, axis, slope,
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

void set_tex(unsigned char num,
             unsigned char **data,
             unsigned char *mask,
             unsigned char *dim) {
    *dim = load_tex(num, data);
    if(*dim == 64) {
        *mask = 0x3F;
        *dim = 6;
    } else if(*dim == 32) {
        *mask = 0x1F;
        *dim = 5;
    }
}

void engine_render(unsigned char *pixels, int w, int h, int pitch) {
    Sector *s, *last_s;
    float angle;
    int axis;
    float slope;
    Line *line;
    Point hit;
    float distance;
    float accumulated_distance;
    float total_distance;
    int x, y;
    float z;
    float wx, wy, wz;
    int tx, ty;
    int top, bottom;
    Point pos;
    float compensation;
    float ceilingdiff;
    float floordiff;
    float next_y;
    unsigned char floor_tex = 0;
    unsigned char floor_dim = 0;
    unsigned char floor_mask = 0;
    unsigned char *floor_data = NULL;
    unsigned char ceiling_tex = 0;
    unsigned char ceiling_dim = 0;
    unsigned char ceiling_mask = 0;
    unsigned char *ceiling_data = NULL;
    unsigned char top_line_tex = 0;
    unsigned char top_line_dim = 0;
    unsigned char top_line_mask = 0;
    unsigned char *top_line_data = NULL;
    unsigned char bottom_line_tex = 0;
    unsigned char bottom_line_dim = 0;
    unsigned char bottom_line_mask = 0;
    unsigned char *bottom_line_data = NULL;
    unsigned short color;

    float h_2 = (float)h / 2.0;
    float fov_2 = v.fov / 2.0;
    float step = v.fov / (float)w;
    float y_to_angle = fov_2 / h_2;
    float angle_to_y = h_2 / fov_2;

    age_slots();

    /* for each column */
    for(x = 0; x < w; x++) {
        angle = fmodf(v.angle + (step * (x - h_2)), M_PI * 2.0);
        if(angle < 0.0) {
            angle += M_PI * 2.0;
        }

        axis = find_slope(angle, &slope);

        s = v.start;
        last_s = NULL;
        top = 0;
        bottom = h - 1;
        pos.x = v.pos.x;
        pos.y = v.pos.y;
        accumulated_distance = 0.0;
        while (1) {
            /* get sector textures */
            if(s->texture[0] != ceiling_tex) {
                ceiling_tex = s->texture[0];
                set_tex(ceiling_tex, &ceiling_data, &ceiling_mask, &ceiling_dim);
            }
            if(s->texture[1] != floor_tex) {
                floor_tex = s->texture[1];
                set_tex(floor_tex, &floor_data, &floor_mask, &floor_dim);
            }

            /* TODO: scan in to portal walls until solid wall or distance reached */
            line = scan_sector(s,
                               &pos, axis, slope,
                               last_s,
                               &hit);

            /* shouldn't happen, but in case the ray misses for some reason */
            if(line == NULL) {
                break;
            }

            /* get the distance between the view and hit coordinates */
            distance = get_distance(&pos, &hit);
            accumulated_distance += distance;

            /* fisheye compensation, this kinda doesn't work 100% but whatever? */
            compensation = cos(-fov_2 + (step * (float)x));

            total_distance = accumulated_distance * compensation;
            /* iterate y (angle transformed with FOV to get an angle from screen Y) with height
             * from bottom solving for distance until total distance is reached.
             * Texture X and Y lookup from offset from view given ray angle */

            /* draw ceiling */
            ceilingdiff = s->height[0] - v.height;
            if(ceilingdiff > 0.0) {
                for(y = top; y < h; y++) {
                    z = ceilingdiff / tanf((h_2 - y) * y_to_angle);
                    if(z >= total_distance) {
                        break;
                    }
                    z /= compensation;
                    if(ceiling_dim == 0) {
                        color = 0x00;
                    } else {
                        wx = v.pos.x + (sin(angle) * z);
                        wy = v.pos.y + (cos(angle) * z);
                        tx = (wx * s->texture_transform[0].xx) +
                             (wy * s->texture_transform[0].xy) +
                             s->texture_bias[0].x;
                        ty = (wx * s->texture_transform[0].yx) +
                             (wy * s->texture_transform[0].yy) +
                             s->texture_bias[0].y;
                        color = ceiling_data[(ty & ceiling_mask) << ceiling_dim | (tx & ceiling_mask)];
                    }
                    if(s->shade[0] & 0xFF00) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += s->shade[0] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += s->shade[0]; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                top = y;
            }

            /* draw floor */
            floordiff = s->height[1] - v.height;
            if(floordiff < 0.0) {
                for(y = bottom; y >= 0; y--) {
                    z = -floordiff / tanf((y - h_2) * y_to_angle);
                    if(z >= total_distance) {
                        break;
                    }
                    z /= compensation;
                    if(floor_dim == 0) {
                        color = 0x00;
                    } else {
                        wx = v.pos.x + (sin(angle) * z);
                        wy = v.pos.y + (cos(angle) * z);
                        tx = (wx * s->texture_transform[1].xx) +
                             (wy * s->texture_transform[1].xy) +
                             s->texture_bias[1].x;
                        ty = (wx * s->texture_transform[1].yx) +
                             (wy * s->texture_transform[1].yy) +
                             s->texture_bias[1].y;
                        color = floor_data[(ty & floor_mask) << floor_dim | (tx & floor_mask)];
                    }
                    if(s->shade[1] & 0xFF00) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += s->shade[1] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += s->shade[1]; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                bottom = y;
            }

            /* get top wall texture as this'll most likely be needed */
            if(line->texture[0] != top_line_tex) {
                top_line_tex = line->texture[0];
                set_tex(top_line_tex, &top_line_data, &top_line_mask, &top_line_dim);
            }

            if(line->sector == NULL) {
                /* solid wall, no sector on the other side */
    
                /* draw wall */
                wx = v.pos.x + (sin(angle) * total_distance);
                wy = v.pos.y + (cos(angle) * total_distance);
                for(y = top;
                    y <= bottom && y < h;
                    y++) {
                    if(top_line_dim == 0) {
                        color = 0x00;
                    } else {
                        wz = ((y - h_2) * y_to_angle) * total_distance - v.height;
                        tx = (wx * line->texture_transform[0].xx) +
                             (wy * line->texture_transform[0].xy) +
                             (wz * line->texture_transform[0].xz) +
                             line->texture_bias[0].x;
                        ty = (wx * line->texture_transform[0].yx) +
                             (wy * line->texture_transform[0].yy) +
                             (wz * line->texture_transform[0].yz) +
                             line->texture_bias[0].y;
                        color = top_line_data[(ty & top_line_mask) << top_line_dim | (tx & top_line_mask)];
                    }
                    if(line->shade[0] & 0xFF00) {
                        color = ~color; /* invert bits */
                        color &= 0xDB; /* unset overflow bits */
                        color += line->shade[0] & 0xFF; /* add which should subtract when inverted back? */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                        color = ~color; /* invert back */
                    } else {
                        color += line->shade[0]; /* add */
                        color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    }
                    pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
                }
                break;
            }
            last_s = s;
            s = line->sector;

            /* draw top wall */
            wx = v.pos.x + (sin(angle) * total_distance);
            wy = v.pos.y + (cos(angle) * total_distance);
            ceilingdiff = s->height[0] - v.height;
            next_y = atan2f(total_distance, ceilingdiff) * angle_to_y - h_2;
            for(y = top;
                y <= next_y && y < h;
                y++) {
                if(top_line_dim == 0) {
                    color = 0x00;
                } else {
                    wz = ((y - h_2) * y_to_angle) * total_distance;
                    tx = (wx * line->texture_transform[0].xx) +
                         (wy * line->texture_transform[0].xy) +
                         (wz * line->texture_transform[0].xz) +
                         line->texture_bias[0].x;
                    ty = (wx * line->texture_transform[0].yx) +
                         (wy * line->texture_transform[0].yy) +
                         (wz * line->texture_transform[0].yz) +
                         line->texture_bias[0].y;
                    color = top_line_data[(ty & top_line_mask) << top_line_dim | (tx & top_line_mask)];
                }
                if(line->shade[0] & 0xFF00) {
                    color = ~color; /* invert bits */
                    color &= 0xDB; /* unset overflow bits */
                    color += line->shade[0] & 0xFF; /* add which should subtract when inverted back? */
                    color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    color = ~color; /* invert back */
                } else {
                    color += line->shade[0]; /* add */
                    color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                }
                pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
            }
            top = y;

            /* draw bottom wall */
            wx = v.pos.x + (sin(angle) * total_distance);
            wy = v.pos.y + (cos(angle) * total_distance);
            floordiff = s->height[1] - v.height;
            next_y = atan2f(total_distance, floordiff) * angle_to_y - h_2;
            if(next_y < bottom) {
                /* bottom wall texture will be needed */
                if(line->texture[1] != bottom_line_tex) {
                    bottom_line_tex = line->texture[1];
                    set_tex(bottom_line_tex, &bottom_line_data, &bottom_line_mask, &bottom_line_dim);
                }
            }
            for(y = bottom;
                y >= next_y && y >= 0;
                y--) {
                if(bottom_line_dim == 0) {
                    color = 0x00;
                } else {
                    wz = ((y - h_2) * y_to_angle) * total_distance;
                    tx = (wx * line->texture_transform[1].xx) +
                         (wy * line->texture_transform[1].xy) +
                         (wz * line->texture_transform[1].xz) +
                         line->texture_bias[1].x;
                    ty = (wx * line->texture_transform[1].yx) +
                         (wy * line->texture_transform[1].yy) +
                         (wz * line->texture_transform[1].yz) +
                         line->texture_bias[1].y;
                    color = bottom_line_data[(ty & bottom_line_mask) << bottom_line_dim | (tx & bottom_line_mask)];
                }
                if(line->shade[1] & 0xFF00) {
                    color = ~color; /* invert bits */
                    color &= 0xDB; /* unset overflow bits */
                    color += line->shade[1] & 0xFF; /* add which should subtract when inverted back? */
                    color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                    color = ~color; /* invert back */
                } else {
                    color += line->shade[1]; /* add */
                    color |= ((color & 0x124) >> 1) | ((color & 0x124) >> 2); /* mask overflow bits over color bits */
                }
                pixels[y * pitch + x] = 0xC0 | ((color & 0xC0) >> 2) | ((color & 0x18) >> 1) | (color & 0x03); /* shift bits in */
            }
            bottom = y;

            /* column fully drawn */
            if(top >= bottom) {
                break;
            }

            pos.x = hit.x;
            pos.y = hit.y;
        }
    }
}

void engine_move(float x, float y) {
    Axis axis;
    float slope;
    Point pos;
    Point hit;
    Line *line;
    Sector *s;
    Sector *last_s;
    float diffx, diffy;
    diffx = x - v.pos.x;
    diffy = y - v.pos.y;

    /* similar to line_hit but rather than a view ray off in to "infinity", a line segment from current position and the difference position */
    if(fabs(diffx) > fabs(diffy)) {
        if(diffx > 0.0) {
            axis = AXIS_PX;
        } else {
            axis = AXIS_NX;
        }
        slope = diffy / diffx;
    } else {
        if(diffy > 0.0) {
            axis = AXIS_PY;
        } else {
            axis = AXIS_NY;
        }
        slope = diffx / diffy;
    }

    pos.x = v.pos.x;
    pos.y = v.pos.y;
    last_s = NULL;
    s = v.start;
    while(true) {
        line = scan_sector(s,
                           &pos, axis, slope,
                           last_s,
                           &hit);

        /* shouldn't happen, but in case the ray misses for some reason */
        if(line == NULL) {
            break;
        }

        /* check if the hit is further than traveled, if it is, a new sector won't be reached, so stop,
         * otherwise, continue to iterate */
        if(axis == AXIS_PX && hit.x > x) {
            break;
        } else if(axis == AXIS_NX && hit.x < x) {
            break;
        } else if(axis == AXIS_PY && hit.y > y) {
            break;
        } else if(axis == AXIS_NY && hit.y < y) {
            break;
        }

        /* if wall collided, don't continue to move
         * this isn't intended to be very interactive, so for now, don't bother with real collision
         * detection, just make sure the view can't leave where there're no sectors */
        if(line->sector == NULL) {
            return;
        }

        pos.x = hit.x;
        pos.y = hit.y;
        last_s = s;
        s = line->sector;
    }

    /* finally, update player position and sector */
    v.pos.x = x;
    v.pos.y = y;
    v.start = s;
    v.height = s->height[1] + VIEW_HEIGHT;
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
float tanf(float angle) {
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

/* TODO it seems fmodf crashes on the watch too but it's rare, could be a division by 0? */
#endif
