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

#ifndef OFONO_BASE_H_
#define OFONO_BASE_H_

#include <glib.h>

struct ofono_base;

typedef void (*ofono_base_result_cb)(gboolean success, void *data);
typedef void (*ofono_property_changed_cb)(void *data);

struct ofono_base_funcs {
	void (*update_property)(const gchar *name, const GVariant *value, void *user_data);

	gboolean (*set_property_finish)(void *proxy, GAsyncResult *res, GError **error);
	void (*set_property)(void *proxy, const gchar *arg_property, GVariant *arg_value,
		GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);

	void (*get_properties)(void *proxy, GCancellable *cancellable,
		GAsyncReadyCallback callback, gpointer user_data);
	gboolean (*get_properties_finish)(void *proxy, GVariant **out_unnamed_arg0,
		GAsyncResult *res, GError **error);
};

struct ofono_base* ofono_base_create(struct ofono_base_funcs *funcs, void *remote, void *user_data);
void ofono_base_free(struct ofono_base *base);

void ofono_base_set_property(struct ofono_base *base, const gchar *name, const GVariant *value,
						 ofono_base_result_cb cb, gpointer user_data);

#endif

// vim:ts=4:sw=4:noexpandtab
