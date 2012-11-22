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

// vim:ts=4:sw=4:noexpandtab

