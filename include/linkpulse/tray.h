#ifndef LINKPULSE_TRAY_H
#define LINKPULSE_TRAY_H

#include <stdbool.h>

#include "linkpulse/sampler.h"

/* Runs the system tray application: creates a hidden window, a Shell_NotifyIcon
   tray icon, and a background thread polling the sampler. Blocks until the
   user chooses Exit from the context menu. Returns non-zero if another
   instance is already running or setup failed. */
int lp_tray_run(const lp_sampler_config_t *config, bool use_bits, unsigned interval_ms);

#endif /* LINKPULSE_TRAY_H */
