#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "particles/icon"
#define LOG_ENABLE_DBG 0
#include "../config-verify.h"
#include "../config.h"
#include "../icon.h"
#include "../log.h"
#include "../particle.h"
#include "../plugin.h"
#include "../tag.h"

struct private
{
    char *text;
    bool use_tag;
    struct particle *fallback;
};

struct eprivate {
    struct exposable *exposable;
    struct icon icon;
    pixman_image_t *rasterized;
};

static void
exposable_destroy(struct exposable *exposable)
{
    struct eprivate *e = exposable->private;
    if (e->exposable)
        e->exposable->destroy(e->exposable);

    reset_icon(&e->icon);
    free(e);
    exposable_default_destroy(exposable);
}

static int
begin_expose(struct exposable *exposable)
{
    struct eprivate *e = exposable->private;

    if (e->icon.type == ICON_NONE) {
        if (e->exposable) {
            exposable->width = e->exposable->begin_expose(e->exposable);
        } else {
            exposable->width = 0;
        }
    } else {
        assert(e->exposable == NULL);
        exposable->width = exposable->particle->right_margin + exposable->particle->left_margin;
        exposable->width += exposable->particle->icon_size;
    }

    return exposable->width;
}

static void
expose(const struct exposable *exposable, pixman_image_t *pix, int x, int y, int height)
{
    exposable_render_deco(exposable, pix, x, y, height);

    const struct eprivate *e = exposable->private;
    const struct particle *p = exposable->particle;

    int target_size = p->icon_size;
    double baseline = (double)y + (double)(height - target_size) / 2.0;

    if (e->exposable != NULL) {
        assert(e->icon.type == ICON_NONE);
        e->exposable->expose(e->exposable, pix, x, y, height);
    } else {
        render_icon(&e->icon, x, baseline, target_size, pix);
    }
}

static struct exposable *
instantiate(const struct particle *particle, const struct tag_set *tags)
{
    struct private *p = (struct private *)particle->private;
    struct eprivate *e = calloc(1, sizeof(*e));

    char *name = tags_expand_template(p->text, tags);
    e->icon.type = ICON_NONE;
    e->exposable = NULL;
    char *icon_path = NULL;
    if (strlen(name) == 0) {
        LOG_WARN("No icon name/tag available");
        goto fallback;
    }

    // TODO: An icon cache
    if (p->use_tag) {
        const struct icon_tag *tag = icon_tag_for_name(tags, name);

        if (!tag) {
            LOG_WARN("No icon tag for %s", name);
            goto fallback;
        }

        struct icon_pixmaps *pixmaps = tag->pixmaps(tag);
        icon_from_pixmaps(&e->icon, pixmaps);
        goto out;
    }

    int min_size = 0, max_size = 0;
    icon_path = find_icon(particle->themes->themes, particle->basedirs->basedirs, name, particle->icon_size,
                          particle->icon_themes->strings, &min_size, &max_size);
    if (icon_path) {
        int len = strlen(icon_path);
        if ((icon_path[len - 3] == 's' && icon_from_svg(&e->icon, icon_path))
            || (icon_path[len - 3] == 'p' && icon_from_png(&e->icon, icon_path))) {
            LOG_DBG("Loaded %s", icon_path);
            goto out;
        }

        LOG_WARN("Failed to load icon path, not png or svg: %s", icon_path);
        goto fallback;
    }
    LOG_WARN("Icon not found: %s", name);

fallback:
    if (p->fallback) {
        e->exposable = p->fallback->instantiate(p->fallback, tags);
        assert(e->exposable != NULL);
    }

out:
    free(icon_path);
    free(name);

    struct exposable *exposable = exposable_common_new(particle, tags);
    exposable->private = e;
    exposable->destroy = &exposable_destroy;
    exposable->begin_expose = &begin_expose;
    exposable->expose = &expose;
    return exposable;
}

static void
particle_destroy(struct particle *particle)
{
    struct private *p = particle->private;
    if (p->fallback)
        p->fallback->destroy(p->fallback);
    free(p->text);
    free(p);
    particle_default_destroy(particle);
}

static struct particle *
icon_new(struct particle *common, const char *name, bool use_tag, struct particle *fallback)
{
    struct private *p = calloc(1, sizeof(*p));
    p->text = strdup(name);
    p->use_tag = use_tag;
    p->fallback = fallback;

    common->private = p;
    common->destroy = &particle_destroy;
    common->instantiate = &instantiate;
    return common;
}

static struct particle *
from_conf(const struct yml_node *node, struct particle *common)
{
    const struct yml_node *name = yml_get_value(node, "name");
    const struct yml_node *use_tag_node = yml_get_value(node, "use-tag");
    const struct yml_node *_fallback = yml_get_value(node, "fallback");

    struct particle *fallback = NULL;
    bool use_tag = false;

    if (use_tag_node)
        use_tag = yml_value_as_bool(use_tag_node);
    if (_fallback)
        fallback = conf_to_particle(_fallback, (struct conf_inherit){common->font, common->font_shaping, common->themes,
                                                                     common->basedirs, common->icon_themes,
                                                                     common->icon_size, common->foreground});

    return icon_new(common, yml_value_as_string(name), use_tag, fallback);
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        {"name", true, &conf_verify_string},
        {"use-tag", false, &conf_verify_bool},
        {"fallback", false, &conf_verify_particle},
        PARTICLE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct particle_iface particle_icon_iface = {
    .verify_conf = &verify_conf,
    .from_conf = &from_conf,
};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct particle_iface iface __attribute__((weak, alias("particle_icon_iface")));
#endif
