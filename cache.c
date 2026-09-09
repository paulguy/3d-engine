#include "engine.h"
#include "log.h"
#include "cache.h"

/* dimensions are small and only 256 resources are available anyway so use unsigned chars */
typedef struct {
    unsigned char dim;
    union {
        unsigned int f_number;
        unsigned char l_number;
        unsigned char s_number[4];
    };
    union {
        unsigned int f_age;
        unsigned char l_age;
        unsigned char s_age[4];
    };
} TexSlot;

get_graphic_dim_t get_graphic_dim_p;
load_graphic_t load_graphic_p;

/* allocated elsewhere! */
unsigned char *texmem;

TexSlot texslot[TEXSLOTS] = {0};

int load_in_empty_slot(unsigned char number, unsigned char dim, unsigned char **data) {
    int i, j;

    if(dim == LARGE_TEX_DIM) {
        for(i = 0; i < TEXSLOTS; i++) {
            if(texslot[i].dim == 0) {
                /* request number - 1 since real textures start at 1, but this wants textures starting at 0 */
                if(load_graphic_p(number - 1, &(texmem[i * LARGE_TEX_SIZE])) != 0) {
                    return(-1);
                }
                *data = &(texmem[i * LARGE_TEX_SIZE]);
                texslot[i].dim = LARGE_TEX_DIM;
                texslot[i].l_number = number;
                texslot[i].l_age = 0;
                LOG("Loaded large texture %d.\n", number - 1);
                return(0);
            }
        }
    } else if(dim == SMALL_TEX_DIM) {
        /* try to fill in an existing small slot first */
        for(i = 0; i < TEXSLOTS; i++) {
            if(texslot[i].dim == SMALL_TEX_DIM) {
                for(j = 0; j < 4; j++) {
                    if(texslot[i].s_number[j] == 0) {
                        if(load_graphic_p(number - 1, &(texmem[i * LARGE_TEX_SIZE + (j * SMALL_TEX_SIZE)])) != 0) {
                            return(-1);
                        }
                        *data = &(texmem[i * LARGE_TEX_SIZE + (j * SMALL_TEX_SIZE)]);
                        texslot[i].s_number[j] = number;
                        texslot[i].s_age[j] = 0;
                        LOG("Loaded small texture %d.\n", number - 1);
                        return(0);
                    }
                }
            }
        }

        /* try to create a new small slot */
        for(i = 0; i < TEXSLOTS; i++) {
            if(texslot[i].dim == 0) {
                if(load_graphic_p(number - 1, &(texmem[i * LARGE_TEX_SIZE])) != 0) {
                    return(-1);
                }
                *data = &(texmem[i * LARGE_TEX_SIZE]);
                texslot[i].dim = SMALL_TEX_DIM;
                /* zero out slots */
                texslot[i].f_number = 0;
                texslot[i].f_age = 0;
                texslot[i].s_number[0] = number;
                texslot[i].s_age[0] = 0;
                LOG("Loaded small texture %d.\n", number - 1);
                return(0);
            }
        }
    }
 
    LOG("Ran out of texture slots!\n");
    /* no free slots */
    return(-1);
}

int free_slot(unsigned char dim) {
    /* search slots for oldest occupied slot to free
     *
     * if it's a large tex, find the oldest large tex or small tex slot by the oldest of that slot
     * this might reload a recently loaded small tex, but it also prevents a stale small tex from
     * never being freed
     * if it's a small tex, find any oldest tex
     */
    int i, j;
    int oldest = 0;
    int oldest2 = 0;
    unsigned char oldest_age = 0;

    for(i = 0; i < TEXSLOTS; i++) {
        if(texslot[i].dim == LARGE_TEX_DIM) {
            if(texslot[i].l_age > oldest_age) {
                oldest = i;
                oldest2 = 0;
                oldest_age = texslot[i].l_age;
            }
        } else if(texslot[i].dim == SMALL_TEX_DIM) {
            if(dim == SMALL_TEX_DIM) {
                for(j = 0; j < 4; j++) {
                    if(texslot[i].s_age[j] > oldest_age) {
                        oldest = i;
                        oldest2 = j;
                        oldest_age = texslot[i].s_age[j];
                    }
                }
            } else { /* dim == LARGE_TEX_DIM */
                /* find the oldest in this slot */
                for(j = 0; j < 4; j++) {
                    if(texslot[i].s_age[j] < oldest_age) {
                        oldest = i;
                        oldest_age = texslot[i].s_age[j];
                    }
                }
            }
        }
    }

    /* return selected entry */
    if(dim == SMALL_TEX_DIM) {
        if(texslot[oldest].dim == SMALL_TEX_DIM) {
            LOG("Freed small slot %d %d for small.\n", oldest, oldest2);
            return(oldest * 4 + oldest2);
        } else { /* LARGE_TEX_DIM */
            LOG("Freed small slots %d for large.\n", oldest);
            texslot[oldest].dim = SMALL_TEX_DIM;
            /* zero out slots */
            texslot[oldest].f_number = 0;
            texslot[oldest].f_age = 0;
            return(oldest * 4);
        }
    } else {
        if(texslot[oldest].dim == SMALL_TEX_DIM) {
            LOG("Freed small slot %d for large.\n", oldest);
            texslot[oldest].dim = LARGE_TEX_DIM;
            return(oldest);
        } else { /* LARGE_TEX_DIM */
            LOG("Freed large slot %d for large.\n", oldest);
            return(oldest);
        }
    }
}

