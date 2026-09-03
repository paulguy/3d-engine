#define SMALL_TEX_DIM (32)
#define SMALL_TEX_SIZE (SMALL_TEX_DIM * SMALL_TEX_DIM)
#define LARGE_TEX_DIM (SMALL_TEX_DIM * 2)
#define LARGE_TEX_SIZE (LARGE_TEX_DIM * LARGE_TEX_DIM)

#define TEXMEM (65536)
#define TEXSLOTS (TEXMEM / LARGE_TEX_DIM)

typedef int (* get_graphic_dim_t)(int number);
typedef int (* load_graphic_t)(int number, unsigned char *data);

extern get_graphic_dim_t get_graphic_dim_p;
extern load_graphic_t load_graphic_p;
extern unsigned char *texmem;

int load_tex(int number, unsigned char **data);
void age_slots();
