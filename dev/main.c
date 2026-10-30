/*
 * PlayTime - PS5 payload that serves a web UI listing installed games and their play time.
 *
 *   main.c    wiring: routes requests to the modules below
 *   games.c   goal: reads installed games from app.db
 *   hltb.c    native PS5 HowLongToBeat login, per-game selection & background auto-sync
 *   server.c  minimal HTTP server (GET, plus POST for play time edits and HLTB sync)
 *   web/index.html  the web page, embedded into the payload at build time
 *
 * Open the UI on the PS5 browser (launched automatically) or from any device
 * on the same network at http://<ps5-ip>:<PLAYTIME_PORT from config.h>
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "config.h"
#include "games.h"
#include "hltb.h"
#include "server.h"

/*
 * Embed web/index.html into the payload at build time (no external tools
 * needed). Defines: const uint8_t web_index_html[]; const size_t web_index_html_size;
 * The path is relative to the directory `make` runs in.
 */
#define INCASSET(name, file)                    \
    __asm__(".section .rodata\n"                \
            ".global " #name "\n"               \
            ".global " #name "_end\n"           \
            ".global " #name "_size\n"          \
            ".align 16\n"                       \
            #name ":\n"                         \
            ".incbin \"" file "\"\n"            \
            #name "_end:\n"                     \
            #name "_size:\n"                    \
            ".quad " #name "_end - " #name "\n" \
            ".previous\n");                     \
    extern const uint8_t name[];                \
    extern const size_t name##_size;

INCASSET(web_index_html, "web/index.html");
INCASSET(res_ps4_logo, "resources/ps4_logo.webp");
INCASSET(res_ps5_logo, "resources/ps5_logo.png");
INCASSET(res_app_icon, "resources/app_icon_sm.png");

/* Titles that are NOT listed: edit PLAYTIME_EXCLUDED_TITLES in config.h. */
static const char *const EXCLUDED_TITLES[] = {PLAYTIME_EXCLUDED_TITLES};

int sceUserServiceInitialize(void *arg);
int sceUserServiceGetUserName(int user_id, char *name, size_t size);
int sceSystemServiceLaunchWebBrowser(const char *uri, void *arg);

typedef struct {
    const char *database_path;
    const char *hltb_database_path;
    int port;
    uint32_t firmware_version;
} context_t;

static void format_firmware(char *buffer, size_t size, uint32_t version) {
    unsigned int major = ((version >> 28) & 0xFu) * 10u + ((version >> 24) & 0xFu);
    unsigned int minor = ((version >> 20) & 0xFu) * 10u + ((version >> 16) & 0xFu);

    snprintf(buffer, size, "%02u.%02u", major, minor);
}

static void send_json_error(int fd, int status, const char *message) {
    char body[384];
    char escaped[256];
    size_t pos = 0;

    for(; *message && pos + 2 < sizeof escaped; message++) {
        if(*message == '"' || *message == '\\') {
            escaped[pos++] = '\\';
            escaped[pos++] = *message;
        } else if((unsigned char)*message >= 0x20u) {
            escaped[pos++] = *message;
        }
    }
    escaped[pos] = '\0';
    snprintf(body, sizeof body, "{\"error\":\"%s\"}", escaped);
    server_send(fd, status, "application/json; charset=utf-8", body,
                strlen(body), 0);
}

