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

#ifndef OFONO_SMS_HISTORY_H_
#define OFONO_SMS_HISTORY_H_

#include <stdbool.h>

/*
 * Status reports for the SMS a modem sent, from org.ofono.SmsHistory
 * (ofono's sms_history plugin). message_path is the path the message had
 * while it was sent, as SendMessage returned it. Without the plugin
 * nothing is reported.
 */
typedef void (*ofono_sms_history_report_cb)(const char *message_path, bool delivered, void *user_data);

struct ofono_sms_history;

struct ofono_sms_history* ofono_sms_history_create(const char *modem_path,
												   ofono_sms_history_report_cb cb, void *user_data);
void ofono_sms_history_free(struct ofono_sms_history *history);

#endif

// vim:ts=4:sw=4:noexpandtab
