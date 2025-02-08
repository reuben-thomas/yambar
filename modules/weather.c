#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>

#include <curl/curl.h>

#define LOG_MODULE "weather"
#define LOG_ENABLE_DBG 0
#include "../bar/bar.h"
#include "../config-verify.h"
#include "../config.h"
#include "../log.h"
#include "../plugin.h"
#include "version.h"

#include "weather.h"

#define DEFAULT_HOST "https://tgftp.nws.noaa.gov/data/observations/metar/decoded/"

static void
free_weather_info(struct weather_info *info)
{
    if (!info) {
        return;
    }
    free(info->station_town);
    free(info->station_state);
    free(info->wind_direction);
    free(info->visibility);
    free(info->sky_condition);
    free(info->weather);
    free(info);
}

struct private
{
    struct particle *label;
    char *host;
    char *station;

    struct weather_info *info;
};

static void
destroy(struct module *mod)
{
    struct private *m = mod->private;
    m->label->destroy(m->label);
    free(m->host);
    free(m->station);
    free_weather_info(m->info);
    free(m);
    module_default_destroy(mod);
}

static const char *
description(const struct module *mod)
{
    return "weather";
}

static struct exposable *
content(struct module *mod)
{
    const struct private *m = mod->private;

    mtx_lock(&mod->lock);

    struct tag_set tags = {
        .tags = (struct tag *[]){
            tag_new_string(mod, "station_town", m->info->station_town),
            tag_new_string(mod, "station_state", m->info->station_state),
            tag_new_int(mod, "year", m->info->year),
            tag_new_int(mod, "month", m->info->month),
            tag_new_int(mod, "day", m->info->day),
            tag_new_int(mod, "hour", m->info->hour),
            tag_new_int(mod, "minute", m->info->minute),
            tag_new_string(mod, "wind_direction", m->info->wind_direction),
            tag_new_int(mod, "wind_azimuth", m->info->wind_azimuth),
            tag_new_int(mod, "wind_mph", m->info->wind_mph),
            tag_new_int(mod, "wind_knots", m->info->wind_knots),
            tag_new_int(mod, "wind_kmph", m->info->wind_kmph),
            tag_new_int(mod, "wind_mps", m->info->wind_mps),
            tag_new_string(mod, "visibility", m->info->visibility),
            tag_new_string(mod, "sky_condition", m->info->sky_condition),
            tag_new_string(mod, "weather", m->info->weather),
            tag_new_float(mod, "temp_c", m->info->temp_c),
            tag_new_float(mod, "temp_f", m->info->temp_f),
            tag_new_float(mod, "heat_index_c", m->info->heat_index_c),
            tag_new_float(mod, "heat_index_f", m->info->heat_index_f),
            tag_new_float(mod, "dew_point_c", m->info->dew_point_c),
            tag_new_float(mod, "dew_point_f", m->info->dew_point_f),
            tag_new_int(mod, "humidity", m->info->humidity),
            tag_new_float(mod, "pressure_mmhg", m->info->pressure_mmhg),
            tag_new_int(mod, "pressure_hpa", m->info->pressure_hpa),
        },
        .count = 25,
    };

    mtx_unlock(&mod->lock);

    struct exposable *exposable = m->label->instantiate(m->label, &tags);

    tag_set_destroy(&tags);
    return exposable;
}

static size_t
weather_curl_buffer_write(void *contents, size_t size, size_t n, void *data)
{
    size_t new_size = size * n;
    struct weather_curl_buffer *buf = (struct weather_curl_buffer *)data;

    // Grow the buffer if necessary.
    size_t needed = buf->size + new_size + 1;
    if (buf->capacity < needed) {
        size_t new_cap = buf->capacity;
        while (new_cap < needed) {
            new_cap <<= 1;
        }
        char *newmem = realloc(buf->buffer, new_cap);
        if (!newmem) {
            // TODO: log
            return 0;
        }

        buf->buffer = newmem;
        buf->capacity = new_cap;
    }

    memcpy(buf->buffer + buf->size, contents, new_size);
    buf->size += new_size;
    buf->buffer[buf->size] = '\0';

    return new_size;
};

struct weather_curl_buffer *
weather_curl_buffer_new()
{
    struct weather_curl_buffer *buf = (struct weather_curl_buffer*)calloc(1, sizeof(*buf));
    // Start with one page of memory.
    buf->buffer = calloc(1, 4096);
    buf->capacity = buf->buffer ? 4096 : 0;

    return buf;
}

void
free_weather_curl_buffer(struct weather_curl_buffer *buf)
{
    if (!buf) {
        return;
    }
    free(buf->buffer);
    free(buf);
}

static int
run(struct module *mod)
{
    struct private *m = mod->private;
    free_weather_info(m->info);
    m->info = calloc(1, sizeof(*m->info));

    static char url[4096];
    snprintf(url, sizeof(url), "%s%s.TXT", m->host, m->station);

    CURL *curl = curl_easy_init();
    struct weather_curl_buffer *buf = weather_curl_buffer_new();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, weather_curl_buffer_write);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "yambar/" YAMBAR_VERSION);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            free_weather_curl_buffer(buf);
            buf = NULL;
        }
    }

    curl_easy_cleanup(curl);

    if (!buf) {
        return 1;
    }

    int ret = parse_weather_info(buf, m->info);
    free_weather_curl_buffer(buf);

    return ret;
}

static struct module *
weather_new(struct particle *label, const char* host, const char *station)
{
    struct private *m = calloc(1, sizeof(*m));
    m->label = label;
    m->host = strdup(host);
    m->station = strdup(station);

    struct module *mod = module_common_new();
    mod->private = m;
    mod->run = &run;
    mod->destroy = &destroy;
    mod->content = &content;
    mod->description = &description;
    return mod;
}

static struct module *
from_conf(const struct yml_node *node, struct conf_inherit inherited)
{
    const struct yml_node *c = yml_get_value(node, "content");
    const struct yml_node *station = yml_get_value(node, "station");
    const struct yml_node *host = yml_get_value(node, "host");

    return weather_new(conf_to_particle(c, inherited), yml_value_as_string(station), host == NULL ? DEFAULT_HOST : yml_value_as_string(host));
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        {"station", true, &conf_verify_string},
        {"host", false, &conf_verify_string},
        MODULE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct module_iface module_weather_iface = {
    .verify_conf = &verify_conf,
    .from_conf = &from_conf,
};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct module_iface iface __attribute__((weak, alias("module_weather_iface")));
#endif
