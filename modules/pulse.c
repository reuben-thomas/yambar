#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pulse/def.h>
#include <pulse/introspect.h>
#include <pulse/operation.h>
#include <pulse/proplist.h>
#include <pulse/thread-mainloop.h>
#include <pulse/volume.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pulse/pulseaudio.h"

#define LOG_MODULE "pulse"
#define LOG_ENABLE_DBG 1

#include "../bar/bar.h"
#include "../config-verify.h"
#include "../config.h"
#include "../log.h"
#include "../plugin.h"

struct private
{
    char *sink_name;
    char *source_name;
    struct particle *label;

    bool sink_online;
    bool sink_muted;
    pa_volume_t sink_cur_volume;

    bool source_online;
    bool source_muted;
    pa_volume_t source_cur_volume;

    bool online;

    uint sink_index;
    uint source_index;
    char *current_sink_name;
    char *current_source_name;
};

static void
destroy(struct module *mod)
{
    struct private *m = mod->private;
    m->label->destroy(m->label);
    free(m->current_sink_name);
    free(m->current_source_name);
    free(m->sink_name);
    free(m->source_name);
    free(m);
    module_default_destroy(mod);
}

static const char *
description(struct module *mod)
{
    static char desc[32];
    snprintf(desc, sizeof(desc), "pulse");
    return desc;
}

