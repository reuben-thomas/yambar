#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include <poll.h>
#include <tllist.h>

#include <sys/eventfd.h>

#include <dbus/dbus.h>

#define LOG_MODULE "mpris"
#define LOG_ENABLE_DBG 1
#include "../bar/bar.h"
#include "../config-verify.h"
#include "../config.h"
#include "../log.h"
#include "../plugin.h"

#define MPRIS_QUERY_TIMEOUT 50
#define MPRIS_EVENT_TIMEOUT 100

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

struct mpris_client {
    bool has_seeked_support;
    enum mpris_status status;
    const char *bus_name;
    const char *bus_unique_name;

    struct mpris_property property;

    /* The unix timestamp of the last position change (ie.
     * seeking, pausing) */
    struct timespec seeked_when;
};

struct mpris_context {
    DBusConnection *monitor_connection;
    DBusMessage *update_message;

    /* FIXME: There is no nice way to pass the desired identities to
     * the event handler for validation. */
    const char **identities_ref;
    const size_t identities_count;

    tll(struct mpris_client *) clients;
    struct mpris_client *current_client;

    bool has_update;
};

struct private
{
    thrd_t refresh_thread_id;
    int refresh_abort_fd;

    size_t identities_count;
    const char **identities;
    struct particle *label;

    struct mpris_context context;
};

static void
property_reset(struct mpris_property *property)
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

static bool
client_free(struct mpris_client *client)
{
    property_reset(&client->property);

    free((void *)client->bus_name);
    free((void *)client->bus_unique_name);
    free(client);

    return true;
}

static bool
clients_free_by_unique_name(struct mpris_context *context, const char *unique_name)
{
    tll_foreach(context->clients, it)
    {
        struct mpris_client *client = it->item;
        if (strcmp(client->bus_unique_name, unique_name) == 0) {
            LOG_DBG("client_remove: Removing client %s", client->bus_name);
            client_free(client);
            tll_remove(context->clients, it);
        }
    }

    return true;
}

static bool
client_free_all(struct mpris_context *context)
{
    tll_foreach(context->clients, it)
    {
        client_free(it->item);
        tll_remove(context->clients, it);
    }

    return true;
}

static bool
client_add(struct mpris_context *context, const char *name, const char *unique_name)
{
    struct mpris_client *client = malloc(sizeof(*client));
    (*client) = (struct mpris_client){
        .bus_name = strdup(name),
        .bus_unique_name = strdup(unique_name),
    };

    tll_push_back(context->clients, client);
    LOG_DBG("client_add: name='%s' unique_name='%s'", name, unique_name);

    return true;
}

static struct mpris_client *
client_lookup_by_unique_name(struct mpris_context *context, const char *unique_name)
{
    tll_foreach(context->clients, it)
    {
        struct mpris_client *client = it->item;
        /*LOG_DBG("client_lookup: client '%s' against '%s'", client->bus_unique_name, unique_name);*/
        if (strcmp(client->bus_unique_name, unique_name) == 0) {
            LOG_DBG("client_lookup: unique name: %s", client->bus_unique_name);
            return client;
        }
    }

    return NULL;
}

static bool
client_change_unique_name(struct mpris_client *client, const char *new_name)
{
    if (client->bus_unique_name != NULL) {
        free((void *)client->bus_unique_name);
    }

    client->bus_unique_name = strdup(new_name);

    return true;
}

static bool
verify_bus_name(const char **idents, const size_t ident_count, const char *name)
{
    assert(idents != NULL);
    assert(name != NULL);
    assert(dbus_validate_bus_name(name, NULL));

    for (size_t i = 0; i < ident_count; i++) {
        const char *ident = idents[i];

        if (strlen(name) < strlen(MPRIS_BUS_NAME ".") + strlen(ident)) {
            continue;
        }

        const char *cmp = name + strlen(MPRIS_BUS_NAME ".");
        if (strncmp(cmp, ident, strlen(ident)) != 0) {
            continue;
        }

        return true;
    }

    return false;
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
    struct mpris_context *context = &m->context;

    client_free_all(context);

    dbus_connection_close(m->context.monitor_connection);

    module_default_destroy(mod);
    m->label->destroy(m->label);
    free(m);
}

