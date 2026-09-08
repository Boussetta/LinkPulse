#ifndef LINKPULSE_NOTIFICATION_H
#define LINKPULSE_NOTIFICATION_H

#include <stdbool.h>

void lp_win32_set_app_user_model_id(void);
bool lp_win32_show_toast(const char *title, const char *message);
bool lp_win32_show_update_toast(const char *version);

#endif /* LINKPULSE_NOTIFICATION_H */
