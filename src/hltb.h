#ifndef PS5_HLTB_HLTB_H
#define PS5_HLTB_HLTB_H

#include <stddef.h>

/*
 * HowLongToBeat (HLTB) native PS5 sync & persistence module.
 *
 * Stores the linked PS5 user, HLTB session cookie & account metadata,
 * per-game sync selection flags, HLTB game/submission IDs, and synced playtime
 * in a dedicated SQLite database (HLTB_DB_PATH).
 *
 * Uses Sony's native libSceHttp / libSceSsl / libSceNet stack so the PS5 ELF
 * can authenticate once (from phone/browser) and thereafter auto-sync selected
 * games directly from the PS5 in the background whenever the ELF is opened.
 */

/* Builds a JSON document with the current HLTB configuration and all per-game
 * selection/sync states:
 *   {"ps5User":"0314904805","loggedIn":1,"hltbUserId":12345,"hltbUsername":"...",
 *    "autoSync":1,"lastSync":"2026-10-29 16:00:00",
 *    "games":[{"user":"0314904805","id":"PPSA01234","selected":1,"hltbId":80123,
 *              "subId":991234,"hltbTitle":"...","syncedSec":3600,
 *              "compMain":74000,"compPlus":118000,"comp100":183000,
 *              "updatedAt":"2026-10-29 16:00:00"}]}
 * Returns a malloc'd NUL-terminated string (caller frees) and sets *length,
 * or NULL on failure with a message in error_message. */
char *hltb_state_json(const char *hltb_db_path, size_t *length,
                      char *error_message, size_t error_message_size);

/* Saves HLTB settings (linked PS5 user, auto-sync flag).
 * Returns 0 on success, -1 on failure. */
int hltb_save_config(const char *hltb_db_path, const char *ps5_user_id,
                     int auto_sync, char *error_message,
                     size_t error_message_size);

/* Logs in to HowLongToBeat from the PS5 using either:
 *   1) username + password + captcha_token (via POST /api/login), or
 *   2) cookie_override (a raw session cookie string, verified via GET /api/user).
 * On success, stores the session cookie, HLTB user ID, and username in
 * hltb_db_path so future PS5 ELF launches auto-sync without needing to log in
 * again. Returns 0 on success, -1 on failure with a message in error_message. */
int hltb_login(const char *hltb_db_path, const char *ps5_user_id,
               const char *username, const char *password,
               const char *captcha_token, const char *cookie_override,
               const char *user_agent, char *error_message,
               size_t error_message_size);

/* Clears the stored HLTB session cookie and account info for ps5_user_id (or active profile if NULL/empty). */
int hltb_logout(const char *hltb_db_path, const char *ps5_user_id,
                char *error_message, size_t error_message_size);

/* Selects or unselects a specific game for HLTB syncing (selected = 1 or 0),
 * and optionally sets or overrides its HLTB Game ID (pass custom_hltb_id < 0
 * to keep the existing HLTB Game ID) and list type ("playing", "backlog",
 * "completed"; pass NULL or empty string to keep existing/default to "playing").
 * Only games with selected = 1 are synced to HLTB. */
int hltb_select_game(const char *hltb_db_path, const char *ps5_user_id,
                     const char *title_id, const char *title, int selected,
                     long long custom_hltb_id, const char *list_type,
                     char *error_message, size_t error_message_size);

/* Syncs all selected games (selected = 1) from app_db_path to HLTB using the
 * stored session in hltb_db_path.
 * If force_all is 0, only games whose PS5 playtime exceeds synced_seconds are
 * pushed; if force_all is 1, all selected games with playtime > 0 are pushed.
 * Sets *synced_count_out (if non-NULL) to the number of games synced.
 * Returns 0 on success, -1 on failure with a message in error_message. */
int hltb_sync_selected(const char *app_db_path, const char *hltb_db_path,
                       int force_all, int *synced_count_out,
                       char *error_message, size_t error_message_size);

/* Automatically discovers non-homebrew games from app_db_path and looks up
 * their completion times from HowLongToBeat, storing them into hltb_db_path
 * so that expected playtimes and progress bars are available for all games. */
int hltb_auto_match_library(const char *app_db_path, const char *hltb_db_path);

/* Starts a detached background thread on the PS5 that immediately runs
 * hltb_sync_selected() on startup (if logged in and auto_sync is enabled)
 * and periodically checks for new playtime while the payload is running. */
void hltb_start_auto_sync_thread(const char *app_db_path,
                                 const char *hltb_db_path);

#endif
