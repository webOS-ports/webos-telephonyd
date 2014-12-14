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

#ifndef OFONO_MESSAGE_H_
#define OFONO_MESSAGE_H_

#include <time.h>

enum ofono_message_type {
	OFONO_MESSAGE_TYPE_UNKNOWN,
	OFONO_MESSAGE_TYPE_CLASS0,
	OFONO_MESSAGE_TYPE_TEXT
};

struct ofono_message;

struct ofono_message *ofono_message_create(void);
void ofono_message_free(struct ofono_message *message);

void ofono_message_set_type(struct ofono_message *message, enum ofono_message_type type);
enum ofono_message_type ofono_message_get_type(struct ofono_message *message);

void ofono_message_set_text(struct ofono_message *message, const char *text);
const char* ofono_message_get_text(struct ofono_message *message);

void ofono_message_set_sender(struct ofono_message *message, const char *sender);
const char* ofono_message_get_sender(struct ofono_message *message);

void ofono_message_set_sent_time(struct ofono_message *message, time_t time);
time_t ofono_message_get_sent_time(struct ofono_message *message);

void ofono_message_set_local_sent_time(struct ofono_message *message, time_t time);
time_t ofono_message_get_local_sent_time(struct ofono_message *message);

#endif
