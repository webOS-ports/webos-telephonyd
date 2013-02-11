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
#include <string.h>

#include "wanservice.h"
#include "wandriver.h"

struct ofono_wan_data {
	struct wan_service *service;
};

int ofono_wan_probe(struct wan_service *service)
{
	struct ofono_wan_data *data;

	data = g_try_new0(struct ofono_wan_data, 1);
	if (!data)
		return -ENOMEM;

	wan_service_set_data(service, data);
	data->service = service;

	return 0;
}

void ofono_wan_remove(struct wan_service *service)
{
	struct ofono_data *data;

	data = wan_service_get_data(service);

	g_free(data);

	wan_service_set_data(service, NULL);
}

struct wan_driver ofono_wan_driver = {
	.probe =		ofono_wan_probe,
	.remove =		ofono_wan_remove,
};

// vim:ts=4:sw=4:noexpandtab
