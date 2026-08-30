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
    {0, 1, {0, 0}},
    {1, 99999, {0, 0}},
    {2, 99999, {0, 0}},
    {3, 99999, {0, 0}},
    {4, 99999, {0, 0}},
    {5, 99999, {0, 0}},
    {6, 99999, {0, 0}},
    {7, 99999, {0, 0}},

    {8, 99999, {0, 0}},
    {9, 99999, {0, 0}},
    {1, 0, {0, 0}},
    {0, 99999, {0, 0}}
};
Sector sdata[] = {
    {&(ldata[0]), 8, -50, 50, {0, 0}},
    {&(ldata[8]), 4, -60, 40, {0, 0}}
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
        logp("Sector %d\n", i);
        for(j = 0; j < (*s)[i].numlines; j++) {
            logp("%f, %f  ", (*(*s)[i].line)[j].point->x, (*(*s)[i].line)[j].point->y);
        }
        logp("\n");
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

    print_sectors();

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
}

void fill_slope(float *points, int num, float fov) {
    float edge = sin(fov / 2.0);
    int i;

    for(i = 0; i < num / 2; i++) {
        points[i] = edge / num / 2 * i;
        points[num - 1 - i] = points[i];
    }
}

Axis find_slope(float angle, float *slope) {
    /* find rise and run of a ray firing from the view such that each unit of slope 
     * given any offset plots a line perpendicular to the view center angle */


    logp("%f %f\n", angle, *slope);
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
                logp("X No %f %f\n", lslope, x);
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
            logp("X %f %f %f\n", lslope, x, y);
            hit->x = x;
            hit->y = y;
            return(1);
        }
        logp("X No %f %f %f\n", lslope, x, y);
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
                logp("Y No %f %f\n", lslope, y);
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
            logp("Y %f %f %f\n", lslope, x, y);
            hit->x = x;
            hit->y = y;
            return(1);
        }
        logp("Y No %f %f %f\n", lslope, x, y);
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
        logp("%d ", i);
        line = &(*s->line)[i];

        /* current line spans from the current point to the next line point */
        nextpoint = (*s->line)[(i + 1) % s->numlines].point;

        /* don't check for lines that would look back towards the previous sector */
        if(last_s != NULL && line->sector == last_s) {
            logp("Looks back in to previous sector.\n");
            /* make sure the point used in the next iteration is updated */
            point = nextpoint;
            continue;
        }

        logp("%f %f %f %f ", point->x, point->y, nextpoint->x, nextpoint->y);
        if(line_hit(point, nextpoint,
                    pos, axis, slope,
                    hit)) {
            return line;
        }

        point = nextpoint;
    }

    return NULL;
}

float get_distance(Axis axis, Point *pos, Point *hit) {
    if(axis == AXIS_NX || axis == AXIS_PX) {
        return(sqrtf(fabs(powf(hit->x - pos->x, 2.0)) + fabs(powf(hit->y - pos->y, 2.0))));
    }

    return(sqrtf(fabs(powf(hit->y - pos->y, 2.0)) + fabs(powf(hit->x - pos->x, 2.0))));
}

