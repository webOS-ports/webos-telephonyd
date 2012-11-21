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

#include "luna_service_utils.h"

void luna_service_message_reply_custom_error(LSHandle *handle, LSMessage *message, const char *error_text)
{
	bool ret;
	LSError lserror;
	char payload[256];

	LSErrorInit(&lserror);

	snprintf(payload, 256, "{\"returnValue\":false, \"errorText\":\"%s\"}", error_text);

	ret = LSMessageReply(handle, message, payload, &lserror);
	if (!ret) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}
}

void luna_service_message_reply_error_unknown(LSHandle *handle, LSMessage *message)
{
	luna_service_message_reply_custom_error(handle, message, "Unknown Error.");
}

void luna_service_message_reply_error_bad_json(LSHandle *handle, LSMessage *message)
{
	luna_service_message_reply_custom_error(handle, message, "Malformed json.");
}

void luna_service_message_reply_error_invalid_params(LSHandle *handle, LSMessage *message)
{
	luna_service_message_reply_custom_error(handle, message, "Invalid parameters.");
}

void luna_service_message_reply_error_not_implemented(LSHandle *handle, LSMessage *message)
{
	luna_service_message_reply_custom_error(handle, message, "Not implemented.");
}

void luna_service_message_reply_error_internal(LSHandle *handle, LSMessage *message)
{
	luna_service_message_reply_custom_error(handle, message, "Internal error.");
}

void luna_service_message_reply_success(LSHandle *handle, LSMessage *message)
{
	bool ret;
	LSError lserror;

	LSErrorInit(&lserror);

	ret = LSMessageReply(handle, message, "{\"returnValue\":true}", &lserror);
	if (!ret) {
		LSErrorPrint(&lserror, stderr);
		LSErrorFree(&lserror);
	}
}

// vim:ts=4:sw=4:noexpandtab
