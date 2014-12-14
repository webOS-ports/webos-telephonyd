/* @@@LICENSE
*
* Copyright (c) 2012-2014 Simon Busch <morphis@gravedo.de>
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

#ifndef OFONO_MESSAGE_WATCH_H_
#define OFONO_MESSAGE_WATCH_H_

enum ofono_message_status {
	OFONO_MESSAGE_STATUS_UNKNOWN,
	OFONO_MESSAGE_STATUS_PENDING,
	OFONO_MESSAGE_STATUS_SENT,
	OFONO_MESSAGE_STATUS_FAILED
};

typedef void (*ofono_message_watch_status_cb)(enum ofono_message_status status, void *user_data);

struct ofono_message_watch;

struct ofono_message_watch* ofono_message_watch_create(const char *path);
void ofono_message_watch_free(struct ofono_message_watch *watch);

void ofono_message_watch_set_status_callback(struct ofono_message_watch *watch, ofono_message_watch_status_cb cb, void *user_data);

#endif
