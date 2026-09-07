#ifndef LINKPULSE_AUTOSTART_H
#define LINKPULSE_AUTOSTART_H

#include <stdbool.h>

#include "linkpulse/status.h"

/* Whether LinkPulse is currently registered to start on login. Queries the
   registry directly (rather than a cached/config value) since this can be
   changed outside the app, e.g. via Windows Settings > Apps > Startup. */
bool lp_autostart_is_enabled(void);

/* Registers (or unregisters) LinkPulse to start on login, running with --tray. */
lp_status_t lp_autostart_set(bool enabled);

#endif /* LINKPULSE_AUTOSTART_H */
