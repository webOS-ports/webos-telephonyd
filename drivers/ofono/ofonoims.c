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
#include "ofonoims.h"
#include "ofono-interface.h"

struct ofono_ims {
	gchar *path;
	OfonoInterfaceIpMultimediaSystem *remote;
	struct ofono_base *base;
	bool registered;
	bool voice_capable;
	ofono_property_changed_cb prop_changed_cb;
	void *prop_changed_data;
};

static void update_property(const gchar *name, GVariant *value, void *user_data)
{
	struct ofono_ims *ims = user_data;

	if (g_variant_classify(value) != G_VARIANT_CLASS_BOOLEAN)
		return;

	if (g_str_equal(name, "Registered"))
		ims->registered = g_variant_get_boolean(value);
	else if (g_str_equal(name, "VoiceCapable"))
		ims->voice_capable = g_variant_get_boolean(value);
	else
		return;

	g_message("[IMS:%s] %s is now %s", ims->path, name, g_variant_get_boolean(value) ? "true" : "false");

	if (ims->prop_changed_cb)
		ims->prop_changed_cb(name, ims->prop_changed_data);
}

static struct ofono_base_funcs ims_base_funcs = {
	.update_property = update_property,
	.set_property = (ofono_base_set_property_fn) ofono_interface_ip_multimedia_system_call_set_property,
	.set_property_finish = (ofono_base_set_property_finish_fn) ofono_interface_ip_multimedia_system_call_set_property_finish,
	.get_properties = (ofono_base_get_properties_fn) ofono_interface_ip_multimedia_system_call_get_properties,
	.get_properties_finish = (ofono_base_get_properties_finish_fn) ofono_interface_ip_multimedia_system_call_get_properties_finish,
	.get_properties_sync = NULL,
};

struct ofono_ims* ofono_ims_create(const gchar *path)
{
	struct ofono_ims *ims;
	GError *error = NULL;

	ims = g_new0(struct ofono_ims, 1);

	/* oFono has no D-Bus properties to load; GetProperties reads them */
	ims->remote = ofono_interface_ip_multimedia_system_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
							G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, "org.ofono", path, NULL, &error);
	if (error) {
		g_warning("Unable to initialize proxy for the org.ofono.IpMultimediaSystem interface: %s",
				  error->message);
		g_error_free(error);
		g_free(ims);
		return NULL;
	}

	ims->path = g_strdup(path);
	ims->base = ofono_base_create(&ims_base_funcs, ims->remote, ims);

	return ims;
}

void ofono_ims_free(struct ofono_ims *ims)
{
	if (!ims)
		return;

	if (ims->base)
		ofono_base_free(ims->base);

	if (ims->remote)
		g_object_unref(ims->remote);

	g_free(ims->path);
	g_free(ims);
}

void ofono_ims_register_prop_changed_handler(struct ofono_ims *ims, ofono_property_changed_cb cb, void *data)
{
	if (!ims)
		return;

	ims->prop_changed_cb = cb;
	ims->prop_changed_data = data;
}

bool ofono_ims_get_registered(struct ofono_ims *ims)
{
	return ims && ims->registered;
}

bool ofono_ims_get_voice_capable(struct ofono_ims *ims)
{
	return ims && ims->voice_capable;
}

bool ofono_ims_voice_available(struct ofono_ims *ims)
{
	return ofono_ims_get_registered(ims) && ofono_ims_get_voice_capable(ims);
}

// vim:ts=4:sw=4:noexpandtab