static int hex_val(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Looks up key in a raw query/form string ("a=1&b=2") and percent-decodes it. */
static int query_get(const char *query, const char *key, char *value,
                     size_t value_size) {
    size_t key_len = strlen(key);
    const char *p = query;

    if(!p) {
        return -1;
    }

    while(*p) {
        const char *end = strchr(p, '&');
        size_t span = end ? (size_t)(end - p) : strlen(p);

        if(span >= key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *src = p + key_len + 1;
            const char *src_end = p + span;
            size_t out = 0;

            while(src < src_end) {
                char c = *src++;
                if(c == '+') {
                    c = ' ';
                } else if(c == '%' && src + 2 <= src_end) {
                    int hi = hex_val(src[0]);
                    int lo = hex_val(src[1]);
                    if(hi >= 0 && lo >= 0) {
                        c = (char)((hi << 4) | lo);
                        src += 2;
                    }
                }
                if(out + 1 >= value_size) {
                    return -1;
                }
                value[out++] = c;
            }
            value[out] = '\0';
            return 0;
        }
        if(!end) {
            break;
        }
        p = end + 1;
    }
    return -1;
}

/* Looks up key in either req->body (form-urlencoded) or req->query. */
static int param_get(const server_request_t *req, const char *key, char *value,
                     size_t value_size) {
    if(req->body && req->body[0] &&
       query_get(req->body, key, value, value_size) == 0) {
        return 0;
    }
    return query_get(req->query, key, value, value_size);
}

/* POST /api/playtime?id=<TITLEID>&user=<userId>&value=<units> */
static void handle_set_playtime(int fd, const server_request_t *req,
                                const context_t *ctx) {
    char id[16];
    char user[24];
    char number[24];
    char error[256];
    char *end = NULL;
    long long value;

    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    if(query_get(req->query, "id", id, sizeof id) != 0 ||
       query_get(req->query, "user", user, sizeof user) != 0 ||
       query_get(req->query, "value", number, sizeof number) != 0 ||
       number[0] == '\0') {
        send_json_error(fd, 400, "Missing or invalid parameters.");
        return;
    }
    value = strtoll(number, &end, 10);
    if(!end || *end != '\0') {
        send_json_error(fd, 400, "Value must be a whole number.");
        return;
    }
    if(games_set_playtime(ctx->database_path, id, user, value, error,
                          sizeof error) != 0) {
        send_json_error(fd, 500, error);
        return;
    }
    server_send(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, 0);
}

/* POST /api/hltb/config?user=<userId>&auto=<0|1> */
static void handle_hltb_config(int fd, const server_request_t *req,
                               const context_t *ctx) {
    char user[24] = {0};
    char auto_str[8] = {0};
    char error[256];
    int auto_sync = 1;

    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    (void)param_get(req, "user", user, sizeof user);
    if(param_get(req, "auto", auto_str, sizeof auto_str) == 0) {
        auto_sync = strcmp(auto_str, "0") != 0;
    }
    if(hltb_save_config(ctx->hltb_database_path, user, auto_sync, error,
                        sizeof error) != 0) {
        send_json_error(fd, 500, error);
        return;
    }
    server_send(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, 0);
}

/* POST /api/hltb/login  (form-urlencoded body: user, username, password, token, cookie, ua) */
static void handle_hltb_login(int fd, const server_request_t *req,
                              const context_t *ctx) {
    char user[24] = {0};
    char username[128] = {0};
    char password[256] = {0};
    char token[4096] = {0};
    char cookie[2048] = {0};
    char ua[256] = {0};
    char error[256] = {0};

    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    (void)param_get(req, "user", user, sizeof user);
    (void)param_get(req, "username", username, sizeof username);
    (void)param_get(req, "password", password, sizeof password);
    (void)param_get(req, "token", token, sizeof token);
    (void)param_get(req, "cookie", cookie, sizeof cookie);
    (void)param_get(req, "ua", ua, sizeof ua);

    if(hltb_login(ctx->hltb_database_path, user, username, password, token,
                  cookie, ua, error, sizeof error) != 0) {
        send_json_error(fd, 400, error[0] ? error : "HLTB login failed.");
        return;
    }
    server_send(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, 0);
}

/* POST /api/hltb/logout[?user=<userId>] */
static void handle_hltb_logout(int fd, const server_request_t *req,
                               const context_t *ctx) {
    char user[24] = {0};
    char error[256] = {0};

    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    (void)param_get(req, "user", user, sizeof user);
    if(hltb_logout(ctx->hltb_database_path, user, error, sizeof error) != 0) {
        send_json_error(fd, 500, error);
        return;
    }
    server_send(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, 0);
}

/* POST /api/hltb/select?user=<userId>&id=<TITLEID>&selected=<0|1>[&hltb_id=<id>][&list=<playing|backlog|completed>] */
static void handle_hltb_select(int fd, const server_request_t *req,
                               const context_t *ctx) {
    char user[24] = {0};
    char id[16] = {0};
    char title[256] = {0};
    char sel_str[8] = {0};
    char gid_str[24] = {0};
    char list_str[16] = {0};
    char error[256] = {0};
    int selected = 0;
    long long custom_hltb_id = -1;

    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    if(param_get(req, "user", user, sizeof user) != 0 ||
       param_get(req, "id", id, sizeof id) != 0 ||
       param_get(req, "selected", sel_str, sizeof sel_str) != 0) {
        send_json_error(fd, 400, "Missing or invalid parameters.");
        return;
    }
    selected = strcmp(sel_str, "0") != 0;
    (void)param_get(req, "title", title, sizeof title);
    if(param_get(req, "hltb_id", gid_str, sizeof gid_str) == 0 && gid_str[0]) {
        custom_hltb_id = strtoll(gid_str, NULL, 10);
    }
    (void)param_get(req, "list", list_str, sizeof list_str);

    if(hltb_select_game(ctx->hltb_database_path, user, id, title, selected,
                        custom_hltb_id, list_str, error, sizeof error) != 0) {
        send_json_error(fd, 500, error);
        return;
    }
    server_send(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, 0);
}

/* POST /api/hltb/sync[?force=1] */
static void handle_hltb_sync(int fd, const server_request_t *req,
                             const context_t *ctx) {
    char force_str[8] = {0};
    char error[256] = {0};
    char resp[128];
    int force = 0;
    int synced = 0;

    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    if(param_get(req, "force", force_str, sizeof force_str) == 0) {
        force = strcmp(force_str, "1") == 0;
    }
    if(hltb_sync_selected(ctx->database_path, ctx->hltb_database_path, force,
                          &synced, error, sizeof error) != 0) {
        send_json_error(fd, 400, error[0] ? error : "HLTB sync failed.");
        return;
    }
    snprintf(resp, sizeof resp, "{\"ok\":true,\"synced\":%d}", synced);
    server_send(fd, 200, "application/json; charset=utf-8", resp, strlen(resp), 0);
}

/* POST /api/hltb/match-all */
static void handle_hltb_match_all(int fd, const server_request_t *req,
                                  const context_t *ctx) {
    if(strcmp(req->method, "POST") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }
    if(!req->has_custom_header) {
        send_json_error(fd, 403, "Missing " PLAYTIME_CSRF_HEADER " header.");
        return;
    }
    (void)hltb_auto_match_library(ctx->database_path, ctx->hltb_database_path);
    server_send(fd, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11, 0);
}

static void handle_request(int fd, const server_request_t *req, void *user) {
    const context_t *ctx = (const context_t *)user;
    const char *path = req->path;

    if(strcmp(path, "/api/playtime") == 0) {
        handle_set_playtime(fd, req, ctx);
        return;
    }
    if(strcmp(path, "/api/hltb/config") == 0) {
        handle_hltb_config(fd, req, ctx);
        return;
    }
    if(strcmp(path, "/api/hltb/login") == 0) {
        handle_hltb_login(fd, req, ctx);
        return;
    }
    if(strcmp(path, "/api/hltb/logout") == 0) {
        handle_hltb_logout(fd, req, ctx);
        return;
    }
    if(strcmp(path, "/api/hltb/select") == 0) {
        handle_hltb_select(fd, req, ctx);
        return;
    }
    if(strcmp(path, "/api/hltb/sync") == 0) {
        handle_hltb_sync(fd, req, ctx);
        return;
    }
    if(strcmp(path, "/api/hltb/match-all") == 0) {
        handle_hltb_match_all(fd, req, ctx);
        return;
    }
    if(strcmp(req->method, "GET") != 0) {
        server_send(fd, 405, "text/plain", "Method not allowed", 18, 0);
        return;
    }

    if(strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        server_send(fd, 200, "text/html; charset=utf-8", (const char *)web_index_html,
                    web_index_html_size, 0);
    } else if(strcmp(path, "/resources/ps4_logo.webp") == 0 || strcmp(path, "/ps4_logo.webp") == 0) {
        server_send(fd, 200, "image/webp", (const char *)res_ps4_logo,
                    res_ps4_logo_size, 1);
    } else if(strcmp(path, "/resources/ps5_logo.png") == 0 || strcmp(path, "/ps5_logo.png") == 0) {
        server_send(fd, 200, "image/png", (const char *)res_ps5_logo,
                    res_ps5_logo_size, 1);
    } else if(strcmp(path, "/resources/app_icon.png") == 0 || strcmp(path, "/favicon.ico") == 0) {
        server_send(fd, 200, "image/png", (const char *)res_app_icon,
                    res_app_icon_size, 1);
    } else if(strcmp(path, "/api/info") == 0) {
        char ip[64];
        char firmware[16];
        char body[768];

        /* Branding strings come from config.h; avoid " and \ in them. */
        server_local_ip(ip, sizeof ip);
        format_firmware(firmware, sizeof firmware, ctx->firmware_version);
        snprintf(body, sizeof body,
                 "{\"ip\":\"%s\",\"port\":%d,\"firmware\":\"%s\","
                 "\"version\":\"%s\",\"name\":\"%s\",\"tagline\":\"%s\","
                 "\"author\":\"%s\",\"repo\":\"%s\",\"csrf\":\"%s\"}",
                 ip, ctx->port, firmware, PAYLOAD_VERSION, PLAYTIME_APP_NAME,
                 PLAYTIME_TAGLINE, PLAYTIME_AUTHOR, PLAYTIME_REPO_URL,
                 PLAYTIME_CSRF_HEADER);
        server_send(fd, 200, "application/json; charset=utf-8", body,
                    strlen(body), 0);
    } else if(strcmp(path, "/api/games") == 0) {
        char error[256];
        size_t length = 0;
        char *json = games_list_json(ctx->database_path, &length, error,
                                     sizeof error);

        if(!json) {
            send_json_error(fd, 500, error);
            return;
        }
        server_send(fd, 200, "application/json; charset=utf-8", json, length, 0);
        free(json);
    } else if(strcmp(path, "/api/hltb") == 0) {
        char error[256];
        size_t length = 0;
        char *json = hltb_state_json(ctx->hltb_database_path, &length, error,
                                     sizeof error);

        if(!json) {
            send_json_error(fd, 500, error);
            return;
        }
        server_send(fd, 200, "application/json; charset=utf-8", json, length, 0);
        free(json);
    } else if(strncmp(path, "/icon/", 6) == 0) {
        char title_id[16];
        char icon[256];
        size_t len = strlen(path + 6);

        /* Expect exactly "<TITLEID>.png"; games_icon_path() validates the ID. */
        if(len == 13 && strcmp(path + 6 + 9, ".png") == 0) {
            memcpy(title_id, path + 6, 9);
            title_id[9] = '\0';
            if(games_icon_path(ctx->database_path, title_id, icon,
                               sizeof icon) == 0 &&
               server_send_file(fd, icon, "image/png") == 0) {
                return;
            }
        }
        server_send(fd, 404, "text/plain", "Not found", 9, 0);
    } else {
        server_send(fd, 404, "text/plain", "Not found", 9, 0);
    }
}

/* User IDs in app.db table names are the decimal console user IDs. */
static int resolve_user_name(const char *user_id, char *name, size_t size) {
    char raw[32] = {0};
    char *end = NULL;
    long id = strtol(user_id, &end, 10);

    if(!end || *end != '\0' || id <= 0 || size < sizeof raw) {
        return -1;
    }
    if(sceUserServiceGetUserName((int)id, raw, sizeof raw) != 0 || !raw[0]) {
        return -1;
    }
    raw[sizeof raw - 1] = '\0';
    snprintf(name, size, "%s", raw);
    return 0;
}

static void launch_ps5_browser(int port) {
    char url[64];

    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    (void)sceSystemServiceLaunchWebBrowser(url, NULL);
}

static void terminate_previous_instance(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    size_t buf_size = 0;
    void *buf = NULL;
    pid_t self = getpid();
    int killed = 0;

    if(sysctl(mib, 4, NULL, &buf_size, NULL, 0) == 0 && buf_size > 0) {
        buf = malloc(buf_size);
        if(buf && sysctl(mib, 4, buf, &buf_size, NULL, 0) == 0) {
            char *ptr = (char *)buf;
            char *end = ptr + buf_size;
            while(ptr < end) {
                struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
                if(ki->ki_structsize <= 0) {
                    break;
                }
                ptr += ki->ki_structsize;
                if(ki->ki_pid != self && ki->ki_pid > 1 &&
                   (strcmp(ki->ki_tdname, PAYLOAD_PROCESS_NAME) == 0 ||
                    strcmp(ki->ki_comm, PAYLOAD_PROCESS_NAME) == 0 ||
                    strcmp(ki->ki_tdname, "PlayTime.elf") == 0 ||
                    strcmp(ki->ki_comm, "PlayTime.elf") == 0)) {
                    klog_printf("[PS5-HLTB] Killing previous instance %s (PID=%d)\n",
                                ki->ki_tdname, (int)ki->ki_pid);
                    kill(ki->ki_pid, SIGKILL);
                    killed = 1;
                }
            }
        }
        free(buf);
    }
    if(killed) {
        usleep(400000);
    }
}

int main(void) {
    context_t ctx;
    int listen_fd = -1;
    int attempt;

    terminate_previous_instance();

    /* Name shown in the running-process list (see config.h). */
    syscall(SYS_thr_set_name, -1, PAYLOAD_PROCESS_NAME);

    ctx.database_path = APP_DB_PATH;
    ctx.hltb_database_path = HLTB_DB_PATH;
    ctx.port = PLAYTIME_PORT;
    ctx.firmware_version = kernel_get_fw_version();

    klog_printf("[PS5-HLTB] Starting %s %s (PID=%d, port=%d)\n",
                PLAYTIME_APP_NAME, PAYLOAD_VERSION, (int)getpid(), ctx.port);

    (void)sceUserServiceInitialize(NULL);
    games_set_user_name_resolver(resolve_user_name);
    games_set_excluded(EXCLUDED_TITLES,
                       sizeof EXCLUDED_TITLES / sizeof EXCLUDED_TITLES[0]);

    for(attempt = 0; attempt < 6; attempt++) {
        listen_fd = server_start(ctx.port);
        if(listen_fd >= 0) {
            break;
        }
        terminate_previous_instance();
        usleep(250000);
    }
    if(listen_fd < 0) {
        char err[256];
        klog_printf("[PS5-HLTB] Port %d still busy; triggering sync & browser\n",
                    ctx.port);
        (void)hltb_sync_selected(ctx.database_path, ctx.hltb_database_path, 0,
                                 NULL, err, sizeof err);
        launch_ps5_browser(ctx.port);
        return 0;
    }

    klog_printf("[PS5-HLTB] HTTP server listening on port %d (fd=%d)\n",
                ctx.port, listen_fd);

    /* Automatically sync selected HLTB games on PS5 ELF startup and in background */
    hltb_start_auto_sync_thread(ctx.database_path, ctx.hltb_database_path);

    launch_ps5_browser(ctx.port);
    server_run(listen_fd, handle_request, &ctx);
    return 0;
}
