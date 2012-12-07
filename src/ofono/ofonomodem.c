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
#include "ofonomodem.h"
#include "ofono-interface.h"

struct ofono_modem {
	gchar *path;
	OfonoInterfaceModem *remote;
	gboolean powered;
	gboolean online;
	gboolean lockdown;
	gboolean emergency;
	gchar *name;
};

static void set_property_cb(GDBusConnection *connection, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_result_cb cb = cbd->cb;
	struct ofono_modem *modem = cbd->user;
	gboolean result = FALSE;
	GError *error = NULL;

	result = ofono_interface_modem_call_set_property_finish(modem->remote, res, &error);
	if (error) {
		g_error("Failed to retrieve properties from modem: %s", error->message);
		g_error_free(error);
	}

	cb(result, cbd->data);
	g_free(cbd);
}

static void set_property(struct ofono_modem *modem, const gchar *name, const GVariant *value,
						 ofono_modem_result_cb cb, gpointer user_data)
{
	struct cb_data *cbd = cb_data_new(cb, user_data);
	cbd->user = modem;

	ofono_interface_modem_call_set_property(modem->remote, NULL, name, value, set_property_cb, cbd);
}

static void update_property(struct ofono_modem *modem, const gchar *name, const GVariant *value)
{
	g_message("[Modem:%s] property %s changed", modem->path, name);

	if (g_str_equal(name, "Powered"))
		modem->powered = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Online"))
		modem->online = g_variant_get_boolean(value);
	else if (g_str_equal(name, "LockDown"))
		modem->lockdown = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Emergency"))
		modem->emergency = g_variant_get_boolean(value);
	else if (g_str_equal(value, "Name"))
		modem->name = g_variant_dup_string(value, NULL);
}

static void get_properties_cb(GDBusConnection *connection, GAsyncResult *res, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	GError *error = NULL;
	gboolean ret = FALSE;
	GVariant *properties = NULL;
	gchar *property_name = NULL;
	GVariant *property_value = NULL;
	GVariantIter iter;

	ret = ofono_interface_modem_call_get_properties_finish(modem->remote, &properties, res, &error);
	if (error) {
		g_error("Failed to retrieve properties from modem: %s", error->message);
		g_error_free(error);
		return;
	}

	g_variant_iter_init(&iter, properties);
	while (g_variant_iter_loop(&iter, "{sv}", &property_name, &property_value)) {
		update_property(modem, property_name, property_value);
	}
}

static void property_changed_cb(OfonoInterfaceModem *object, const gchar *name, GVariant *value, gpointer *user_data)
{
	struct ofono_modem *modem = user_data;

	g_debug("[Modem:%s] property %s changed", modem->path, name);

	update_property(modem, name, value);
}

struct ofono_modem* ofono_modem_create(const gchar *path)
{
	struct ofono_modem *modem;
	GError *error = NULL;

	modem = g_try_new0(struct ofono_modem, 1);
	if (!modem)
		return NULL;

	modem->remote = ofono_interface_modem_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.modem interface");
		g_error_free(error);
		g_free(modem);
		return NULL;
	}

	modem->path = g_strdup(path);

	modem->powered = FALSE;
	modem->online = FALSE;
	modem->lockdown = FALSE;
	modem->emergency = FALSE;
	modem->name = NULL;

	g_signal_connect(G_OBJECT(modem->remote), "property-changed",
		G_CALLBACK(property_changed_cb), modem);

	ofono_interface_modem_call_get_properties(modem->remote, NULL, get_properties_cb, modem);

	return modem;
}

void ofono_modem_free(struct ofono_modem *modem)
{
	if (!modem)
		return;

	if (modem->remote)
		g_object_unref(modem->remote);

	g_free(modem);
}

const gchar* ofono_modem_get_path(struct ofono_modem *modem)
{
	if (!modem)
		return NULL;

	return modem->path;
}

int ofono_modem_set_powered(struct ofono_modem *modem, gboolean powered, ofono_modem_result_cb cb, gpointer user_data)
{
	if (!modem)
		return -1;

	/* check wether we're already in the desired powered state */
	if (powered == modem->powered) {
		cb(TRUE, user_data);
		return 0;
	}

	set_property(modem, "Powered", g_variant_new_boolean(powered), cb, user_data);

	return -EINPROGRESS;
}

bool ofono_modem_get_powered(struct ofono_modem *modem)
{
	if (!modem)
		return false;

	return modem->powered;
}

// vim:ts=4:sw=4:noexpandtab

