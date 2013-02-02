/* @@@LICENSE
*
* Copyright (c) 2012 networkon Busch <morphis@gravedo.de>
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
#include "ofononetworkoperator.h"
#include "ofono-interface.h"

struct ofono_network_operator {
	gchar *path;
	OfonoInterfaceNetworkOperator *remote;
	struct ofono_base *base;
	char *name;
	char *mcc;
	char *mnc;
	enum ofono_network_operator_status status;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_network_operator *netop = user_data;

	g_message("[NetworkOperator:%s] property %s changed", netop->path, name);
}

/* NOTE: We can't use async fetch of the operator properties here so setting all
 * other fields to NULL */
struct ofono_base_funcs netop_base_funcs = {
	.update_property = update_property,
	.set_property = NULL,
	.set_property_finish = NULL,
	.get_properties = NULL,
	.get_properties_finish = NULL,
	.get_properties_sync = ofono_interface_network_operator_call_get_properties_sync,
};

struct ofono_network_operator* ofono_network_operator_create(const char *path)
{
	struct ofono_network_operator *netop;
	GError *error = NULL;

	netop = g_try_new0(struct ofono_network_operator, 1);
	if (!netop)
		return NULL;

	netop->remote = ofono_interface_network_operator_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.network interface");
		g_error_free(error);
		g_free(netop);
		return NULL;
	}

	netop->path = g_strdup(path);
	netop->base = ofono_base_create(&netop_base_funcs, netop->remote, netop);

	return netop;
}

void ofono_network_operator_free(struct ofono_network_operator *netop)
{
	if (!netop)
		return;

	if (netop->remote)
		g_object_unref(netop->remote);

	if (netop->path)
		g_free(netop->path);

	if (netop->name)
		g_free(netop->name);

	if (netop->mcc)
		g_free(netop->mcc);

	if (netop->mnc)
		g_free(netop->mnc);

	g_free(netop);
}

static void register_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_network_operator *netop = cbd->user;
	ofono_base_result_cb cb = cbd->cb;
	struct ofono_error oerr;
	gboolean success;
	GError *error = NULL;

	success = ofono_interface_network_operator_call_register_finish(netop->remote, res, &error);
	if (!success) {
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
}

void ofono_network_operator_register(struct ofono_network_operator *netop, ofono_base_result_cb cb, void *user_data)
{
	struct ofono_error oerr;
	struct cb_data *cbd;

	if (!netop) {
		oerr.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		cb(&oerr, user_data);
		return;
	}

	cbd = cb_data_new(cb, user_data);

	ofono_interface_network_operator_call_register(netop->remote, NULL, register_cb, cbd);
}

const char* ofono_network_operator_get_name(struct ofono_network_operator *netop)
{
	if (!netop)
		return NULL;

	return netop->name;
}

enum ofono_network_operator_status ofono_network_operator_get_status(struct ofono_network_operator *netop)
{
	if (!netop)
		return OFONO_NETWORK_OPERATOR_STATUS_UNKNOWN;

	return netop->status;
}

const char* ofono_network_operator_get_mcc(struct ofono_network_operator *netop)
{
	if (!netop)
		return NULL;

	return netop->mcc;
}

const char* ofono_network_operator_get_mnc(struct ofono_network_operator *netop)
{
	if (!netop)
		return NULL;

	return netop->mnc;
}

// vim:ts=4:sw=4:noexpandtab
