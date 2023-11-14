#pragma once

#include <nanosvg/nanosvg.h>
#include <pixman.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <tllist.h>

struct ref {
    void (*free)(const struct ref *);
    atomic_size_t count;
};

static inline void
ref_inc(const struct ref *ref)
{
    atomic_fetch_add((atomic_size_t *)&ref->count, 1);
}

static inline void
ref_dec(const struct ref *ref)
{
    if (atomic_fetch_sub((atomic_size_t *)&ref->count, 1) == 1) {
        ref->free(ref);
    }
}

struct icon_pixmap {
    int size;
    unsigned char pixels[];
};

typedef tll(struct icon_pixmap *) icon_pixmaps_t;

struct icon_pixmaps {
    struct ref refcount;
    icon_pixmaps_t list;
};

enum icon_type { ICON_NONE, ICON_PNG, ICON_SVG, ICON_PIXMAP };

struct icon {
    enum icon_type type;
    union {
        NSVGimage *svg;
        pixman_image_t *png;
        struct icon_pixmaps *pixmaps;
    };
};

enum icon_dir_type { ICON_DIR_FIXED, ICON_DIR_SCALABLE, ICON_DIR_THRESHOLD };

struct icon_theme_subdir {
    char *name;
    char *context;

    int size;
    int max_size;
    int min_size;
    int scale;
    int threshold;
    enum icon_dir_type type;
};

typedef tll(struct icon_theme *) themes_t;
typedef tll(char *) string_list_t;
typedef tll(struct icon_theme_subdir *) subdirs_t;

struct themes {
    struct ref refcount;
    themes_t themes;
};

struct basedirs {
    struct ref refcount;
    string_list_t basedirs;
};

struct string_list {
    struct ref refcount;
    string_list_t strings;
};

struct icon_theme {
    char *name;
    char *comment;
    string_list_t inherits;    // char *
    string_list_t directories; // char *

    char *dir;
    subdirs_t subdirs; // struct icon_theme_subdir *
};

bool dir_exists(char *path);

void icon_from_pixmaps(struct icon *icon, struct icon_pixmaps *p);
bool icon_from_png(struct icon *icon, const char *file_name);
bool icon_from_svg(struct icon *icon, const char *file_name);
void render_icon(const struct icon *icon, int x, int y, int size, pixman_image_t *dest);
void reset_icon(struct icon *icon);

struct string_list *string_list_new();
void string_list_dec(struct string_list *s);
struct string_list *string_list_inc(struct string_list *s);

struct icon_pixmaps *new_icon_pixmaps();
void icon_pixmaps_dec(struct icon_pixmaps *p);
struct icon_pixmaps *icon_pixmaps_inc(struct icon_pixmaps *ip);

struct basedirs *get_basedirs();
struct basedirs *basedirs_new();
void basedirs_dec(struct basedirs *p);
struct basedirs *basedirs_inc(struct basedirs *p);

struct themes *init_themes(struct basedirs *basedirs);
void themes_dec(struct themes *p);
struct themes *themes_inc(struct themes *p);

char *find_icon(themes_t themes, string_list_t basedirs, char *name, int size, string_list_t icon_themes, int *min_size,
                int *max_size);

void log_loaded_themes(themes_t themes);
