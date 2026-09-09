#ifndef LINKPULSE_NOTIFICATION_H
#define LINKPULSE_NOTIFICATION_H

#include <stdbool.h>

/* Sets the process identity used by Windows notifications and taskbar grouping. */
void lp_win32_set_app_user_model_id(void);

/* Displays a normal informational toast and reports whether submission succeeded. */
bool lp_win32_show_toast(const char *title, const char *message);

/* Displays an update toast containing the version and download action. */
bool lp_win32_show_update_toast(const char *version);

#endif /* LINKPULSE_NOTIFICATION_H */
