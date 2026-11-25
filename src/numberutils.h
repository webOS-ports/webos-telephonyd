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

#ifndef NUMBERUTILS_H_
#define NUMBERUTILS_H_

#include <glib.h>

/* A destination number has at most this many digits (3GPP TS 24.011) */
#define PHONE_NUMBER_MAX_DIGITS 20

/*
 * The number as the network takes it: a leading "+" and digits only.
 * Spaces, tabs, dashes, dots and brackets are dropped, so
 * "+31 6 1234-5678" as stored with a contact becomes "+31612345678".
 * Returns NULL for anything that isn't a phone number (an e-mail
 * address, letters, a "+" anywhere but first, more than
 * PHONE_NUMBER_MAX_DIGITS digits). Free the result with g_free().
 */
gchar *phone_number_normalize(const char *addr);

#endif