void age_slots() {
    unsigned char i, j;
    for(i = 0; i < TEXSLOTS; i++) {
        if(texslot[i].dim == LARGE_TEX_DIM) {
            if(texslot[i].l_age < 255) {
                texslot[i].l_age++;
            }
        } else if(texslot[i].dim == SMALL_TEX_DIM) {
            for(j = 0; j < 4; j++) {
                if(texslot[i].s_age[i] < 255) {
                    texslot[i].s_age[i]++;
                }
            }
        }
    }
}

int load_tex(unsigned char number, unsigned char **data) {
    unsigned char i, j;
    unsigned char dim;
    unsigned char found = 0;

    /* make 0 be no texture since it's used for empty slots */
    if(number == 0) {
        return(0);
    }

    /* search for already loaded textures */
    for(i = 0; i < TEXSLOTS; i++) {
        if(texslot[i].dim == LARGE_TEX_DIM) {
            if(texslot[i].l_number == number) {
                *data = &(texmem[i * LARGE_TEX_SIZE]);
                found = LARGE_TEX_DIM;
                /* reset age */
                texslot[i].l_age = 0;
                /* these cause a LOT of log spam
                LOG("Found in large slot %d\n", i);
                */
                break;
            }
        } else if(texslot[i].dim == SMALL_TEX_DIM) {
            for(j = 0; j < 4; j++) {
                if(texslot[i].s_number[i] == number) {
                    *data = &(texmem[i * LARGE_TEX_SIZE + (j * SMALL_TEX_SIZE)]);
                    found = SMALL_TEX_DIM;
                    texslot[i].s_age[i] = 0;
                    /*
                    LOG("Found in small slot %d %d\n", i, j);
                    */
                    break;
                }
            }
        }
    }

    /* texture was loaded, return its dimension */
    if(found != 0) {
        return(found);
    }

    /* get graphic dim to not have to repeat lookup later */
    dim = get_graphic_dim_p(number - 1);
    if(dim == 0) {
        LOG("Texture %d doesn't exist.\n", number - 1);
        return(0);
    } else if(dim != SMALL_TEX_DIM && dim != LARGE_TEX_DIM) {
        LOG("Texture dimensions must be either %dpx or %dpx!\n", SMALL_TEX_DIM, LARGE_TEX_DIM);
        return(0);
    }

    /* try to load it in to an empty slot */
    if(load_in_empty_slot(number, dim, data) != 0) {
        /* failed to find an open slot, free a texture and load it in there */
        found = free_slot(dim);
        if(dim == SMALL_TEX_DIM) {
            if(load_graphic_p(number - 1, &(texmem[found * SMALL_TEX_SIZE])) != 0) {
                LOG("Failed to load texture %d\n", number - 1);
                return(0);
            }
            texslot[found / 4].s_number[found % 4] = number;
            texslot[found / 4].s_age[found % 4] = 0;
            *data = &(texmem[found * SMALL_TEX_SIZE]);
        } else { /* LARGE_TEX_DIM */
            if(load_graphic_p(number - 1, &(texmem[found * LARGE_TEX_SIZE])) != 0) {
                LOG("Failed to load texture %d\n", number - 1);
                return(0);
            }
            texslot[found].l_number = number + 1;
            texslot[found].l_age = 0;
            *data = &(texmem[found * LARGE_TEX_SIZE]);
        }
    }

    return(dim);
}
