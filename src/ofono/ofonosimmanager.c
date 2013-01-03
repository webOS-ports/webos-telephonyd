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
	GList *locked_pins;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_sim_manager *sim = user_data;
	gchar *pin_required;

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
		pin_required = g_variant_dup_string(value, NULL);

		if (g_str_equal(pin_required, "none"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_NONE;
		else if (g_str_equal(pin_required, "pin"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_PIN;
		else if (g_str_equal(pin_required, "phone"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_PHONE;
		else if (g_str_equal(pin_required, "firstphone"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_FIRST_PHONE;
		else if (g_str_equal(pin_required, "pin2"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_PIN2;
		else if (g_str_equal(pin_required, "network"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_NETWORK;
		else if (g_str_equal(pin_required, "netsub"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_NET_SUB;
		else if (g_str_equal(pin_required, "service"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_SERVICE;
		else if (g_str_equal(pin_required, "corp"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_CORP;
		else if (g_str_equal(pin_required, "puk"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_PUK;
		else if (g_str_equal(pin_required, "firstphonepuk"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_FIRST_PHONE_PUK;
		else if (g_str_equal(pin_required, "puk2"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_PUK2;
		else if (g_str_equal(pin_required, "networkpuk"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_NETWORK_PUK;
		else if (g_str_equal(pin_required, "netsubpuk"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_NET_SUB_PUK;
		else if (g_str_equal(pin_required, "servicepuk"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_SERVICE_PUK;
		else if (g_str_equal(pin_required, "corppuk"))
			sim->pin_required = OFONO_SIM_PIN_TYPE_CORP_PUK;
	}
	else if (g_str_equal(name, "LockedPins")) {
	}
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

	return sim;
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

// vim:ts=4:sw=4:noexpandtab

