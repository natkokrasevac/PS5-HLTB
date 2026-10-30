#ifndef PS5_HLTB_CONFIG_H
#define PS5_HLTB_CONFIG_H

/*
 * Central place for every project-specific string and constant.
 * Change them here; nothing else in the code base hard-codes them.
 * Values wrapped in #ifndef can also be overridden from the Makefile (-D...).
 */

/* Version is normally injected by the Makefile (-DPAYLOAD_VERSION). */
#ifndef PAYLOAD_VERSION
#define PAYLOAD_VERSION "v1.0.0"
#endif

/* Branding shown in the web UI (sent to the page by GET /api/info; do not use
 * " or \ in these strings) */
#define PS5_HLTB_APP_NAME       "PS5-HLTB"
#define PS5_HLTB_TAGLINE        "Play time tracker & HowLongToBeat sync"
#define PS5_HLTB_AUTHOR         "Mahdi Hosseini (Original) & Natko (Fork)"
#define PS5_HLTB_REPO_URL       "https://github.com/natkokrasevac/PS5-HLTB"

/* Name shown in the PS5 running-process list (thread name, max 19 chars). */
#ifndef PAYLOAD_PROCESS_NAME
#define PAYLOAD_PROCESS_NAME   "PS5-HLTB.elf"
#endif

/* Web server */
#ifndef PS5_HLTB_PORT
#define PS5_HLTB_PORT 7788
#endif

/* Anti-CSRF header the UI sends on edits ("<name>: 1"); requests without it
 * are rejected. Matched case-insensitively. The page reads the name from
 * /api/info, so changing it here is enough. */
#define PS5_HLTB_CSRF_HEADER    "X-PS5-HLTB"

/* Console database */
#ifndef APP_DB_PATH
#define APP_DB_PATH "/system_data/priv/mms/app.db"
#endif

/* Where app.db is copied once before the first edit. */
#ifndef GAMES_BACKUP_PATH
#define GAMES_BACKUP_PATH "/data/playtime_app.db.bak"
#endif

/* Where HowLongToBeat sync state and game mappings are persisted. */
#ifndef HLTB_DB_PATH
#define HLTB_DB_PATH "/data/playtime_hltb.db"
#endif

/* Upper bound accepted for an edited value (units; ~12 years of seconds). */
#define PS5_HLTB_MAX_VALUE 400000000LL

/* Seconds represented by one unit of playedTimeOnConsole. If the numbers on the
 * dashboard turn out to be off by a constant factor, this is the one place to
 * fix it (1 = the column counts seconds, which matches what the console shows).
 * It is sent to the UI as unitSeconds. */
#ifndef PS5_HLTB_UNIT_SECONDS
#define PS5_HLTB_UNIT_SECONDS 1
#endif

/* Maximum number of console users listed (one tbl_iconinfo_<userId> each). */
#define PS5_HLTB_MAX_USERS 8

/* Titles that are NOT listed: 9-character title IDs, comma separated. */
#define PS5_HLTB_EXCLUDED_TITLES \
    "PPSA01650", /* YouTube */    \
    "PPSA01651", /* YouTube */    \
    "PPSA01652"  /* YouTube */

#endif
