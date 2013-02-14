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
#include "ofonovoicecall.h"
#include "ofono-interface.h"

struct ofono_voicecall {
	gchar *path;
	OfonoInterfaceVoiceCall *remote;
	struct ofono_base *base;
	int ref_count;
	enum ofono_voicecall_state state;
	char *line_identification;
	char *incoming_line;
	char *name;
	char *start_time;
	bool multiparty;
	bool emergency;
	bool remote_held;
	bool remote_multiparty;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
};

static enum ofono_voicecall_state parse_ofono_voicecall_state(const char *state)
{
	if (g_str_equal(state, "active"))
		return OFONO_VOICECALL_STATE_ACTIVE;
	else if (g_str_equal(state, "held"))
		return OFONO_VOICECALL_STATE_HELD;
	else if (g_str_equal(state, "dialing"))
		return OFONO_VOICECALL_STATE_DIALING;
	else if (g_str_equal(state, "alerting"))
		return OFONO_VOICECALL_STATE_ALERTING;
	else if (g_str_equal(state, "incoming"))
		return OFONO_VOICECALL_STATE_INCOMING;
	else if (g_str_equal(state, "waiting"))
		return OFONO_VOICECALL_STATE_WAITING;

	return OFONO_VOICECALL_STATE_DISCONNECTED;
}

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_voicecall *call = user_data;
	const char *state_str = NULL;

	g_message("[VoiceCall:%s] property %s changed", call->path, name);

	if (g_str_equal(name, "LineIdentification")) {
		if (call->line_identification)
			g_free(call->line_identification);

		call->line_identification = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "IncomingLine")) {
		if (call->incoming_line)
			g_free(call->incoming_line);

		call->incoming_line = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "Name")) {
		if (call->name)
			g_free(call->name);

		call->name = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "StartTime")) {
		if (call->start_time)
			g_free(call->start_time);

		call->start_time = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "State")) {
		state_str = g_variant_get_string(value, NULL);
		call->state = parse_ofono_voicecall_state(state_str);
	}
	else if (g_str_equal(name, "Multiparty"))
		call->multiparty = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Emergency"))
		call->emergency = g_variant_get_boolean(value);
	else if (g_str_equal(name, "RemoteHeld"))
		call->remote_held = g_variant_get_boolean(value);
	else if (g_str_equal(name, "RemoteMultiparty"))
		call->remote_multiparty = g_variant_get_boolean(value);

	if (call->prop_changed_cb)
		call->prop_changed_cb(name, call->prop_changed_data);
}

struct ofono_base_funcs call_base_funcs = {
	.update_property = update_property,
	.set_property = ofono_interface_voice_call_call_set_property,
	.set_property_finish = ofono_interface_voice_call_call_set_property_finish,
	.get_properties = ofono_interface_voice_call_call_get_properties,
	.get_properties_finish = ofono_interface_voice_call_call_get_properties_finish
};

struct ofono_voicecall* ofono_voicecall_create(const gchar *path)
{
	struct ofono_voicecall *call;
	GError *error = NULL;

	call = g_try_new0(struct ofono_voicecall, 1);
	if (!call)
		return NULL;

	call->remote = ofono_interface_voice_call_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.VoiceCall interface");
		g_error_free(error);
		g_free(call);
		return NULL;
	}

	call->path = g_strdup(path);
	call->base = ofono_base_create(&call_base_funcs, call->remote, call);

	return call;
}

void ofono_voicecall_ref(struct ofono_voicecall *call)
{
	if (!call)
		return;

	__sync_fetch_and_add(&call->ref_count, 1);
}

void ofono_voicecall_unref(struct ofono_voicecall *call)
{
	if (!call)
		return;

	if (__sync_sub_and_fetch(&call->ref_count, 1))
		return;

	ofono_voicecall_free(call);
}

void ofono_voicecall_free(struct ofono_voicecall *call)
{
	if (!call)
		return;

	if (call->remote)
		g_object_unref(call->remote);

	g_free(call);
}

void ofono_voicecall_register_prop_changed_cb(struct ofono_voicecall *call,
													   ofono_property_changed_cb cb, void *data)
{
	if (!call)
		return;

	call->prop_changed_cb = cb;
	call->prop_changed_data = data;
}

const char* ofono_voicecall_get_path(struct ofono_voicecall *call)
{
	if (!call)
		return NULL;

	return call->path;
}

static void common_cb(GObject *source, GAsyncResult *res, gpointer data)
{
	struct cb_data *cbd = data;
	struct cb_data *cbd2 = cbd->user;
	ofono_base_result_cb cb = cbd->cb;
	glib_common_async_finish_cb finish_cb = cbd2->cb;
	struct ofono_voicecall *call = cbd2->user;
	struct ofono_error oerr;
	GError *error;
	gboolean success = false;

	success = finish_cb(call->remote, res, &error);
	if (success == FALSE) {
		oerr.type = OFONO_ERROR_TYPE_FAILED;
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
		goto cleanup;
	}

	cb(NULL, cbd->data);

cleanup:
	g_free(cbd2);
	g_free(cbd);
}

void ofono_voicecall_deflect(struct ofono_voicecall *call, const char *number, ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;
	struct ofono_error oerr;

	if (!call) {
		oerr.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		cb(&oerr, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_call_deflect_finish, NULL);
	cbd2->user = call;
	cbd->user = cbd2;

	ofono_interface_voice_call_call_deflect(call->remote, number, NULL, common_cb, cbd);
}

void ofono_voicecall_hangup(struct ofono_voicecall *call, ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;
	struct ofono_error oerr;

	if (!call) {
		oerr.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		cb(&oerr, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_call_hangup_finish, NULL);
	cbd2->user = call;
	cbd->user = cbd2;

	ofono_interface_voice_call_call_hangup(call->remote, NULL, common_cb, cbd);
}

void ofono_voicecall_answer(struct ofono_voicecall *call, ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;
	struct cb_data *cbd2;
	struct ofono_error oerr;

	if (!call) {
		oerr.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		cb(&oerr, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd2 = cb_data_new(ofono_interface_voice_call_call_answer_finish, NULL);
	cbd2->user = call;
	cbd->user = cbd2;

	ofono_interface_voice_call_call_answer(call->remote, NULL, common_cb, cbd);
}

const char* ofono_voicecall_get_line_identification(struct ofono_voicecall *call)
{
	if (!call)
		return NULL;

	return call->line_identification;
}

const char* ofono_voicecall_get_incoming_line(struct ofono_voicecall *call)
{
	if (!call)
		return NULL;

	return call->incoming_line;
}

const char* ofono_voicecall_get_name(struct ofono_voicecall *call)
{
	if (!call)
		return NULL;

	return call->name;
}

bool ofono_voicecall_get_mutliparty(struct ofono_voicecall *call)
{
	if (!call)
		return false;

	return call->multiparty;
}

const char* ofono_voicecall_get_start_time(struct ofono_voicecall *call)
{
	if (!call)
		return NULL;

	return call->start_time;
}

bool ofono_voicecall_get_emergency(struct ofono_voicecall *call)
{
	if (!call)
		return false;

	return call->emergency;
}

bool ofono_voicecall_get_remote_held(struct ofono_voicecall *call)
{
	if (!call)
		return false;

	return call->remote_held;
}

bool ofono_voicecall_get_remote_multiparty(struct ofono_voicecall *call)
{
	if (!call)
		return false;

	return call->remote_multiparty;
}

// vim:ts=4:sw=4:noexpandtab
