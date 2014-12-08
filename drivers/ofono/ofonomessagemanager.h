/* @@@LICENSE
*
* Copyright (c) 2012-2014 Simon Busch <morphis@gravedo.de>
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

#ifndef OFONO_MESSAGE_MANAGER_H_
#define OFONO_MESSAGE_MANAGER_H_

#include "ofonobase.h"

struct ofono_message;
struct ofono_message_manager;

typedef void (*ofono_message_manager_incoming_message_cb)(struct ofono_message *message, gpointer user_data);
typedef void (*ofono_message_manager_send_message_cb)(struct ofono_error *error, const char *path, gpointer user_data);

struct ofono_message_manager* ofono_message_manager_create(const char *path);
void ofono_message_manager_free(struct ofono_message_manager *manager);

void ofono_message_manager_set_prop_changed_callback(struct ofono_message_manager *manager,
                                                     ofono_property_changed_cb cb, void *data);

void ofono_message_manager_set_incoming_message_callback(struct ofono_message_manager *manager,
                                                         ofono_message_manager_incoming_message_cb cb,
                                                         gpointer user_data);

void ofono_message_manager_send_message(struct ofono_message_manager *manager, const char *to, const char *text, ofono_message_manager_send_message_cb cb, void *data);

#endif
