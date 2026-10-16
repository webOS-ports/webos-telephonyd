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

/* The JSON side of the SIM toolkit luna methods; needs pbnjson only. */

#ifndef STK_JSON_H_
#define STK_JSON_H_

#include <stdbool.h>
#include <pbnjson.h>

#include "stkcore.h"

/* {requestId, simId, type, session, text, iconId, answer, allowed{...}, ...} */
jvalue_ref stk_request_to_json(const struct stk_request *request);

/* {title, iconId, items:[{index, label, iconId}], idleModeText} */
jvalue_ref stk_menu_to_json(const struct stk_menu *menu);

/* {requestId, simId, type, reason} for a request that left the pending set */
jvalue_ref stk_ended_to_json(const struct stk_request *request, enum stk_end_reason reason);

/**
 * The parsed stkRespond payload. string points into owned_string, which
 * stk_respond_params_clear() releases.
 */
struct stk_respond_params {
	guint request_id;
	const char *action;		/* static string, "answer" when absent */
	struct stk_answer answer;
	raw_buffer owned_string;
};

/* {requestId: number, action?: string, value?: number|string|boolean} */
bool stk_respond_params_parse(jvalue_ref payload, struct stk_respond_params *params,
                              const char **error_text);
void stk_respond_params_clear(struct stk_respond_params *params);

#endif

// vim:ts=4:sw=4:noexpandtab
