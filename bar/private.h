#pragma once

#include "../bar/bar.h"
#include "backend.h"

struct section {
        struct module **mods;
        struct exposable **exps;
        size_t count;
};

struct private {
    /* From bar_config */
    char *monitor;
    enum bar_layer layer;
    enum bar_location location;
    int height;
    int left_spacing, right_spacing;
    int left_margin, right_margin;
    int width;
    int top_spacing, bottom_spacing;
    int top_margin, bottom_margin;
    int trackpad_sensitivity;

    pixman_color_t background;

    struct {
        int left_width, right_width;
        int top_width, bottom_width;
        pixman_color_t color;
        int left_margin, right_margin;
        int top_margin, bottom_margin;
    } border;

    struct section left;
    struct section center;
    struct section right;

    /* Calculated run-time */
    int height_with_border;
    int width_with_border;

    pixman_image_t *pix;

    struct {
        void *data;
        const struct backend *iface;
    } backend;
};
