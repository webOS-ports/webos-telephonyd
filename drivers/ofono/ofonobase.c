/* @@@LICENSE
*
* Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <string.h>
#include <errno.h>

#include <glib-object.h>
#include <gio/gio.h>

#include "utils.h"
#include "ofonobase.h"
#include "ofono-interface.h"

struct ofono_base {
	void *remote;
	void *user_data;
	gulong property_changed_signal;
	struct ofono_base_funcs *funcs;
	GCancellable *cancellable;
	int ref_count;
	gboolean dead;
};

/* An ofono interface can disappear while one of its D-Bus calls is still in
 * flight: a PIN-locked modem publishes SimManager and VoiceCallManager only,
 * and grows the rest of its interfaces the moment the PIN is accepted, so the
 * objects backing them are created and destroyed as Interfaces changes arrive.
 * The reply then landed on a freed base and dereferenced its funcs pointer,
 * which killed telephonyd with SIGSEGV in get_properties_cb.
 *
 * Keep the base alive for as long as a call can still call back, and let the
 * last one drop it. The cancellable makes the pending calls fail fast rather
 * than waiting out the D-Bus timeout; the dead flag is what the callbacks
 * check, since a cancelled call still gets its callback.
 */
static void ofono_base_ref(struct ofono_base *base)
{
	base->ref_count++;
}

static void ofono_base_unref(struct ofono_base *base)
{
	if (--base->ref_count > 0)
		return;

	if (base->cancellable)
		g_object_unref(base->cancellable);

	g_free(base);
}

static void set_property_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_base_result_cb cb = cbd->cb;
	struct ofono_base *base = cbd->user;
	gboolean success = FALSE;
	GError *error = NULL;
	struct ofono_error oerr;

	if (base->dead) {
		/* The interface is gone; still answer the caller so wrapper cb_data
		 * and pending requests further up are released, not leaked. */
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = "Interface is no longer available";
		cb(&oerr, cbd->data);
		g_free(cbd);
		ofono_base_unref(base);
		return;
	}

	success = base->funcs->set_property_finish(base->remote, res, &error);
	if (!success) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
	ofono_base_unref(base);
}

void ofono_base_set_property(struct ofono_base *base, const gchar *name, GVariant *value,
						 ofono_base_result_cb cb, gpointer user_data)
{
	struct cb_data *cbd = cb_data_new(cb, user_data);
	cbd->user = base;

	ofono_base_ref(base);
	base->funcs->set_property(base->remote, name, value, base->cancellable, set_property_cb, cbd);
}

static void handle_get_properties_result(struct ofono_base *base, GVariant *properties)
{
	gchar *property_name = NULL;
	GVariant *property_value = NULL;
	GVariantIter iter;

	g_variant_iter_init(&iter, properties);
	while (g_variant_iter_loop(&iter, "{sv}", &property_name, &property_value)) {
		base->funcs->update_property(property_name, property_value, base->user_data);
	}
}

static void get_properties_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct ofono_base *base = user_data;
	GError *error = NULL;
	gboolean success = FALSE;
	GVariant *properties = NULL;

	if (base->dead) {
		ofono_base_unref(base);
		return;
	}

	success = base->funcs->get_properties_finish(base->remote, &properties, res, &error);
	if (!success) {
		g_warning("Failed to retrieve properties from base: %s", error->message);
		g_error_free(error);
		ofono_base_unref(base);
		return;
	}

	handle_get_properties_result(base, properties);
	g_variant_unref(properties);
	ofono_base_unref(base);
}

static void property_changed_cb(void *object, const gchar *name, GVariant *value, gpointer user_data)
{
	struct ofono_base *base = user_data;
	GVariant *inner;

	/* update_property implementations treat the value as borrowed (the
	 * GetProperties path hands them g_variant_iter_loop values), so the
	 * unwrapped variant has to be released here. */
	inner = g_variant_get_variant(value);
	base->funcs->update_property(name, inner, base->user_data);
	g_variant_unref(inner);
}

struct ofono_base* ofono_base_create(struct ofono_base_funcs *funcs, void *remote, void *user_data)
{
	struct ofono_base *base;
	GError *error = NULL;
	GVariant *properties = NULL;

	base = g_try_new0(struct ofono_base, 1);
	if (!base)
		return NULL;

	base->remote = remote;
	base->user_data = user_data;
	base->funcs = funcs;
	base->cancellable = g_cancellable_new();
	base->ref_count = 1;

	base->property_changed_signal = g_signal_connect(G_OBJECT(base->remote), "property-changed",
		G_CALLBACK(property_changed_cb), base);

	if (base->funcs->get_properties) {
		ofono_base_ref(base);
		base->funcs->get_properties(base->remote, base->cancellable, get_properties_cb, base);
	}
	else {
		base->funcs->get_properties_sync(base->remote, &properties, NULL, &error);
		if (error) {
			g_warning("Failed to retrieve properties from base: %s", error->message);
			g_error_free(error);
		}
		else if (properties) {
			handle_get_properties_result(base, properties);
			g_variant_unref(properties);
		}
	}

	return base;
}

void ofono_base_free(struct ofono_base *base)
{
	if (!base)
		return;

	g_signal_handler_disconnect(G_OBJECT(base->remote), base->property_changed_signal);

	base->dead = TRUE;
	g_cancellable_cancel(base->cancellable);

	ofono_base_unref(base);
}

// vim:ts=4:sw=4:noexpandtab
