#include "dynlist.h"

#include <stdlib.h>
#include <assert.h>

#define LOG_MODULE "dynlist"
#include "../log.h"
#include "../particle.h"

struct private {
    int pre_spacing;
    int post_spacing;

    bool vertical;

    struct exposable **exposables;
    size_t count;
    int *widths;
    int *heights;
};

static void
dynlist_destroy(struct exposable *exposable)
{
    struct private *e = exposable->private;
    for (size_t i = 0; i < e->count; i++) {
        struct exposable *ee = e->exposables[i];
        ee->destroy(ee);
    }

    free(e->exposables);
    free(e->widths);
    free(e->heights);
    free(e);
    free(exposable);
}

static int
dynlist_begin_expose(struct exposable *exposable)
{
    const struct private *e = exposable->private;
    
    exposable->width = 0;
    exposable->height = 0;
    bool have_at_least_one = false;

    for (size_t i = 0; i < e->count; i++) {
        struct exposable *ee = e->exposables[i];
        ee->begin_expose(ee);
        e->widths[i] = ee->width;
        e->heights[i] = ee->height;

        assert(e->widths[i] >= 0 && e->heights[i] >= 0);

        if (e->widths[i] > 0 && e->heights[i] > 0) {
            if (e->vertical) {
                exposable->height += e->pre_spacing + e->heights[i] + e->post_spacing;
                if (e->widths[i] > exposable->width)
                    exposable->width = e->widths[i];
            } else {
                exposable->width += e->pre_spacing + e->widths[i] + e->post_spacing;
                if (e->heights[i] > exposable->height)
                    exposable->height = e->heights[i];
            }
            have_at_least_one = true;
        }
    }

    if (have_at_least_one) {
        if (e->vertical)
            exposable->height -= e->pre_spacing + e->post_spacing;
        else
            exposable->width -= e->pre_spacing + e->post_spacing;
    }
    else
        assert(exposable->width == 0 || exposable->height == 0);
    return exposable->width;
}

static void
dynlist_expose(const struct exposable *exposable, pixman_image_t *pix, int x, int y, int height)
{
    const struct private *e = exposable->private;

    int pre_spacing = e->pre_spacing;
    int post_spacing = e->post_spacing;

    if (e->vertical) {
        y -= pre_spacing;

        for (size_t i = 0; i < e->count; i++) {
            const struct exposable *ee = e->exposables[i];
            ee->expose(ee, pix, x, y + pre_spacing, height);
            y += pre_spacing + e->heights[i] + post_spacing;
        }
    } else {
        x -= pre_spacing;

        for (size_t i = 0; i < e->count; i++) {
            const struct exposable *ee = e->exposables[i];
            ee->expose(ee, pix, x + pre_spacing, y, height);
            x += pre_spacing + e->widths[i] + post_spacing;
        }
    }
}

static void
on_mouse(struct exposable *exposable, struct bar *bar,
         enum mouse_event event, enum mouse_button btn, int x, int y)
{
    const struct private *e = exposable->private;

    if (exposable->on_click[btn] != NULL) {
        exposable_default_on_mouse(exposable, bar, event, btn, x, y);
        return;
    }

    int px = /*p->left_margin;*/0;
    for (size_t i = 0; i < e->count; i++) {
        if (x >= px && x < px + e->exposables[i]->width) {
            if (e->exposables[i]->on_mouse != NULL) {
                e->exposables[i]->on_mouse(
                    e->exposables[i], bar, event, btn, x - px, y);
            }
            return;
        }

        px += e->pre_spacing + e->exposables[i]->width + e->post_spacing;
    }

    LOG_DBG("on_mouse missed all sub-particles");
    exposable_default_on_mouse(exposable, bar, event, btn, x, y);
}

struct exposable *
dynlist_exposable_new(struct exposable **exposables, size_t count, bool vertical,
                      int pre_spacing, int post_spacing)
{
    struct private *e = calloc(1, sizeof(*e));
    e->count = count;
    e->vertical = vertical;
    e->exposables = malloc(count * sizeof(e->exposables[0]));
    e->widths = calloc(count, sizeof(e->widths[0]));
    e->heights = calloc(count, sizeof(e->heights[0]));
    e->pre_spacing = pre_spacing;
    e->post_spacing = post_spacing;

    for (size_t i = 0; i < count; i++)
        e->exposables[i] = exposables[i];

    struct exposable *exposable = exposable_common_new(NULL, NULL);
    exposable->private = e;
    exposable->destroy = &dynlist_destroy;
    exposable->begin_expose = &dynlist_begin_expose;
    exposable->expose = &dynlist_expose;
    exposable->on_mouse = &on_mouse;
    return exposable;
}
