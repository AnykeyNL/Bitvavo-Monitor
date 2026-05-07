#pragma once

#ifdef __cplusplus
void settings_ui_init(const char *app_version, const char *app_version_date);
void settings_ui_poll(void);
void settings_ui_tick(void);
bool settings_ui_is_active(void);
#endif
