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

#ifndef OFONO_IMS_H_
#define OFONO_IMS_H_

#include <stdbool.h>

#include <glib.h>

#include "ofonobase.h"

/*
 * A modem's IMS registration, org.ofono.IpMultimediaSystem. Registered with
 * voice is what a status bar shows as VoLTE: voice calls can go over IMS.
 * Over what, LTE, NR or Wi-Fi, is not ofono's to say; see ofonoimsbearer.h.
 */

struct ofono_ims;

struct ofono_ims* ofono_ims_create(const gchar *path);
void ofono_ims_free(struct ofono_ims *ims);

/* Called with the property's name after a change, the first read included. */
void ofono_ims_register_prop_changed_handler(struct ofono_ims *ims, ofono_property_changed_cb cb, void *data);

bool ofono_ims_get_registered(struct ofono_ims *ims);
bool ofono_ims_get_voice_capable(struct ofono_ims *ims);
bool ofono_ims_get_sms_capable(struct ofono_ims *ims);

/* Registered with IMS and able to carry voice; false without IMS. */
bool ofono_ims_voice_available(struct ofono_ims *ims);

#endif

// vim:ts=4:sw=4:noexpandtab
