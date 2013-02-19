/* @@@LICENSE
*
* Copyright (c) 2013 Simon Busch <morphis@gravedo.de>
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

#include "glib-helpers.h"
#include "utils.h"
#include "ofonovoicecallmanager.h"
#include "ofonovoicecall.h"
#include "ofono-interface.h"

struct ofono_voicecall_manager {
	gchar *path;
	OfonoInterfaceVoiceCallManager *remote;
	struct ofono_base *base;
	int ref_count;
	GSList *emergency_numbers;
	GSList *calls;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
	ofono_base_cb calls_changed_cb;
	void *calls_changed_data;
	ofono_voicecall_manager_call_changed_cb call_added_cb;
	void *call_added_data;
	ofono_voicecall_manager_call_changed_cb call_removed_cb;
	void *call_removed_data;
};

const char* ofono_voicecall_clir_option_to_string(enum ofono_voicecall_clir_option clir)
{
	switch (clir) {
	case OFONO_VOICECALL_CLIR_OPTION_DEFAULT:
		return "default";
	case OFONO_VOICECALL_CLIR_OPTION_DISABLED:
		return "disabled";
	case OFONO_VOICECALL_CLIR_OPTION_ENABLED:
		return "enabled";
	}

	return NULL;
}

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_voicecall_manager *vm = user_data;
	char *number = NULL;
	GVariant *child;
	int n;

	g_message("[VoicecallManager:%s] property %s changed", vm->path, name);

	if (g_str_equal(name, "EmergencyNumbers")) {
		if (vm->emergency_numbers)
			g_slist_free_full(vm->emergency_numbers, g_free);

		for (n = 0; n < g_variant_n_children(value); n++) {
			child = g_variant_get_child_value(value, n);

			number = g_variant_dup_string(child, NULL);
			vm->emergency_numbers = g_slist_append(vm->emergency_numbers, number);
		}
	}

	if (vm->prop_changed_cb)
		vm->prop_changed_cb(name, vm->prop_changed_data);
}

struct ofono_base_funcs vm_base_funcs = {
	.update_property = update_property,
	.set_property = ofono_interface_voice_call_manager_call_set_property,
	.set_property_finish = ofono_interface_voice_call_manager_call_set_property_finish,
	.get_properties = ofono_interface_voice_call_manager_call_get_properties,
	.get_properties_finish = ofono_interface_voice_call_manager_call_get_properties_finish
};

struct ofono_voicecall_manager* ofono_voicecall_manager_create(const gchar *path)
{
	struct ofono_voicecall_manager *vm;
	GError *error = NULL;

	vm = g_try_new0(struct ofono_voicecall_manager, 1);
	if (!vm)
		return NULL;

	vm->remote = ofono_interface_voice_call_manager_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.VoiceCallManager interface");
		g_error_free(error);
		g_free(vm);
		return NULL;
	}

	vm->path = g_strdup(path);
	vm->base = ofono_base_create(&vm_base_funcs, vm->remote, vm);

	return vm;
}

void ofono_voicecall_manager_ref(struct ofono_voicecall_manager *vm)
{
	if (!vm)
		return;

	__sync_fetch_and_add(&vm->ref_count, 1);
}

void ofono_voicecall_manager_unref(struct ofono_voicecall_manager *vm)
{
	if (!vm)
		return;

	if (__sync_sub_and_fetch(&vm->ref_count, 1))
		return;

	ofono_voicecall_manager_free(vm);
}

void ofono_voicecall_manager_free(struct ofono_voicecall_manager *vm)
{
	if (!vm)
		return;

	if (vm->remote)
		g_object_unref(vm->remote);

	g_free(vm);
}

void ofono_voicecall_manager_register_prop_changed_cb(struct ofono_voicecall_manager *vm,
													   ofono_property_changed_cb cb, void *data)
{
	if (!vm)
		return;

	vm->prop_changed_cb = cb;
	vm->prop_changed_data = data;
}

void ofono_voicecall_manager_register_calls_changed_cb(struct ofono_voicecall_manager *vm,
												  ofono_base_cb cb, void *data)
{
	if (!vm)
		return;

	vm->calls_changed_cb = cb;
	vm->calls_changed_data = data;
}

void ofono_voicecall_manager_register_call_added_cb(struct ofono_voicecall_manager *vm,
													ofono_voicecall_manager_call_changed_cb cb, void *data)
{
	if (!vm)
		return;

	vm->call_added_cb = cb;
	vm->call_added_data = data;
}

void ofono_voicecall_manager_register_call_removed_cb(struct ofono_voicecall_manager *vm,
													ofono_voicecall_manager_call_changed_cb cb, void *data)
{
	if (!vm)
		return;

	vm->call_removed_cb = cb;
	vm->call_removed_data = data;
}

static void dial_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	struct cb_data *cbd = data;
	ofono_voicecall_manager_dial_cb cb = cbd->cb;
	struct ofono_voicecall_manager *vm = cbd->user;
	GError *error;
	const char *path = NULL;
	struct ofono_error oerr;
	gboolean success = FALSE;

	success = ofono_interface_voice_call_manager_call_dial_finish(vm->remote, &path, res, &error);
	if (success == FALSE) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, NULL, cbd->data);
		g_error_free(error);
		goto cleanup;
	}

	cb(NULL, path, cbd->data);

cleanup:
	g_free(cbd);
}

void ofono_voicecall_manager_dial(struct ofono_voicecall_manager *vm,
								  const char *number, enum ofono_voicecall_clir_option clir,
								  ofono_voicecall_manager_dial_cb cb, void *data)
{
	struct cb_data *cbd;

	cbd = cb_data_new(cb, data);
	cbd->user = vm;

	ofono_interface_voice_call_manager_call_dial(vm->remote, number,
												 ofono_voicecall_clir_option_to_string(clir),
												 NULL, dial_cb, cbd);
}

static void vm_common_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	struct cb_data *cbd = data;
	struct cb_data *cbd2 = cbd->user;
	ofono_base_result_cb cb = cbd->cb;
	glib_common_async_finish_cb finish_cb = cbd2->cb;
	struct ofono_voicecall_manager *vm = cbd2->user;
	GError *error;
	struct ofono_error oerr;
	gboolean success = FALSE;

	success = finish_cb(vm->remote, res, &error);
	if (success == FALSE) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
		goto cleanup;
	}

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd);
	g_free(cbd2);
}

void ofono_voicecall_manager_transfer(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_manager_call_transfer_finish, NULL);
	cbd2->user = vm;
	cbd->user = cbd2;

	ofono_interface_voice_call_manager_call_transfer(vm->remote, NULL, vm_common_cb, cbd);
}

void ofono_voicecall_manager_swap_calls(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_manager_call_swap_calls_finish, NULL);
	cbd2->user = vm;
	cbd->user = cbd2;

	ofono_interface_voice_call_manager_call_swap_calls(vm->remote, NULL, vm_common_cb, cbd);
}

void ofono_voicecall_manager_release_and_answer(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_manager_call_release_and_answer_finish, NULL);
	cbd2->user = vm;
	cbd->user = cbd2;

	ofono_interface_voice_call_manager_call_release_and_answer(vm->remote, NULL, vm_common_cb, cbd);
}

void ofono_voicecall_manager_release_and_swap(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_manager_call_release_and_swap_finish, NULL);
	cbd2->user = vm;
	cbd->user = cbd2;

	ofono_interface_voice_call_manager_call_release_and_swap(vm->remote, NULL, vm_common_cb, cbd);
}

void ofono_voicecall_manager_hold_and_answer(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_manager_call_hold_and_answer_finish, NULL);
	cbd2->user = vm;
	cbd->user = cbd2;

	ofono_interface_voice_call_manager_call_hold_and_answer(vm->remote, NULL, vm_common_cb, cbd);
}

void ofono_voicecall_manager_hangup_all(struct ofono_voicecall_manager *vm,
									  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_manager_call_hangup_all_finish, NULL);
	cbd2->user = vm;
	cbd->user = cbd2;

	ofono_interface_voice_call_manager_call_hangup_all(vm->remote, NULL, vm_common_cb, cbd);
}

void ofono_voicecall_manager_send_tones(struct ofono_voicecall_manager *vm, const char *tones,
									  ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_manager_call_send_tones_finish, NULL);
	cbd2->user = vm;
	cbd->user = cbd2;

	ofono_interface_voice_call_manager_call_send_tones(vm->remote, tones, NULL, vm_common_cb, cbd);
}

static void call_added_cb(OfonoInterfaceConnectionManager *source, const gchar *path,
							 GVariant *properties, gpointer user_data)
{
	struct ofono_voicecall_manager *vm = user_data;
	GSList *iter;
	struct ofono_voicecall *call;
	const char *call_path;

	for (iter = vm->calls; iter != NULL; iter = g_slist_next(iter)) {
		call = iter->data;
		call_path = ofono_voicecall_get_path(call);

		if (g_str_equal(call_path, path))
			return;
	}

	call = ofono_voicecall_create(path);
	vm->calls = g_slist_append(vm->calls, call);

	if (vm->calls_changed_cb)
		vm->calls_changed_cb(vm->calls_changed_data);

	if (vm->call_added_cb)
		vm->call_added_cb(path, vm->call_added_data);
}

static void call_removed_cb(OfonoInterfaceConnectionManager *source, const gchar *path,
							   gpointer user_data)
{
	struct ofono_voicecall_manager *vm = user_data;
	GSList *iter, *removable = NULL;
	struct ofono_voicecall *call;
	const char *call_path;

	for (iter = vm->calls; iter != NULL; iter = g_slist_next(iter)) {
		call = iter->data;
		call_path = ofono_voicecall_get_path(call);

		if (g_str_equal(call_path, path)) {
			removable = iter;
			break;
		}
	}

	if (removable) {
		call = removable->data;
		ofono_voicecall_free(call);
		vm->calls = g_slist_remove(vm->calls, removable);
	}

	if (vm->calls_changed_cb)
		vm->calls_changed_cb(vm->calls_changed_data);

	if (vm->call_removed_cb)
		vm->call_removed_cb(path, vm->call_removed_data);
}

static void get_calls_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_voicecall_manager *vm = cbd->user;
	ofono_voicecall_manager_get_calls_cb cb = cbd->cb;
	struct ofono_error oerr;
	GError *error = NULL;
	gboolean success = FALSE;
	GVariant *calls_v, *call_v, *path_v;
	const char *path = NULL;
	int n;
	struct ofono_voicecall *call;

	success = ofono_interface_voice_call_manager_call_get_calls_finish(vm->remote, &calls_v,
																		  res, &error);
	if (!success) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, NULL, cbd->data);
		goto cleanup;
	}

	for (n = 0; n < g_variant_n_children(calls_v); n++) {
		call_v = g_variant_get_child_value(calls_v, n);
		path_v = g_variant_get_child_value(call_v, 0);
		path = g_variant_get_string(path_v, NULL);

		call = ofono_voicecall_create(path);

		vm->calls = g_slist_append(vm->calls, call);
	}

	/* As we're not called a second time connect here to any possible call updates */
	g_signal_connect(G_OBJECT(vm->remote), "call-added",
		G_CALLBACK(call_added_cb), vm);
	g_signal_connect(G_OBJECT(vm->remote), "call-removed",
		G_CALLBACK(call_removed_cb), vm);

	cb(NULL, vm->calls, cbd->data);

cleanup:
	g_free(cbd);
}

void ofono_voicecall_manager_get_calls(struct ofono_voicecall_manager *vm,
										ofono_voicecall_manager_get_calls_cb cb, void *data)
{
	struct cb_data *cbd;
	struct ofono_error error;

	if (!vm) {
		error.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		error.message = NULL;
		cb(&error, NULL, data);
		return;
	}

	/* Not the first time, so just return known calls */
	if (g_slist_length(vm->calls) > 0) {
		cb(NULL, vm->calls, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = vm;

	ofono_interface_voice_call_manager_call_get_calls(vm->remote, NULL, get_calls_cb, cbd);
}

GSList* ofono_voicecall_manager_get_emergency_numbers(struct ofono_voicecall_manager *vm)
{
	if (!vm)
		return NULL;

	return vm->emergency_numbers;
}

// vim:ts=4:sw=4:noexpandtab
