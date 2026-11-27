/* Mistakes in the game's data files, put right as the game loads them: shim_kernel32.c hands the
 * game a corrected copy, the file on disk stays as it is.
 *
 * DAT\ST0E.BIN, US version (issue #20): the stage's textures are TIM images, each with the place in
 * video memory it is copied to. The green Sharukurusu's (entry 11, palette at 0,242) is addressed
 * to 704,0, where the image of entry 13 then lands on top of it. Its polygons read the page at
 * 512,0, which nothing fills: the enemy is invisible but for its untextured parts, or wears what an
 * earlier area left there. The Japanese file holds the same image addressed to 512,0, the only one
 * of the two versions' stage images placed differently. */
#include <windows.h>   /* (_stricmp; compat/windows.h elsewhere) */
#include <string.h>

#include "datafix.h"
#include "log.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned rd16(const uint8_t *p)
{
    return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static int rect_is(const uint8_t *p, unsigned x, unsigned y, unsigned w, unsigned h)
{
    return rd16(p) == x && rd16(p + 2) == y && rd16(p + 4) == w && rd16(p + 6) == h;
}

/* A package entry holding a texture is "TM", 6 bytes, then a TIM: u32 0x10, u32 flags, the
   palette block (u32 its length, its rectangle, its colours) and the image block laid out alike.
   Moves entry k's image from (from_x, y) to (to_x, y) when it is the one described. */
static int move_image(uint8_t *d, size_t size, uint32_t k, unsigned clut_x, unsigned clut_y,
                      unsigned from_x, unsigned to_x, unsigned y, unsigned w, unsigned h)
{
    uint32_t count, from, to, clut_len, image_len = 12 + w * h * 2;
    uint8_t *e, *image;
    if (size < 8)
        return 0;
    count = rd32(d) / 4;   /* the entries' offsets, the last one the file's size */
    if (k + 1 >= count || (size_t)count * 4 > size)
        return 0;
    from = rd32(d + 4 * k);
    to = rd32(d + 4 * k + 4);
    if (from >= to || to > size || to - from < 28)
        return 0;
    e = d + from;
    if (e[0] != 'T' || e[1] != 'M' || rd32(e + 8) != 0x10 || !rect_is(e + 20, clut_x, clut_y, 16, 8))
        return 0;
    clut_len = rd32(e + 16);
    if (clut_len > to - from || 16 + (size_t)clut_len + image_len > to - from)
        return 0;
    image = e + 16 + clut_len;
    if (rd32(image) != image_len || !rect_is(image + 4, from_x, y, w, h))
        return 0;
    image[4] = (uint8_t)to_x;
    image[5] = (uint8_t)(to_x >> 8);
    return 1;
}

int datafix_wants(const char *path)
{
    const char *base = path + strlen(path);
    while (base > path && base[-1] != '\\' && base[-1] != '/')
        base--;
    return _stricmp(base, "ST0E.BIN") == 0;
}

int datafix_file(const char *path, uint8_t *d, size_t size)
{
    /* (the Japanese file has the image at 512 already: it doesn't match) */
    if (!datafix_wants(path) || !move_image(d, size, 11, 0, 242, 704, 512, 0, 64, 128))
        return 0;
    log_printf("datafix: %s: the green Sharukurusu's texture put where the enemy reads it", path);
    return 1;
}
