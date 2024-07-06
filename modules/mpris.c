#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include <libgen.h>
#include <poll.h>

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <dbus/dbus.h>

#define LOG_MODULE "mpris"
#define LOG_ENABLE_DBG 1
#include "../bar/bar.h"
#include "../config-verify.h"
#include "../config.h"
#include "../log.h"
#include "../plugin.h"

/* TODO: Process 'NameOwnerChanged'/'PropertiesChanged' signals.
 * Should we listen for signals, instead of querying?
 * + We get notified, if something changed, reducing our own overhead
 * + A convinient way to handle missing properties, since we will
 *   never be notified there nonexistence.
 * - "MPRIS compatible" clients do not necessarily correctly implement
 *   the MPRIS interface (for example, firefox reports some missing
 *   properties as 'InvalidArg', when the error should be
 *   'UnhandledProperty'). While this is also an issue for querying,
 *   the impact is a lot more managable, since we know right away
 *   what properties we are missing, and don't have to wait for them
 *   to change. This also assumes that the client emits the
 *   'PropertiesChanged' signal correctly, which (again) does not seem to be
 *   for everyone ('').
 * - The relevant signals are only emitted when, well, something changed.
 *   This means that we have fall back to querying if we want to build an initial state. */

/* TODO: Move from 'Get' to the 'GetAll' method on the 'org.freedesktop.DBus.Properties' interface.
 * + A generalized way to handle missing properties
 * + Reduced overhead, since we only call a single method
 * + Buid a list of available properties (bitmap)
 * - Complex parsing logic */

enum mpris_playback_state {
    MPRIS_PLAYBACK_INVALID,
    MPRIS_PLAYBACK_STOPPED,
    MPRIS_PLAYBACK_PLAYING,
    MPRIS_PLAYBACK_PAUSED,
};

enum mpris_loop_state {
    MPRIS_LOOP_INVALID,
    MPRIS_LOOP_NONE,
    MPRIS_LOOP_TRACK,
    MPRIS_LOOP_PLAYLIST,
};

struct mpris_metadata {
    uint64_t length_usec;
    char *trackid;
    char *artists;
    char *album;
    char *title;
};

struct mpris_property {
    enum mpris_playback_state playback_status;
    enum mpris_loop_state loop_status;
    struct mpris_metadata metadata;
    uint64_t position_usec;
    double rate;
    double volume;
    bool shuffle;
};

struct private
{
    struct particle *label;
    /* TODO: This should be an array of options */
    const char *desired_bus_name;
    DBusConnection *connection;

    const char *target_bus_name;
    const char *target_bus_identity;

    uint64_t previous_position_usec;
    struct mpris_property property;

    struct {
        uint64_t value_ns;
        struct timespec when;
    } elapsed;

    thrd_t refresh_thread_id;
    int refresh_abort_fd;
};

/* DBus specific */

#define MPRIS_QUERY_TIMEOUT 50

#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_BUS "org.mpris.MediaPlayer2"
#define MPRIS_INTERFACE "org.mpris.MediaPlayer2"
#define MPRIS_INTERFACE_PLAYER MPRIS_INTERFACE ".Player"

#define MPRIS_DBUS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_DBUS_BUS "org.freedesktop.DBus"
#define MPRIS_DBUS_INTERFACE "org.freedesktop.DBus"

static DBusMessage *
mpris_call_method_and_block(DBusConnection *connection, DBusMessage *message)
{
    DBusPendingCall *pending = NULL;

    if (!dbus_connection_send_with_reply(connection, message, &pending, MPRIS_QUERY_TIMEOUT)) {
        LOG_ERR("dbus: error: failed to allocate message object");
        return NULL;
    }

    dbus_pending_call_block(pending);
    dbus_message_unref(message);

    /* Handle and gracrfully return different error types
     * ('org.freedesktop.DBus.Error.NotSupported' etc.)*/
    DBusMessage *reply = dbus_pending_call_steal_reply(pending);
    DBusError error = {0};
    if (dbus_set_error_from_message(&error, reply)) {
        if ((strcmp(error.name, DBUS_ERROR_INVALID_ARGS) != 0) && (strcmp(error.name, DBUS_ERROR_NOT_SUPPORTED) != 0)) {
            LOG_ERR("Unhandled error: '%s' (%s)", error.message, error.name);
        } else {
            LOG_DBG("%s: %s", error.name, error.message);
        }

        dbus_message_unref(reply);
        dbus_error_free(&error);
        reply = NULL;
    }

    dbus_pending_call_unref(pending);
    return reply;
}

