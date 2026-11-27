/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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
 * The serving cells' signal: ofono-ext's CellInfo properties converted to
 * dBm and dB, and the CellInfo subscription held only while asked, against
 * a fake ofono on a private bus.
 *
 * The fake implements org.nemomobile.ofono.CellInfo on /ril_1 as ofono-ext's
 * cell-info-dbus.c does (GetCells subscribes the caller and lists the cell
 * paths, Unsubscribe ends that) and org.nemomobile.ofono.Cell on each cell
 * (GetAll: version, type, registered, properties).
 */

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "ofonocellinfo.h"

#define MODEM_PATH "/ril_1"
#define LINGER_MS 300

static const gchar fake_ofono_xml[] =
	"<node>"
	"  <interface name='org.nemomobile.ofono.CellInfo'>"
	"    <method name='GetCells'><arg type='ao' direction='out'/></method>"
	"    <method name='Unsubscribe'/>"
	"  </interface>"
	"  <interface name='org.nemomobile.ofono.Cell'>"
	"    <method name='GetAll'><arg type='i' direction='out'/><arg type='s' direction='out'/>"
	"      <arg type='b' direction='out'/><arg type='a{sv}' direction='out'/></method>"
	"  </interface>"
	"</node>";

static struct {
	GDBusConnection *conn;
	int get_cells;
	int unsubscribes;
	gboolean fail_get_cells;
} fake;

/* What the code under test answered */
static struct {
	int answers;
	gboolean failed;
	GArray *cells;
} seen;

/* The Brax3's serving LTE cell and a neighbour, as its ofono reported them */
static GVariant* lte_properties(gint32 signal_strength, gint32 rsrp, gint32 rsrq, gint32 rssnr)
{
	GVariantBuilder b;

	g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&b, "{sv}", "mcc", g_variant_new_int32(204));
	g_variant_builder_add(&b, "{sv}", "signalStrength", g_variant_new_int32(signal_strength));
	g_variant_builder_add(&b, "{sv}", "rsrp", g_variant_new_int32(rsrp));
	g_variant_builder_add(&b, "{sv}", "rsrq", g_variant_new_int32(rsrq));
	g_variant_builder_add(&b, "{sv}", "rssnr", g_variant_new_int32(rssnr));
	g_variant_builder_add(&b, "{sv}", "cqi", g_variant_new_int32(30));
	return g_variant_builder_end(&b);
}

static void fake_method_call(GDBusConnection *conn, const gchar *sender, const gchar *path,
                             const gchar *iface, const gchar *method, GVariant *params,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
	if (g_str_equal(method, "GetCells")) {
		GVariantBuilder paths;

		fake.get_cells++;
		if (fake.fail_get_cells) {
			g_dbus_method_invocation_return_dbus_error(invocation, "org.ofono.Error.Failed", "Operation failed");
			return;
		}
		g_variant_builder_init(&paths, G_VARIANT_TYPE("ao"));
		g_variant_builder_add(&paths, "o", MODEM_PATH "/cell_1");
		g_variant_builder_add(&paths, "o", MODEM_PATH "/cell_2");
		/* listed, then gone before its GetAll */
		g_variant_builder_add(&paths, "o", MODEM_PATH "/cell_9");
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(ao)", &paths));
	}
	else if (g_str_equal(method, "Unsubscribe")) {
		fake.unsubscribes++;
		g_dbus_method_invocation_return_value(invocation, NULL);
	}
	else if (g_str_equal(method, "GetAll") && g_str_equal(path, MODEM_PATH "/cell_1")) {
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(isb@a{sv})", 1, "lte", TRUE, lte_properties(11, 98, 7, 0)));
	}
	else if (g_str_equal(method, "GetAll") && g_str_equal(path, MODEM_PATH "/cell_2")) {
		g_dbus_method_invocation_return_value(invocation,
			g_variant_new("(isb@a{sv})", 1, "lte", FALSE, lte_properties(14, 99, 14, 0)));
	}
	else
		g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownObject", path);
}

