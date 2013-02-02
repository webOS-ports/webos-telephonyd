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
#include "ofonosimmanager.h"
#include "ofono-interface.h"

struct ofono_sim_manager {
	gchar *path;
	OfonoInterfaceSimManager *remote;
	struct ofono_base *base;
	int ref_count;
	bool present;
	gchar *imsi;
	gchar *mcc;
	gchar *mnc;
	enum ofono_sim_pin pin_required;
	bool locked_pins[OFONO_SIM_PIN_TYPE_MAX];
	int pin_retries[OFONO_SIM_PIN_TYPE_MAX];
	bool fixed_dialing;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
};

typedef gboolean (*_common_async_finish_cb)(void *proxy, GAsyncResult *res, GError **error);

enum ofono_sim_pin parse_ofono_sim_pin_type(const gchar *pin)
{
	if (g_str_equal(pin, "none"))
		return OFONO_SIM_PIN_TYPE_NONE;
	else if (g_str_equal(pin, "pin"))
		return OFONO_SIM_PIN_TYPE_PIN;
	else if (g_str_equal(pin, "phone"))
		return OFONO_SIM_PIN_TYPE_PHONE;
	else if (g_str_equal(pin, "firstphone"))
		return OFONO_SIM_PIN_TYPE_FIRST_PHONE;
	else if (g_str_equal(pin, "pin2"))
		return OFONO_SIM_PIN_TYPE_PIN2;
	else if (g_str_equal(pin, "network"))
		return OFONO_SIM_PIN_TYPE_NETWORK;
	else if (g_str_equal(pin, "netsub"))
		return OFONO_SIM_PIN_TYPE_NET_SUB;
	else if (g_str_equal(pin, "service"))
		return OFONO_SIM_PIN_TYPE_SERVICE;
	else if (g_str_equal(pin, "corp"))
		return OFONO_SIM_PIN_TYPE_CORP;
	else if (g_str_equal(pin, "puk"))
		return OFONO_SIM_PIN_TYPE_PUK;
	else if (g_str_equal(pin, "firstphonepuk"))
		return OFONO_SIM_PIN_TYPE_FIRST_PHONE_PUK;
	else if (g_str_equal(pin, "puk2"))
		return OFONO_SIM_PIN_TYPE_PUK2;
	else if (g_str_equal(pin, "networkpuk"))
		return OFONO_SIM_PIN_TYPE_NETWORK_PUK;
	else if (g_str_equal(pin, "netsubpuk"))
		return OFONO_SIM_PIN_TYPE_NET_SUB_PUK;
	else if (g_str_equal(pin, "servicepuk"))
		return OFONO_SIM_PIN_TYPE_SERVICE_PUK;
	else if (g_str_equal(pin, "corppuk"))
		return OFONO_SIM_PIN_TYPE_CORP_PUK;

	return OFONO_SIM_PIN_TYPE_INVALID;
}

