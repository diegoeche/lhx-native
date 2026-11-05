/* Original horizon measurements and pixel-art overlay for the native scene. */
#include "scene_composite.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

enum { ORIGINAL_WIDTH = 320, ORIGINAL_HEIGHT = 200 };

void lhx_scene_overlay(const LhxSceneFrame *frame,const uint8_t *vga,
                        const uint8_t palette[768],uint32_t pixels[64000])
{
    if (!vga || !palette || !pixels) return;
    uint32_t colors[256];
    for (unsigned i=0;i<256;i++)
        colors[i]=0xff000000u|((palette[i*3]&63)*255/63<<16)|
                  ((palette[i*3+1]&63)*255/63<<8)|((palette[i*3+2]&63)*255/63);
    for (unsigned i=0;i<64000;i++) pixels[i]=colors[vga[i]];
    if (!frame || !frame->views) return;
    for (unsigned i=0;i<frame->view_count;i++) {
        const LhxSceneView *v=frame->views+i;
        int x0=v->x0<0?0:v->x0,x1=v->x1>320?320:v->x1;
        int y0=v->y0<0?0:v->y0,y1=v->y1>200?200:v->y1;
        for (int y=y0;y<y1;y++) for (int x=x0;x<x1;x++)
            if (vga[y*320+x]==255) pixels[y*320+x]=0;
    }
}

typedef struct {
    double x, y, xx, yy, xy;
    unsigned count, horizontal, vertical;
} Boundary;

static void boundary_point(Boundary *b, double x, double y, int horizontal)
{
    b->x += x;
    b->y += y;
    b->xx += x * x;
    b->yy += y * y;
    b->xy += x * y;
    b->count++;
    b->horizontal += horizontal != 0;
    b->vertical += horizontal == 0;
}

void lhx_scene_read_horizon(CPU *cpu, LhxSceneView *view)
{
    if (!cpu || !cpu->mem || !view)
        return;
    memset(&view->horizon, 0, sizeof(view->horizon));
    unsigned x0 = mem_read16(cpu, cpu->ds, 0xeb5a);
    unsigned width = (unsigned)mem_read16(cpu, cpu->ds, 0xeb58) * 2;
    unsigned y0 = mem_read16(cpu, cpu->ds, 0xeed1);
    unsigned bottom = mem_read16(cpu, cpu->ds, 0xeed3);
    if (!width || x0 >= ORIGINAL_WIDTH || y0 >= ORIGINAL_HEIGHT || bottom < y0)
        return;
    if (width > ORIGINAL_WIDTH - x0)
        width = ORIGINAL_WIDTH - x0;
    if (bottom >= ORIGINAL_HEIGHT)
        bottom = ORIGINAL_HEIGHT - 1;
    unsigned height = bottom - y0 + 1;
    uint16_t segment = mem_read16(cpu, cpu->ds, 0xecfe);
    uint8_t pixels[ORIGINAL_WIDTH * ORIGINAL_HEIGHT];
    unsigned counts[256] = {0};
    for (unsigned y = 0; y < height; y++) {
        uint16_t row = mem_read16(cpu, cpu->ds, (uint16_t)(0xeb6e + 2 * (y0 + y)));
        for (unsigned x = 0; x < width; x++) {
            unsigned color = mem_read8(cpu, segment, (uint16_t)(row + x0 + x));
            pixels[y * width + x] = (uint8_t)color;
            counts[color]++;
        }
    }
    unsigned a = 0, b = 0;
    for (unsigned color = 1; color < 256; color++)
        if (counts[color] > counts[a])
            a = color;
    b = a == 0 ? 1 : 0;
    for (unsigned color = 0; color < 256; color++)
        if (color != a && counts[color] > counts[b])
            b = color;
    LhxSceneHorizon *horizon = &view->horizon;
    horizon->colors[0] = horizon->colors[1] = a;
    if (!counts[b])
        return;

    /* Adjacent pixel centers straddle the rasterized horizon. Fit its line
     * through their edge midpoints, ignoring any less frequent third colors.
     * Keep narrow ground/sky slivers: they can grow at a higher resolution. */
    Boundary edge = {0};
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            unsigned p = pixels[y * width + x];
            if (p != a && p != b)
                continue;
            if (x + 1 < width) {
                unsigned q = pixels[y * width + x + 1];
                if ((q == a || q == b) && q != p)
                    boundary_point(&edge, x + 1.0, y + 0.5, 1);
            }
            if (y + 1 < height) {
                unsigned q = pixels[(y + 1) * width + x];
                if ((q == a || q == b) && q != p)
                    boundary_point(&edge, x + 0.5, y + 1.0, 0);
            }
        }
    }
    if (!edge.count)
        return;
    double cx = edge.x / edge.count, cy = edge.y / edge.count;
    double nx, ny;
    if (edge.count == 1) {
        nx = edge.horizontal ? 1.0 : 0.0;
        ny = edge.horizontal ? 0.0 : 1.0;
    } else {
        double xx = edge.xx - edge.x * cx, yy = edge.yy - edge.y * cy;
        double xy = edge.xy - edge.x * cy;
        double angle = 0.5 * atan2(2.0 * xy, xx - yy);
        nx = -sin(angle);
        ny = cos(angle);
    }
    /* Orient from every color sample, rather than a few samples that can fall
     * outside the window when a rolled horizon clips one corner. */
    int score = 0;
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            unsigned color = pixels[y * width + x];
            if (color != a && color != b)
                continue;
            double side = (x + 0.5 - cx) * nx + (y + 0.5 - cy) * ny;
            if (side != 0)
                score += ((side > 0) == (color == a)) ? 1 : -1;
        }
    }
    horizon->two_colors = 1;
    horizon->colors[0] = score >= 0 ? a : b;
    horizon->colors[1] = score >= 0 ? b : a;
    horizon->x = (float)(x0 + cx);
    horizon->y = (float)(y0 + cy);
    horizon->nx = (float)nx;
    horizon->ny = (float)ny;
}

