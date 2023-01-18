#pragma once

#include <stddef.h>
#include <stdbool.h>

struct particle;
struct exposable *dynlist_exposable_new(
    struct exposable **exposables, size_t count, bool vertical, int pre_spacing, int post_spacing);