const gchar* ofono_sim_pin_to_string(enum ofono_sim_pin type)
{
	switch(type) {
	case OFONO_SIM_PIN_TYPE_NONE:
		return "none";
	case OFONO_SIM_PIN_TYPE_PIN:
		return "pin";
	case OFONO_SIM_PIN_TYPE_PHONE:
		return "phone";
	case OFONO_SIM_PIN_TYPE_FIRST_PHONE:
		return "firstphone";
	case OFONO_SIM_PIN_TYPE_PIN2:
		return "pin2";
	case OFONO_SIM_PIN_TYPE_NETWORK:
		return "network";
	case OFONO_SIM_PIN_TYPE_NET_SUB:
		return "netsub";
	case OFONO_SIM_PIN_TYPE_SERVICE:
		return "service";
	case OFONO_SIM_PIN_TYPE_CORP:
		return "corp";
	case OFONO_SIM_PIN_TYPE_PUK:
		return "puk";
	case OFONO_SIM_PIN_TYPE_FIRST_PHONE_PUK:
		return "phonepuk";
	case OFONO_SIM_PIN_TYPE_PUK2:
		return "puk2";
	case OFONO_SIM_PIN_TYPE_NETWORK_PUK:
		return "networkpuk";
	case OFONO_SIM_PIN_TYPE_NET_SUB_PUK:
		return "netsubpuk";
	case OFONO_SIM_PIN_TYPE_SERVICE_PUK:
		return "servicepuk";
	case OFONO_SIM_PIN_TYPE_CORP_PUK:
		return "corppuk";
	default:
		break;
	}

	return "invalid";
}

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_sim_manager *sim = user_data;
	gchar *pin_type_str = NULL;
	int n;
	GVariant *child, *prop_value, *prop_key;
	enum ofono_sim_pin pin_type;

	g_message("[SIM:%s] property %s changed", sim->path, name);

	if (g_str_equal(name, "Present"))
		sim->present = g_variant_get_boolean(value);
	else if (g_str_equal(name, "SubscriberIdentity"))
		sim->imsi = g_variant_dup_string(value, NULL);
	else if (g_str_equal(name, "MobileCountryCode"))
		sim->mcc = g_variant_dup_string(value, NULL);
	else if (g_str_equal(name, "MobileNetworkCode"))
		sim->mnc = g_variant_dup_string(value, NULL);
	else if (g_str_equal(name, "PinRequired")) {
		pin_type_str = g_variant_dup_string(value, NULL);
		sim->pin_required = parse_ofono_sim_pin_type(pin_type_str);
		g_free(pin_type_str);
	}
	else if (g_str_equal(name, "LockedPins")) {
		for (n = 0; n < g_variant_n_children(value); n++) {
			child = g_variant_get_child_value(value, n);

			pin_type_str = g_variant_dup_string(child, NULL);
			pin_type = parse_ofono_sim_pin_type(pin_type_str);
			g_free(pin_type_str);

			sim->locked_pins[pin_type] = (pin_type != OFONO_SIM_PIN_TYPE_INVALID);
		}
	}
	else if (g_str_equal(name, "Retries")) {
		memset(sim->pin_retries, 0, sizeof(sim->pin_retries));

		for (n = 0; n < g_variant_n_children(value); n++) {
			child = g_variant_get_child_value(value, n);

			prop_key = g_variant_get_child_value(child, 0);
			prop_value = g_variant_get_child_value(child, 1);

			pin_type_str = g_variant_dup_string(prop_key, NULL);
			pin_type = parse_ofono_sim_pin_type(pin_type_str);
			g_free(pin_type_str);

			sim->pin_retries[pin_type] = (int) g_variant_get_byte(prop_value);
		}
	}
	else if (g_str_equal(name, "FixedDialing")) {
		sim->fixed_dialing = g_variant_get_boolean(value);
	}
	else if (g_str_equal(name, "MobileCountryCode")) {
		if (sim->mcc)
			g_free(sim->mcc);
		sim->mcc = g_variant_dup_string(value, NULL);
	}
	else if (g_str_equal(name, "MobileNetworkCode")) {
		if (sim->mnc)
			g_free(sim->mnc);
		sim->mnc = g_variant_dup_string(value, NULL);
	}

	if (sim->prop_changed_cb)
		sim->prop_changed_cb(name, sim->prop_changed_data);
}

struct ofono_base_funcs sim_base_funcs = {
	.update_property = update_property,
	.set_property = ofono_interface_sim_manager_call_set_property,
	.set_property_finish = ofono_interface_sim_manager_call_set_property_finish,
	.get_properties = ofono_interface_sim_manager_call_get_properties,
	.get_properties_finish = ofono_interface_sim_manager_call_get_properties_finish
};

struct ofono_sim_manager* ofono_sim_manager_create(const gchar *path)
{
	struct ofono_sim_manager *sim;
	GError *error = NULL;

	sim = g_try_new0(struct ofono_sim_manager, 1);
	if (!sim)
		return NULL;

	sim->remote = ofono_interface_sim_manager_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_error("Unable to initialize proxy for the org.ofono.sim interface");
		g_error_free(error);
		g_free(sim);
		return NULL;
	}

	sim->path = g_strdup(path);
	sim->base = ofono_base_create(&sim_base_funcs, sim->remote, sim);
	memset(sim->pin_retries, 0, sizeof(sim->pin_retries));
	memset(sim->locked_pins, 0, sizeof(sim->locked_pins));
	sim->fixed_dialing = false;

	return sim;
}

void ofono_sim_manager_register_prop_changed_handler(struct ofono_sim_manager *sim, ofono_property_changed_cb cb, void *data)
{
	if (!sim)
		return;

	sim->prop_changed_cb = cb;
	sim->prop_changed_data = data;
}

void ofono_sim_manager_ref(struct ofono_sim_manager *sim)
{
	if (!sim)
		return;

	__sync_fetch_and_add(&sim->ref_count, 1);
}

void ofono_sim_manager_unref(struct ofono_sim_manager *sim)
{
	if (!sim)
		return;

	if (__sync_sub_and_fetch(&sim->ref_count, 1))
		return;

	ofono_sim_manager_free(sim);
}

