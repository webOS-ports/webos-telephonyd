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

#include <string.h>

#include "stkjson.h"

static const char* answer_kind_to_string(enum stk_answer_kind kind)
{
	switch (kind) {
	case STK_ANSWER_INDEX:
		return "index";
	case STK_ANSWER_STRING:
		return "string";
	case STK_ANSWER_BOOLEAN:
		return "boolean";
	case STK_ANSWER_NONE:
	default:
		break;
	}

	return "none";
}

static jvalue_ref string_or_empty(const char *s)
{
	return jstring_create(s ? s : "");
}

static jvalue_ref items_to_json(const struct stk_menu_item *items, guint n_items)
{
	jvalue_ref array = jarray_create(NULL);
	guint n;

	for (n = 0; items && n < n_items; n++) {
		jvalue_ref item = jobject_create();

		jobject_put(item, J_CSTR_TO_JVAL("index"), jnumber_create_i32((int32_t) n));
		jobject_put(item, J_CSTR_TO_JVAL("label"), string_or_empty(items[n].label));
		jobject_put(item, J_CSTR_TO_JVAL("iconId"), jnumber_create_i32(items[n].icon_id));
		jarray_append(array, item);
	}

	return array;
}

jvalue_ref stk_request_to_json(const struct stk_request *request)
{
	jvalue_ref obj = jobject_create();
	jvalue_ref allowed;
	unsigned int errors = stk_request_allowed_errors(request->type);

	jobject_put(obj, J_CSTR_TO_JVAL("requestId"), jnumber_create_i64(request->id));
	jobject_put(obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(request->sim_id));
	jobject_put(obj, J_CSTR_TO_JVAL("type"), jstring_create(stk_request_type_to_string(request->type)));
	jobject_put(obj, J_CSTR_TO_JVAL("session"), jboolean_create(request->session));
	jobject_put(obj, J_CSTR_TO_JVAL("text"), string_or_empty(request->text));
	jobject_put(obj, J_CSTR_TO_JVAL("iconId"), jnumber_create_i32(request->icon_id));
	jobject_put(obj, J_CSTR_TO_JVAL("answer"),
				jstring_create(answer_kind_to_string(stk_request_answer_kind(request->type))));

	allowed = jobject_create();
	jobject_put(allowed, J_CSTR_TO_JVAL("goBack"), jboolean_create(errors & STK_ALLOW_GO_BACK));
	jobject_put(allowed, J_CSTR_TO_JVAL("endSession"), jboolean_create(errors & STK_ALLOW_END_SESSION));
	jobject_put(allowed, J_CSTR_TO_JVAL("busy"), jboolean_create(errors & STK_ALLOW_BUSY));
	jobject_put(obj, J_CSTR_TO_JVAL("allowed"), allowed);

	switch (request->type) {
	case STK_REQUEST_SELECTION:
		jobject_put(obj, J_CSTR_TO_JVAL("items"), items_to_json(request->items, request->n_items));
		jobject_put(obj, J_CSTR_TO_JVAL("defaultItem"), jnumber_create_i32(request->default_item));
		break;
	case STK_REQUEST_INPUT:
	case STK_REQUEST_DIGITS:
		jobject_put(obj, J_CSTR_TO_JVAL("defaultText"), string_or_empty(request->default_text));
		jobject_put(obj, J_CSTR_TO_JVAL("minLength"), jnumber_create_i32((int32_t) request->min_length));
		jobject_put(obj, J_CSTR_TO_JVAL("maxLength"), jnumber_create_i32((int32_t) request->max_length));
		jobject_put(obj, J_CSTR_TO_JVAL("hideTyping"), jboolean_create(request->hide_typing));
		break;
	case STK_REQUEST_DISPLAY_TEXT:
		jobject_put(obj, J_CSTR_TO_JVAL("urgent"), jboolean_create(request->urgent));
		break;
	case STK_REQUEST_PLAY_TONE:
	case STK_REQUEST_LOOP_TONE:
		jobject_put(obj, J_CSTR_TO_JVAL("tone"), string_or_empty(request->tone));
		break;
	case STK_REQUEST_CONFIRM_LAUNCH_BROWSER:
		jobject_put(obj, J_CSTR_TO_JVAL("url"), string_or_empty(request->url));
		break;
	default:
		break;
	}

	return obj;
}

