/*
 * Just enough of luna-service2's header for src/utils.h, so that the D-Bus
 * level tests build on a development machine without the webOS stack. The
 * inline helpers that use these are never called by the tested units.
 */
#ifndef TEST_STUB_LUNASERVICE_H_
#define TEST_STUB_LUNASERVICE_H_

#include <stdbool.h>

typedef struct LSHandle LSHandle;
typedef struct LSMessage LSMessage;

void LSMessageRef(LSMessage *message);
void LSMessageUnref(LSMessage *message);

#endif
