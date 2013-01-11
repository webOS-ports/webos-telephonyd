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
#include <stdbool.h>
#include "telephonydriver.h"

const char* telephony_platform_type_to_string(enum telephony_platform_type type)
{
	switch (type) {
		case TELEPHONY_PLATFORM_TYPE_GSM:
			return "gsm";
		case TELEPHONY_PLATFORM_TYPE_CDMA:
			return "cdma";
	}

	return "unknown";
}

const char* telephony_sim_status_to_string(enum telephony_sim_status sim_status)
{
	switch (sim_status) {
		case TELEPHONY_SIM_STATUS_SIM_NOT_FOUND:
			return "simnotfound";
		case TELEPHONY_SIM_STATUS_SIM_INVALID:
			return "siminvalid";
		case TELEPHONY_SIM_STATUS_SIM_READY:
			return "simready";
		case TELEPHONY_SIM_STATUS_PIN_REQUIRED:
			return "pinrequired";
		case TELEPHONY_SIM_STATUS_PUK_REQUIRED:
			return "pukrequired";
		case TELEPHONY_SIM_STATUS_PIN_PERM_BLOCKED:
			return "pinpermblocked";
		default:
			break;
	}

	return "unknown";
}

const char* telephony_network_state_to_string(enum telephony_network_state state)
{
	switch (state) {
		case TELEPHONY_NETWORK_STATE_SERVICE:
			return "service";
		case TELEPHONY_NETWORK_STATE_NO_SERVICE:
			return "noservice";
		case TELEPHONY_NETWORK_STATE_LIMITED:
			return "limited";
		default:
			break;
	}

	return "unknown";
}

const char* telephony_network_registration_to_string(enum telephony_network_registration netreg)
{
	switch (netreg) {
		case TELEPHONY_NETWORK_REGISTRATION_HOME:
			return "home";
		case TELEPHONY_NETWORK_REGISTRATION_ROAM:
			return "roam";
		case TELEPHONY_NETWORK_REGISTRATION_ROAM_BLINK:
			return "roamblink";
		case TELEPHONY_NETWORK_REGISTRATION_SEARCHING:
			return "searching";
		case TELEPHONY_NETWORK_REGISTRATION_DENIED:
			return "denied";
		default:
			break;
	}

	return "unknown";
}

// vim:ts=4:sw=4:noexpandtab
