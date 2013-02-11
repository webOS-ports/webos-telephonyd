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

#ifndef WAN_SERVICE_H_
#define WAN_SERVICE_H_

#include <luna-service2/lunaservice.h>
#include "wandriver.h"

struct wan_service* wan_service_create(void);
void wan_service_free(struct wan_service *service);

void wan_service_set_data(struct wan_service *service, void *data);
void* wan_service_get_data(struct wan_service *service);
void wan_service_register_driver(struct wan_service *service, struct wan_driver *driver);
void wan_service_unregister_driver(struct wan_service *service, struct wan_driver *driver);

#endif

// vim:ts=4:sw=4:noexpandtab