static const GDBusInterfaceVTable fake_vtable = { fake_method_call, NULL, NULL, { 0 } };

static void answered_cb(const GError *error, const struct telephony_cell_signal *cells, unsigned int count, void *data)
{
	g_assert_true(data == &seen);
	seen.answers++;
	seen.failed = error != NULL;
	g_array_set_size(seen.cells, 0);
	if (count)
		g_array_append_vals(seen.cells, cells, count);
}

static gboolean timed_out(gpointer data)
{
	*(gboolean *) data = TRUE;
	return G_SOURCE_REMOVE;
}

static void wait_for_answers(int answers)
{
	gboolean timeout = FALSE;
	guint id = g_timeout_add(3000, timed_out, &timeout);

	while (seen.answers < answers && !timeout)
		g_main_context_iteration(NULL, TRUE);
	if (!timeout)
		g_source_remove(id);
	g_assert_cmpint(seen.answers, ==, answers);
}

static void spin(unsigned int ms)
{
	gboolean timeout = FALSE;

	g_timeout_add(ms, timed_out, &timeout);
	while (!timeout)
		g_main_context_iteration(NULL, TRUE);
}

static void reset(void)
{
	fake.get_cells = 0;
	fake.unsubscribes = 0;
	fake.fail_get_cells = FALSE;
	seen.answers = 0;
	seen.failed = FALSE;
	g_array_set_size(seen.cells, 0);
}

static GVariant* props(const char *first, ...)
{
	GVariantBuilder b;
	const char *name;
	va_list ap;

	g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
	va_start(ap, first);
	for (name = first; name; name = va_arg(ap, const char *))
		g_variant_builder_add(&b, "{sv}", name, g_variant_new_int32(va_arg(ap, int)));
	va_end(ap);
	return g_variant_ref_sink(g_variant_builder_end(&b));
}

static void test_parse_lte(void)
{
	struct telephony_cell_signal s;
	GVariant *p = lte_properties(11, 98, 7, 125);

	g_variant_ref_sink(p);
	g_assert_true(ofono_cell_signal_parse("lte", p, &s));
	g_assert_cmpstr(s.type, ==, "lte");
	g_assert_true(s.has_rssi);
	g_assert_cmpint(s.rssi, ==, -91);
	g_assert_true(s.has_rsrp);
	g_assert_cmpint(s.rsrp, ==, -98);
	g_assert_true(s.has_rsrq);
	g_assert_cmpint(s.rsrq, ==, -7);
	g_assert_true(s.has_sinr);
	g_assert_cmpfloat(s.sinr, ==, 12.5);
	g_variant_unref(p);

	/* The ends of each range, and just past them */
	p = props("signalStrength", 31, "rsrp", 44, "rsrq", 3, "rssnr", -200, NULL);
	g_assert_true(ofono_cell_signal_parse("lte", p, &s));
	g_assert_cmpint(s.rssi, ==, -51);
	g_assert_cmpint(s.rsrp, ==, -44);
	g_assert_cmpint(s.rsrq, ==, -3);
	g_assert_cmpfloat(s.sinr, ==, -20.0);
	g_variant_unref(p);

	p = props("signalStrength", 0, "rsrp", 140, "rsrq", 20, "rssnr", 300, NULL);
	g_assert_true(ofono_cell_signal_parse("lte", p, &s));
	g_assert_cmpint(s.rssi, ==, -113);
	g_assert_cmpint(s.rsrp, ==, -140);
	g_assert_cmpint(s.rsrq, ==, -20);
	g_assert_cmpfloat(s.sinr, ==, 30.0);
	g_variant_unref(p);

	/* 99 is TS 27.007's "not known"; the rest is out of the HAL's ranges */
	p = props("signalStrength", 99, "rsrp", 43, "rsrq", 21, "rssnr", 301, NULL);
	g_assert_true(ofono_cell_signal_parse("lte", p, &s));
	g_assert_false(s.has_rssi);
	g_assert_false(s.has_rsrp);
	g_assert_false(s.has_rsrq);
	g_assert_false(s.has_sinr);
	g_variant_unref(p);

	p = props("rsrp", 141, "rsrq", 2, "rssnr", -201, "signalStrength", -1, NULL);
	g_assert_true(ofono_cell_signal_parse("lte", p, &s));
	g_assert_false(s.has_rssi || s.has_rsrp || s.has_rsrq || s.has_sinr);
	g_variant_unref(p);

	/* ofono leaves out what the modem did not report */
	p = props("rsrp", 100, NULL);
	g_assert_true(ofono_cell_signal_parse("lte", p, &s));
	g_assert_true(s.has_rsrp);
	g_assert_false(s.has_rssi || s.has_rsrq || s.has_sinr);
	g_variant_unref(p);
}