void ofono_sim_manager_free(struct ofono_sim_manager *sim)
{
	if (!sim)
		return;

	if (sim->remote)
		g_object_unref(sim->remote);

	g_free(sim);
}

static void common_pin_cb(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct cb_data *cbd2 = cbd->user;
	ofono_base_result_cb cb = cbd->cb;
	struct ofono_sim_manager *sim = cbd2->data;
	_common_async_finish_cb finish_cb = cbd2->cb;
	struct ofono_error oerr;
	gboolean success;
	GError *error = NULL;

	success = finish_cb(sim->remote, res, &error);
	if (error) {
		oerr.message = error->message;
		cb(&oerr, cbd->data);
		g_error_free(error);
	}
	else {
		cb(NULL, cbd->data);
	}

	g_free(cbd);
	g_free(cbd2);
}

void ofono_sim_manager_enter_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *pin,
						ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd, *cbd2;

	if (!sim) {
		cb(NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = cb_data_new(ofono_interface_sim_manager_call_enter_pin_finish, sim);

	ofono_interface_sim_manager_call_enter_pin(sim->remote, ofono_sim_pin_to_string(type),
											pin, NULL, common_pin_cb, cbd);
}

void ofono_sim_manager_lock_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *pin,
						ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;

	if (!sim) {
		cb(NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = cb_data_new(ofono_interface_sim_manager_call_lock_pin_finish, sim);

	ofono_interface_sim_manager_call_lock_pin(sim->remote, ofono_sim_pin_to_string(type),
											  pin, NULL, common_pin_cb, cbd);
}

void ofono_sim_manager_unlock_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *pin,
						ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;

	if (!sim) {
		cb(NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = cb_data_new(ofono_interface_sim_manager_call_unlock_pin_finish, sim);

	ofono_interface_sim_manager_call_unlock_pin(sim->remote, ofono_sim_pin_to_string(type),
											  pin, NULL, common_pin_cb, cbd);

}

void ofono_sim_manager_change_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *old_pin,
						const gchar *new_pin, ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;

	if (!sim) {
		cb(NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = cb_data_new(ofono_interface_sim_manager_call_change_pin_finish, sim);

	ofono_interface_sim_manager_call_change_pin(sim->remote, ofono_sim_pin_to_string(type),
											old_pin, new_pin, NULL, common_pin_cb, cbd);
}

void ofono_sim_manager_reset_pin(struct ofono_sim_manager *sim, enum ofono_sim_pin type, const gchar *puk,
						const gchar *new_pin, ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd;

	if (!sim) {
		cb(NULL, data);
		return;
	}

	cbd = cb_data_new(cb, data);
	cbd->user = cb_data_new(ofono_interface_sim_manager_call_reset_pin_finish, sim);

	ofono_interface_sim_manager_call_reset_pin(sim->remote, ofono_sim_pin_to_string(type),
											puk, new_pin, NULL, common_pin_cb, cbd);
}

const gchar* ofono_sim_manager_get_path(struct ofono_sim_manager *sim)
{
	if (!sim)
		return NULL;

	return sim->path;
}

bool ofono_sim_manager_get_present(struct ofono_sim_manager *sim)
{
	if (!sim)
		return false;

	return sim->present;
}

enum ofono_sim_pin ofono_sim_manager_get_pin_required(struct ofono_sim_manager *sim)
{
	if (!sim)
		return OFONO_SIM_PIN_TYPE_INVALID;

	return sim->pin_required;
}

int ofono_sim_manager_get_pin_retries(struct ofono_sim_manager *sim, enum ofono_sim_pin pin_type)
{
	if (!sim)
		return -1;

	return sim->pin_retries[pin_type];
}

bool ofono_sim_manager_is_pin_locked(struct ofono_sim_manager *sim, enum ofono_sim_pin pin_type)
{
	if (!sim)
		return false;

	return sim->locked_pins[pin_type];
}

bool ofono_sim_manager_get_fixed_dialing(struct ofono_sim_manager *sim)
{
	if (!sim)
		return false;

	return sim->fixed_dialing;
}

const char* ofono_sim_manager_get_mcc(struct ofono_sim_manager *sim)
{
	if (!sim)
		return NULL;

	return sim->mcc;
}

const char* ofono_sim_manager_get_mnc(struct ofono_sim_manager *sim)
{
	if (!sim)
		return NULL;

	return sim->mnc;
}

// vim:ts=4:sw=4:noexpandtab

