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

#include <glib.h>
#include <string.h>
#include <pbnjson.h>

#include "stkjson.h"

static jvalue_ref parse(const char *json)
{
	jschema_ref schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	JSchemaInfo info;
	jvalue_ref v;

	jschema_info_init(&info, schema, NULL, NULL);
	v = jdom_parse(j_cstr_to_buffer(json), DOMOPT_NOOPT, &info);
	jschema_release(&schema);

	return v;
}

static gchar* to_string(jvalue_ref v)
{
	jschema_ref schema = jschema_parse(j_cstr_to_buffer("{}"), DOMOPT_NOOPT, NULL);
	gchar *s = g_strdup(jvalue_tostring(v, schema));

	jschema_release(&schema);

	return s;
}

static gchar* get_string(jvalue_ref obj, const char *key)
{
	jvalue_ref v = NULL;
	raw_buffer buf;

	if (!jobject_get_exists(obj, j_cstr_to_buffer(key), &v) || !jis_string(v))
		return NULL;

	buf = jstring_get_fast(v);

	return g_strndup(buf.m_str, buf.m_len);
}

static gint64 get_int(jvalue_ref obj, const char *key)
{
	jvalue_ref v = NULL;
	int64_t n = -12345;

	if (jobject_get_exists(obj, j_cstr_to_buffer(key), &v) && jis_number(v))
		jnumber_get_i64(v, &n);

	return n;
}

static int get_bool(jvalue_ref obj, const char *key)
{
	jvalue_ref v = NULL;
	bool b = false;

	if (!jobject_get_exists(obj, j_cstr_to_buffer(key), &v) || !jis_boolean(v))
		return -1;

	jboolean_get(v, &b);

	return b ? 1 : 0;
}

static bool has_key(jvalue_ref obj, const char *key)
{
	jvalue_ref v = NULL;

	return jobject_get_exists(obj, j_cstr_to_buffer(key), &v);
}

#define ASSERT_STR(obj, key, expected) \
	do { gchar *s_ = get_string(obj, key); g_assert_cmpstr(s_, ==, expected); g_free(s_); } while (0)

static void test_selection_json(void)
{
	struct stk_request *req = stk_request_new(STK_REQUEST_SELECTION, 1);
	jvalue_ref obj, items, item, allowed;

	req->id = 42;
	req->text = g_strdup("Menu \"quoted\"");
	req->icon_id = 3;
	req->items = g_new0(struct stk_menu_item, 2);
	req->n_items = 2;
	req->items[0].label = g_strdup("One");
	req->items[1].label = g_strdup("Two");
	req->items[1].icon_id = 9;
	req->default_item = 1;
	req->session = true;

	obj = stk_request_to_json(req);

	g_assert_cmpint(get_int(obj, "requestId"), ==, 42);
	g_assert_cmpint(get_int(obj, "simId"), ==, 1);
	ASSERT_STR(obj, "type", "selection");
	ASSERT_STR(obj, "text", "Menu \"quoted\"");
	ASSERT_STR(obj, "answer", "index");
	g_assert_cmpint(get_int(obj, "iconId"), ==, 3);
	g_assert_cmpint(get_int(obj, "defaultItem"), ==, 1);
	g_assert_cmpint(get_bool(obj, "session"), ==, 1);
	g_assert_false(has_key(obj, "minLength"));

	g_assert_true(jobject_get_exists(obj, J_CSTR_TO_BUF("allowed"), &allowed));
	g_assert_cmpint(get_bool(allowed, "goBack"), ==, 1);
	g_assert_cmpint(get_bool(allowed, "endSession"), ==, 1);
	g_assert_cmpint(get_bool(allowed, "busy"), ==, 0);

	g_assert_true(jobject_get_exists(obj, J_CSTR_TO_BUF("items"), &items));
	g_assert_cmpint(jarray_size(items), ==, 2);
	item = jarray_get(items, 1);
	ASSERT_STR(item, "label", "Two");
	g_assert_cmpint(get_int(item, "index"), ==, 1);
	g_assert_cmpint(get_int(item, "iconId"), ==, 9);

	j_release(&obj);
	stk_request_free(req);
}

