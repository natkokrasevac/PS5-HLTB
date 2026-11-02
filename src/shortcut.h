#ifndef SHORTCUT_H
#define SHORTCUT_H

#include <stddef.h>

#define SHORTCUT_TITLE_ID "HLTB00001"

/* Checks if the PS5-HLTB Media tab shortcut is currently installed */
int shortcut_is_installed(void);

/* Installs or updates the shortcut tile on the PS5 Media tab */
int shortcut_install(int port, const void *icon_data, size_t icon_size,
                     char *err_buf, size_t err_size);

/* Uninstalls the shortcut tile from the PS5 Media tab */
int shortcut_uninstall(char *err_buf, size_t err_size);

#endif /* SHORTCUT_H */

