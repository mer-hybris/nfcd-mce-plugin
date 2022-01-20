/*
 * Copyright (C) 2020-2022 Jolla Ltd.
 * Copyright (C) 2020-2022 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#define GLOG_MODULE_NAME mce_plugin_log

#include <nfc_plugin_impl.h>
#include <nfc_manager.h>
#include <nfc_config.h>

#include <mce_display.h>
#include <mce_tklock.h>
#include <mce_log.h>

#include <gutil_log.h>

GLOG_MODULE_DEFINE("mce-plugin");

enum display_events {
    DISPLAY_VALID,
    DISPLAY_STATE,
    DISPLAY_EVENT_COUNT
};

enum tklock_events {
    TKLOCK_VALID,
    TKLOCK_LOCKED,
    TKLOCK_EVENT_COUNT
};

enum manager_events {
    MANAGER_ENABLED,
    MANAGER_EVENT_COUNT
};

typedef NfcPluginClass McePluginClass;
typedef struct mce_plugin {
    NfcPlugin parent;
    NfcManager* manager;
    MceDisplay* display;
    MceTklock* tklock;
    gulong manager_event_id[MANAGER_EVENT_COUNT];
    gulong display_event_id[DISPLAY_EVENT_COUNT];
    gulong tklock_event_id[TKLOCK_EVENT_COUNT];
    gboolean require_unlock;
    gboolean always_on;
} McePlugin;

/* Configuration keys */
static const char MCE_PLUGIN_CONFIG_KEY_REQUIRE_UNLOCK[]= "RequireUnlock";
static const char MCE_PLUGIN_CONFIG_KEY_ALWAYS_ON[] = "AlwaysOn";

/* Default values */
#define MCE_PLUGIN_CONFIG_DEFAULT_REQUIRE_UNLOCK FALSE
#define MCE_PLUGIN_CONFIG_DEFAULT_ALWAYS_ON      FALSE

static void mce_plugin_config_init(NfcConfigurableInterface* iface);
G_DEFINE_TYPE_WITH_CODE(McePlugin, mce_plugin, NFC_TYPE_PLUGIN,
G_IMPLEMENT_INTERFACE(NFC_TYPE_CONFIGURABLE, mce_plugin_config_init))
#define THIS_TYPE (mce_plugin_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, McePlugin))

enum mce_plugin_signal {
     SIGNAL_CONFIG_VALUE_CHANGED,
     SIGNAL_COUNT
};

#define SIGNAL_CONFIG_VALUE_CHANGED_NAME "mce-plugin-config-value-changed"

static guint mce_plugin_signals[SIGNAL_COUNT] = { 0 };

#define mce_plugin_display_on(display) \
    ((display) && (display)->valid && (display)->state != MCE_DISPLAY_STATE_OFF)
#define mce_plugin_tk_unlocked(tklock) \
    ((tklock) && (tklock)->valid && !(tklock)->locked)

static
void
mce_plugin_update_state(
    McePlugin* self);

static
void
mce_plugin_manager_state_handler(
    NfcManager* manager,
    void* plugin)
{
    mce_plugin_update_state(THIS(plugin));
}

static
void
mce_plugin_display_state_handler(
    MceDisplay* display,
    gpointer plugin)
{
    mce_plugin_update_state(THIS(plugin));
}

static
void
mce_plugin_tklock_state_handler(
    MceTklock* tklock,
    void* plugin)
{
    mce_plugin_update_state(THIS(plugin));
}

static
void
mce_plugin_update_state(
    McePlugin* self)
{
    const gboolean track_display_state = !self->always_on;
    const gboolean track_lock_state = !self->always_on && self->require_unlock;

    if (track_display_state) {
        if (!self->display) {
            GDEBUG("Tracking display state");
            self->display = mce_display_new();
            self->display_event_id[DISPLAY_VALID] =
                mce_display_add_valid_changed_handler(self->display,
                    mce_plugin_display_state_handler, self);
            self->display_event_id[DISPLAY_STATE] =
                mce_display_add_state_changed_handler(self->display,
                    mce_plugin_display_state_handler, self);
        }
    } else if (self->display) {
        GDEBUG("Not tracking display state");
        mce_display_remove_all_handlers(self->display, self->display_event_id);
        mce_display_unref(self->display);
        self->display = NULL;
    }

    if (track_lock_state) {
        if (!self->tklock) {
            GDEBUG("Tracking lock state");
            self->tklock = mce_tklock_new();
            self->tklock_event_id[TKLOCK_VALID] =
                mce_tklock_add_valid_changed_handler(self->tklock,
                    mce_plugin_tklock_state_handler, self);
            self->tklock_event_id[TKLOCK_LOCKED] =
                mce_tklock_add_locked_changed_handler(self->tklock,
                    mce_plugin_tklock_state_handler, self);
        }
    } else if (self->tklock) {
        GDEBUG("Not tracking lock state");
        mce_tklock_remove_all_handlers(self->tklock, self->tklock_event_id);
        mce_tklock_unref(self->tklock);
        self->tklock = NULL;
    }

    nfc_manager_request_power(self->manager, self->manager->enabled &&
        (!track_display_state || mce_plugin_display_on(self->display)) &&
        (!track_lock_state || mce_plugin_tk_unlocked(self->tklock)));
}

