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
 * org.ofono.SimToolkit of one modem, plus the two org.ofono.SimToolkitAgent
 * objects telephonyd exports for it: the default agent (registered with
 * RegisterAgent, receives what the SIM starts on its own) and the session
 * agent (handed to SelectItem, receives what a menu selection starts).
 */

#ifndef OFONO_STK_H_
#define OFONO_STK_H_

#include <stdbool.h>
#include <glib.h>

#include "ofonobase.h"
#include "stkcore.h"

struct ofono_stk;

struct ofono_stk_callbacks {
	/* The main menu changed; menu is owned by the ofono_stk. */
	void (*menu_changed)(const struct stk_menu *menu, void *data);

	/**
	 * ofono asks the user something. The request (owned by the callee from
	 * now on) must eventually be answered with ofono_stk_send_reply(token,
	 * ...), exactly once - stk_core does that.
	 */
	void (*request)(struct stk_request *request, void *token, void *data);

	/**
	 * Whatever is pending on one of the agents is over: ofono called
	 * Cancel() (released == false) or Release(), or the agent goes away.
	 */
	void (*cancel)(bool session, bool released, void *data);
};

struct ofono_stk* ofono_stk_create(const gchar *modem_path, int sim_id,
                                   const struct ofono_stk_callbacks *callbacks, void *data);
void ofono_stk_free(struct ofono_stk *stk);

const struct stk_menu* ofono_stk_get_menu(struct ofono_stk *stk);
bool ofono_stk_is_agent_registered(struct ofono_stk *stk);
const gchar* ofono_stk_get_agent_path(struct ofono_stk *stk);
const gchar* ofono_stk_get_session_agent_path(struct ofono_stk *stk);

/* Start a user session with a main menu item; index is into the menu. */
void ofono_stk_select_item(struct ofono_stk *stk, guint8 index, ofono_base_result_cb cb, void *data);

/* Matches stk_reply_fn; token is a GDBusMethodInvocation. */
void ofono_stk_send_reply(void *token, const struct stk_request *request, const struct stk_reply *reply);

/* Re-registration back-off; exposed for the tests. */
void ofono_stk_set_retry_interval(guint first_ms, guint max_attempts);

#endif

// vim:ts=4:sw=4:noexpandtab