static void test_parse_nr(void)
{
	struct telephony_cell_signal s;
	GVariant *p;

	p = props("ssRsrp", 100, "ssRsrq", 11, "ssSinr", 17, "csiRsrp", 90, NULL);
	g_assert_true(ofono_cell_signal_parse("nr", p, &s));
	g_assert_cmpstr(s.type, ==, "nr");
	g_assert_false(s.has_rssi);
	g_assert_cmpint(s.rsrp, ==, -100);
	g_assert_cmpint(s.rsrq, ==, -11);
	g_assert_cmpfloat(s.sinr, ==, 17.0);
	g_variant_unref(p);

	/* SS-RSRQ's range runs from -20 to 43 before the sign is turned */
	p = props("ssRsrp", 44, "ssRsrq", -20, "ssSinr", -23, NULL);
	g_assert_true(ofono_cell_signal_parse("nr", p, &s));
	g_assert_cmpint(s.rsrp, ==, -44);
	g_assert_cmpint(s.rsrq, ==, 20);
	g_assert_cmpfloat(s.sinr, ==, -23.0);
	g_variant_unref(p);

	p = props("ssRsrp", 140, "ssRsrq", 43, "ssSinr", 40, NULL);
	g_assert_true(ofono_cell_signal_parse("nr", p, &s));
	g_assert_cmpint(s.rsrp, ==, -140);
	g_assert_cmpint(s.rsrq, ==, -43);
	g_assert_cmpfloat(s.sinr, ==, 40.0);
	g_variant_unref(p);

	p = props("ssRsrp", 141, "ssRsrq", 44, "ssSinr", 41, NULL);
	g_assert_true(ofono_cell_signal_parse("nr", p, &s));
	g_assert_false(s.has_rsrp || s.has_rsrq || s.has_sinr);
	g_variant_unref(p);

	p = props("ssRsrp", 43, "ssRsrq", -21, "ssSinr", -24, NULL);
	g_assert_true(ofono_cell_signal_parse("nr", p, &s));
	g_assert_false(s.has_rsrp || s.has_rsrq || s.has_sinr);
	g_variant_unref(p);
}

static void test_parse_gsm_wcdma(void)
{
	struct telephony_cell_signal s;
	GVariant *p = props("signalStrength", 20, "bitErrorRate", 0, "rsrp", 90, NULL);

	g_assert_true(ofono_cell_signal_parse("gsm", p, &s));
	g_assert_cmpstr(s.type, ==, "gsm");
	g_assert_cmpint(s.rssi, ==, -73);
	/* not a GSM measurement */
	g_assert_false(s.has_rsrp);

	g_assert_true(ofono_cell_signal_parse("wcdma", p, &s));
	g_assert_cmpstr(s.type, ==, "wcdma");
	g_assert_cmpint(s.rssi, ==, -73);
	g_variant_unref(p);

	p = props("signalStrength", 32, NULL);
	g_assert_true(ofono_cell_signal_parse("gsm", p, &s));
	g_assert_false(s.has_rssi);
	g_variant_unref(p);
}