static void
context_event_handle_name_owner_changed(DBusConnection *connection, DBusMessage *message, struct mpris_context *context)
{
    /* NameOwnerChanged (STRING name, STRING old_owner, STRING new_owner) */
    /* This signal indicates that the owner of a name has changed, ie.
     * it was acquired, lost or changed */

    const char *bus_name = NULL, *old_owner = NULL, *new_owner = NULL;
    DBusError error = {0};
    dbus_error_init(&error);
    if (!dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &bus_name, DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID)) {
        LOG_ERR("%s", error.message);
        dbus_error_free(&error);
        return;
    }

    /*LOG_DBG("event_handler: 'NameOwnerChanged': bus_name: '%s' old_owner: '%s' new_ower: '%s'", bus_name, old_owner,*/
    /*        new_owner);*/

    if (strlen(new_owner) == 0 && strlen(old_owner) > 0) {
        /* Target bus has been lost */
        struct mpris_client *client = client_lookup_by_unique_name(context, old_owner);

        if (client == NULL)
            return;

        LOG_DBG("event_handler: 'NameOwnerChanged': Target bus disappeared: %s", client->bus_name);
        clients_free_by_unique_name(context, client->bus_unique_name);

        if (context->current_client == client)
            context->current_client = NULL;

        return;
    } else if (strlen(old_owner) == 0 && strlen(new_owner) > 0) {
        /* New unique name registered. Not used */
        return;
    }

    /* Name changed */
    assert(new_owner != NULL && strlen(new_owner) > 0);
    assert(old_owner != NULL && strlen(old_owner) > 0);

    struct mpris_client *client = client_lookup_by_unique_name(context, old_owner);
    LOG_DBG("event_handler: 'NameOwnerChanged': Name changed from '%s' to '%s' for client '%s'", old_owner, new_owner,
            client->bus_name);
    client_change_unique_name(client, new_owner);
}

