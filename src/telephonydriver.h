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

#ifndef TELEPHONY_DRIVER_H_
#define TELEPHONY_DRIVER_H_

struct telephony_service;

typedef void (*telephony_driver_result_cb)(void *data);

typedef int (*telephony_power_set_cb)(bool success, void *data);
typedef int (*telephony_power_query_cb)(bool success, bool power_state, void *data);

struct telephony_driver {
	int (*probe)(struct telephony_service *service);
	void (*remove)(struct telephony_service *service);

	int (*power_set)(struct telephony_service *service, bool power, telephony_power_set_cb cb, void *data);
	int (*power_query)(struct telephony_service *service, telephony_power_query_cb, void *data);
};

#endif

// vim:ts=4:sw=4:noexpandtab
