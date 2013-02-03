/* @@@LICENSE
*
* Copyright (c) 2013 networkon Busch <morphis@gravedo.de>
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
#include <glib.h>

#include "utils.h"
#include "ofonobase.h"
#include "ofonoradiosettings.h"
#include "ofono-interface.h"

struct ofono_radio_settings {
	gchar *path;
	OfonoInterfaceRadioSettings *remote;
	struct ofono_base *base;
	enum ofono_radio_access_mode technology_preference;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_radio_settings *ras = user_data;
	char *technology_preference_str;

	g_message("[RadioSettings:%s] property %s changed", ras->path, name);

	if (g_str_equal(name, "TechnologyPreference")) {
		technology_preference_str = g_variant_dup_string(value, NULL);

		if (g_str_equal(technology_preference_str, "any"))
			ras->technology_preference = OFONO_RADIO_ACCESS_MODE_ANY;
		else if (g_str_equal(technology_preference_str, "gsm"))
			ras->technology_preference = OFONO_RADIO_ACCESS_MODE_GSM;
		else if (g_str_equal(technology_preference_str, "umts"))
			ras->technology_preference = OFONO_RADIO_ACCESS_MODE_UMTS;
		else if (g_str_equal(technology_preference_str, "lte"))
			ras->technology_preference = OFONO_RADIO_ACCESS_MODE_LTE;
		else
			ras->technology_preference = OFONO_RADIO_ACCESS_MODE_UNKNOWN;

		g_free(technology_preference_str);
	}
}

struct ofono_base_funcs ras_base_funcs = {
	.update_property = update_property,
	.set_property = ofono_interface_radio_settings_call_set_property,
	.set_property_finish = ofono_interface_radio_settings_call_set_property_finish,
	.get_properties = ofono_interface_radio_settings_call_get_properties,
	.get_properties_finish = ofono_interface_radio_settings_call_get_properties_finish,
	.get_properties_sync = NULL,
};

struct ofono_radio_settings* ofono_radio_settings_create(const char *path)
{
	struct ofono_radio_settings *ras = NULL;
	GError *error = NULL;

	ras = g_try_new0(struct ofono_radio_settings, 1);
	if (!ras) {
		g_warning("Failed to allocate memory for radio settings object");
		return NULL;
	}

	ras->remote = ofono_interface_radio_settings_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_NONE, "org.ofono", path, NULL, &error);
	if (error) {
		g_warning("Unable to initialize proxy for the org.ofono.network interface");
		g_error_free(error);
		g_free(ras);
		return NULL;
	}

	ras->path = g_strdup(path);
	ras->base = ofono_base_create(&ras_base_funcs, ras->remote, ras);

	return ras;
}

void ofono_radio_settings_free(struct ofono_radio_settings *ras)
{
	if (!ras)
		return;

	if (ras->remote)
		g_object_unref(ras->remote);

	g_free(ras);
}

void set_technology_preference_cb(struct ofono_error *error, void *data)
{
	struct cb_data *cbd = data;
	ofono_base_result_cb cb = cbd->cb;

	cb(error, cbd->data);
	g_free(cbd);
}

void ofono_radio_settings_set_technology_preference(struct ofono_radio_settings *ras, enum ofono_radio_access_mode mode,
													ofono_base_result_cb cb, void *data)
{
	struct cb_data *cbd = NULL;
	char *mode_str;
	struct ofono_error error;
	GVariant *value = NULL;

	if (!ras)
		return;

	cbd = cb_data_new(cb, data);

	switch (mode) {
	case OFONO_RADIO_ACCESS_MODE_ANY:
		mode_str = g_strdup("any");
		break;
	case OFONO_RADIO_ACCESS_MODE_GSM:
		mode_str = g_strdup("gsm");
		break;
	case OFONO_RADIO_ACCESS_MODE_LTE:
		mode_str = g_strdup("lte");
		break;
	case OFONO_RADIO_ACCESS_MODE_UMTS:
		mode_str = g_strdup("umts");
		break;
	default:
		error.type = OFONO_ERROR_TYPE_INVALID_ARGUMENTS;
		cb(&error, data);
		g_free(cbd);
		return;
	}

	value = g_variant_new_variant(g_variant_new_string(mode_str));
	ofono_base_set_property(ras->base, "TechnologyPreference",
							value, set_technology_preference_cb, cbd);
	g_free(mode_str);

	return;
}

enum ofono_radio_access_mode ofono_radio_settings_get_technology_preference(struct ofono_radio_settings *ras)
{
	if (!ras)
		return OFONO_RADIO_ACCESS_MODE_UNKNOWN;

	return ras->technology_preference;
}