static
gboolean
mce_plugin_set_value(
    McePlugin* self,
    const char* key,
    GVariant* value,
    gboolean* storage,
    gboolean default_value)
{
    gboolean ok = FALSE;
    gboolean newval = default_value;

    if (!value) {
        ok = TRUE;
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        newval = g_variant_get_boolean(value);
        ok = TRUE;
    }

    if (ok && *storage != newval) {
        GDEBUG("%s %s", key, newval ? "on" : "off");
        *storage = newval;
        g_signal_emit(self, mce_plugin_signals[SIGNAL_CONFIG_VALUE_CHANGED],
            g_quark_from_string(key), key, value);
        mce_plugin_update_state(self);
    }

    return ok;
}

/*==========================================================================*
 * NfcConfigurable
 *==========================================================================*/

static
const char* const*
mce_plugin_config_get_keys(
    NfcConfigurable* config)
{
    static const char* const mce_plugin_keys[] = {
        MCE_PLUGIN_CONFIG_KEY_REQUIRE_UNLOCK,
        MCE_PLUGIN_CONFIG_KEY_ALWAYS_ON,
        NULL
    };

    return mce_plugin_keys;
}

static
GVariant*
mce_plugin_config_get_value(
    NfcConfigurable* config,
    const char* key)
{
    McePlugin* self = THIS(config);

    /* OK to return a floating reference */
    if (!g_strcmp0(key, MCE_PLUGIN_CONFIG_KEY_REQUIRE_UNLOCK)) {
        return g_variant_new_boolean(self->require_unlock);
    } else if (!g_strcmp0(key, MCE_PLUGIN_CONFIG_KEY_ALWAYS_ON)) {
        return g_variant_new_boolean(self->always_on);
    } else {
        return NULL;
    }
}

static
gboolean
mce_plugin_config_set_value(
    NfcConfigurable* config,
    const char* key,
    GVariant* value)
{
    McePlugin* self = THIS(config);
    gboolean ok = FALSE;

    if (!g_strcmp0(key, MCE_PLUGIN_CONFIG_KEY_REQUIRE_UNLOCK)) {
        ok = mce_plugin_set_value(self, key, value, &self->require_unlock,
            MCE_PLUGIN_CONFIG_DEFAULT_REQUIRE_UNLOCK);
    } else if (!g_strcmp0(key, MCE_PLUGIN_CONFIG_KEY_ALWAYS_ON)) {
        ok = mce_plugin_set_value(self, key, value, &self->always_on,
            MCE_PLUGIN_CONFIG_DEFAULT_ALWAYS_ON);
    }
    return ok;
}

static
gulong
mce_plugin_config_add_change_handler(
    NfcConfigurable* config,
    const char* key,
    NfcConfigChangeFunc func,
    void* user_data)
{
    return g_signal_connect_closure_by_id(THIS(config),
        mce_plugin_signals[SIGNAL_CONFIG_VALUE_CHANGED],
        key ? g_quark_from_string(key) : 0,
        g_cclosure_new(G_CALLBACK(func), user_data, NULL), FALSE);
}

static
void
mce_plugin_config_init(
    NfcConfigurableInterface* iface)
{
    iface->get_keys = mce_plugin_config_get_keys;
    iface->get_value = mce_plugin_config_get_value;
    iface->set_value = mce_plugin_config_set_value;
    iface->add_change_handler = mce_plugin_config_add_change_handler;
}

/*==========================================================================*
 * NfcPlugin
 *==========================================================================*/

static
gboolean
mce_plugin_start(
    NfcPlugin* plugin,
    NfcManager* manager)
{
    McePlugin* self = THIS(plugin);

    GVERBOSE("Starting");
    GASSERT(!self->manager);
    self->manager = nfc_manager_ref(manager);
    self->manager_event_id[MANAGER_ENABLED] =
        nfc_manager_add_enabled_changed_handler(manager,
            mce_plugin_manager_state_handler, self);

    mce_plugin_update_state(self);
    return TRUE;
}

static
void
mce_plugin_stop(
    NfcPlugin* plugin)
{
    McePlugin* self = THIS(plugin);

    GVERBOSE("Stopping");
    if (self->display) {
        mce_display_remove_all_handlers(self->display, self->display_event_id);
        mce_display_unref(self->display);
        self->display = NULL;
    }
    if (self->tklock) {
        mce_tklock_remove_all_handlers(self->tklock, self->tklock_event_id);
        mce_tklock_unref(self->tklock);
        self->tklock = NULL;
    }
    nfc_manager_remove_all_handlers(self->manager, self->manager_event_id);
    nfc_manager_unref(self->manager);
    self->manager = NULL;
}

static
void
mce_plugin_init(
    McePlugin* self)
{
    self->require_unlock = MCE_PLUGIN_CONFIG_DEFAULT_REQUIRE_UNLOCK;
    self->always_on = MCE_PLUGIN_CONFIG_DEFAULT_ALWAYS_ON;
}

static
void
mce_plugin_class_init(
    McePluginClass* klass)
{
    GType type = G_OBJECT_CLASS_TYPE(klass);

    klass->start = mce_plugin_start;
    klass->stop = mce_plugin_stop;

    mce_plugin_signals[SIGNAL_CONFIG_VALUE_CHANGED] =
        g_signal_new(SIGNAL_CONFIG_VALUE_CHANGED_NAME, type,
            G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_VARIANT);
}

static
NfcPlugin*
mce_plugin_create(
    void)
{
    GDEBUG("Plugin loaded");
    return g_object_new(THIS_TYPE, NULL);
}

static GLogModule* const mce_plugin_logs[] = {
    &GLOG_MODULE_NAME,
    &MCE_LOG_MODULE,
    NULL
};

NFC_PLUGIN_DEFINE2(mce, "mce-base state tracking", mce_plugin_create,
    mce_plugin_logs, 0)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