static DBusMessage *
mpris_get_property(DBusConnection *connection, const char *bus_name, const char *interface, const char *property_name)
{
    assert(bus_name != NULL && strlen(bus_name) > 0);
    assert(interface != NULL && strlen(interface) > 0);
    assert(property_name != NULL && strlen(property_name) > 0);

    DBusMessage *message
        = dbus_message_new_method_call(bus_name, MPRIS_PATH, MPRIS_DBUS_INTERFACE ".Properties", "Get");
    dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property_name,
                             DBUS_TYPE_INVALID);
    return mpris_call_method_and_block(connection, message);
}

/* TODO: Handle name changes
 * We essentially have two options:
 * 1. Listen for 'NameOwnerChanged' */

static char *
mpris_get_bus_name(DBusConnection *connection, const char *identity_name)
{
    assert(identity_name != NULL && strlen(identity_name) > 0);

    DBusMessage *message
        = dbus_message_new_method_call(MPRIS_DBUS_BUS, MPRIS_DBUS_PATH, MPRIS_DBUS_INTERFACE, "ListNames");
    DBusMessage *reply = mpris_call_method_and_block(connection, message);

    if (reply == NULL) {
        return NULL;
    }

    DBusError error = {0};
    char **bus_names;
    dbus_int32_t bus_count;

    dbus_error_init(&error);
    if (!dbus_message_get_args(reply, &error, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &bus_names, &bus_count,
                               DBUS_TYPE_INVALID)) {
        LOG_ERR("%s", error.message);
        dbus_error_free(&error);
        return NULL;
    }

    if (bus_count == 0) {
        return NULL;
    }

    char *string = NULL;
    for (dbus_int32_t i = 0; i < bus_count; i++) {
        if (strlen(bus_names[i]) < strlen(MPRIS_BUS ".") + strlen(identity_name)) {
            continue;
        }

        if (strncmp(bus_names[i] + strlen(MPRIS_BUS "."), identity_name, strlen(identity_name)) != 0) {
            continue;
        }

        string = strdup(bus_names[i]);
        break;
    }

    dbus_free_string_array(bus_names);
    dbus_error_free(&error);
    dbus_message_unref(reply);

    LOG_DBG("Found bus name: %s", string);

    return string;
}

static bool
mpris_unwrap_iter(DBusMessageIter *iter, dbus_int32_t type, void *target)
{
    DBusMessageIter type_iter = {0};

    assert(dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_VARIANT);
    dbus_message_iter_recurse(iter, &type_iter);
    char *tmp = dbus_message_iter_get_signature(&type_iter);
    (void)tmp;
    assert(dbus_message_iter_get_arg_type(&type_iter) == type);
    bool status = !dbus_message_iter_has_next(&type_iter);

    DBusBasicValue value = {0};
    switch (type) {
    case DBUS_TYPE_STRING:
        dbus_message_iter_get_basic(&type_iter, &value);
        *((char **)target) = strdup(value.str);
        break;
    case DBUS_TYPE_DOUBLE:
        dbus_message_iter_get_basic(&type_iter, &value);
        *((double *)target) = value.dbl;
        break;
    case DBUS_TYPE_BOOLEAN:
        dbus_message_iter_get_basic(&type_iter, &value);
        *((bool *)target) = value.bool_val;
        break;
    case DBUS_TYPE_INT64:
        dbus_message_iter_get_basic(&type_iter, &value);
        *((int64_t *)target) = value.i64;
        break;
    default:;
        char *signature = dbus_message_iter_get_signature(&type_iter);
        LOG_WARN("Trying to unwrap unsupported type: %s. Ignoring", signature);
        dbus_free(signature);
        status = false;
    }

    return status;
}

static bool
mpris_unwrap_message(DBusMessage *message, dbus_int32_t type, void *target)
{
    assert(message != NULL);
    DBusMessageIter iter = {0};
    dbus_message_iter_init(message, &iter);
    return mpris_unwrap_iter(&iter, type, target);
}

