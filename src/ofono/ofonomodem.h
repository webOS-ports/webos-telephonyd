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

#ifndef OFONO_MODEM_H_
#define OFONO_MODEM_H_

#include <glib.h>

struct ofono_modem;

struct ofono_modem* ofono_modem_create(const gchar *path);
void ofono_modem_free(struct ofono_modem *modem);
const gchar* ofono_modem_get_path(struct ofono_modem *modem);

bool ofono_modem_get_powered(struct ofono_modem *modem);

#endif

// vim:ts=4:sw=4:noexpandtab