static void test_parse_refused(void)
{
	struct telephony_cell_signal s;
	GVariant *p = props("signalStrength", 20, NULL);
	GVariant *wrong;

	g_assert_false(ofono_cell_signal_parse("cdma", p, &s));
	g_assert_false(ofono_cell_signal_parse("unknown", p, &s));
	g_assert_false(ofono_cell_signal_parse(NULL, p, &s));
	g_assert_false(ofono_cell_signal_parse("lte", NULL, &s));
	g_variant_unref(p);

	wrong = g_variant_ref_sink(g_variant_new_int32(3));
	g_assert_false(ofono_cell_signal_parse("lte", wrong, &s));
	g_variant_unref(wrong);

	/* A property of another type is not read as a number */
	{
		GVariantBuilder b;

		g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&b, "{sv}", "rsrp", g_variant_new_string("98"));
		p = g_variant_ref_sink(g_variant_builder_end(&b));
		g_assert_true(ofono_cell_signal_parse("lte", p, &s));
		g_assert_false(s.has_rsrp);
		g_variant_unref(p);
	}
}

/* The serving cell only, converted; the neighbour and a cell gone are not */
static void test_query(void)
{
	struct ofono_cell_info *info;
	struct telephony_cell_signal *cell;

	reset();
	info = ofono_cell_info_create(MODEM_PATH, LINGER_MS);
	g_assert_nonnull(info);
	g_assert_false(ofono_cell_info_is_subscribed(info));

	ofono_cell_info_query(info, answered_cb, &seen);
	wait_for_answers(1);
	g_assert_false(seen.failed);
	g_assert_cmpuint(seen.cells->len, ==, 1);
	cell = &g_array_index(seen.cells, struct telephony_cell_signal, 0);
	g_assert_cmpstr(cell->type, ==, "lte");
	g_assert_cmpint(cell->rsrp, ==, -98);
	g_assert_cmpint(cell->rsrq, ==, -7);
	g_assert_cmpint(cell->rssi, ==, -91);
	g_assert_true(cell->has_sinr);
	g_assert_cmpfloat(cell->sinr, ==, 0.0);
	g_assert_cmpint(fake.get_cells, ==, 1);
	g_assert_true(ofono_cell_info_is_subscribed(info));

	ofono_cell_info_free(info);
	spin(100);
	/* freed while subscribed: unsubscribed */
	g_assert_cmpint(fake.unsubscribes, ==, 1);
}

/* Subscribed while asked, and not for long after */
static void test_subscription_lingers(void)
{
	struct ofono_cell_info *info;

	reset();
	info = ofono_cell_info_create(MODEM_PATH, LINGER_MS);
	ofono_cell_info_query(info, answered_cb, &seen);
	wait_for_answers(1);

	/* asked again before the linger ran out: it starts over */
	spin(LINGER_MS * 2 / 3);
	ofono_cell_info_query(info, answered_cb, &seen);
	wait_for_answers(2);
	g_assert_cmpint(fake.get_cells, ==, 2);
	spin(LINGER_MS * 2 / 3);
	g_assert_cmpint(fake.unsubscribes, ==, 0);
	g_assert_true(ofono_cell_info_is_subscribed(info));

	spin(LINGER_MS);
	g_assert_cmpint(fake.unsubscribes, ==, 1);
	g_assert_false(ofono_cell_info_is_subscribed(info));

	/* nothing more to end when freed */
	ofono_cell_info_free(info);
	spin(100);
	g_assert_cmpint(fake.unsubscribes, ==, 1);
}

static void test_get_cells_fails(void)
{
	struct ofono_cell_info *info;

	reset();
	fake.fail_get_cells = TRUE;
	info = ofono_cell_info_create(MODEM_PATH, LINGER_MS);
	ofono_cell_info_query(info, answered_cb, &seen);
	wait_for_answers(1);
	g_assert_true(seen.failed);
	g_assert_false(ofono_cell_info_is_subscribed(info));

	ofono_cell_info_free(info);
	spin(100);
	g_assert_cmpint(fake.unsubscribes, ==, 0);
}

