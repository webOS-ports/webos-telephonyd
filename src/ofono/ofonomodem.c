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
#include "ofonomodem.h"
#include "ofono-interface.h"

struct ofono_modem {
	gchar *path;
	OfonoInterfaceModem *remote;
	struct ofono_base *base;
	gboolean powered;
	gboolean online;
	gboolean lockdown;
	gboolean emergency;
	gchar *name;
	gchar *serial;
	gchar *revision;
	int interfaces[OFONO_MODEM_INTERFACE_MAX];
	int ref_count;
	ofono_property_changed_cb powered_changed_cb;
	void *powered_changed_data;
	ofono_property_changed_cb interfaces_changed_cb;
	void *interfaces_changed_data;
	ofono_property_changed_cb online_changed_cb;
	void *online_changed_data;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_modem *modem = user_data;
	gchar *interface_name;
	GVariant *child;
	int n;

	g_message("[Modem:%s] property %s changed", modem->path, name);

	if (g_str_equal(name, "Powered")) {
		modem->powered = g_variant_get_boolean(value);
		if (modem->powered_changed_cb != NULL)
			modem->powered_changed_cb(modem->powered_changed_data);
	}
	else if (g_str_equal(name, "Online")) {
		modem->online = g_variant_get_boolean(value);
		if (modem->online_changed_cb != NULL)
			modem->online_changed_cb(modem->online_changed_data);
	}
	else if (g_str_equal(name, "LockDown"))
		modem->lockdown = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Emergency"))
		modem->emergency = g_variant_get_boolean(value);
	else if (g_str_equal(name, "Name"))
		modem->name = g_variant_dup_string(value, NULL);
	else if (g_str_equal(name, "Serial"))
		modem->serial = g_variant_dup_string(value, NULL);
	else if (g_str_equal(name, "Revision"))
		modem->revision = g_variant_dup_string(value, NULL);
	else if (g_str_equal(name, "Interfaces")) {
		memset(modem->interfaces, 0, sizeof(modem->interfaces));

		for (n = 0; n < g_variant_n_children(value); n++) {
			child = g_variant_get_child_value(value, n);
			interface_name = g_variant_dup_string(child, NULL);

			if (g_str_equal(interface_name, "org.ofono.AssistedSatelliteNavigation"))
				modem->interfaces[OFONO_MODEM_INTERFACE_ASSISTET_SATELLITE_NAVIGATION] = 1;
			else if (g_str_equal(interface_name, "org.ofono.AudioSettings"))
				modem->interfaces[OFONO_MODEM_INTERFACE_AUDIO_SETTINGS] = 1;
			else if (g_str_equal(interface_name, "org.ofono.CallBarring"))
				modem->interfaces[OFONO_MODEM_INTERFACE_CALL_BARRING] = 1;
			else if (g_str_equal(interface_name, "org.ofono.CallForwarding"))
				modem->interfaces[OFONO_MODEM_INTERFACE_CALL_FORWARDING] = 1;
			else if (g_str_equal(interface_name, "org.ofono.CallMeter"))
				modem->interfaces[OFONO_MODEM_INTERFACE_CALL_METER] = 1;
			else if (g_str_equal(interface_name, "org.ofono.CallSettings"))
				modem->interfaces[OFONO_MODEM_INTERFACE_CALL_SETTINGS] = 1;
			else if (g_str_equal(interface_name, "org.ofono.CallVolume"))
				modem->interfaces[OFONO_MODEM_INTERFACE_CALL_VOLUME] = 1;
			else if (g_str_equal(interface_name, "org.ofono.CellBroadcast"))
				modem->interfaces[OFONO_MODEM_INTERFACE_CELL_BROADCAST] = 1;
			else if (g_str_equal(interface_name, "org.ofono.Handsfree"))
				modem->interfaces[OFONO_MODEM_INTERFACE_HANDSFREE] = 1;
			else if (g_str_equal(interface_name, "org.ofono.LocationReporting"))
				modem->interfaces[OFONO_MODEM_INTERFACE_LOCATION_REPORTING] = 1;
			else if (g_str_equal(interface_name, "org.ofono.MessageManager"))
				modem->interfaces[OFONO_MODEM_INTERFACE_MESSAGE_MANAGER] = 1;
			else if (g_str_equal(interface_name, "org.ofono.MessageWaiting"))
				modem->interfaces[OFONO_MODEM_INTERFACE_MESSAGE_WAITING] = 1;
			else if (g_str_equal(interface_name, "org.ofono.NetworkRegistration"))
				modem->interfaces[OFONO_MODEM_INTERFACE_NETWORK_REGISTRATION] = 1;
			else if (g_str_equal(interface_name, "org.ofono.Phonebook"))
				modem->interfaces[OFONO_MODEM_INTERFACE_PHONEBOOK] = 1;
			else if (g_str_equal(interface_name, "org.ofono.PushNotification"))
				modem->interfaces[OFONO_MODEM_INTERFACE_PUSH_NOTIFICATION] = 1;
			else if (g_str_equal(interface_name, "org.ofono.RadioSettings"))
				modem->interfaces[OFONO_MODEM_INTERFACE_RADIO_SETTINGS] = 1;
			else if (g_str_equal(interface_name, "org.ofono.SimManager"))
				modem->interfaces[OFONO_MODEM_INTERFACE_SIM_MANAGER] = 1;
			else if (g_str_equal(interface_name, "org.ofono.SmartMessaging"))
				modem->interfaces[OFONO_MODEM_INTERFACE_SMART_MESSAGING] = 1;
			else if (g_str_equal(interface_name, "org.ofono.SupplementaryServices"))
				modem->interfaces[OFONO_MODEM_INTERFACE_SUPPLEMENTARY_SERVICES] = 1;
			else if (g_str_equal(interface_name, "org.ofono.TextTelephony"))
				modem->interfaces[OFONO_MODEM_INTERFACE_TEXT_TELEPHONY] = 1;
			else if (g_str_equal(interface_name, "org.ofono.VoiceCallManager"))
				modem->interfaces[OFONO_MODEM_INTERFACE_VOICE_CALL_MANAGER] = 1;
		}

		if (modem->interfaces_changed_cb != NULL)
			modem->interfaces_changed_cb(modem->interfaces_changed_data);
	}
}

