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

#define MPRIS_QUERY_TIMEOUT 50
#define MPRIS_LISTENER_TIMEOUT 100

#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_BUS_NAME "org.mpris.MediaPlayer2"
#define MPRIS_SERVICE "org.mpris.MediaPlayer2"
#define MPRIS_INTERFACE_ROOT "org.mpris.MediaPlayer2"
#define MPRIS_INTERFACE_PLAYER MPRIS_INTERFACE_ROOT ".Player"

enum mpris_status {
    MPRIS_STATUS_OFFLINE,
    MPRIS_STATUS_PLAYING,
    MPRIS_STATUS_PAUSED,
    MPRIS_STATUS_STOPPED,
    MPRIS_STATUS_ERROR,
};

struct mpris_metadata {
    uint64_t length_us;
    char *trackid;
    char *artists;
    char *album;
    char *title;
};

struct mpris_property {
    struct mpris_metadata metadata;
    char *playback_status;
    char *loop_status;
    uint64_t position_us;
    double rate;
    double volume;
    bool shuffle;
};

struct mpris_listener_context {
    DBusConnection *connection;
    DBusMessage *update_message;
    char *bus_name;
    char *bus_name_unique;
    bool has_update;
    bool has_target;
};

struct mpris_client {
    bool has_seeked_support;
    enum mpris_status status;
    char *bus_name;

    struct mpris_property property;

    /* The unix timestamp of the last position change (ie.
     * seeking, pausing) */
    struct timespec seeked_when;
};

struct private
{
    uint32_t query_timeout_ms;
    thrd_t refresh_thread_id;
    int refresh_abort_fd;

    DBusConnection *connection;
    struct particle *label;
    /* TODO: This should be an array of options */
    const char *identity;

    struct mpris_client client;
};

static bool
mpris_validate_bus_name(const char *identity, const char *name)
{
    assert(identity != NULL);
    assert(name != NULL);
    assert(dbus_validate_bus_name(name, NULL));

    if (strlen(name) < strlen(MPRIS_BUS_NAME ".") + strlen(identity)) {
        return false;
    }

    if (strncmp(name + strlen(MPRIS_BUS_NAME "."), identity, strlen(identity)) != 0) {
        return false;
    }

    return true;
}

static DBusMessage *
mpris_call_method_and_block(DBusConnection *connection, DBusMessage *message, uint32_t timeout_ms)
{
    DBusPendingCall *pending = NULL;

    if (!dbus_connection_send_with_reply(connection, message, &pending, timeout_ms)) {
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
        LOG_WARN("Unhandled error: '%s' (%s)", error.message, error.name);

        if (strcmp(error.name, DBUS_ERROR_NO_REPLY) == 0)
            LOG_WARN("The client took too long to respont. Try increasing the poll timeout");

        dbus_message_unref(reply);
        dbus_error_free(&error);
        reply = NULL;
    }

    dbus_pending_call_unref(pending);
    return reply;
}

static char *
mpris_find_bus_name(DBusConnection *connection, const char *identity)
{
    assert(identity != NULL && strlen(identity) > 0);

    DBusMessage *message
        = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListNames");
    DBusMessage *reply = mpris_call_method_and_block(connection, message, MPRIS_QUERY_TIMEOUT);

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

    char *string = NULL;
    for (dbus_int32_t i = 0; i < bus_count; i++) {
        if (mpris_validate_bus_name(identity, bus_names[i])) {
            string = strdup(bus_names[i]);
            break;
        }
    }

    if (bus_names != NULL)
        dbus_free_string_array(bus_names);
    dbus_error_free(&error);
    dbus_message_unref(reply);

    if (string == NULL) {
        LOG_INFO("Failed to find bus name for identity: %s", identity);
        return NULL;
    }

    LOG_INFO("Found bus name: %s", string);

    return string;
}

static char *
mpris_get_unique_name(DBusConnection *connection, const char *bus_name)
{
    DBusMessage *message
        = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
    dbus_message_append_args(message, DBUS_TYPE_STRING, &bus_name, DBUS_TYPE_INVALID);
    DBusMessage *reply = mpris_call_method_and_block(connection, message, MPRIS_QUERY_TIMEOUT);

    if (dbus_message_is_error(reply, DBUS_ERROR_NAME_HAS_NO_OWNER)) {
        LOG_ERR("Bus name has no owner: %s", bus_name);
        return NULL;
    }

    DBusError error = {0};
    char *string = NULL;
    dbus_message_get_args(reply, &error, DBUS_TYPE_STRING, &string, DBUS_TYPE_INVALID);

    return strdup(string);
}