/* The modem goes with a query on its way: the query is still answered */
static void test_free_while_asking(void)
{
	struct ofono_cell_info *info;

	reset();
	info = ofono_cell_info_create(MODEM_PATH, LINGER_MS);
	ofono_cell_info_query(info, answered_cb, &seen);
	ofono_cell_info_free(info);
	wait_for_answers(1);
	g_assert_true(seen.failed);
	spin(100);
}

static void test_without_cell_info(void)
{
	reset();
	ofono_cell_info_query(NULL, answered_cb, &seen);
	g_assert_cmpint(seen.answers, ==, 1);
	g_assert_false(seen.failed);
	g_assert_cmpuint(seen.cells->len, ==, 0);
	g_assert_false(ofono_cell_info_is_subscribed(NULL));
	ofono_cell_info_free(NULL);
}

int main(int argc, char **argv)
{
	GTestDBus *bus;
	GDBusNodeInfo *node;
	GError *error = NULL;
	GVariant *result;
	guint ids[3];
	unsigned int i;
	int ret;

	g_test_init(&argc, &argv, NULL);
	seen.cells = g_array_new(FALSE, TRUE, sizeof(struct telephony_cell_signal));

	bus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(bus);
	/* the code under test uses G_BUS_TYPE_SYSTEM */
	g_setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(bus), TRUE);

	fake.conn = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, NULL, &error);
	g_assert_no_error(error);

	node = g_dbus_node_info_new_for_xml(fake_ofono_xml, &error);
	g_assert_no_error(error);
	ids[0] = g_dbus_connection_register_object(fake.conn, MODEM_PATH, node->interfaces[0],
											   &fake_vtable, NULL, NULL, &error);
	g_assert_no_error(error);
	ids[1] = g_dbus_connection_register_object(fake.conn, MODEM_PATH "/cell_1", node->interfaces[1],
											   &fake_vtable, NULL, NULL, &error);
	g_assert_no_error(error);
	ids[2] = g_dbus_connection_register_object(fake.conn, MODEM_PATH "/cell_2", node->interfaces[1],
											   &fake_vtable, NULL, NULL, &error);
	g_assert_no_error(error);

	result = g_dbus_connection_call_sync(fake.conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
										 "org.freedesktop.DBus", "RequestName",
										 g_variant_new("(su)", "org.ofono", 0x4), G_VARIANT_TYPE("(u)"),
										 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	g_assert_no_error(error);
	g_variant_unref(result);

	g_test_add_func("/ofonocellinfo/parse-lte", test_parse_lte);
	g_test_add_func("/ofonocellinfo/parse-nr", test_parse_nr);
	g_test_add_func("/ofonocellinfo/parse-gsm-wcdma", test_parse_gsm_wcdma);
	g_test_add_func("/ofonocellinfo/parse-refused", test_parse_refused);
	g_test_add_func("/ofonocellinfo/query", test_query);
	g_test_add_func("/ofonocellinfo/subscription-lingers", test_subscription_lingers);
	g_test_add_func("/ofonocellinfo/get-cells-fails", test_get_cells_fails);
	g_test_add_func("/ofonocellinfo/free-while-asking", test_free_while_asking);
	g_test_add_func("/ofonocellinfo/without-cell-info", test_without_cell_info);

	ret = g_test_run();

	for (i = 0; i < G_N_ELEMENTS(ids); i++)
		g_dbus_connection_unregister_object(fake.conn, ids[i]);
	g_dbus_node_info_unref(node);
	g_object_unref(fake.conn);
	g_array_free(seen.cells, TRUE);
	g_test_dbus_down(bus);
	g_object_unref(bus);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab
