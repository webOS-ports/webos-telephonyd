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

#ifndef OFONO_SERVICE_BASE_H_
#define OFONO_SERVICE_BASE_H_

#include <glib.h>

struct ofono_service_base;

typedef void (*ofono_service_base_result_cb)(gboolean success, void *user_data);

struct ofono_service_base* ofono_service_base_create(const char *path, const char *interface_name);
void ofono_service_base_free(struct ofono_service_base *service);

int ofono_service_base_set_property(struct ofono_service_base *service, const char *name, GVariant *value,
                                    ofono_service_base_result_cb cb, void *user_data);
GVariant* ofono_service_base_get_property(struct ofono_service_base *service, const char *name);

#endif

// vim:ts=4:sw=4:noexpandtab