static struct exposable *
content(struct module *mod)
{
    struct private *p = mod->private;

    mtx_lock(&mod->lock);

    int sink_percent = (p->sink_cur_volume * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM;
    int source_percent = (p->source_cur_volume * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM;

    // Note that Pulseaudio source/sink can go over 100% volume
    struct tag_set tags = {
        .tags =
            (struct tag *[]){
                tag_new_bool(mod, "online", p->online),
                tag_new_bool(mod, "sink_online", p->sink_online),
                tag_new_bool(mod, "sink_muted", p->sink_muted),
                tag_new_int_range(mod, "sink_percent", sink_percent, 0, 100),
                tag_new_bool(mod, "source_online", p->source_online),
                tag_new_bool(mod, "source_muted", p->source_muted),
                tag_new_int_range(mod, "source_percent", source_percent, 0, 100),
            },
        .count = 7,
    };
    mtx_unlock(&mod->lock);

    struct exposable *exposable = p->label->instantiate(p->label, &tags);

    tag_set_destroy(&tags);
    return exposable;
}

void
sink_info_callback(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
    struct module *mod = (struct module *)userdata;
    struct private *p = mod->private;
    if (!i)
        return;

    // Average of the channels
    pa_volume_t vol = pa_cvolume_avg(&i->volume);

    mtx_lock(&mod->lock);
    p->sink_cur_volume = vol;
    p->sink_muted = i->mute;
    p->sink_online = true;
    p->sink_index = i->index;
    mtx_unlock(&mod->lock);

    mod->bar->refresh(mod->bar);
}

void
source_info_callback(pa_context *c, const pa_source_info *i, int eol, void *userdata)
{
    struct module *mod = (struct module *)userdata;
    struct private *p = mod->private;
    if (!i)
        return;

    // Average of the channels
    pa_volume_t vol = pa_cvolume_avg(&i->volume);

    mtx_lock(&mod->lock);
    p->source_cur_volume = vol;
    p->source_online = true;
    p->source_muted = i->mute;
    p->source_index = i->index;
    mtx_unlock(&mod->lock);

    mod->bar->refresh(mod->bar);
}

void
server_info_callback(pa_context *c, const pa_server_info *i, void *userdata)
{
    struct module *mod = (struct module *)userdata;
    struct private *p = mod->private;
    pa_operation *o;

    // If they match defaults that means there are no sinks/sources
    if (strcmp(i->default_sink_name, "@DEFAULT_SINK@") == 0) {
        mtx_lock(&mod->lock);
        p->sink_online = false;
        p->sink_index = 0;
        mtx_unlock(&mod->lock);

        mod->bar->refresh(mod->bar);
    }
    if (strcmp(i->default_source_name, "@DEFAULT_SOURCE@") == 0) {
        mtx_lock(&mod->lock);
        p->source_online = false;
        p->source_index = 0;
        mtx_unlock(&mod->lock);

        mod->bar->refresh(mod->bar);
    }

    if (!p->current_sink_name || strcmp(i->default_sink_name, p->current_sink_name) != 0) {
        LOG_DBG("Default sink changed (%s) - calling get_sink_info_by_name", i->default_sink_name);
        mtx_lock(&mod->lock);
        free(p->current_sink_name);
        p->current_sink_name = strdup(i->default_sink_name);
        mtx_unlock(&mod->lock);

        if (!(o = pa_context_get_sink_info_by_name(c, p->sink_name, sink_info_callback,
                                                   userdata))) {
            LOG_ERR("pa_context_get_sink_info_by_name() failed: %s",
                    pa_strerror(pa_context_errno(c)));
        }
        pa_operation_unref(o);
    } else if (!p->current_source_name ||
               strcmp(i->default_source_name, p->current_source_name) != 0) {
        LOG_DBG("Default source changed (%s) - calling get_sink_info_by_name",
                i->default_source_name);
        mtx_lock(&mod->lock);
        free(p->current_source_name);
        p->current_source_name = strdup(i->default_source_name);
        mtx_unlock(&mod->lock);

        if (!(o = pa_context_get_source_info_by_name(c, p->source_name, source_info_callback,
                                                     userdata))) {
            LOG_ERR("pa_context_get_source_info_by_name() failed: %s",
                    pa_strerror(pa_context_errno(c)));
        }
        pa_operation_unref(o);
    }
}

/*
  We subscribe to events on sink/sources and server configuration changes

  Sinks and sources so that we can react to volume/mute changes for example.
  Server change subscription gets triggered when default sink/source changes
  (among other things) so it lets us immediately react
*/
void
pa_subscription_callback(pa_context *c, pa_subscription_event_type_t t, uint idx, void *userdata)
{
    struct module *mod = (struct module *)userdata;
    struct private *p = mod->private;
    pa_operation *o;

    switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
    case PA_SUBSCRIPTION_EVENT_SINK:
        if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
            if (idx == p->sink_index) {
                mtx_lock(&mod->lock);
                p->sink_online = false;
                p->sink_index = 0;
                mtx_unlock(&mod->lock);

                mod->bar->refresh(mod->bar);
            }
        }

        if (!p->sink_index || p->sink_index == idx) {
            LOG_DBG("calling get_sink_info_by_name: %d", idx);
            if (!(o = pa_context_get_sink_info_by_name(c, p->sink_name, sink_info_callback,
                                                       userdata))) {
                LOG_ERR("pa_context_get_sink_info_by_name() failed: %s",
                        pa_strerror(pa_context_errno(c)));
            }
            pa_operation_unref(o);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SOURCE:
        if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
            if (idx == p->source_index) {
                mtx_lock(&mod->lock);
                p->source_online = false;
                p->source_index = 0;
                mtx_unlock(&mod->lock);

                mod->bar->refresh(mod->bar);
            }
        }

        if (!p->source_index || p->source_index == idx) {
            LOG_DBG("calling get_source_info_by_name: %d", idx);
            if (!(o = pa_context_get_source_info_by_name(c, p->source_name, source_info_callback,
                                                         userdata))) {
                LOG_ERR("pa_context_get_source_info_by_name() failed: %s",
                        pa_strerror(pa_context_errno(c)));
            }
            pa_operation_unref(o);
        }
        break;
    case PA_SUBSCRIPTION_EVENT_SERVER:
        // Update when server state changes - for example changes in default sink/source
        if (!(o = pa_context_get_server_info(c, server_info_callback, userdata))) {
            LOG_ERR("pa_context_get_server_info() failed: %s", pa_strerror(pa_context_errno(c)));
        }
        pa_operation_unref(o);
        break;
    }
}