void engine_render(char *pixels, int w, int h, int pitch) {
    /* these might end up needing to be heap-allocated */
    float w_slope[w];
    float h_slope[h];

    int i, j;
    Sector *s, *last_s;
    int lastaxis = 0;
    float angle;
    int axis;
    float slope;
    Line *line;
    Point hit;
    float distance;
    float total_distance;
    int y;
    int top, bottom;
    Point pos;

    float edge = v.angle - (v.fov / 2.0);
    float step = v.fov / (float)w;
    logp("%f %f %f %f %d %f %f\n", v.pos.x, v.pos.y, v.angle, v.fov, w, edge, step);

/*
    fill_delta(&w_delta, w, v.fov);
    fill_delta(&h_delta, h, v.fov);
*/

    /* for each column */
    for(i = 0; i < w; i++) {
        angle = fmodf(edge + (step * (float)i), M_PI * 2.0);
        if(angle < 0.0) {
            angle += M_PI * 2.0;
        }

        axis = find_slope(angle, &slope);

        s = v.start;
        last_s = NULL;
        top = -1;
        bottom = h;
        pos.x = v.pos.x;
        pos.y = v.pos.y;
        total_distance = 0.0;
        while (1) {
            /* TODO: scan in to portal walls until solid wall or distance reached */
            line = scan_sector(s,
                               &pos, axis, slope,
                               last_s,
                               &hit);

            /* shouldn't happen, but in case the ray misses for some reason */
            if(line == NULL) {
                logp("wall missed!\n");
                break;
            }

            switch(axis) {
                case AXIS_NX:
                    logp("-X ");
                    break;
                case AXIS_PX:
                    logp("+X ");
                    break;
                case AXIS_NY:
                    logp("-Y ");
                    break;
                case AXIS_PY:
                    logp("+Y ");
                    break;
            }

            /* get the distance between the view and hit coordinates */
            distance = get_distance(axis, &pos, &hit);

            logp("%f %f %d %f\n", hit.x, hit.y, i, distance);

            /* fisheye compensation, this kinda doesn't work 100% but whatever? */
            distance *= cos(-(v.fov / 2.0) + (step * (float)i));

            total_distance += distance;
            /* visualize floor edge */
            y = atan2f(total_distance, s->floor_h - v.height) / (v.fov / 2.0) * ((float)h / 2.0) - ((float)h / 2.0);
            if(y < bottom) {
                if(y >= 0 && y < h) {
                    pixels[y * pitch + i] = 0xFF;
                }
                bottom = y;
            }

            /* visualize ceiling edge */
            y = atan2f(total_distance, s->ceiling_h - v.height) / (v.fov / 2.0) * ((float)h / 2.0) - ((float)h / 2.0);
            if(y > top) {
                if(y >= 0 && y < h) {
                    pixels[y * pitch + i] = 0xFF;
                }
                top = y;
            }

            if(line->sector == NULL) {
                break;
            }
            last_s = s;
            s = line->sector;
            logp("Found a sector\n");

            /* visualize next sector floor edge */
            y = atan2f(total_distance, s->floor_h - v.height) / (v.fov / 2.0) * ((float)h / 2.0) - ((float)h / 2.0);
            if(y < bottom) {
                if(y >= 0 && y < h) {
                    pixels[y * pitch + i] = 0xFF;
                }
                bottom = y;
            }

            /* visualize next sector ceiling edge */
            y = atan2f(total_distance, s->ceiling_h - v.height) / (v.fov / 2.0) * ((float)h / 2.0) - ((float)h / 2.0);
            if(y > top) {
                if(y >= 0 && y < h) {
                    pixels[y * pitch + i] = 0xFF;
                }
                top = y;
            }

            /* column fully drawn */
            if(top >= bottom) {
                logp("Column filled up %d %d\n", top, bottom);
                break;
            }

            pos.x = hit.x;
            pos.y = hit.y;
        }

        /*
        if(axis != lastaxis) {
            for(j = 0; j < 40; j++) {
                pixels[j * pitch + i] = 0xFF;
                lastaxis = axis;
            }
        }
        */
    }
}

void engine_move(float x, float y) {
    Axis axis;
    float slope;
    Point pos;
    Point hit;
    float distance;
    Line *line;
    Sector *s;
    Sector *last_s;

    /* similar to line_hit but rather than a view ray off in to "infinity", a line segment from current position and the difference position */
    if(fabs(x) > fabs(y)) {
        if(x > 0.0) {
            axis = AXIS_PX;
        } else {
            axis = AXIS_NX;
        }
        slope = y / x;
    } else {
        if(y > 0.0) {
            axis = AXIS_PY;
        } else {
            axis = AXIS_NY;
        }
        slope = x / y;
    }

    /* the difference isn't needed anymore */
    x += v.pos.x;
    y += v.pos.y;

    switch(axis) {
        case AXIS_NX:
            logp("-X ");
            break;
        case AXIS_PX:
            logp("+X ");
            break;
        case AXIS_NY:
            logp("-Y ");
            break;
        case AXIS_PY:
            logp("+Y ");
            break;
    }
    logp("%f %f %f %f %f\n", v.pos.x, v.pos.y, x, y, slope);

    pos.x = v.pos.x;
    pos.y = v.pos.y;
    last_s = NULL;
    s = v.start;
    while(true) {
        line = scan_sector(s,
                           &pos, axis, slope,
                           last_s,
                           &hit);
        logp("\n%f %f\n", hit.x, hit.y);

        /* shouldn't happen, but in case the ray misses for some reason */
        if(line == NULL) {
            logp("wall missed!\n");
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
        logp("left sector.\n");

        /* if wall collided, don't continue to move
         * this isn't intended to be very interactive, so for now, don't bother with real collision
         * detection, just make sure the view can't leave where there're no sectors */
        if(line->sector == NULL) {
            logp("Hit wall.\n");
            return;
        }
        logp("Entered new sector.\n");

        pos.x = hit.x;
        pos.y = hit.y;
        last_s = s;
        s = line->sector;
    }

    /* finally, update player position and sector */
    v.pos.x = x;
    v.pos.y = y;
    v.start = s;
    v.height = s->floor_h + VIEW_HEIGHT;
}
