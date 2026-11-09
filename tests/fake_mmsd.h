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
 * A stand-in for mmsd on a private bus (GTestDBus), with the interfaces,
 * signatures, signals and error names of mmsd's doc/manager-api.txt, service-api.txt,
 * message-api.txt and
 * src/service.c at the revision the layer builds (c259df5):
 *
 *   /org/ofono/mms           Manager  GetServices -> a(oa{sv})
 *                                     ServiceAdded(oa{sv}), ServiceRemoved(o)
 *   /org/ofono/mms/<id>      Service  SendMessage(as, s, a(sss)) -> o
 *                                     GetMessages -> a(oa{sv})
 *                                     MessageAdded(oa{sv})
 *   /org/ofono/mms/<id>/<m>  Message  Delete, PropertyChanged(sv)
 *
 * SendMessage, like mmsd's, reads every attachment file while it is called
 * (mmsd mmaps and encodes them before it answers); the fake keeps what it
 * read so the test can look at it after the files are gone again.
 */

#ifndef FAKE_MMSD_H_
#define FAKE_MMSD_H_

#include <glib.h>
#include <gio/gio.h>

struct fake_mmsd {
	GTestDBus *bus;
	GDBusConnection *conn;
	guint manager_id;
	GHashTable *objects;		/* path -> registration id */
	GPtrArray *services;		/* gchar* paths */
	gboolean owned;

	/* SendMessage */
	guint sends;
	GVariant *last_send;		/* (assa(sss)) */
	GPtrArray *last_send_data;	/* GBytes* per attachment, as read at call time */
	GPtrArray *last_send_modes;	/* GUINT_TO_POINTER(st_mode & 0777) per attachment */
	gchar *send_error;		/* reply with this error name when set */
	gchar *last_message;		/* path of the last message SendMessage made */
	guint counter;

	/* Delete */
	GPtrArray *deleted;		/* gchar* paths */

	/* what GetMessages answers per service */
	GHashTable *stored;		/* service path -> GPtrArray of GVariant (oa{sv}) */
};

/* Starts the private bus, points the system bus at it, and connects. */
void fake_mmsd_start(struct fake_mmsd *fake);
void fake_mmsd_stop(struct fake_mmsd *fake);

/* Takes / releases org.ofono.mms, like mmsd starting and stopping. */
void fake_mmsd_own(struct fake_mmsd *fake);
void fake_mmsd_release(struct fake_mmsd *fake);

/* A service for a SIM, signalled with ServiceAdded when signal is set. */
const char* fake_mmsd_add_service(struct fake_mmsd *fake, const char *identity, gboolean signal);
void fake_mmsd_remove_service(struct fake_mmsd *fake, const char *path);

/* A message GetMessages lists for a service (props: a{sv}, floating ok). */
void fake_mmsd_store_message(struct fake_mmsd *fake, const char *service_path, const char *message_path,
                             GVariant *props);
/* MessageAdded on a service; the message object is registered. */
void fake_mmsd_emit_message_added(struct fake_mmsd *fake, const char *service_path, const char *message_path,
                                  GVariant *props);
/* PropertyChanged("Status", status) on a message. */
void fake_mmsd_emit_status(struct fake_mmsd *fake, GDBusConnection *from, const char *message_path,
                           const char *status);

/* Runs the main loop until cond(data) or 5 s pass; false on time-out. */
gboolean fake_wait(gboolean (*cond)(gpointer), gpointer data);
/* Runs the main loop for a little while (pending replies and signals). */
void fake_settle(void);

#endif

// vim:ts=4:sw=4:noexpandtab