struct ofono_base_funcs modem_base_funcs = {
	.update_property = update_property,
	.set_property = ofono_interface_modem_call_set_property,
	.set_property_finish = ofono_interface_modem_call_set_property_finish,
	.get_properties = ofono_interface_modem_call_get_properties,
	.get_properties_finish = ofono_interface_modem_call_get_properties_finish
};

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
	memset(modem->interfaces, 0, sizeof(modem->interfaces));

	modem->base = ofono_base_create(&modem_base_funcs, modem->remote, modem);

	return modem;
}

void ofono_modem_ref(struct ofono_modem *modem)
{
	if (!modem)
		return;

	__sync_fetch_and_add(&modem->ref_count, 1);
}

void ofono_modem_unref(struct ofono_modem *modem)
{
	if (!modem)
		return;

	if (__sync_sub_and_fetch(&modem->ref_count, 1))
		return;

	ofono_modem_free(modem);
}

void ofono_modem_free(struct ofono_modem *modem)
{
	if (!modem)
		return;

	if (modem->remote)
		g_object_unref(modem->remote);

	if (modem->base)
		ofono_base_free(modem->base);

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
	GVariant *value = NULL;

	if (!modem)
		return -EINVAL;

	/* check wether we're already in the desired powered state */
	if (powered == modem->powered) {
		cb(TRUE, user_data);
		return 0;
	}

	value = g_variant_new_variant(g_variant_new_boolean(powered));
	ofono_base_set_property(modem->base, "Powered", value, cb, user_data);

	return -EINPROGRESS;
}

int ofono_modem_set_online(struct ofono_modem *modem, gboolean online, ofono_modem_result_cb cb, gpointer user_data)
{
	GVariant *value = NULL;

	if (!modem)
		return -EINVAL;

	/* check wether we're already in the desired state */
	if (online == modem->online) {
		cb(TRUE, user_data);
		return 0;
	}

	value = g_variant_new_variant(g_variant_new_boolean(online));
	ofono_base_set_property(modem->base, "Online", value, cb, user_data);

	return -EINPROGRESS;
}

bool ofono_modem_get_powered(struct ofono_modem *modem)
{
	if (!modem)
		return false;

	return modem->powered;
}

bool ofono_modem_get_online(struct ofono_modem *modem)
{
	if (!modem)
		return false;

	return modem->online;
}

const gchar* ofono_modem_get_serial(struct ofono_modem *modem)
{
	if (!modem)
		return NULL;

	return modem->serial;
}

const gchar* ofono_modem_get_revision(struct ofono_modem *modem)
{
	if (!modem)
		return NULL;

	return modem->revision;
}

bool ofono_modem_is_interface_supported(struct ofono_modem *modem, enum ofono_modem_interface interface)
{
	if (!modem)
		return false;

	return modem->interfaces[interface];
}

void ofono_modem_set_powered_changed_handler(struct ofono_modem *modem, ofono_property_changed_cb cb, void *data)
{
	if (!modem)
		return;

	modem->powered_changed_cb = cb;
	modem->powered_changed_data = data;
}

void ofono_modem_set_online_changed_handler(struct ofono_modem *modem, ofono_property_changed_cb cb, void *data)
{
	if (!modem)
		return;

	modem->online_changed_cb = cb;
	modem->online_changed_data = data;
}

void ofono_modem_set_interfaces_changed_handler(struct ofono_modem *modem, ofono_property_changed_cb cb, void *data)
{
	if (!modem)
		return;

	modem->interfaces_changed_cb = cb;
	modem->interfaces_changed_data = data;
}

// vim:ts=4:sw=4:noexpandtab