static bool
mpris_metadata_parse(const char *entry_name, DBusMessageIter *entry_iter, struct mpris_metadata *buffer)
{
    const char *string_value = NULL;
    DBusMessageIter array_iter = {0};

    if (strcmp(entry_name, "mpris:trackid") == 0) {
        assert(dbus_message_iter_get_arg_type(entry_iter) == DBUS_TYPE_OBJECT_PATH);
        dbus_message_iter_get_basic(entry_iter, &string_value);
        buffer->trackid = strdup(string_value);

    } else if (strcmp(entry_name, "xesam:album") == 0) {
        assert(dbus_message_iter_get_arg_type(entry_iter) == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(entry_iter, &string_value);
        buffer->album = strdup(string_value);

    } else if (strcmp(entry_name, "xesam:artist") == 0) {
        /* TODO: Propertly format string arrays */
        /* NOTE: Currently, only the first artist will be shown, as we
         * ignore the rest */
        assert(dbus_message_iter_get_arg_type(entry_iter) == DBUS_TYPE_ARRAY);
        dbus_message_iter_recurse(entry_iter, &array_iter);
        assert(dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRING);

        dbus_message_iter_get_basic(&array_iter, &string_value);
        buffer->artists = strdup(string_value);

    } else if (strcmp(entry_name, "xesam:title") == 0) {
        assert(dbus_message_iter_get_arg_type(entry_iter) == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(entry_iter, &string_value);
        buffer->title = strdup(string_value);

    } else if (strcmp(entry_name, "mpris:length") == 0) {
        assert(dbus_message_iter_get_arg_type(entry_iter) == DBUS_TYPE_INT64);
        dbus_message_iter_get_basic(entry_iter, &buffer->length_usec);
    } else {
        /*LOG_DBG("Ignoring unhandled metadata property: %s", entry_name);*/
    }

    return true;
}

static bool
mpris_unwrap_metadata_message(DBusMessage *message, struct mpris_metadata *metadata)
{
    bool status = true;

    /* Unpack values returned from DBus method calls */
    assert(dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
    DBusMessageIter message_iter = {0}, outer_array_iter = {0}, array_iter = {0};
    dbus_message_iter_init(message, &message_iter);
    assert(dbus_message_iter_get_arg_type(&message_iter) == DBUS_TYPE_VARIANT);
    dbus_message_iter_recurse(&message_iter, &outer_array_iter);
    assert(dbus_message_iter_get_arg_type(&outer_array_iter) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(&outer_array_iter, &array_iter);

    dbus_int32_t current_type = 0;
    while ((current_type = dbus_message_iter_get_arg_type(&array_iter)) != DBUS_TYPE_INVALID) {
        assert(current_type == DBUS_TYPE_DICT_ENTRY);

        const char *entry_name = NULL;
        DBusMessageIter entry_iter = {0}, entry_sub_iter = {0};

        dbus_message_iter_recurse(&array_iter, &entry_iter);
        assert(dbus_message_iter_get_arg_type(&entry_iter) == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(&entry_iter, &entry_name);

        dbus_message_iter_next(&entry_iter);
        dbus_message_iter_recurse(&entry_iter, &entry_sub_iter);
        status = mpris_metadata_parse(entry_name, &entry_sub_iter, metadata);

        dbus_message_iter_next(&array_iter);
    }

    return status;
}

/* ------------- */

static void
mpris_clear(struct mpris_property *property)
{
    struct mpris_metadata *metadata = &property->metadata;
    if (metadata->album != NULL) {
        free(metadata->album);
    }
    if (metadata->artists != NULL) {
        free(metadata->artists);
    }
    if (metadata->title != NULL) {
        free(metadata->title);
    }

    memset(property, 0, sizeof(*property));
}

static void
secs_to_str(unsigned secs, char *s, size_t sz)
{
    unsigned hours = secs / (60 * 60);
    unsigned minutes = secs % (60 * 60) / 60;
    secs %= 60;

    if (hours > 0)
        snprintf(s, sz, "%02u:%02u:%02u", hours, minutes, secs);
    else
        snprintf(s, sz, "%02u:%02u", minutes, secs);
}

static void
destroy(struct module *mod)
{
    struct private *m = mod->private;
    dbus_connection_close(m->connection);

    free((void *)m->target_bus_name);
    free((void *)m->desired_bus_name);

    mpris_clear(&m->property);

    m->label->destroy(m->label);

    free(m);
    module_default_destroy(mod);
}

static const char *
description(const struct module *mod)
{
    return "mpris";
}

static struct exposable *
content(struct module *mod)
{
    const struct private *m = mod->private;
    const struct mpris_metadata metadata = m->property.metadata;

    /* usec -> msec -> sec */
    uint32_t position_sec = m->property.position_usec / 1000 / 1000;
    uint32_t length_sec = metadata.length_usec / 1000 / 1000;

    char pos_buffer[16] = {0}, end_buffer[16] = {0};
    if (length_sec > 0) {
        secs_to_str(position_sec, pos_buffer, sizeof(pos_buffer));
        secs_to_str(length_sec, end_buffer, sizeof(end_buffer));
    }

    char *tag_playback_value = NULL;
    switch (m->property.playback_status) {
    case MPRIS_PLAYBACK_STOPPED:
        tag_playback_value = "stopped";
        break;
    case MPRIS_PLAYBACK_PLAYING:
        tag_playback_value = "playing";
        break;
    case MPRIS_PLAYBACK_PAUSED:
        tag_playback_value = "paused";
        break;
    case MPRIS_PLAYBACK_INVALID:
        tag_playback_value = "offline";
    }

    char *tag_loop_value = NULL;
    switch (m->property.loop_status) {
    case MPRIS_LOOP_NONE:
        tag_loop_value = "none";
        break;
    case MPRIS_LOOP_TRACK:
        tag_loop_value = "track";
        break;
    case MPRIS_LOOP_PLAYLIST:
        tag_loop_value = "playlist";
        break;
    case MPRIS_LOOP_INVALID:
        tag_loop_value = "";
        break;
    }

    const char *tag_identity_value = (m->desired_bus_name == NULL) ? "" : m->desired_bus_name;
    const char *tag_album_value = (metadata.album == NULL) ? "" : metadata.album;
    const char *tag_artists_value = (metadata.album == NULL) ? "" : metadata.artists;
    const char *tag_title_value = (metadata.album == NULL) ? "" : metadata.title;
    const char *tag_end_value = end_buffer;
    const char *tag_pos_value = pos_buffer;

    uint32_t tag_volume_value = (m->property.volume >= 0.995) ? 100 : 100 * m->property.volume;
    bool tag_shuffle_value = m->property.shuffle;

    struct tag_set tags = {
        .tags = (struct tag *[]){
            tag_new_string(mod, "state", tag_playback_value),
            tag_new_string(mod, "identity", tag_identity_value),
            tag_new_bool(mod, "random", tag_shuffle_value),
            tag_new_string(mod, "loop", tag_loop_value),
            tag_new_int_range(mod, "volume", tag_volume_value, 0, 100),
            tag_new_string(mod, "album", tag_album_value),
            tag_new_string(mod, "artist", tag_artists_value),
            tag_new_string(mod, "title", tag_title_value),
            tag_new_string(mod, "pos", tag_end_value),
            tag_new_string(mod, "end", tag_pos_value),
            tag_new_int_realtime(
                mod, "elapsed", position_sec, 0, length_sec, TAG_REALTIME_SECS),
        },
        .count = 11,
    };

    mtx_unlock(&mod->lock);

    struct exposable *exposable = m->label->instantiate(m->label, &tags);

    tag_set_destroy(&tags);
    return exposable;
}

static bool
update_status(struct module *mod)
{
    struct private *m = mod->private;
    mtx_lock(&mod->lock);

    /* Property: Metadata */
    mpris_clear(&m->property);
    DBusMessage *message = mpris_get_property(m->connection, m->target_bus_name, MPRIS_INTERFACE_PLAYER, "Metadata");
    if (message != NULL) {
        mpris_unwrap_metadata_message(message, &m->property.metadata);
        dbus_message_unref(message);
    }

    /* Update remaining properties */
    /* Property: PlaybackStatus */
    char *string = NULL;
    message = mpris_get_property(m->connection, m->target_bus_name, MPRIS_INTERFACE_PLAYER, "PlaybackStatus");
    if (message != NULL) {
        mpris_unwrap_message(message, DBUS_TYPE_STRING, &string);
        if (strcmp(string, "Stopped")) {
            m->property.playback_status = MPRIS_PLAYBACK_STOPPED;
        } else if (strcmp(string, "Paused")) {
            m->property.playback_status = MPRIS_PLAYBACK_PAUSED;
        } else if (strcmp(string, "Playing")) {
            m->property.playback_status = MPRIS_PLAYBACK_PLAYING;
        }
        dbus_message_unref(message);
    }

    /* Property: LoopStatus */
    message = mpris_get_property(m->connection, m->target_bus_name, MPRIS_INTERFACE_PLAYER, "LoopStatus");
    if (message != NULL) {
        mpris_unwrap_message(message, DBUS_TYPE_STRING, &string);
        if (strcmp(string, "None")) {
            m->property.loop_status = MPRIS_LOOP_NONE;
        } else if (strcmp(string, "Track")) {
            m->property.loop_status = MPRIS_LOOP_TRACK;
        } else if (strcmp(string, "Playlist")) {
            m->property.loop_status = MPRIS_LOOP_PLAYLIST;
        }
        dbus_message_unref(message);
    }

    /* Property: Volume */
    message = mpris_get_property(m->connection, m->target_bus_name, MPRIS_INTERFACE_PLAYER, "Volume");
    if (message != NULL) {
        mpris_unwrap_message(message, DBUS_TYPE_DOUBLE, &m->property.volume);
        dbus_message_unref(message);
    }

    /* Property: Rate */
    message = mpris_get_property(m->connection, m->target_bus_name, MPRIS_INTERFACE_PLAYER, "Rate");
    if (message != NULL) {
        mpris_unwrap_message(message, DBUS_TYPE_DOUBLE, &m->property.rate);
        dbus_message_unref(message);
    }

    /* Property: Position */
    message = mpris_get_property(m->connection, m->target_bus_name, MPRIS_INTERFACE_PLAYER, "Position");
    if (message != NULL) {
        mpris_unwrap_message(message, DBUS_TYPE_INT64, &m->property.position_usec);
        dbus_message_unref(message);
    }

    mtx_unlock(&mod->lock);

    return true;
}

static int
run(struct module *mod)
{
    /*const struct private *m = mod->private;*/
    const struct bar *bar = mod->bar;
    struct private *m = mod->private;

    DBusError error = {0};
    DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        LOG_ERR("Failed to connect to session bus: %s", error.message);
        return -1;
    }
    m->connection = connection;

    int ret = 0;
    bool aborted = false;
    while (ret == 0 && !aborted) {
        const uint32_t timeout_ms = 250;

        struct pollfd fds[] = {{.fd = mod->abort_fd, .events = POLLIN}};
        if (poll(fds, 1, timeout_ms) < 0) {
            if (errno == EINTR)
                continue;

            LOG_ERRNO("failed to poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            aborted = true;
            break;
        }

        /* TODO: Set up listener to catch disconnect events */
        if (m->target_bus_name == NULL) {
            m->target_bus_name = mpris_get_bus_name(m->connection, m->desired_bus_name);
            if (m->target_bus_name == NULL) {
                continue;
            }

            DBusMessage *message = mpris_get_property(m->connection, m->target_bus_name, MPRIS_INTERFACE, "Identity");
            if (message != NULL) {
                mpris_unwrap_message(message, DBUS_TYPE_STRING, &m->desired_bus_name);
                LOG_DBG("Player identity: %s", m->desired_bus_name);
            }
        }

        aborted = !update_status(mod);
        bar->refresh(bar);
    }

    return ret;
}

struct refresh_context {
    struct module *mod;
    int abort_fd;
    long milli_seconds;
};

static bool
refresh_in(struct module *mod, long milli_seconds)
{
    return true;
}

static struct module *
mpris_new(const char *identity, struct particle *label)
{
    struct private *priv = calloc(1, sizeof(*priv));
    priv->label = label;
    priv->desired_bus_name = strdup(identity);

    struct module *mod = module_common_new();
    mod->private = priv;
    mod->run = &run;
    mod->destroy = &destroy;
    mod->content = &content;
    mod->refresh_in = &refresh_in;
    mod->description = &description;
    return mod;
}

static struct module *
from_conf(const struct yml_node *node, struct conf_inherit inherited)
{
    const struct yml_node *identity = yml_get_value(node, "identity");
    const struct yml_node *c = yml_get_value(node, "content");

    return mpris_new(yml_value_as_string(identity), conf_to_particle(c, inherited));
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    // TODO: Add the ability to display the status of the most
    // recently active player. This will require a listener.
    static const struct attr_info attrs[] = {
        {"identity", true, &conf_verify_string},
        MODULE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct module_iface module_mpris_iface = {
    .verify_conf = &verify_conf,
    .from_conf = &from_conf,
};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct module_iface iface __attribute__((weak, alias("module_mpd_iface")));
#endif
