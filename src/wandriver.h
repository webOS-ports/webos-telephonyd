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

#ifndef WAN_DRIVER_H_
#define WAN_DRIVER_H_

struct wan_service;

struct wan_error {
	int code;
};

enum wan_network_type {
	WAN_NETWORK_TYPE_NONE = 0,
	WAN_NETWORK_TYPE_GPRS,
	WAN_NETWORK_TYPE_EDGE,
	WAN_NETWORK_TYPE_UMTS,
	WAN_NETWORK_TYPE_HSDPA,
	WAN_NETWORK_TYPE_1X,
	WAN_NETWORK_TYPE_EVDO
};

enum wan_connection_status {
	WAN_CONNECTION_STATUS_DISCONNECTED = 0,
	WAN_CONNECTION_STATUS_ACTIVE,
	WAN_CONNECTION_STATUS_DORMANT,
	WAN_CONNECTION_STATUS_CONNECTING,
	WAN_CONNECTION_STATUS_DISCONNECTING
};

enum wan_status_type {
	WAN_STATUS_TYPE_DISABLE = 0,
	WAN_STATUS_TYPE_DISABLING,
	WAN_STATUS_TYPE_ENABLE
};

enum wan_service_type {
	WAN_SERVICE_TYPE_UNKNOWN = 0,
	WAN_SERVICE_TYPE_INTERNET,
	WAN_SERVICE_TYPE_MMS,
	WAN_SERVICE_TYPE_SPRINT_PROVISIONING,
	WAN_SERVICE_TYPE_TETHERED,
	WAN_SERVICE_TYPE_MAX
};

enum wan_request_status {
	WAN_REQUEST_STATUS_CONNECT_FAILED = 0,
	WAN_REQUEST_STATUS_CONNECT_SUCCEEDED,
	WAN_REQUEST_STATUS_DISCONNECT_FAILED,
	WAN_REQUEST_STATUS_DISCONNECT_SUCCEEDED,
	WAN_REQUEST_STATUS_EVENT
};

struct wan_connected_service {
	bool services[WAN_SERVICE_TYPE_MAX];
	int cid;
	enum wan_connection_status connection_status;
	const char *ipaddress;
	enum wan_request_status req_status;
	int error_code;
	int cause_code;
	int mip_failure_code;
};

struct wan_status {
	bool state;
	bool roam_guard;
	enum wan_network_type network_type;
	enum wan_connection_status connection_status;
	enum wan_status_type wan_status;
	bool dataaccess_usable;
	bool network_attached;
	bool disablewan;
	GSList *connected_services;
};

struct wan_configuration {
	bool roamguard;
	bool disablewan;
};

typedef void (*wan_result_cb)(const struct wan_error* error, void *data);
typedef void (*wan_get_status_cb)(const struct wan_error *error, struct wan_status *status, void *data);

struct wan_driver {
	int (*probe)(struct wan_service *service);
	void (*remove)(struct wan_service *service);

	int (*get_status)(struct wan_service *service, wan_get_status_cb cb, void *data);
	int (*set_configuration)(struct wan_service *service, struct wan_configuration *configuration,
							 wan_result_cb cb, void *data);
};

#endif

// vim:ts=4:sw=4:noexpandtab
