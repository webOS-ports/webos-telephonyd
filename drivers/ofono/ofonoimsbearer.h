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

#ifndef OFONO_IMS_BEARER_H_
#define OFONO_IMS_BEARER_H_

#include <glib.h>

#include "ofonobase.h"
#include "ofonoims.h"
#include "telephonydriver.h"

/*
 * What carries a modem's IMS registration, from the MediaTek extension to
 * ofono-binder, org.ofono.mtk.IpMultimediaSystem: its Bearer property is
 * "wifi" while IMS is registered over Wi-Fi (VoWiFi), "cellular" over LTE or
 * NR, and "" while not registered. Stock ofono has no such interface; the bearer is then
 * unknown, and telephonyd never reports Wi-Fi calling.
 */

enum ofono_ims_bearer {
	OFONO_IMS_BEARER_NONE = 0,
	OFONO_IMS_BEARER_CELLULAR,
	OFONO_IMS_BEARER_WIFI,
};

struct ofono_ims_bearer_info;

struct ofono_ims_bearer_info* ofono_ims_bearer_create(const gchar *path);
void ofono_ims_bearer_free(struct ofono_ims_bearer_info *info);

/* Called with "Bearer" after it changed, the first read included. */
void ofono_ims_bearer_register_prop_changed_handler(struct ofono_ims_bearer_info *info,
													ofono_property_changed_cb cb, void *data);

/* OFONO_IMS_BEARER_NONE without the interface, or for a bearer it does not know */
enum ofono_ims_bearer ofono_ims_bearer_get(struct ofono_ims_bearer_info *info);

/* "wifi", "cellular" or "" */
const char* ofono_ims_bearer_to_string(enum ofono_ims_bearer bearer);

/*
 * The IMS part of a network status: volte while registered for voice over
 * any bearer, vowifi while that is over Wi-Fi. Either may be NULL, for a
 * modem without IMS or without the extension.
 */
void ofono_ims_fill_network_status(struct ofono_ims *ims, struct ofono_ims_bearer_info *info,
								   struct telephony_network_status *status);

#endif

// vim:ts=4:sw=4:noexpandtab