static bool
mpris_metadata_parse_property(const char *property_name, DBusMessageIter *iter, struct mpris_metadata *buffer)
{
    const char *string_value = NULL;
    DBusMessageIter array_iter = {0};
    __attribute__((unused)) dbus_int32_t type = dbus_message_iter_get_arg_type(iter);

    /* Do not parse empty arrays */
    if (type == DBUS_TYPE_ARRAY && dbus_message_iter_get_element_count(iter) == 0) {
        return true;
    }

    if (strcmp(property_name, "mpris:trackid") == 0) {
        assert(type == DBUS_TYPE_OBJECT_PATH || type == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(iter, &string_value);
        buffer->trackid = strdup(string_value);

    } else if (strcmp(property_name, "xesam:album") == 0) {
        assert(type == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(iter, &string_value);
        buffer->album = strdup(string_value);

    } else if (strcmp(property_name, "xesam:artist") == 0) {
        /* TODO: Properly format string arrays */
        /* NOTE: Currently, only the first artist will be shown, as we
         * ignore the rest */
        assert(type == DBUS_TYPE_ARRAY);
        dbus_message_iter_recurse(iter, &array_iter);
        assert(dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRING);

        dbus_message_iter_get_basic(&array_iter, &string_value);
        buffer->artists = strdup(string_value);

    } else if (strcmp(property_name, "xesam:title") == 0) {
        assert(type == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(iter, &string_value);
        buffer->title = strdup(string_value);

    } else if (strcmp(property_name, "mpris:length") == 0) {
        assert(type == DBUS_TYPE_INT64 || type == DBUS_TYPE_UINT64);
        dbus_message_iter_get_basic(iter, &buffer->length_us);
    } else {
        /*LOG_DBG("Ignoring metadata property: %s", entry_name);*/
    }

    return true;
}

static bool
mpris_metadata_parse_array(struct mpris_metadata *metadata, DBusMessageIter *iter)
{
    bool status = true;
    DBusMessageIter array_iter = {0};

    assert(dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(iter, &array_iter);

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
        status = mpris_metadata_parse_property(entry_name, &entry_sub_iter, metadata);

        dbus_message_iter_next(&array_iter);
    }

    return status;
}

static bool
mpris_property_parse(struct mpris_property *prop, const char *property_name, DBusMessageIter *iter_)
{
    DBusMessageIter iter = {0};
    bool status = true;

    /* This function is called in two different ways:
     * 1. update_status(): The property is passed directly
     * 2. update_status_from_message(): The property is passed wrapped
     *    inside a variant and has to be unpacked */
    if (dbus_message_iter_get_arg_type(iter_) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(iter_, &iter);
    } else {
        iter = *iter_;
    }

    if (strcmp(property_name, "PlaybackStatus") == 0) {
        assert(dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(&iter, &prop->playback_status);
    } else if (strcmp(property_name, "LoopStatus") == 0) {
        assert(dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING);
        dbus_message_iter_get_basic(&iter, &prop->loop_status);
    } else if (strcmp(property_name, "Position") == 0) {
        assert(dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT64);
        dbus_message_iter_get_basic(&iter, &prop->position_us);
    } else if (strcmp(property_name, "Shuffle") == 0) {
        assert(dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_BOOLEAN);
        dbus_message_iter_get_basic(&iter, &prop->shuffle);
    } else if (strcmp(property_name, "Metadata") == 0) {
        status = mpris_metadata_parse_array(&prop->metadata, &iter);
    } else {
        /*LOG_DBG("Ignoring property: %s", property_name);*/
    }

    return status;
}

static void
mpris_reset_property(struct mpris_property *property)
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
mpris_reset_client(struct mpris_client *client)
{
    if (client->bus_name != NULL)
        free(client->bus_name);

    memset(client, 0, sizeof(*client));
}

/* ------------- */

static void
format_usec_timestamp(unsigned usec, char *s, size_t sz)
{
    uint32_t secs = usec / 1000 / 1000;
    uint32_t hours = secs / (60 * 60);
    uint32_t minutes = secs % (60 * 60) / 60;
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

    free((void *)m->identity);
    free(m->client.bus_name);

    m->label->destroy(m->label);

    free(m);
    module_default_destroy(mod);
}

static const char *
description(const struct module *mod)
{
    return "mpris";
}

static uint64_t
timespec_diff_us(const struct timespec *a, const struct timespec *b)
{
    uint64_t nsecs_a = a->tv_sec * 1000000000 + a->tv_nsec;
    uint64_t nsecs_b = b->tv_sec * 1000000000 + b->tv_nsec;

    assert(nsecs_a >= nsecs_b);
    uint64_t nsec_diff = nsecs_a - nsecs_b;
    return nsec_diff / 1000;
}

static struct exposable *
content(struct module *mod)
{
    const struct private *m = mod->private;
    const struct mpris_client *client = &m->client;
    const struct mpris_metadata metadata = m->client.property.metadata;
    const struct mpris_property *property = &m->client.property;

    /* Calculate the current playback position */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    uint64_t elapsed_us = client->property.position_us;
    uint64_t length_us = metadata.length_us;

    if (m->client.status == MPRIS_STATUS_PLAYING) {
        elapsed_us += timespec_diff_us(&now, &client->seeked_when);
        if (elapsed_us > length_us) {
            LOG_DBG("dynamic update of elapsed overflowed: "
                    "elapsed=%" PRIu64 ", duration=%" PRIu64,
                    elapsed_us, length_us);
            elapsed_us = length_us;
        }
    }

    char tag_pos_value[16] = {0}, tag_end_value[16] = {0};
    if (length_us > 0) {
        format_usec_timestamp(elapsed_us, tag_pos_value, sizeof(tag_pos_value));
        format_usec_timestamp(length_us, tag_end_value, sizeof(tag_end_value));
    }

    char *tag_state_value = NULL;
    switch (client->status) {
    case MPRIS_STATUS_ERROR:
        tag_state_value = "error";
        break;
    case MPRIS_STATUS_OFFLINE:
        tag_state_value = "offline";
        break;
    case MPRIS_STATUS_PLAYING:
        tag_state_value = "playing";
        break;
    case MPRIS_STATUS_PAUSED:
        tag_state_value = "paused";
        break;
    case MPRIS_STATUS_STOPPED:
        tag_state_value = "stopped";
        break;
    }

    const char *tag_loop_value = (property->loop_status == NULL) ? "" : property->loop_status;
    const char *tag_album_value = (metadata.album == NULL) ? "" : metadata.album;
    const char *tag_artists_value = (metadata.album == NULL) ? "" : metadata.artists;
    const char *tag_title_value = (metadata.album == NULL) ? "" : metadata.title;
    const uint32_t tag_volume_value = (property->volume >= 0.995) ? 100 : 100 * property->volume;
    const bool tag_shuffle_value = property->shuffle;
    const enum tag_realtime_unit realtime_unit
        = (client->status == MPRIS_STATUS_PLAYING) ? TAG_REALTIME_SECS : TAG_REALTIME_NONE;

    struct tag_set tags = {
        .tags = (struct tag *[]){
            tag_new_string(mod, "state", tag_state_value),
            tag_new_bool(mod, "shuffle", tag_shuffle_value),
            tag_new_string(mod, "loop", tag_loop_value),
            tag_new_int_range(mod, "volume", tag_volume_value, 0, 100),
            tag_new_string(mod, "album", tag_album_value),
            tag_new_string(mod, "artist", tag_artists_value),
            tag_new_string(mod, "title", tag_title_value),
            tag_new_string(mod, "pos", tag_pos_value),
            tag_new_string(mod, "end", tag_end_value),
            tag_new_int_realtime(
                mod, "elapsed", elapsed_us / 1000, 0, length_us / 1000, realtime_unit),
        },
        .count = 10,
    };

    mtx_unlock(&mod->lock);

    struct exposable *exposable = m->label->instantiate(m->label, &tags);

    tag_set_destroy(&tags);
    return exposable;
}

__attribute__((unused)) static bool
update_status(struct module *mod)
{
    struct private *m = mod->private;
    mtx_lock(&mod->lock);

    mpris_reset_property(&m->client.property);

    if (m->client.bus_name == NULL) {
        mtx_unlock(&mod->lock);
        return true;
    }

    const char *interface = MPRIS_INTERFACE_PLAYER;
    DBusMessage *message
        = dbus_message_new_method_call(m->client.bus_name, MPRIS_PATH, DBUS_INTERFACE_PROPERTIES, "GetAll");
    dbus_message_append_args(message, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID);

    DBusMessage *reply = mpris_call_method_and_block(m->connection, message, m->query_timeout_ms);
    if (reply == NULL) {
        LOG_ERR("Failed to query internal state");
        mtx_unlock(&mod->lock);
        return false;
    }

    DBusMessageIter reply_iter = {0}, dict_iter = {0};
    dbus_message_iter_init(reply, &reply_iter);
    assert(dbus_message_iter_get_arg_type(&reply_iter) == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(&reply_iter, &dict_iter);

    dbus_int32_t current_type = DBUS_TYPE_INVALID;
    while ((current_type = dbus_message_iter_get_arg_type(&dict_iter)) != DBUS_TYPE_INVALID) {
        DBusMessageIter dict_entry_iter = {0};
        dbus_message_iter_recurse(&dict_iter, &dict_entry_iter);

        const char *property_name = NULL;
        dbus_message_iter_get_basic(&dict_entry_iter, &property_name);
        assert(dbus_message_iter_has_next(&dict_entry_iter));
        dbus_message_iter_next(&dict_entry_iter);

        DBusMessageIter property_iter = {0};
        dbus_message_iter_recurse(&dict_entry_iter, &property_iter);

        if (!mpris_property_parse(&m->client.property, property_name, &property_iter)) {
            LOG_ERR("Failed to parse property: %s", property_name);
            mtx_unlock(&mod->lock);
            return false;
        }

        if (strcmp(property_name, "PlaybackStatus") == 0) {
            if (strcmp(m->client.property.playback_status, "Stopped") == 0) {
                m->client.status = MPRIS_STATUS_STOPPED;
            } else if (strcmp(m->client.property.playback_status, "Playing") == 0) {
                m->client.status = MPRIS_STATUS_PLAYING;
            } else if (strcmp(m->client.property.playback_status, "Paused") == 0) {
                m->client.status = MPRIS_STATUS_PAUSED;
            } else {
                m->client.status = MPRIS_STATUS_OFFLINE;
            }
        }

        dbus_message_iter_next(&dict_iter);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    m->client.seeked_when = now;

    mtx_unlock(&mod->lock);

    return true;
}

static bool
update_status_from_message(struct module *mod, DBusMessage *message)
{
    struct private *m = mod->private;
    mtx_lock(&mod->lock);

    /* Player.Seeked (UINT64 position)*/
    if (strcmp(dbus_message_get_member(message), "Seeked") == 0) {
        m->client.has_seeked_support = true;
        DBusMessageIter iter = {0};
        dbus_message_iter_init(message, &iter);
        dbus_message_iter_get_basic(&iter, &m->client.property.position_us);

        clock_gettime(CLOCK_MONOTONIC, &m->client.seeked_when);

        return true;
    }

    /* Properties.PropertiesChanged (STRING interface_name,
     *                               ARRAY of DICT_ENTRY<STRING,VARIANT> changed_properties,
     *                               ARRAY<STRING> invalidated_properties); */
    assert(strcmp(dbus_message_get_member(message), "PropertiesChanged") == 0);

    DBusMessageIter iter = {0};
    dbus_message_iter_init(message, &iter);

    dbus_int32_t current_type = 0;
    const char *interface_name = NULL;

    /* Signature: s */
    current_type = dbus_message_iter_get_arg_type(&iter);
    assert(current_type == DBUS_TYPE_STRING);
    dbus_message_iter_get_basic(&iter, &interface_name);
    dbus_message_iter_next(&iter);

    /* Signature: a{sv} */
    DBusMessageIter changed_properties_iter = {0};
    current_type = dbus_message_iter_get_arg_type(&iter);
    assert(current_type == DBUS_TYPE_ARRAY);
    dbus_message_iter_recurse(&iter, &changed_properties_iter);
    dbus_message_iter_next(&iter);

    /* Signature: as */
    /* The MPRIS interface should not change on the fly ie. we can
     * probably ignore the 'invalid_properties' argument */
    current_type = dbus_message_iter_get_arg_type(&iter);
    assert(current_type == DBUS_TYPE_ARRAY);
    assert(dbus_message_iter_get_element_count(&iter) == 0);

    if (strcmp(interface_name, MPRIS_INTERFACE_PLAYER) != 0) {
        LOG_DBG("Ignoring interface: %s", interface_name);
        mtx_unlock(&mod->lock);
        return true;
    }

    /* Make sure we reset the position on metadata change unless the
     * update contains its own position value */
    bool should_reset_position = true;
    while ((current_type = dbus_message_iter_get_arg_type(&changed_properties_iter)) != DBUS_TYPE_INVALID) {
        DBusMessageIter dict_iter = {0};
        dbus_message_iter_recurse(&changed_properties_iter, &dict_iter);

        const char *property_name = NULL;
        dbus_message_iter_get_basic(&dict_iter, &property_name);
        assert(dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_STRING);

        dbus_message_iter_next(&dict_iter);
        current_type = dbus_message_iter_get_arg_type(&dict_iter);
        assert(dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_VARIANT);

        mpris_property_parse(&m->client.property, property_name, &dict_iter);

        if (strcmp(property_name, "PlaybackStatus") == 0) {
            if (strcmp(m->client.property.playback_status, "Stopped") == 0) {
                m->client.status = MPRIS_STATUS_STOPPED;
            } else if (strcmp(m->client.property.playback_status, "Playing") == 0) {
                clock_gettime(CLOCK_MONOTONIC, &m->client.seeked_when);
                m->client.status = MPRIS_STATUS_PLAYING;
            } else if (strcmp(m->client.property.playback_status, "Paused") == 0) {
                /* Update our position to include the elapsed time */
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                m->client.status = MPRIS_STATUS_PAUSED;
                m->client.property.position_us += timespec_diff_us(&now, &m->client.seeked_when);
            }
        }

        if (strcmp(property_name, "Metadata") == 0 && should_reset_position) {
            m->client.property.position_us = 0;
        }

        if (strcmp(property_name, "Position") == 0) {
            should_reset_position = false;
        }

        dbus_message_iter_next(&changed_properties_iter);
    }

    mtx_unlock(&mod->lock);
    return true;
}

static void
listener_event_handle_name_owner_changed(DBusConnection *connection, DBusMessage *message,
                                         struct mpris_listener_context *listener)
{
    /* NameOwnerChanged (STRING name, STRING old_owner, STRING new_owner) */
    /* This signal indicates that the owner of a name has changed, ie.
     * it was acquired, lost or changed */

    const char *bus_name = NULL, *old_owner = NULL, *new_owner = NULL;
    DBusError error = {0};
    dbus_error_init(&error);
    if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &bus_name, DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID)) {
        /* TODO: If the 'NameOwnerChanged' signal is malformed,
         * something went really wrong and we should probably abort ... */
        LOG_ERR("%s", error.message);
        dbus_error_free(&error);
        return;
    }

    /*LOG_DBG("listener: 'NameOwnerChanged': bus_name: '%s' old_owner: '%s' new_ower: '%s'", bus_name, old_owner,
     * new_owner);*/

    if (strcmp(bus_name, listener->bus_name_unique) != 0) {
        return;
    }

    if (new_owner == NULL || strlen(new_owner) == 0) {
        /* Target bus has been lost */
        LOG_DBG("Target bus disappeared: %s", listener->bus_name);
        free(listener->bus_name_unique);
        free(listener->bus_name);
        listener->bus_name_unique = NULL;
        listener->bus_name = NULL;
        listener->has_target = false;
        return;
    } else if (old_owner == NULL || strlen(old_owner) == 0) {
        /* New name registered. At this point our target already
         * exists, ie. this code should never be called */
        /* FIXME: Figure out if this assumption is actually correct... */
        assert(0);
    }

    /* Name changed */
    LOG_DBG("listener: 'NameOwnerChanged': Name changed from '%s' to '%s'", old_owner, new_owner);
    assert(listener->bus_name_unique != NULL);
    free(listener->bus_name_unique);
    listener->bus_name_unique = strdup(new_owner);
}

static void
listener_event_handle_name_acquired(DBusConnection *connection, DBusMessage *message,
                                    struct mpris_listener_context *listener)
{
    /* Spy on applications that requested an "MPRIS style" bus name */

    /* NameAcquired (STRING name) */
    /* " This signal is sent to a specific application when it gains ownership of a name. " */
    char *name = NULL;
    DBusError error;

    dbus_error_init(&error);
    dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    if (dbus_error_is_set(&error)) {
        LOG_ERR("listener: 'NameAcquired': %s", error.message);
        dbus_error_free(&error);
    }

    if (strncmp(name, MPRIS_BUS_NAME, strlen(MPRIS_BUS_NAME)) != 0) {
        LOG_DBG("listener: 'NameAcquired': Ignoring unrelated name: %s", name);
        return;
    }

    listener->has_target = true;
    listener->bus_name = strdup(name);

    LOG_DBG("listener: 'NameAcquired': Found potential match: %s", name);
}

static DBusHandlerResult
listener_filter_func(DBusConnection *connection, DBusMessage *message, void *userdata)
{
    struct mpris_listener_context *listener = userdata;

    const char *self = dbus_bus_get_unique_name(connection);
    const char *destination = dbus_message_get_destination(message);
    const char *member = dbus_message_get_member(message);
    const char *sender = dbus_message_get_sender(message);
    const char *path_name = dbus_message_get_path(message);

    /*LOG_DBG("listener: member: '%s' self: '%s' dest: '%s' sender: '%s' target: %s", member, self, destination,
     * sender,*/
    /*        listener->bus_name_unique);*/

    /* Wait for a bus connection */
    if (listener->bus_name == NULL) {
        if (strcmp(path_name, DBUS_PATH_DBUS) == 0 && strcmp(member, "NameAcquired") == 0) {
            listener_event_handle_name_acquired(connection, message, listener);
        }

        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(path_name, DBUS_PATH_DBUS) == 0 && strcmp(member, "NameOwnerChanged") == 0) {
        listener_event_handle_name_owner_changed(connection, message, listener);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (strcmp(sender, listener->bus_name_unique) != 0) {
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    LOG_DBG("listener: member: '%s' self: '%s' dest: '%s' sender: '%s' target: %s", member, self, destination, sender,
            listener->bus_name_unique);

    /* Copy the 'PropertiesChanged/Seeked' message, so it can be parsed
     * later on */
    if (strcmp(path_name, MPRIS_PATH) == 0
        && (strcmp(member, "PropertiesChanged") == 0 || strcmp(member, "Seeked") == 0)) {
        listener->update_message = dbus_message_copy(message);
        listener->has_update = true;
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static bool
listener_setup(struct module *mod, struct mpris_listener_context **context)
{
    DBusError error = {0};
    bool status = true;

    struct mpris_listener_context *listener = malloc(sizeof(*listener));
    (*listener) = (struct mpris_listener_context){
        .connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error),
    };
    if (dbus_error_is_set(&error)) {
        LOG_ERR("Failed to connect to the desktop bus: %s", error.message);
        return -1;
    }

    dbus_connection_add_filter(listener->connection, listener_filter_func, listener, NULL);

    /* Turn this connection into a monitor */
    DBusMessage *message
        = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_MONITORING, "BecomeMonitor");
    DBusMessageIter args_iter = {0}, array_iter = {0};
    const char *matching_rules[] = {
        /* Listen for... */
        /* ... new MPRIS clients */
        "type='signal',interface='org.freedesktop.DBus',member='NameAcquired',path='/org/freedesktop/"
        "DBus',arg0namespace='org.mpris.MediaPlayer2'",
        /* ... name changes */
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "path='/org/freedesktop/DBus'",
        /* ... property changes */
        "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged', "
        "path='/org/mpris/MediaPlayer2'",
        /* ... changes in playback position */
        "type='signal',interface='org.mpris.MediaPlayer2.Player',member='Seeked', "
        "path='/org/mpris/MediaPlayer2'",
    };

    /* "BecomeMonitor": (Rules: String[], Flags: UINT32) */
    /* https://dbus.freedesktop.org/doc/dbus-specification.html#bus-messages-become-monitor */
    /* FIXME: Most of the subsequent dbus function fail on OOM (out of memory) */
    dbus_message_iter_init_append(message, &args_iter);
    dbus_message_iter_open_container(&args_iter, DBUS_TYPE_ARRAY, "s", &array_iter);

    for (uint32_t i = 0; i < sizeof(matching_rules) / sizeof(matching_rules[0]); i++) {
        dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &matching_rules[i]);
    }

    dbus_message_iter_close_container(&args_iter, &array_iter);
    dbus_message_iter_append_basic(&args_iter, DBUS_TYPE_UINT32, &(uint32_t){0});

    DBusMessage *reply = mpris_call_method_and_block(listener->connection, message, MPRIS_QUERY_TIMEOUT);

    if (reply == NULL) {
        LOG_ERR("Failed to setup monitor connection. Your dbus implementation does not support monitoring");
        dbus_connection_close(listener->connection);
        return false;
    }

    (*context) = listener;

    dbus_connection_flush(listener->connection);
    dbus_message_unref(reply);

    return status;
}

static bool
listener_poll(struct mpris_listener_context *listener, uint32_t timeout_ms)
{
    if (!dbus_connection_read_write_dispatch(listener->connection, timeout_ms)) {
        /* Figure out what might terminate our connection (with the
         * exception of calling disconnect manually) */
        LOG_DBG("Listener: Disconnect signal has been processed");
    }

    if (dbus_connection_get_dispatch_status(listener->connection) == DBUS_DISPATCH_NEED_MEMORY) {
        /* TODO: Handle OOM */
        return false;
    }

    return true;
}

struct refresh_context {
    struct module *mod;
    int abort_fd;
    long milli_seconds;
};

static int
refresh_in_thread(void *arg)
{
    struct refresh_context *ctx = arg;
    struct module *mod = ctx->mod;

    /* Extract data from context so that we can free it */
    int abort_fd = ctx->abort_fd;
    long milli_seconds = ctx->milli_seconds;
    free(ctx);

    /*LOG_DBG("going to sleep for %ldms", milli_seconds);*/

    /* Wait for timeout, or abort signal */
    struct pollfd fds[] = {{.fd = abort_fd, .events = POLLIN}};
    int r = poll(fds, 1, milli_seconds);

    if (r < 0) {
        LOG_ERRNO("failed to poll() in refresh thread");
        return 1;
    }

    /* Aborted? */
    if (r == 1) {
        assert(fds[0].revents & POLLIN);
        /*LOG_DBG("refresh thread aborted");*/
        return 0;
    }

    /*LOG_DBG("timed refresh");*/
    mod->bar->refresh(mod->bar);

    return 0;
}

static bool
refresh_in(struct module *mod, long milli_seconds)
{
    struct private *m = mod->private;

    /* Abort currently running refresh thread */
    if (m->refresh_thread_id != 0) {
        /*LOG_DBG("aborting current refresh thread");*/

        /* Signal abort to thread */
        assert(m->refresh_abort_fd != -1);
        if (write(m->refresh_abort_fd, &(uint64_t){1}, sizeof(uint64_t)) != sizeof(uint64_t)) {
            LOG_ERRNO("failed to signal abort to refresher thread");
            return false;
        }

        /* Wait for it to finish */
        int res;
        thrd_join(m->refresh_thread_id, &res);

        /* Close and cleanup */
        close(m->refresh_abort_fd);
        m->refresh_abort_fd = -1;
        m->refresh_thread_id = 0;
    }

    /* Create a new eventfd, to be able to signal abort to the thread */
    int abort_fd = eventfd(0, EFD_CLOEXEC);
    if (abort_fd == -1) {
        LOG_ERRNO("failed to create eventfd");
        return false;
    }

    /* Thread context */
    struct refresh_context *ctx = malloc(sizeof(*ctx));
    ctx->mod = mod;
    ctx->abort_fd = m->refresh_abort_fd = abort_fd;
    ctx->milli_seconds = milli_seconds;

    /* Create thread */
    int r = thrd_create(&m->refresh_thread_id, &refresh_in_thread, ctx);

    if (r != thrd_success) {
        LOG_ERR("failed to create refresh thread");
        close(m->refresh_abort_fd);
        m->refresh_abort_fd = -1;
        m->refresh_thread_id = 0;
        free(ctx);
    }

    /* Detach - we don't want to have to thrd_join() it */
    // thrd_detach(tid);
    return r == 0;
}

static int
run(struct module *mod)
{
    const struct bar *bar = mod->bar;
    struct private *m = mod->private;
    struct mpris_client *client = &m->client;

    /* Setup connection */
    DBusError error = {0};
    m->connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        LOG_ERR("Failed to connect to the desktop bus: %s", error.message);
        return -1;
    }

    struct mpris_listener_context *listener = NULL;
    if (!listener_setup(mod, &listener)) {
        LOG_ERR("Failed to setup listener");
        return -1;
    }

    client->bus_name = mpris_find_bus_name(m->connection, m->identity);
    listener->has_target = client->bus_name != NULL;
    listener->bus_name = (listener->has_target) ? strdup(client->bus_name) : NULL;

    int ret = 0;
    bool aborted = false;
    while (ret == 0 && !aborted) {
        const uint32_t timeout_ms = 50;
        struct pollfd fds[] = {{.fd = mod->abort_fd, .events = POLLIN}};

        /* Check for abort event */
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

        /* Listen for name registrations that match our target identity */
        if (!listener->has_target) {
            listener_poll(listener, MPRIS_LISTENER_TIMEOUT);

            if (!listener->has_target)
                continue;

            if (!mpris_validate_bus_name(m->identity, listener->bus_name)) {
                LOG_DBG("Target name does not match the expected identity: %s", m->identity);
                listener->has_target = false;
                listener->bus_name = NULL;
                free(listener->bus_name);
                continue;
            }

            client->bus_name = strdup(listener->bus_name);
        }

        /* We found a match. Build an initial state by manually
         * querying the client */
        LOG_DBG("Found target. Performing manual update");
        update_status(mod);
        listener->bus_name_unique = mpris_get_unique_name(m->connection, client->bus_name);

        while (ret == 0 && !aborted && listener->has_target) {
            const uint32_t timeout_ms = 50;
            struct pollfd fds[] = {{.fd = mod->abort_fd, .events = POLLIN}};

            /* Check for abort event */
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

            /* Poll the listener for status updates/target changes */
            if (!listener_poll(listener, MPRIS_LISTENER_TIMEOUT)) {
                aborted = true;
                break;
            }

            /* We lost our target */
            if (!listener->has_target) {
                mpris_reset_client(client);
                bar->refresh(bar);

                continue;
            }

            /* Process dynamic updates, revieved through the listener/the
             * 'PropertiesChanged' signal */
            if (listener->has_update) {
                listener->has_update = false;
                aborted = !update_status_from_message(mod, listener->update_message);
                /*aborted = !update_status(mod);*/
                dbus_message_unref(listener->update_message);
                listener->update_message = NULL;
            }

            if (aborted) {
                client->status = MPRIS_STATUS_OFFLINE;
            }

            bar->refresh(bar);
        }
    }

    dbus_connection_close(m->connection);
    dbus_connection_close(listener->connection);

    if (listener->bus_name_unique != NULL)
        free(listener->bus_name_unique);
    if (listener->bus_name != NULL)
        free(listener->bus_name);
    free(listener);

    return ret;
}

static struct module *
mpris_new(const char *identity, uint32_t poll, struct particle *label)
{
    struct private *priv = calloc(1, sizeof(*priv));
    priv->label = label;
    priv->identity = strdup(identity);
    priv->query_timeout_ms = poll;

    struct module *mod = module_common_new();
    mod->private = priv;
    mod->run = &run;
    mod->destroy = &destroy;
    mod->content = &content;
    mod->description = &description;
    mod->refresh_in = &refresh_in;
    return mod;
}

static struct module *
from_conf(const struct yml_node *node, struct conf_inherit inherited)
{
    const struct yml_node *identity_node = yml_get_value(node, "identity");
    const struct yml_node *query_node = yml_get_value(node, "query-timeout");
    const struct yml_node *c_node = yml_get_value(node, "content");

    const char *identity = yml_value_as_string(identity_node);
    const uint32_t query = (query_node) ? yml_value_as_int(query_node) : 500;

    return mpris_new(identity, query, conf_to_particle(c_node, inherited));
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        {"identity", true, &conf_verify_string},
        {"query-timeout", false, &conf_verify_unsigned},
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