void lhx_scene_composite(const LhxSceneFrame *frame, const uint8_t *vga,
                         const uint8_t palette[768], uint32_t *pixels,
                         unsigned width, unsigned height)
{
    if (!vga || !palette || !pixels || !width || !height ||
        (size_t)height > SIZE_MAX / width)
        return;
    uint8_t holes[ORIGINAL_WIDTH * ORIGINAL_HEIGHT] = {0};
    if (frame && frame->views) {
        for (unsigned i = 0; i < frame->view_count; i++) {
            const LhxSceneView *view = &frame->views[i];
            int x0 = view->x0 < 0 ? 0 : view->x0;
            int y0 = view->y0 < 0 ? 0 : view->y0;
            int x1 = view->x1 > ORIGINAL_WIDTH ? ORIGINAL_WIDTH : view->x1;
            int y1 = view->y1 > ORIGINAL_HEIGHT ? ORIGINAL_HEIGHT : view->y1;
            if (x0 >= x1 || y0 >= y1)
                continue;
            for (int y = y0; y < y1; y++)
                memset(holes + y * ORIGINAL_WIDTH + x0, 1, (size_t)(x1 - x0));
        }
    }
    uint32_t colors[256];
    for (unsigned color = 0; color < 256; color++) {
        unsigned p = color * 3;
        colors[color] = 0xff000000u | ((unsigned)(palette[p] & 63) * 255 / 63 << 16) |
                        ((unsigned)(palette[p + 1] & 63) * 255 / 63 << 8) |
                        ((unsigned)(palette[p + 2] & 63) * 255 / 63);
    }
    /* Invert floor(x * 320 / width): source column s covers output columns
     * [ceil(s * width / 320), ceil((s + 1) * width / 320)). Do the same for
     * rows. This preserves nearest-neighbor sampling even at fractional scales
     * and when downsampling, without a division/mask lookup per output pixel.
     */
    unsigned edges[ORIGINAL_WIDTH + 1];
    for (unsigned x = 0; x <= ORIGINAL_WIDTH; x++)
        edges[x] = (unsigned)(((uint64_t)x * width + ORIGINAL_WIDTH - 1) /
                               ORIGINAL_WIDTH);
    for (unsigned sy = 0; sy < ORIGINAL_HEIGHT; sy++) {
        unsigned y0 = (unsigned)(((uint64_t)sy * height + ORIGINAL_HEIGHT - 1) /
                                  ORIGINAL_HEIGHT);
        unsigned y1 = (unsigned)(((uint64_t)(sy + 1) * height + ORIGINAL_HEIGHT - 1) /
                                  ORIGINAL_HEIGHT);
        if (y0 == y1)
            continue;
        unsigned row = sy * ORIGINAL_WIDTH;
        for (unsigned sx = 0; sx < ORIGINAL_WIDTH;) {
            unsigned color = vga[row + sx];
            if (color == 255 && holes[row + sx]) {
                sx++;
                continue;
            }
            unsigned end = sx + 1;
            while (end < ORIGINAL_WIDTH && vga[row + end] == color &&
                   (color != 255 || !holes[row + end]))
                end++;
            /* Copy only opaque runs; copying a whole expanded row would also
             * overwrite native geometry in the transparent portions below it.
             */
            for (unsigned y = y0; y < y1; y++) {
                uint32_t *target = pixels + (size_t)y * width;
                for (unsigned x = edges[sx]; x < edges[end]; x++)
                    target[x] = colors[color];
            }
            sx = end;
        }
    }
}
