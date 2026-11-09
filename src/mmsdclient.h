/* @@@LICENSE
*
* Copyright (c) 2026 LuneOS project
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

/*
 * A client of mmsd (org.ofono.mms on the system bus), as its doc/manager-api.txt,
 * service-api.txt and message-api.txt describe it:
 *
 *   /org/ofono/mms              org.ofono.mms.Manager  GetServices,
 *                                                      ServiceAdded/Removed
 *   /org/ofono/mms/<identity>   org.ofono.mms.Service  SendMessage,
 *                                                      GetMessages,
 *                                                      MessageAdded/Removed
 *   .../<identity>/<uuid>       org.ofono.mms.Message  Delete, MarkRead,
 *                                                      PropertyChanged
 *
 * mmsd has one service per SIM, named by the SIM's IMSI, and only once
 * ofono offers that modem a connection context of type "mms". The client
 * follows the name, the services and the messages, and only listens to
 * signals from the name's current owner.
 */

#ifndef MMSD_CLIENT_H_
#define MMSD_CLIENT_H_

#include <glib.h>
#include <gio/gio.h>

#define MMSD_SERVICE "org.ofono.mms"
#define MMSD_MANAGER_PATH "/org/ofono/mms"
#define MMSD_MANAGER_INTERFACE "org.ofono.mms.Manager"
#define MMSD_SERVICE_INTERFACE "org.ofono.mms.Service"
#define MMSD_MESSAGE_INTERFACE "org.ofono.mms.Message"

struct mmsd_client;

struct mmsd_client_callbacks {
	/* The number of services went from 0 to more or back. */
	void (*availability_changed)(struct mmsd_client *client, gboolean available, gpointer user_data);
	/* A message mmsd announced (MessageAdded) or listed (GetMessages once a
	 * service is there); props is the a{sv} of its properties. */
	void (*message_added)(struct mmsd_client *client, const char *service_path, const char *message_path,
	                      GVariant *props, gpointer user_data);
	/* A message's Status changed ("sent", "draft", "read"). */
	void (*message_status)(struct mmsd_client *client, const char *message_path, const char *status,
	                       gpointer user_data);
};

/* Connects to the bus asynchronously; nothing is called back before the
 * main loop runs. */
struct mmsd_client* mmsd_client_new(GBusType bus_type, const struct mmsd_client_callbacks *callbacks,
                                    gpointer user_data);
void mmsd_client_free(struct mmsd_client *client);

guint mmsd_client_service_count(struct mmsd_client *client);

/**
 * The service to send with: the one whose last path component is identity
 * (the IMSI of the SIM the message goes out on), else the only/first one.
 * NULL when there is none. Valid until the next main loop iteration.
 */
const char* mmsd_client_pick_service(struct mmsd_client *client, const char *identity);

/* message_path is the new message's object path; error is set instead when
 * mmsd refused it or could not be reached. */
typedef void (*mmsd_send_cb)(const char *message_path, const GError *error, gpointer user_data);

/**
 * SendMessage(as recipients, s smil, a(sss) attachments) on a service.
 * parts are struct mms_send_part* (content id, type and file). The callback
 * runs exactly once, also when the client is freed first (with
 * G_IO_ERROR_CANCELLED).
 */
void mmsd_client_send(struct mmsd_client *client, const char *service_path, GPtrArray *recipients,
                      const char *smil, GPtrArray *parts, mmsd_send_cb callback, gpointer user_data);

/* Message.Delete(), fire and forget. */
void mmsd_client_delete_message(struct mmsd_client *client, const char *message_path);

#endif

// vim:ts=4:sw=4:noexpandtab
