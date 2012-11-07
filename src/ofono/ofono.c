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

#include <glib.h>
#include <errno.h>

#include "telephonyservice.h"
#include "telephonydriver.h"

struct ofono_data {
};

int ofono_power_set(struct telephony_service *service, int power, telephony_power_set_cb cb, void *data)
{
	return 0;
}

int ofono_probe(struct telephony_service *service)
{
	struct ofono_data *data;

	data = g_try_new0(struct ofono_data, 1);
	if (!data)
		return -ENOMEM;

	telephony_service_set_data(service, data);

	return 0;
}

void ofono_remove(struct telephony_service *service)
{
	struct ofono_data *data;

	data = telephony_service_get_data(service);

	g_free(data);

	telephony_service_set_data(service, NULL);
}

struct telephony_driver driver = {
    .probe =		ofono_probe,
    .remove =		ofono_remove,
    .power_set =	ofono_power_set,
};

void ofono_init(struct telephony_service *service)
{
    telephony_service_register_driver(service, &driver);
}

void ofono_exit(struct telephony_service *service)
{
}

// vim:ts=4:sw=4:noexpandtab
