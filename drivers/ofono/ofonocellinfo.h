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

#ifndef OFONO_CELL_INFO_H_
#define OFONO_CELL_INFO_H_

#include <stdbool.h>

#include <glib.h>

#include "telephonydriver.h"

/*
 * The signal of the cells a modem is registered on, from ofono-ext's
 * org.nemomobile.ofono.CellInfo (GetCells) and org.nemomobile.ofono.Cell
 * (GetAll) on each cell.
 *
 * GetCells subscribes the caller, and while anyone is subscribed ofono asks
 * the modem for its cells every 5 seconds. So telephonyd subscribes only
 * while it is asked: each query renews the subscription, and it ends a
 * while after the last one.
 */

struct ofono_cell_info;

typedef void (*ofono_cell_info_query_cb)(const GError *error, const struct telephony_cell_signal *cells,
										 unsigned int count, void *data);

/* linger_ms: how long the subscription outlives the last query */
struct ofono_cell_info* ofono_cell_info_create(const gchar *path, unsigned int linger_ms);
/* Pending queries are answered with an error */
void ofono_cell_info_free(struct ofono_cell_info *info);

void ofono_cell_info_query(struct ofono_cell_info *info, ofono_cell_info_query_cb cb, void *data);

/* Whether ofono has telephonyd subscribed right now */
bool ofono_cell_info_is_subscribed(const struct ofono_cell_info *info);

/*
 * A cell's GetAll type and properties in dBm and dB, as the radio HAL's
 * SignalStrength types define them: RSRP and RSRQ come multiplied by -1,
 * LTE's RS-SNR in 0.1 dB, the GSM, UMTS and LTE signal strength as TS 27.007
 * 8.5's 0-31. False for a type it does not know.
 */
bool ofono_cell_signal_parse(const gchar *type, GVariant *properties, struct telephony_cell_signal *signal);

#endif

// vim:ts=4:sw=4:noexpandtab