static void test_type_specific_fields(void)
{
	struct stk_request *req;
	jvalue_ref obj;

	req = stk_request_new(STK_REQUEST_DIGITS, 0);
	req->min_length = 4;
	req->max_length = 8;
	req->hide_typing = true;
	obj = stk_request_to_json(req);
	ASSERT_STR(obj, "text", "");			/* NULL text becomes "" */
	ASSERT_STR(obj, "defaultText", "");
	ASSERT_STR(obj, "answer", "string");
	g_assert_cmpint(get_int(obj, "minLength"), ==, 4);
	g_assert_cmpint(get_int(obj, "maxLength"), ==, 8);
	g_assert_cmpint(get_bool(obj, "hideTyping"), ==, 1);
	g_assert_false(has_key(obj, "items"));
	j_release(&obj);
	stk_request_free(req);

	req = stk_request_new(STK_REQUEST_LOOP_TONE, 0);
	req->tone = g_strdup("ringing-tone");
	obj = stk_request_to_json(req);
	ASSERT_STR(obj, "tone", "ringing-tone");
	ASSERT_STR(obj, "answer", "none");
	j_release(&obj);
	stk_request_free(req);

	req = stk_request_new(STK_REQUEST_CONFIRM_LAUNCH_BROWSER, 0);
	req->url = g_strdup("http://example.com/");
	obj = stk_request_to_json(req);
	ASSERT_STR(obj, "url", "http://example.com/");
	ASSERT_STR(obj, "answer", "boolean");
	j_release(&obj);
	stk_request_free(req);

	req = stk_request_new(STK_REQUEST_DISPLAY_TEXT, 0);
	req->urgent = true;
	obj = stk_request_to_json(req);
	g_assert_cmpint(get_bool(obj, "urgent"), ==, 1);
	j_release(&obj);
	stk_request_free(req);
}

static void test_menu_and_ended(void)
{
	struct stk_menu_item items[1] = { { "Balance", 2 } };
	struct stk_menu menu = { "Operator", 1, items, 1, NULL };
	struct stk_request *req = stk_request_new(STK_REQUEST_INPUT, 2);
	jvalue_ref obj, arr;
	gchar *s;

	obj = stk_menu_to_json(&menu);
	ASSERT_STR(obj, "title", "Operator");
	ASSERT_STR(obj, "idleModeText", "");
	g_assert_true(jobject_get_exists(obj, J_CSTR_TO_BUF("items"), &arr));
	g_assert_cmpint(jarray_size(arr), ==, 1);
	j_release(&obj);

	/* no menu: an empty one */
	obj = stk_menu_to_json(NULL);
	g_assert_true(jobject_get_exists(obj, J_CSTR_TO_BUF("items"), &arr));
	g_assert_cmpint(jarray_size(arr), ==, 0);
	s = to_string(obj);
	g_assert_nonnull(strstr(s, "\"title\":\"\""));
	g_free(s);
	j_release(&obj);

	req->id = 7;
	obj = stk_ended_to_json(req, STK_END_TIMEOUT);
	g_assert_cmpint(get_int(obj, "requestId"), ==, 7);
	g_assert_cmpint(get_int(obj, "simId"), ==, 2);
	ASSERT_STR(obj, "reason", "timeout");
	ASSERT_STR(obj, "type", "input");
	j_release(&obj);
	stk_request_free(req);
}

static bool parse_respond(const char *json, struct stk_respond_params *p, const char **err)
{
	jvalue_ref v = parse(json);
	bool ok = stk_respond_params_parse(v, p, err);

	if (!jis_null(v))
		j_release(&v);

	return ok;
}

static void test_respond_parsing(void)
{
	struct stk_respond_params p;
	const char *err = NULL;

	g_assert_true(parse_respond("{\"requestId\":5,\"value\":\"12\\u00e4\"}", &p, &err));
	g_assert_cmpuint(p.request_id, ==, 5);
	g_assert_cmpstr(p.action, ==, "answer");
	g_assert_true(p.answer.has_string);
	g_assert_cmpstr(p.answer.string, ==, "12\xc3\xa4");
	g_assert_cmpuint(p.answer.string_len, ==, 4);
	stk_respond_params_clear(&p);

	g_assert_true(parse_respond("{\"requestId\":5,\"action\":\"goBack\"}", &p, &err));
	g_assert_cmpstr(p.action, ==, "goBack");
	g_assert_false(p.answer.has_string || p.answer.has_index || p.answer.has_boolean);
	stk_respond_params_clear(&p);

	g_assert_true(parse_respond("{\"requestId\":5,\"value\":2}", &p, &err));
	g_assert_true(p.answer.has_index);
	g_assert_cmpint(p.answer.index, ==, 2);
	stk_respond_params_clear(&p);

	g_assert_true(parse_respond("{\"requestId\":5,\"value\":false}", &p, &err));
	g_assert_true(p.answer.has_boolean);
	g_assert_false(p.answer.boolean);
	stk_respond_params_clear(&p);

	g_assert_true(parse_respond("{\"requestId\":5,\"value\":null}", &p, &err));
	stk_respond_params_clear(&p);

	/* embedded NUL survives as a length mismatch for stkcore to refuse */
	g_assert_true(parse_respond("{\"requestId\":5,\"value\":\"a\\u0000b\"}", &p, &err));
	g_assert_true(p.answer.has_string);
	g_assert_cmpuint(p.answer.string_len, ==, 3);
	g_assert_cmpuint(strlen(p.answer.string), ==, 1);
	stk_respond_params_clear(&p);

	g_assert_false(parse_respond("{\"value\":1}", &p, &err));
	g_assert_cmpstr(err, ==, "A valid requestId is required");
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("{\"requestId\":0}", &p, &err));
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("{\"requestId\":-3}", &p, &err));
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("{\"requestId\":1.5}", &p, &err));
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("{\"requestId\":\"5\"}", &p, &err));
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("{\"requestId\":99999999999}", &p, &err));
	stk_respond_params_clear(&p);

	g_assert_false(parse_respond("{\"requestId\":5,\"action\":\"explode\"}", &p, &err));
	g_assert_cmpstr(err, ==, "Unknown action");
	stk_respond_params_clear(&p);
	/* a prefix of a known action is not that action */
	g_assert_false(parse_respond("{\"requestId\":5,\"action\":\"go\"}", &p, &err));
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("{\"requestId\":5,\"action\":3}", &p, &err));
	stk_respond_params_clear(&p);

	g_assert_false(parse_respond("{\"requestId\":5,\"value\":2.5}", &p, &err));
	g_assert_cmpstr(err, ==, "The value has to be an integer");
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("{\"requestId\":5,\"value\":[1]}", &p, &err));
	g_assert_cmpstr(err, ==, "Unsupported value type");
	stk_respond_params_clear(&p);

	g_assert_false(stk_respond_params_parse(NULL, &p, &err));
	g_assert_cmpstr(err, ==, "Malformed json.");
	stk_respond_params_clear(&p);
	g_assert_false(parse_respond("[1,2]", &p, &err));
	stk_respond_params_clear(&p);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/stkjson/selection", test_selection_json);
	g_test_add_func("/stkjson/type-specific-fields", test_type_specific_fields);
	g_test_add_func("/stkjson/menu-and-ended", test_menu_and_ended);
	g_test_add_func("/stkjson/respond-parsing", test_respond_parsing);

	return g_test_run();
}

// vim:ts=4:sw=4:noexpandtab