/* This is called whenever the context status changes - we connect/disconnect etc */
static void
context_state_callback(pa_context *c, void *userdata)
{
    assert(c);
    struct module *mod = (struct module *)userdata;
    struct private *p = mod->private;
    LOG_DBG("Context state callback called");

    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        mtx_lock(&mod->lock);
        p->online = false;
        mtx_unlock(&mod->lock);

        mod->bar->refresh(mod->bar);
        break;

    case PA_CONTEXT_READY: {
        pa_operation *o;
        assert(c);
        mtx_lock(&mod->lock);
        p->online = true;
        mtx_unlock(&mod->lock);

        // Usually not needed since later callbacks refresh the bar, but it doesn't hurt just in case
        mod->bar->refresh(mod->bar);

        LOG_DBG("pulse connection established.");
        pa_context_set_subscribe_callback(c, pa_subscription_callback, mod);
        if (!(o = pa_context_subscribe(c,
                                       PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
                                           PA_SUBSCRIPTION_MASK_SERVER,
                                       NULL, mod))) {
            LOG_ERR("pa_context_subscribe failed: %s", pa_strerror(pa_context_errno(c)));
        }

        if (!(o = pa_context_get_sink_info_by_name(c, p->sink_name, sink_info_callback, mod))) {
            LOG_ERR("pa_context_get_sink_info_by_name failed: %s",
                    pa_strerror(pa_context_errno(c)));
        }
        if (!(o = pa_context_get_source_info_by_name(c, p->source_name, source_info_callback,
                                                     mod))) {
            LOG_ERR("pa_context_get_source_info_by_name failed: %s",
                    pa_strerror(pa_context_errno(c)));
        }
        pa_operation_unref(o);
        break;
    }

    case PA_CONTEXT_TERMINATED:
    case PA_CONTEXT_FAILED:
    default:
        mtx_lock(&mod->lock);
        p->online = false;
        p->sink_online = false;
        p->source_online = false;
        mtx_unlock(&mod->lock);
        LOG_ERR("pulse connection failure: %s", pa_strerror(pa_context_errno(c)));
        mod->bar->refresh(mod->bar);
        // TODO - possibly schedule a recurring reconnect attempt?
        break;
    }
    return;
}

int
run(struct module *mod)
{
    int ret = 1;
    pa_threaded_mainloop *m = NULL;
    pa_context *context = NULL;
    pa_mainloop_api *mainloop_api = NULL;

    if (!(m = pa_threaded_mainloop_new())) {
        LOG_ERR("pa_mainloop_new() failed");
        goto out;
    }

    mainloop_api = pa_threaded_mainloop_get_api(m);

    if (!(context = pa_context_new(mainloop_api, NULL))) {
        LOG_ERR("pa_context_new() failed.");
        goto out;
    }

    pa_context_set_state_callback(context, context_state_callback, mod);

    /* Connect the context */
    if (pa_context_connect(context, NULL, 0, NULL) < 0) {
        LOG_ERR("pa_context_connect() failed: %s\n", pa_strerror(pa_context_errno(context)));
        goto out;
    }

    /* Let's start a separate thread for handling all pulseaudio events */
    pa_threaded_mainloop_start(m);

    /* And wait for termination event from abort_fd */
    while (true) {
        struct pollfd fds[] = {{.fd = mod->abort_fd, .events = POLLIN}};
        int r = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;

            LOG_ERRNO("failed to poll");
            goto out;
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            // This is the only way to exit without an error
            ret = 0;
            goto out;
        }
    }

out:
    if (m) {
        pa_threaded_mainloop_stop(m);
        pa_threaded_mainloop_free(m);
    }

    return ret;
}

static struct module *
pulse_new(const char *sink_name, const char *source_name, struct particle *label)
{
    struct private *priv = calloc(1, sizeof(*priv));
    priv->label = label;
    priv->sink_name = strdup(sink_name);
    priv->source_name = strdup(source_name);
    priv->source_online = false;
    priv->sink_online = false;
    priv->online = false;

    struct module *mod = module_common_new();
    mod->private = priv;
    mod->run = &run;
    mod->destroy = &destroy;
    mod->content = &content;
    mod->description = &description;
    return mod;
}

static struct module *
from_conf(const struct yml_node *node, struct conf_inherit inherited)
{
    const struct yml_node *sink_name = yml_get_value(node, "sink_name");
    const struct yml_node *source_name = yml_get_value(node, "source_name");
    const struct yml_node *content = yml_get_value(node, "content");

    return pulse_new(sink_name != NULL ? yml_value_as_string(sink_name) : "@DEFAULT_SINK@",
                     source_name != NULL ? yml_value_as_string(source_name) : "@DEFAULT_SOURCE@",
                     conf_to_particle(content, inherited));
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        {"sink_name", false, &conf_verify_string},
        {"source_name", false, &conf_verify_string},
        MODULE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct module_iface module_pulse_iface = {
    .verify_conf = &verify_conf,
    .from_conf = &from_conf,
};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct module_iface iface __attribute__((weak, alias("module_pulse_iface")));
#endif