jvalue_ref stk_menu_to_json(const struct stk_menu *menu)
{
	jvalue_ref obj = jobject_create();

	jobject_put(obj, J_CSTR_TO_JVAL("title"), string_or_empty(menu ? menu->title : NULL));
	jobject_put(obj, J_CSTR_TO_JVAL("iconId"), jnumber_create_i32(menu ? menu->icon_id : 0));
	jobject_put(obj, J_CSTR_TO_JVAL("items"),
				items_to_json(menu ? menu->items : NULL, menu ? menu->n_items : 0));
	jobject_put(obj, J_CSTR_TO_JVAL("idleModeText"), string_or_empty(menu ? menu->idle_mode_text : NULL));

	return obj;
}

jvalue_ref stk_ended_to_json(const struct stk_request *request, enum stk_end_reason reason)
{
	jvalue_ref obj = jobject_create();

	jobject_put(obj, J_CSTR_TO_JVAL("requestId"), jnumber_create_i64(request->id));
	jobject_put(obj, J_CSTR_TO_JVAL("simId"), jnumber_create_i32(request->sim_id));
	jobject_put(obj, J_CSTR_TO_JVAL("type"), jstring_create(stk_request_type_to_string(request->type)));
	jobject_put(obj, J_CSTR_TO_JVAL("reason"), jstring_create(stk_end_reason_to_string(reason)));

	return obj;
}

static const char *const known_actions[] = {
	"answer", "goBack", "endSession", "busy", "cancel", NULL
};

bool stk_respond_params_parse(jvalue_ref payload, struct stk_respond_params *params,
                              const char **error_text)
{
	jvalue_ref id_obj = NULL;
	jvalue_ref action_obj = NULL;
	jvalue_ref value_obj = NULL;
	int64_t id = 0;
	int64_t index = 0;
	bool boolean = false;

	memset(params, 0, sizeof(*params));
	params->action = "answer";

	if (jis_null(payload) || !jis_object(payload)) {
		*error_text = "Malformed json.";
		return false;
	}

	if (!jobject_get_exists(payload, J_CSTR_TO_BUF("requestId"), &id_obj) || !jis_number(id_obj) ||
		jnumber_get_i64(id_obj, &id) != CONV_OK || id <= 0 || id > G_MAXUINT) {
		*error_text = "A valid requestId is required";
		return false;
	}
	params->request_id = (guint) id;

	if (jobject_get_exists(payload, J_CSTR_TO_BUF("action"), &action_obj)) {
		if (!jis_string(action_obj)) {
			*error_text = "Unknown action";
			return false;
		}

		raw_buffer action_buf = jstring_get_fast(action_obj);
		int n;

		params->action = NULL;
		for (n = 0; known_actions[n]; n++) {
			/* known_actions are literals */
			if (action_buf.m_str && action_buf.m_len == strlen(known_actions[n]) && /* Flawfinder: ignore */
				memcmp(action_buf.m_str, known_actions[n], action_buf.m_len) == 0) {
				params->action = known_actions[n];
				break;
			}
		}

		if (!params->action) {
			*error_text = "Unknown action";
			return false;
		}
	}

	if (!jobject_get_exists(payload, J_CSTR_TO_BUF("value"), &value_obj))
		return true;

	if (jis_boolean(value_obj)) {
		jboolean_get(value_obj, &boolean);
		params->answer.has_boolean = true;
		params->answer.boolean = boolean;
	}
	else if (jis_number(value_obj)) {
		if (jnumber_get_i64(value_obj, &index) != CONV_OK) {
			*error_text = "The value has to be an integer";
			return false;
		}
		params->answer.has_index = true;
		params->answer.index = index;
	}
	else if (jis_string(value_obj)) {
		params->owned_string = jstring_get(value_obj);
		if (!params->owned_string.m_str) {
			*error_text = "Internal error";
			return false;
		}
		params->answer.has_string = true;
		params->answer.string = params->owned_string.m_str;
		params->answer.string_len = params->owned_string.m_len;
	}
	else if (!jis_null(value_obj)) {
		*error_text = "Unsupported value type";
		return false;
	}

	return true;
}

void stk_respond_params_clear(struct stk_respond_params *params)
{
	if (params->owned_string.m_str)
		jstring_free_buffer(params->owned_string);

	memset(params, 0, sizeof(*params));
}

// vim:ts=4:sw=4:noexpandtab
