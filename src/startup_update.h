#ifndef STARTUP_UPDATE_H
#define STARTUP_UPDATE_H

#include <glib-object.h>

typedef void (*StartupBanlistReadyFunc)(GObject *owner, const char *filename);
void startup_update_banlists(GObject *owner, StartupBanlistReadyFunc callback);

#endif
