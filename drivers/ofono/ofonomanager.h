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

#ifndef OFONO_MANAGER_H_
#define OFONO_MANAGER_H_

#include <glib.h>

struct ofono_manager;

typedef void (*ofono_manager_modems_chanaged_cb)(gpointer user_data);

struct ofono_manager* ofono_manager_create(void);
void ofono_manager_free(struct ofono_manager *manager);

const GList* ofono_manager_get_modems(struct ofono_manager *manager);

void ofono_manager_set_modems_changed_callback(struct ofono_manager *manager, ofono_manager_modems_chanaged_cb cb, gpointer user_data);

#endif

// vim:ts=4:sw=4:noexpandtab
