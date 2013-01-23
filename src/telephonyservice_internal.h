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

#ifndef TELEPHONY_SERVICE_INTERNAL_H_
#define TELEPHONY_SERVICE_INTERNAL_H_

struct telephony_service {
	struct telephony_driver *driver;
	void *data;
	LSPalmService *palm_service;
	LSHandle *private_service;
	bool initialized;
	bool initial_power_state;
	bool power_off_pending;
};

int telephonyservice_common_finish(const struct telephony_error *error, void *data);

#endif

// vim:ts=4:sw=4:noexpandtab