static void
context_event_handle_name_acquired(DBusConnection *connection, DBusMessage *message, struct mpris_context *context)
{
    /* Spy on applications that requested an "MPRIS style" bus name */

    /* NameAcquired (STRING name) */
    /* " This signal is sent to a specific application when it gains ownership of a name. " */
    char *name = NULL;
    DBusError error;

    dbus_error_init(&error);
    dbus_message_get_args(message, &error, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    if (dbus_error_is_set(&error)) {
        LOG_ERR("event_handler: 'NameAcquired': %s", error.message);
        dbus_error_free(&error);
        return;
    }

    LOG_DBG("event_handler: 'NameAcquired': name: '%s'", name);

    if (strncmp(name, MPRIS_BUS_NAME, strlen(MPRIS_BUS_NAME)) != 0) {
        LOG_DBG("event_handler: 'NameAcquired': Ignoring unrelated name: %s", name);
        return;
    }

    if (verify_bus_name(context->identities_ref, context->identities_count, name)) {
        const char *unique_name = dbus_message_get_destination(message);
        LOG_DBG("event_handler: 'NameAcquired': Acquired new client: %s unique: %s", name, unique_name);
        client_add(context, name, unique_name);
    }
}

static DBusHandlerResult
context_event_handler(DBusConnection *connection, DBusMessage *message, void *userdata)
{
    struct mpris_context *context = userdata;

    const char *member = dbus_message_get_member(message);
    const char *sender = dbus_message_get_sender(message);
    const char *path_name = dbus_message_get_path(message);

    if (tll_length(context->clients) == 0 && strcmp(member, "NameAcquired") != 0) {
        return DBUS_HANDLER_RESULT_HANDLED;
    }

#if 0
    const char *self = dbus_bus_get_unique_name(connection);
    const char *destination = dbus_message_get_destination(message);
    LOG_DBG("event_handler: member: '%s' self: '%s' dest: '%s' sender: '%s' destination: %s", member, self, destination,
            sender, destination);
#endif

    /* TODO: Allow multiple clients to connect */
    if (strcmp(path_name, DBUS_PATH_DBUS) == 0 && strcmp(member, "NameAcquired") == 0) {
        context_event_handle_name_acquired(connection, message, context);
    }

    if (strcmp(path_name, DBUS_PATH_DBUS) == 0 && strcmp(member, "NameOwnerChanged") == 0) {
        context_event_handle_name_owner_changed(connection, message, context);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    /* Copy the 'PropertiesChanged/Seeked' message, so it can be parsed
     * later on */
    if (strcmp(path_name, MPRIS_PATH) == 0
        && (strcmp(member, "PropertiesChanged") == 0 || strcmp(member, "Seeked") == 0)) {
        struct mpris_client *client = client_lookup_by_unique_name(context, sender);
        if (client == NULL)
            return DBUS_HANDLER_RESULT_HANDLED;

        LOG_DBG("event_handler: 'PropertiesChanged': name: '%s' unique_name: '%s'", client->bus_name,
                client->bus_unique_name);
        context->has_update = true;
        context->current_client = client;
        context->update_message = dbus_message_copy(message);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

static bool
context_new(struct private *m, struct mpris_context *context)
{
    DBusError error = {0};
    bool status = true;

    DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        LOG_ERR("Failed to connect to the desktop bus: %s", error.message);
        return -1;
    }

    dbus_connection_add_filter(connection, context_event_handler, context, NULL);

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

    DBusMessage *reply = mpris_call_method_and_block(connection, message, MPRIS_QUERY_TIMEOUT);

    if (reply == NULL) {
        LOG_ERR("Failed to setup monitor connection. Your dbus implementation does not support monitoring");
        dbus_connection_close(context->monitor_connection);
        return false;
    }

    struct mpris_context content = {
        .monitor_connection = connection,
        .identities_ref = m->identities,
        .identities_count = m->identities_count,
    };

    dbus_connection_flush(connection);
    dbus_message_unref(reply);

    memcpy(context, &content, sizeof(content));

    return status;
}

static bool
context_process_events(struct mpris_context *context, uint32_t timeout_ms)
{
    if (!dbus_connection_read_write_dispatch(context->monitor_connection, timeout_ms)) {
        /* Figure out what might terminate our connection (with the
         * exception of calling disconnect manually) */
        LOG_DBG("event_handler: Disconnect signal has been processed");
    }

    if (dbus_connection_get_dispatch_status(context->monitor_connection) == DBUS_DISPATCH_NEED_MEMORY) {
        /* TODO: Handle OOM */
        return false;
    }

    return true;
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

static bool
update_status_from_message(struct module *mod, DBusMessage *message)
{
    struct private *m = mod->private;
    mtx_lock(&mod->lock);

    struct mpris_client *client = m->context.current_client;

    /* Player.Seeked (UINT64 position)*/
    if (strcmp(dbus_message_get_member(message), "Seeked") == 0) {
        client->has_seeked_support = true;
        DBusMessageIter iter = {0};
        dbus_message_iter_init(message, &iter);
        dbus_message_iter_get_basic(&iter, &client->property.position_us);

        clock_gettime(CLOCK_MONOTONIC, &client->seeked_when);

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

        LOG_DBG("update_status: reding property: %s", property_name);
        mpris_property_parse(&client->property, property_name, &dict_iter);

        if (strcmp(property_name, "PlaybackStatus") == 0) {
            if (strcmp(client->property.playback_status, "Stopped") == 0) {
                client->status = MPRIS_STATUS_STOPPED;
            } else if (strcmp(client->property.playback_status, "Playing") == 0) {
                clock_gettime(CLOCK_MONOTONIC, &client->seeked_when);
                client->status = MPRIS_STATUS_PLAYING;
            } else if (strcmp(client->property.playback_status, "Paused") == 0) {
                /* Update our position to include the elapsed time */
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                client->status = MPRIS_STATUS_PAUSED;
                client->property.position_us += timespec_diff_us(&now, &client->seeked_when);
            }
        }

        /* Make sure to reset the position upon metadata/song changes */
        if (should_reset_position && strcmp(property_name, "Metadata") == 0) {
            client->property.position_us = 0;

            if (client->property.playback_status == NULL) {
                client->property.playback_status = "Paused";
                client->status = MPRIS_STATUS_PAUSED;
            }
        }

        if (strcmp(property_name, "Position") == 0) {
            should_reset_position = false;
        }

        dbus_message_iter_next(&changed_properties_iter);
    }

    mtx_unlock(&mod->lock);
    return true;
}

static struct exposable *
content_empty(struct module *mod)
{
    struct private *m = mod->private;

    struct tag_set tags = {
        .tags = (struct tag *[]){
            tag_new_bool(mod, "has-seeked-support", "false"),
            tag_new_string(mod, "state", "offline"),
            tag_new_bool(mod, "shuffle", "false"),
            tag_new_string(mod, "loop", "None"),
            tag_new_int_range(mod, "volume", 0, 0, 100),
            tag_new_string(mod, "album", ""),
            tag_new_string(mod, "artist", ""),
            tag_new_string(mod, "title", ""),
            tag_new_string(mod, "pos", ""),
            tag_new_string(mod, "end", ""),
            tag_new_int_realtime(
                mod, "elapsed", 0, 0, 0, TAG_REALTIME_NONE),
        },
        .count = 10,
    };

    mtx_unlock(&mod->lock);

    struct exposable *exposable = m->label->instantiate(m->label, &tags);

    tag_set_destroy(&tags);
    return exposable;
}

static struct exposable *
content(struct module *mod)
{
    const struct private *m = mod->private;
    const struct mpris_client *client = m->context.current_client;

    if (client == NULL) {
        return content_empty(mod);
    }

    const struct mpris_metadata *metadata = &client->property.metadata;
    const struct mpris_property *property = &client->property;

    /* Calculate the current playback position */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    uint64_t elapsed_us = client->property.position_us;
    uint64_t length_us = metadata->length_us;

    if (client->has_seeked_support && client->status == MPRIS_STATUS_PLAYING) {
        elapsed_us += timespec_diff_us(&now, &client->seeked_when);
        if (elapsed_us > length_us) {
            LOG_DBG("dynamic update of elapsed overflowed: "
                    "elapsed=%" PRIu64 ", duration=%" PRIu64,
                    elapsed_us, length_us);
            elapsed_us = length_us;
        }
    }

    /* Some clients can report misleading or incomplete updates to the
     * playback position, potentially causing the position to exceed
     * the length */
    if (elapsed_us > length_us)
        elapsed_us = length_us = 0;

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
    const char *tag_album_value = (metadata->album == NULL) ? "" : metadata->album;
    const char *tag_artists_value = (metadata->artists == NULL) ? "" : metadata->artists;
    const char *tag_title_value = (metadata->title == NULL) ? "" : metadata->title;
    const uint32_t tag_volume_value = (property->volume >= 0.995) ? 100 : 100 * property->volume;
    const bool tag_shuffle_value = property->shuffle;
    const enum tag_realtime_unit realtime_unit = (client->has_seeked_support && client->status == MPRIS_STATUS_PLAYING)
                                                     ? TAG_REALTIME_MSECS
                                                     : TAG_REALTIME_NONE;

    struct tag_set tags = {
        .tags = (struct tag *[]){
            tag_new_bool(mod, "has_seeked_support", client->has_seeked_support),
            tag_new_bool(mod, "shuffle", tag_shuffle_value),
            tag_new_int_range(mod, "volume", tag_volume_value, 0, 100),
            tag_new_string(mod, "album", tag_album_value),
            tag_new_string(mod, "artist", tag_artists_value),
            tag_new_string(mod, "end", tag_end_value),
            tag_new_string(mod, "loop", tag_loop_value),
            tag_new_string(mod, "pos", tag_pos_value),
            tag_new_string(mod, "state", tag_state_value),
            tag_new_string(mod, "title", tag_title_value),
            tag_new_int_realtime(
                mod, "elapsed", elapsed_us, 0, length_us, realtime_unit),
        },
        .count = 11,
    };

    mtx_unlock(&mod->lock);

    struct exposable *exposable = m->label->instantiate(m->label, &tags);

    tag_set_destroy(&tags);
    return exposable;
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

    LOG_DBG("timed refresh");
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

    if (!context_new(m, &m->context)) {
        LOG_ERR("Failed to setup context");
        return -1;
    }

    struct mpris_context *context = &m->context;

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

        if (!context_process_events(context, MPRIS_EVENT_TIMEOUT)) {
            aborted = true;
            break;
        }

        /* Process dynamic updates, revieved through the contexts
         * monitor connection */
        if (context->has_update) {
            assert(context->current_client != NULL);

            context->has_update = false;
            aborted = !update_status_from_message(mod, context->update_message);
            dbus_message_unref(context->update_message);
            context->update_message = NULL;
        }

        bar->refresh(bar);
    }

    return ret;
}

static const char *
description(const struct module *mod)
{
    return "mpris";
}

static struct module *
mpris_new(const char **ident, size_t ident_count, struct particle *label)
{
    struct private *priv = calloc(1, sizeof(*priv));
    priv->label = label;
    priv->identities = malloc(sizeof(*ident) * ident_count);
    priv->identities_count = ident_count;

    for (size_t i = 0; i < ident_count; i++) {
        priv->identities[i] = strdup(ident[i]);
    }

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
    const struct yml_node *ident_list = yml_get_value(node, "identities");
    const struct yml_node *c = yml_get_value(node, "content");

    const size_t ident_count = yml_list_length(ident_list);
    const char *ident[ident_count];
    size_t i = 0;
    for (struct yml_list_iter iter = yml_list_iter(ident_list); iter.node != NULL; yml_list_next(&iter), i++) {
        ident[i] = yml_value_as_string(iter.node);
    }

    return mpris_new(ident, ident_count, conf_to_particle(c, inherited));
}

static bool
conf_verify_indentities(keychain_t *chain, const struct yml_node *node)
{
    return conf_verify_list(chain, node, &conf_verify_string);
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        {"identities", true, &conf_verify_indentities},
        MODULE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct module_iface module_mpris_iface = {
    .verify_conf = &verify_conf,
    .from_conf = &from_conf,
};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct module_iface iface __attribute__((weak, alias("module_mpris_iface")));
#endif
