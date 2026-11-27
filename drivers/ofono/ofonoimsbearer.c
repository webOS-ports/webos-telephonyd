/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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

#include <glib.h>
#include <gio/gio.h>

#include "ofonobase.h"
#include "ofonoimsbearer.h"
#include "ofono-interface.h"

struct ofono_ims_bearer_info {
	gchar *path;
	OfonoInterfaceMtkIpMultimediaSystem *remote;
	struct ofono_base *base;
	enum ofono_ims_bearer bearer;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
};

static enum ofono_ims_bearer parse_bearer(const gchar *bearer)
{
	if (g_str_equal(bearer, "wifi"))
		return OFONO_IMS_BEARER_WIFI;
	if (g_str_equal(bearer, "cellular"))
		return OFONO_IMS_BEARER_CELLULAR;

	return OFONO_IMS_BEARER_NONE;
}

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_ims_bearer_info *info = user_data;
	enum ofono_ims_bearer bearer;

	if (!g_str_equal(name, "Bearer") || g_variant_classify(value) != G_VARIANT_CLASS_STRING)
		return;

	bearer = parse_bearer(g_variant_get_string(value, NULL));
	g_message("[IMS:%s] Bearer is now \"%s\"", info->path, ofono_ims_bearer_to_string(bearer));

	info->bearer = bearer;
	if (info->prop_changed_cb)
		info->prop_changed_cb(name, info->prop_changed_data);
}

/* The interface has no SetProperty: nothing on it is ours to change */
static struct ofono_base_funcs ims_bearer_base_funcs = {
	.update_property = update_property,
	.set_property = NULL,
	.set_property_finish = NULL,
	.get_properties = (ofono_base_get_properties_fn) ofono_interface_mtk_ip_multimedia_system_call_get_properties,
	.get_properties_finish = (ofono_base_get_properties_finish_fn) ofono_interface_mtk_ip_multimedia_system_call_get_properties_finish,
	.get_properties_sync = NULL,
};

struct ofono_ims_bearer_info* ofono_ims_bearer_create(const gchar *path)
{
	struct ofono_ims_bearer_info *info;
	GError *error = NULL;

	info = g_new0(struct ofono_ims_bearer_info, 1);

	info->remote = ofono_interface_mtk_ip_multimedia_system_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, "org.ofono", path, NULL, &error);
	if (error) {
		g_warning("Unable to initialize proxy for the org.ofono.mtk.IpMultimediaSystem interface: %s",
				  error->message);
		g_error_free(error);
		g_free(info);
		return NULL;
	}

	info->path = g_strdup(path);
	info->base = ofono_base_create(&ims_bearer_base_funcs, info->remote, info);

	return info;
}

void ofono_ims_bearer_free(struct ofono_ims_bearer_info *info)
{
	if (!info)
		return;

	if (info->base)
		ofono_base_free(info->base);

	if (info->remote)
		g_object_unref(info->remote);

	g_free(info->path);
	g_free(info);
}

void ofono_ims_bearer_register_prop_changed_handler(struct ofono_ims_bearer_info *info,
													ofono_property_changed_cb cb, void *data)
{
	if (!info)
		return;

	info->prop_changed_cb = cb;
	info->prop_changed_data = data;
}

enum ofono_ims_bearer ofono_ims_bearer_get(struct ofono_ims_bearer_info *info)
{
	return info ? info->bearer : OFONO_IMS_BEARER_NONE;
}

const char* ofono_ims_bearer_to_string(enum ofono_ims_bearer bearer)
{
	switch (bearer) {
	case OFONO_IMS_BEARER_WIFI:
		return "wifi";
	case OFONO_IMS_BEARER_CELLULAR:
		return "cellular";
	default:
		return "";
	}
}

void ofono_ims_fill_network_status(struct ofono_ims *ims, struct ofono_ims_bearer_info *info,
								   struct telephony_network_status *status)
{
	enum ofono_ims_bearer bearer = OFONO_IMS_BEARER_NONE;

	status->ims_registered = ofono_ims_get_registered(ims);
	status->ims_voice_capable = ofono_ims_get_voice_capable(ims);
	status->ims_sms_capable = ofono_ims_get_sms_capable(ims);
	status->volte = ofono_ims_voice_available(ims);

	/* The extension's bearer outlives a registration ofono already dropped,
	 * and means nothing without one */
	if (status->ims_registered)
		bearer = ofono_ims_bearer_get(info);
	status->ims_bearer = ofono_ims_bearer_to_string(bearer);
	status->vowifi = status->volte && bearer == OFONO_IMS_BEARER_WIFI;
}

// vim:ts=4:sw=4:noexpandtab
