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

#ifndef OFONO_DATA_SIM_H_
#define OFONO_DATA_SIM_H_

/*
 * Tells ofono which SIM carries packet data.
 *
 * On a multi-SIM modem only one slot may attach for data, and with the
 * Sailfish slot manager (ofono-ext, used by ofono-binder-plugin) ofono itself
 * decides which: org.nemomobile.ofono.ModemManager.DefaultDataSim on "/",
 * keyed by IMSI. Picking a data SIM in our own settings alone therefore never
 * moved the data attach. This hands the choice to ofono.
 *
 * Create one per appearance of org.ofono and free it when ofono goes away.
 * Without the slot manager (ofono without ofono-ext, phonesim) the calls fail
 * once, it is noted, and nothing else happens.
 */

struct ofono_data_sim;

struct ofono_data_sim* ofono_data_sim_create(void);
void ofono_data_sim_free(struct ofono_data_sim *ds);

/*
 * The IMSI of the SIM that should carry data; NULL while it is not known.
 * Sent to ofono once its current setting is known, and only when it differs.
 */
void ofono_data_sim_set_imsi(struct ofono_data_sim *ds, const char *imsi);

/* ofono's DefaultDataSim as last seen ("auto" when none), NULL until known. */
const char* ofono_data_sim_get_current(struct ofono_data_sim *ds);

/* FALSE once ofono turned out not to have the slot manager. */
gboolean ofono_data_sim_is_supported(struct ofono_data_sim *ds);

#endif

// vim:ts=4:sw=4:noexpandtab
