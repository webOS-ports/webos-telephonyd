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
#include "ofonoconnectionmanager.h"
#include "ofonoconnectioncontext.h"
#include "ofono-interface.h"

struct ofono_connection_manager {
	gchar *path;
	OfonoInterfaceConnectionManager *remote;
	struct ofono_base *base;
	GCancellable *cancellable;
	int ref_count;
	gboolean contexts_fetched;
	bool attached;
	bool powered;
	bool suspended;
	bool roaming_allowed;
	enum ofono_connection_bearer bearer;
	GSList *contexts;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
	ofono_base_cb contexts_changed_cb;
	void *contexts_changed_data;
};

static enum ofono_connection_bearer parse_ofono_connection_bearer(const char *bearer)
{
	if (g_str_equal(bearer, "gprs"))
		return OFONO_CONNECTION_BEARER_GPRS;
	else if (g_str_equal(bearer, "edge"))
		return OFONO_CONNECTION_BEARER_EDGE;
	else if (g_str_equal(bearer, "umts"))
		return OFONO_CONNECTION_BEARER_UMTS;
	else if (g_str_equal(bearer, "hsupa"))
		return OFONO_CONNECTION_BEARER_HSUPA;
	else if (g_str_equal(bearer, "hsdpa"))
		return OFONO_CONNECTION_BEARER_HSDPA;
	else if (g_str_equal(bearer, "hspa"))
		return OFONO_CONNECTION_BEARER_HSPA;
	else if (g_str_equal(bearer, "lte"))
		return OFONO_CONNECTION_BEARER_LTE;

	return OFONO_CONNECTION_BEARER_UNKNOWN;
}

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_connection_manager *cm = user_data;
	const char *bearer = NULL;

	g_message("[ConnectionManager:%s] property %s changed", cm->path, name);

	if (g_str_equal(name, "Powered"))
		cm->powered = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Suspended"))
		cm->suspended = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Attached"))
		cm->attached = g_variant_get_boolean(value);
	else if (g_str_equal(name, "RoamingAllowed"))
		cm->roaming_allowed = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Bearer")) {
		bearer = g_variant_get_string(value, NULL);
		cm->bearer = parse_ofono_connection_bearer(bearer);
	}

	if (cm->prop_changed_cb)
		cm->prop_changed_cb(name, cm->prop_changed_data);
}

struct ofono_base_funcs cm_base_funcs = {
	.update_property = update_property,
	.set_property = (ofono_base_set_property_fn) ofono_interface_connection_manager_call_set_property,
	.set_property_finish = (ofono_base_set_property_finish_fn) ofono_interface_connection_manager_call_set_property_finish,
	.get_properties = (ofono_base_get_properties_fn) ofono_interface_connection_manager_call_get_properties,
	.get_properties_finish = (ofono_base_get_properties_finish_fn) ofono_interface_connection_manager_call_get_properties_finish
};

static void context_added_cb(OfonoInterfaceConnectionManager *source, const gchar *path,
							 GVariant *properties, gpointer user_data);
static void context_removed_cb(OfonoInterfaceConnectionManager *source, const gchar *path,
							   gpointer user_data);

struct ofono_connection_manager* ofono_connection_manager_create(const gchar *path)
{
	struct ofono_connection_manager *cm;
	GError *error = NULL;

	cm = g_try_new0(struct ofono_connection_manager, 1);
	if (!cm)
		return NULL;

	cm->remote = ofono_interface_connection_manager_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_critical("Unable to initialize proxy for the org.ofono.ConnectionManager interface");
		g_error_free(error);
		g_free(cm);
		return NULL;
	}

	cm->path = g_strdup(path);
	cm->cancellable = g_cancellable_new();
	cm->base = ofono_base_create(&cm_base_funcs, cm->remote, cm);

	/* Connect once here; connecting from get_contexts_cb stacked one handler
	 * per GetContexts round trip since an empty context list is re-queried. */
	g_signal_connect(G_OBJECT(cm->remote), "context-added",
		G_CALLBACK(context_added_cb), cm);
	g_signal_connect(G_OBJECT(cm->remote), "context-removed",
		G_CALLBACK(context_removed_cb), cm);

	return cm;
}

void ofono_connection_manager_ref(struct ofono_connection_manager *cm)
{
	if (!cm)
		return;

	__sync_fetch_and_add(&cm->ref_count, 1);
}

void ofono_connection_manager_unref(struct ofono_connection_manager *cm)
{
	if (!cm)
		return;

	if (__sync_sub_and_fetch(&cm->ref_count, 1))
		return;

	ofono_connection_manager_free(cm);
}

void ofono_connection_manager_free(struct ofono_connection_manager *cm)
{
	if (!cm)
		return;

	/* In-flight async replies still hold a proxy ref, so the signal
	 * handlers do not die with our unref below and must go explicitly. */
	if (cm->remote)
		g_signal_handlers_disconnect_by_data(cm->remote, cm);

	g_cancellable_cancel(cm->cancellable);
	g_object_unref(cm->cancellable);

	if (cm->base)
		ofono_base_free(cm->base);

	if (cm->remote)
		g_object_unref(cm->remote);

	g_slist_free_full(cm->contexts, (GDestroyNotify) ofono_connection_context_free);
	g_free(cm->path);
	g_free(cm);
}

void ofono_connection_manager_register_prop_changed_cb(struct ofono_connection_manager *cm,
													   ofono_property_changed_cb cb, void *data)
{
	if (!cm)
		return;

	cm->prop_changed_cb = cb;
	cm->prop_changed_data = data;
}

void ofono_connection_manager_register_contexts_changed_cb(struct ofono_connection_manager *cm,
												  ofono_base_cb cb, void *data)
{
	if (!cm)
		return;

	cm->contexts_changed_cb = cb;
	cm->contexts_changed_data = data;
}

static void deactivate_all_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_base_result_cb cb = cbd->cb;
	struct ofono_error oerr;
	gboolean success = FALSE;
	GError *error = NULL;

	/* Finish against the proxy GIO hands back; the manager can already be
	 * gone when the reply lands. */
	success = ofono_interface_connection_manager_call_deactivate_all_finish(
		OFONO_INTERFACE_CONNECTION_MANAGER(source), res, &error);
	if (!success) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
		goto cleanup;
	}

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd);
}

void ofono_connection_manager_deactivate_all(struct ofono_connection_manager *cm, ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct ofono_error error;

	if (!cm) {
		error.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		error.message = "No connection manager available";
		cb(&error, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = cm;

	ofono_interface_connection_manager_call_deactivate_all(cm->remote, NULL, deactivate_all_cb, cbd);
}

static void context_added_cb(OfonoInterfaceConnectionManager *source, const gchar *path,
							 GVariant *properties, gpointer user_data)
{
	struct ofono_connection_manager *cm = user_data;
	GSList *iter;
	struct ofono_connection_context *context;
	const char *context_path;

	for (iter = cm->contexts; iter != NULL; iter = g_slist_next(iter)) {
		context = iter->data;
		context_path = ofono_connection_context_get_path(context);

		if (g_str_equal(context_path, path))
			return;
	}

	context = ofono_connection_context_create(path);
	cm->contexts = g_slist_append(cm->contexts, context);

	if (cm->contexts_changed_cb)
		cm->contexts_changed_cb(cm->contexts_changed_data);
}

static void context_removed_cb(OfonoInterfaceConnectionManager *source, const gchar *path,
							   gpointer user_data)
{
	struct ofono_connection_manager *cm = user_data;
	GSList *iter, *removable = NULL;
	struct ofono_connection_context *context;
	const char *context_path;

	for (iter = cm->contexts; iter != NULL; iter = g_slist_next(iter)) {
		context = iter->data;
		context_path = ofono_connection_context_get_path(context);

		if (g_str_equal(context_path, path)) {
			removable = iter;
			break;
		}
	}

	if (removable) {
		context = removable->data;
		ofono_connection_context_free(context);
		/* g_slist_remove() matches on data; removable is the link itself */
		cm->contexts = g_slist_delete_link(cm->contexts, removable);
	}

	if (cm->contexts_changed_cb)
		cm->contexts_changed_cb(cm->contexts_changed_data);
}

static gboolean connection_manager_has_context(struct ofono_connection_manager *cm, const char *path)
{
	GSList *iter;

	for (iter = cm->contexts; iter != NULL; iter = g_slist_next(iter)) {
		if (g_strcmp0(ofono_connection_context_get_path(iter->data), path) == 0)
			return TRUE;
	}

	return FALSE;
}

static void get_contexts_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_connection_manager *cm = cbd->user;
	ofono_connection_manager_get_contexts_cb cb = cbd->cb;
	struct ofono_error oerr;
	GError *error = NULL;
	gboolean success = FALSE;
	GVariant *contexts_v = NULL, *context_v, *path_v;
	const char *path = NULL;
	gsize n;
	struct ofono_connection_context *context;

	success = ofono_interface_connection_manager_call_get_contexts_finish(
		OFONO_INTERFACE_CONNECTION_MANAGER(source), &contexts_v, res, &error);
	if (!success) {
		/* This covers cancellation (the object being freed mid-flight):
		 * the caller is always answered so pending requests further up
		 * are released, not leaked. Cancelled errors must not touch cm. */
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, NULL, cbd->data);
		g_error_free(error);
		goto cleanup;
	}

	for (n = 0; n < g_variant_n_children(contexts_v); n++) {
		context_v = g_variant_get_child_value(contexts_v, n);
		path_v = g_variant_get_child_value(context_v, 0);
		path = g_variant_get_string(path_v, NULL);

		/* The context-added signal can have raced us here */
		if (!connection_manager_has_context(cm, path)) {
			context = ofono_connection_context_create(path);
			if (context)
				cm->contexts = g_slist_append(cm->contexts, context);
		}

		g_variant_unref(path_v);
		g_variant_unref(context_v);
	}

	g_variant_unref(contexts_v);

	cb(NULL, cm->contexts, cbd->data);

cleanup:
	g_free(cbd);
}

void ofono_connection_manager_get_contexts(struct ofono_connection_manager *cm, ofono_connection_manager_get_contexts_cb cb, void *data)
{
	struct cb_data *cbd;
	struct ofono_error error;

	if (!cm) {
		error.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		error.message = "No connection manager available";
		cb(&error, NULL, data);
		return;
	}

	/* Not the first time, so just return known contexts; the signal handlers
	 * connected in create keep the list current from here on */
	if (cm->contexts_fetched) {
		cb(NULL, cm->contexts, data);
		return;
	}

	cm->contexts_fetched = TRUE;

	cbd = cb_data_new(cb, data);
	cbd->user = cm;

	ofono_interface_connection_manager_call_get_contexts(cm->remote, cm->cancellable, get_contexts_cb, cbd);
}

bool ofono_connection_manager_get_attached(struct ofono_connection_manager *cm)
{
	if (!cm)
		return false;

	return cm->attached;
}

enum ofono_connection_bearer ofono_connection_manager_get_bearer(struct ofono_connection_manager *cm)
{
	if (!cm)
		return OFONO_CONNECTION_BEARER_UNKNOWN;

	return cm->bearer;
}

bool ofono_connection_manager_get_powered(struct ofono_connection_manager *cm)
{
	if (!cm)
		return false;

	return cm->powered;
}

bool ofono_connection_manager_get_suspended(struct ofono_connection_manager *cm)
{
	if (!cm)
		return false;

	return cm->suspended;
}

bool ofono_connection_manager_get_roaming_allowed(struct ofono_connection_manager *cm)
{
	if (!cm)
		return false;

	return cm->roaming_allowed;
}

static void set_roaming_allowed_cb(struct ofono_error *error, void *data)
{
	if (!data)
		return;

	struct cb_data *cbd = data;
	ofono_base_result_cb cb = cbd->cb;

	cb(error, cbd->data);
	g_free(cbd);
}

void ofono_connection_manager_set_roaming_allowed(struct ofono_connection_manager *cm, bool roaming_allowed,
												  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	GVariant *value;
	struct ofono_error oerr;

	if (!cm) {
		oerr.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		oerr.message = "No connection manager available";
		cb(&oerr, data);
		return;
	}

	cbd = cb_data_new(cb, data);

	value = g_variant_new_variant(g_variant_new_boolean(roaming_allowed));
	ofono_base_set_property(cm->base, "RoamingAllowed",
							value, set_roaming_allowed_cb, cbd);
}

// vim:ts=4:sw=4:noexpandtab
