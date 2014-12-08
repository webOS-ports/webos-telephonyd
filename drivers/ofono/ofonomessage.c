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

#include <string.h>
#include <errno.h>
#include <glib.h>

#include "ofonomessage.h"

struct ofono_message {
	enum ofono_message_type type;
	char *text;
	char *sender;
	time_t sent_time;
	time_t local_sent_time;
};

struct ofono_message* ofono_message_create()
{
	struct ofono_message *msg;

	msg = g_try_new0(struct ofono_message, 1);
	if (!msg)
		return NULL;

	return msg;
}

void ofono_message_free(struct ofono_message *message)
{
	if (!message)
		return;

	if (message->text)
		g_free(message->text);

	if (message->sender)
		g_free(message->sender);

	g_free(message);
}

void ofono_message_set_type(struct ofono_message *message, enum ofono_message_type type)
{
	if (!message)
		return;

	message->type = type;
}

enum ofono_message_type ofono_message_get_type(struct ofono_message *message)
{
	if (!message)
		return OFONO_MESSAGE_TYPE_UNKNOWN;

	return message->type;
}

void ofono_message_set_text(struct ofono_message *message, const char *text)
{
	if (!message)
		return;

	if (message->text)
		g_free(message->text);

	message->text = g_strdup(text);
}

const char* ofono_message_get_text(struct ofono_message *message)
{
	if (!message)
		return NULL;

	return message->text;
}

void ofono_message_set_sender(struct ofono_message *message, const char *sender)
{
	if (!message)
		return;

	if (message->sender)
		g_free(message->sender);

	message->sender = g_strdup(sender);
}

const char* ofono_message_get_sender(struct ofono_message *message)
{
	if (!message)
		return NULL;

	return message->sender;
}

void ofono_message_set_sent_time(struct ofono_message *message, time_t time)
{
	if (!message)
		return;

	message->sent_time = time;
}

time_t ofono_message_get_sent_time(struct ofono_message *message)
{
	if (!message)
		return 0;

	return message->sent_time;
}

void ofono_message_set_local_sent_time(struct ofono_message *message, time_t time)
{
	if (!message)
		return;

	message->local_sent_time = time;
}

time_t ofono_message_get_local_sent_time(struct ofono_message *message)
{
	if (!message)
		return 0;

	return message->local_sent_time;
}
