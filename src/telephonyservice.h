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

#ifndef TELEPHONY_SERVICE_H_
#define TELEPHONY_SERVICE_H_

#include <luna-service2/lunaservice.h>
#include "telephonydriver.h"

struct telephony_service* telephony_service_create(LSPalmService *palm_service);
void telephony_service_free(struct telephony_service *service);

void telephony_service_set_data(struct telephony_service *service, void *data);
void* telephony_service_get_data(struct telephony_service *service);
void telephony_service_register_driver(struct telephony_service *service, struct telephony_driver *driver);
void telephony_service_unregister_driver(struct telephony_service *service, struct telephony_driver *driver);

void telephony_service_availability_changed_notify(struct telephony_service *service, bool available);
void telephony_service_power_status_notify(struct telephony_service *service, bool power);
void telephony_service_pin1_status_changed_notify(struct telephony_service *service, struct telephony_pin_status *pin_status);
void telephony_service_sim_status_notify(struct telephony_service *service, enum telephony_sim_status sim_status);

void telephony_service_network_status_changed_notify(struct telephony_service *service, struct telephony_network_status *net_status);
void telephony_service_signal_strength_changed_notify(struct telephony_service *service, int bars);

#endif

// vim:ts=4:sw=4:noexpandtab
