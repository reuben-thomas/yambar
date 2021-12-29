#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define LOG_MODULE "temperature"
#define LOG_ENABLE_DBG 0
#define SMALLEST_INTERVAL 500
#include "../bar/bar.h"
#include "../config-verify.h"
#include "../config.h"
#include "../log.h"
#include "../plugin.h"

enum temp_unit { TEMP_UNIT_INVALID, TEMP_UNIT_CELSIUS, TEMP_UNIT_FAHRENHEIT };

struct private
{
    struct particle *label;
    uint16_t interval;
    uint16_t thermal_zone;
    enum temp_unit unit;
};

static void
destroy(struct module *mod)
{
    struct private *m = mod->private;
    m->label->destroy(m->label);
    free(m);
    module_default_destroy(mod);
}

static const char *
description(struct module *mod)
{
    return "temperature";
}

static bool
get_temperature(uint16_t thermal_zone, enum temp_unit unit, double *temp)
{
    FILE *fp = NULL;
    char *line = NULL;
    size_t len = 0;
    int32_t read_temp = 0;
    bool res = false;

    ssize_t filename_len = snprintf(NULL, 0, "/sys/class/thermal/thermal_zone%i/temp", thermal_zone);
    char *filename = malloc(filename_len + 1);
    snprintf(filename, filename_len + 1, "/sys/class/thermal/thermal_zone%i/temp", thermal_zone);

    fp = fopen(filename, "r");
    if (NULL == fp) {
        LOG_ERRNO("unable to open /sys/class/thermal/thermal_zone%i/temp", thermal_zone);
        goto exit;
    }

    if (-1 == getline(&line, &len, fp)) {
        LOG_ERRNO("unable to get temperature for thermal zone %i", thermal_zone);
        goto exit;
    }

    if (1 != sscanf(line, "%" SCNi32, &read_temp)) {
        LOG_ERRNO("unable to get temperature for thermal zone %i", thermal_zone);
        goto exit;
    }

    *temp = read_temp / 1000;
    if (TEMP_UNIT_FAHRENHEIT == unit) {
        *temp = (*temp * (9.0 / 5.0)) + 32;
    }
    res = true;

exit:
    free(line);
    fclose(fp);

    free(filename);
    return res;
}

static struct exposable *
content(struct module *mod)
{
    const struct private *p = mod->private;
    double temperature = 0;

    if (!get_temperature(p->thermal_zone, p->unit, &temperature)) {
        LOG_ERR("unable to retrieve the temperature");
    }

    struct tag_set tags = {
        .tags = (struct tag *[]){tag_new_int(mod, "temperature", round(temperature))},
        .count = 1,
    };

    struct exposable *exposable = p->label->instantiate(p->label, &tags);
    tag_set_destroy(&tags);
    return exposable;
}

static int
run(struct module *mod)
{
    const struct bar *bar = mod->bar;
    bar->refresh(bar);
    struct private *p = mod->private;
    while (true) {
        struct pollfd fds[] = {{.fd = mod->abort_fd, .events = POLLIN}};

        int res = poll(fds, 1, p->interval);

        if (res < 0) {
            if (EINTR == errno)
                continue;
            LOG_ERRNO("unable to poll abort fd");
            return -1;
        }

        if (fds[0].revents & POLLIN)
            break;

        bar->refresh(bar);
    }

    return 0;
}

static enum temp_unit
str_to_unit(const char *unit_str)
{
    if (0 == strcasecmp(unit_str, "C") || 0 == strcasecmp(unit_str, "Celsius")) {
        return TEMP_UNIT_CELSIUS;
    }

    if (0 == strcasecmp(unit_str, "F") || 0 == strcasecmp(unit_str, "Fahrenheit")) {
        return TEMP_UNIT_FAHRENHEIT;
    }

    return TEMP_UNIT_INVALID;
}

static struct module *
temp_new(uint16_t interval, uint16_t thermal_zone, enum temp_unit unit, struct particle *label)
{
    struct private *p = calloc(1, sizeof(*p));
    p->label = label;
    p->interval = interval;
    p->unit = unit;

    struct module *mod = module_common_new();
    mod->private = p;
    mod->run = &run;
    mod->destroy = &destroy;
    mod->content = &content;
    mod->description = &description;
    return mod;
}

static struct module *
from_conf(const struct yml_node *node, struct conf_inherit inherited)
{
    const struct yml_node *interval = yml_get_value(node, "interval");
    const struct yml_node *unit = yml_get_value(node, "unit");
    const struct yml_node *thermal_zone = yml_get_value(node, "thermal_zone");
    const struct yml_node *c = yml_get_value(node, "content");

    return temp_new(interval == NULL ? SMALLEST_INTERVAL : yml_value_as_int(interval), yml_value_as_int(thermal_zone),
                    unit == NULL ? TEMP_UNIT_CELSIUS : str_to_unit(yml_value_as_string(unit)),
                    conf_to_particle(c, inherited));
}

static bool
conf_verify_interval(keychain_t *chain, const struct yml_node *node)
{
    if (!conf_verify_unsigned(chain, node))
        return false;

    if (yml_value_as_int(node) < SMALLEST_INTERVAL) {
        LOG_ERR("%s: interval value cannot be less than %d ms", conf_err_prefix(chain, node), SMALLEST_INTERVAL);
        return false;
    }

    return true;
}

static bool
conf_verify_unit(keychain_t *chain, const struct yml_node *node)
{
    return conf_verify_enum(chain, node, (const char *[]){"C", "F", "Celsius", "Fahrenheit"}, 4);
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        {"interval", false, &conf_verify_interval},
        {"thermal_zone", true, &conf_verify_unsigned},
        {"unit", false, &conf_verify_unit},
        MODULE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct module_iface module_temperature_iface = {
    .verify_conf = &verify_conf,
    .from_conf = &from_conf,
};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct module_iface iface __attribute__((weak, alias("module_temperature_iface")));
#endif
