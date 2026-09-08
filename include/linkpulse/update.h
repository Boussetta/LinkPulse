#ifndef LINKPULSE_UPDATE_H
#define LINKPULSE_UPDATE_H

#include <stddef.h>

#include "linkpulse/status.h"

/* Returns LP_OK and the newer tag when an update exists, LP_ERR_NOT_FOUND when
   the current version is up to date, or an error when the check cannot run. */
lp_status_t lp_update_check_latest(const char *current_version, char *latest_version,
                                   size_t latest_cap);

lp_status_t lp_update_download_latest(char *installer_path, size_t path_cap);

#endif /* LINKPULSE_UPDATE_H */
