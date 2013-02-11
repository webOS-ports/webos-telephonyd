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

extern struct telephony_driver ofono_telephony_driver;

#include <glib.h>
#include <errno.h>
#include <string.h>

int ofono_init(void)
{
	return telephony_driver_register(&ofono_telephony_driver);
}

void ofono_exit(void)
{
	telephony_driver_unregister(&ofono_telephony_driver);
}

// vim:ts=4:sw=4:noexpandtab
