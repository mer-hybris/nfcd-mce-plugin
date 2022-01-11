/*
 * Copyright (C) 2020 Jolla Ltd.
 * Copyright (C) 2020 Slava Monich <slava.monich@jolla.com>
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
#include <mce_display.h>

#ifdef NFC_DISABLE_IN_LOCKED
#include <mce_tklock.h>
#endif

#include <mce_log.h>
#include <gutil_log.h>

GLOG_MODULE_DEFINE("mce-plugin");

enum display_events {
    DISPLAY_VALID,
    DISPLAY_STATE,
    DISPLAY_EVENT_COUNT
};

enum manager_events {
    MANAGER_ENABLED,
    MANAGER_EVENT_COUNT
};

#ifdef NFC_DISABLE_IN_LOCKED
enum tklock_events {
	TKLOCK_EVENT_VALID,
	TKLOCK_EVENT_MODE,
	TKLOCK_EVENT_COUNT
};
#endif

typedef NfcPluginClass McePluginClass;
typedef struct mce_plugin {
    NfcPlugin parent;
    NfcManager* manager;
    MceDisplay* display;
    gulong manager_event_id[MANAGER_EVENT_COUNT];
    gulong display_event_id[DISPLAY_EVENT_COUNT];
    gboolean always_on;
#ifdef NFC_DISABLE_IN_LOCKED
    gboolean nfc_disable_in_locked;
    MceTklock* tklock;
    gulong tklock_event_id[TKLOCK_EVENT_COUNT];
#endif
} McePlugin;

G_DEFINE_TYPE(McePlugin, mce_plugin, NFC_TYPE_PLUGIN)
#define THIS_TYPE (mce_plugin_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, McePlugin))

#define mce_plugin_display_on(display) \
    ((display) && (display)->valid && (display)->state != MCE_DISPLAY_STATE_OFF)

#ifdef NFC_DISABLE_IN_LOCKED
//not sure that it is enough
#define mce_plugin_unlocked_on(tklock) \
    ((tklock) && (tklock)->valid && (tklock)->mode != MCE_TKLOCK_MODE_LOCKED)
#endif

/* These need to be synchronized with the settings plugin */
#define SETTINGS_STORAGE_PATH   "/var/lib/nfcd/settings"
#define SETTINGS_GROUP          "Settings"
#define SETTINGS_KEY_ALWAYS_ON  "AlwaysOn"

static
void
mce_plugin_update_power(
    McePlugin* self)
{
    nfc_manager_request_power(self->manager, self->manager->enabled &&
        (self->always_on || mce_plugin_display_on(self->display))
#ifdef NFC_DISABLE_IN_LOCKED
        && (self->nfc_disable_in_locked && mce_plugin_unlocked_on(self->tklock))
#endif
        );
}

static
void
mce_plugin_manager_state_handler(
    NfcManager* manager,
    void* plugin)
{
    mce_plugin_update_power(THIS(plugin));
}

static
void
mce_plugin_display_state_handler(
    MceDisplay* display,
    gpointer plugin)
{
    mce_plugin_update_power(THIS(plugin));
}

#ifdef NFC_DISABLE_IN_LOCKED
static
void
mce_plugin_tklock_state_handler(
    MceTklock *tklock,
    gpointer plugin)
{
	mce_plugin_update_power(THIS(plugin));
}
#endif


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

    /* No need to track the display state if we are always on */
    if (!self->always_on) {
        self->display = mce_display_new();
        self->display_event_id[DISPLAY_VALID] =
            mce_display_add_valid_changed_handler(self->display,
                mce_plugin_display_state_handler, self);
        self->display_event_id[DISPLAY_STATE] =
            mce_display_add_state_changed_handler(self->display,
                mce_plugin_display_state_handler, self);
    }
#ifdef NFC_DISABLE_IN_LOCKED
    /* Reserved for future use with separate settings */
    self->nfc_disable_in_locked = TRUE;
	/* Track lock state */
	self->tklock = mce_tklock_new();
	self->tklock_event_id[TKLOCK_EVENT_VALID] =
		mce_tklock_add_valid_changed_handler(self->tklock,
				mce_plugin_tklock_state_handler, self);
	self->tklock_event_id[TKLOCK_EVENT_MODE] =
			mce_tklock_add_mode_changed_handler(self->tklock,
				mce_plugin_tklock_state_handler, self);
#endif
    mce_plugin_update_power(self);
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
#ifdef NFC_DISABLE_IN_LOCKED
    if (self->tklock) {
        mce_tklock_remove_all_handlers(self->tklock, self->tklock_event_id);
	    mce_tklock_unref(self->tklock);
        self->tklock = NULL;
    }
#endif
    nfc_manager_remove_all_handlers(self->manager, self->manager_event_id);
    nfc_manager_unref(self->manager);
    self->manager = NULL;
}

static
void
mce_plugin_init(
    McePlugin* self)
{
    GKeyFile* config = g_key_file_new();

    if (g_key_file_load_from_file(config, SETTINGS_STORAGE_PATH, 0, NULL)) {
        self->always_on = g_key_file_get_boolean(config, SETTINGS_GROUP,
            SETTINGS_KEY_ALWAYS_ON, NULL);
    }
    g_key_file_unref(config);
}

static
void
mce_plugin_class_init(
    McePluginClass* klass)
{
    klass->start = mce_plugin_start;
    klass->stop = mce_plugin_stop;
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
