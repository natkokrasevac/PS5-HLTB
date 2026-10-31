#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>

#include <ps5/klog.h>

#include "sqlite/sqlite3.h"
#include "config.h"
#include "games.h"
#include "hltb.h"

/* ---- Sony PS5 libSceSysmodule / libSceNet / libSceSsl / libSceHttp(2) ---- */

uint32_t sceSysmoduleLoadModuleInternal(uint32_t id);

#define SCE_SYSMODULE_INTERNAL_HTTP  0x8000000AU
#define SCE_SYSMODULE_INTERNAL_SSL   0x8000000BU
#define SCE_SYSMODULE_INTERNAL_NET   0x8000001CU

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int memid);

int sceSslInit(size_t poolSize);
int sceSslTerm(int sslId);

typedef int (*SceHttpsCallback)(int libsslCtxId, unsigned int verifyErr,
                                void *const sslCert[], int certNum,
                                void *userArg);

int sceHttpInit(int libnetMemId, int libsslCtxId, size_t poolSize);
int sceHttpTerm(int libhttpCtxId);
int sceHttpCreateTemplate(int libhttpCtxId, const char *userAgent, int httpVer,
                          int isAutoProxyConf);
int sceHttpDeleteTemplate(int tmplId);
int sceHttpsDisableOption(int id, unsigned int sslFlags);
int sceHttpsSetSslCallback(int id, SceHttpsCallback cbfunc, void *userArg);
int sceHttpsGetSslError(int id, int *errNum, unsigned int *detail);
int sceHttpGetLastErrno(int reqId, int *errNum);
int sceHttpSetAutoRedirect(int id, int isEnable);
int sceHttpSetConnectTimeOut(int id, unsigned int usec);
int sceHttpSetSendTimeOut(int id, unsigned int usec);
int sceHttpSetRecvTimeOut(int id, unsigned int usec);
int sceHttpCreateConnectionWithURL(int tmplId, const char *url,
                                   int enableKeepalive);
int sceHttpDeleteConnection(int connId);
int sceHttpCreateRequestWithURL(int connId, int method, const char *url,
                                uint64_t contentLength);
int sceHttpDeleteRequest(int reqId);
int sceHttpAddRequestHeader(int id, const char *name, const char *value,
                            int mode);
int sceHttpSendRequest(int reqId, const void *postData, size_t size);
int sceHttpGetStatusCode(int reqId, int *statusCode);
int sceHttpGetAllResponseHeaders(int reqId, char **header, size_t *headerSize);
int sceHttpReadData(int reqId, void *data, unsigned int size);

/* libSceHttp2 fallback */
int sceHttp2Init(int libnetMemId, int libsslCtxId, size_t poolSize,
                 int maxConcurrentRequest);
int sceHttp2Term(int libhttp2CtxId);
int sceHttp2CreateTemplate(int libhttp2CtxId, const char *userAgent,
                           int httpVer, int isAutoProxyConf);
int sceHttp2DeleteTemplate(int tmplId);
int sceHttp2SslDisableOption(int id, unsigned int sslFlags);
int sceHttp2SetSslCallback(int id, SceHttpsCallback cbfunc, void *userArg);
int sceHttp2SetConnectTimeOut(int id, unsigned int usec);
int sceHttp2SetSendTimeOut(int id, unsigned int usec);
int sceHttp2SetRecvTimeOut(int id, unsigned int usec);
int sceHttp2CreateRequestWithURL(int tmplId, const char *method,
                                 const char *url, uint64_t contentLength);
int sceHttp2DeleteRequest(int reqId);
int sceHttp2AddRequestHeader(int id, const char *name, const char *value,
                             int mode);
int sceHttp2SendRequest(int reqId, const void *postData, size_t size);
int sceHttp2GetStatusCode(int reqId, int *statusCode);
int sceHttp2GetAllResponseHeaders(int reqId, char **header, size_t *headerSize);
int sceHttp2ReadData(int reqId, void *data, size_t size);

#define SCE_HTTP_METHOD_GET       0
#define SCE_HTTP_METHOD_POST      1
#define SCE_HTTP_VERSION_1_1      2
#define SCE_HTTP2_VERSION_2_0     3
#define SCE_HTTP_HEADER_OVERWRITE 0
#define SCE_HTTPS_FLAG_ALL        0x3FU

#define DEFAULT_USER_AGENT \
    "Mozilla/5.0 (PlayStation; PlayStation 5/8.00) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.0 Safari/605.1.15"

static pthread_mutex_t g_hltb_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_net_inited = 0;
static char g_last_http_error[192] = {0};

/* Live sync status exposed to web frontend */
static volatile int g_sync_active = 0;
static volatile int g_sync_step = 0; /* 0: idle, 1: syncing selected, 2: matching library */
static volatile int g_last_synced_count = 0;
static char g_sync_msg[128] = "Idle";

/* ---- dynamic string buffer ---------------------------------------------- */

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} buffer_t;

static int buffer_reserve(buffer_t *b, size_t extra) {
    size_t needed = b->length + extra + 1;
    char *grown;
    size_t capacity;

    if(needed <= b->capacity) {
        return 0;
    }
    capacity = b->capacity ? b->capacity : 4096;
    while(capacity < needed) {
        capacity *= 2;
    }
    grown = (char *)realloc(b->data, capacity);
    if(!grown) {
        return -1;
    }
    b->data = grown;
    b->capacity = capacity;
    return 0;
}

static int buffer_append(buffer_t *b, const char *text, size_t length) {
    if(buffer_reserve(b, length) != 0) {
        return -1;
    }
    memcpy(b->data + b->length, text, length);
    b->length += length;
    b->data[b->length] = '\0';
    return 0;
}

static int buffer_append_str(buffer_t *b, const char *text) {
    return buffer_append(b, text, strlen(text));
}

static int buffer_append_json_string(buffer_t *b, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");

    if(buffer_append(b, "\"", 1) != 0) {
        return -1;
    }
    for(; *p; p++) {
        char escape[8];
        int n = 0;

        switch(*p) {
            case '"':  n = snprintf(escape, sizeof escape, "\\\""); break;
            case '\\': n = snprintf(escape, sizeof escape, "\\\\"); break;
            case '\n': n = snprintf(escape, sizeof escape, "\\n"); break;
            case '\r': n = snprintf(escape, sizeof escape, "\\r"); break;
            case '\t': n = snprintf(escape, sizeof escape, "\\t"); break;
            default:
                if(*p < 0x20u) {
                    n = snprintf(escape, sizeof escape, "\\u%04x", *p);
                } else {
                    escape[0] = (char)*p;
                    n = 1;
                }
                break;
        }
        if(buffer_append(b, escape, (size_t)n) != 0) {
            return -1;
        }
    }
    return buffer_append(b, "\"", 1);
}

/* ---- Native HTTPS Client ------------------------------------------------ */

typedef struct {
    int mem_id;
    int ssl_id;
    int http_id;
    int tmpl_id;
    int http2_id;
    int tmpl2_id;
} http_ctx_t;

static int skip_ssl_callback(int libsslCtxId, unsigned int verifyErr,
                             void *const sslCert[], int certNum,
                             void *userArg) {
    (void)libsslCtxId;
    (void)sslCert;
    (void)certNum;
    (void)userArg;
    if(verifyErr != 0) {
        klog_printf("[PS5-HLTB] SSL verifyErr=0x%08X (bypassing)\n",
                    verifyErr);
    }
    return 0; /* SCE_OK */
}

static int http_ctx_init(http_ctx_t *ctx, const char *user_agent) {
    const char *ua = (user_agent && user_agent[0]) ? user_agent : DEFAULT_USER_AGENT;

    ctx->mem_id = -1;
    ctx->ssl_id = -1;
    ctx->http_id = -1;
    ctx->tmpl_id = -1;
    ctx->http2_id = -1;
    ctx->tmpl2_id = -1;
    g_last_http_error[0] = '\0';

    if(!g_net_inited) {
        uint32_t r_net = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_NET);
        uint32_t r_ssl = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_SSL);
        uint32_t r_http = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_HTTP);
        int r_init = sceNetInit();
        klog_printf("[PS5-HLTB] Sysmodules: NET=0x%08X SSL=0x%08X HTTP=0x%08X sceNetInit=0x%08X\n",
                    r_net, r_ssl, r_http, (unsigned int)r_init);
        g_net_inited = 1;
    }

    ctx->mem_id = sceNetPoolCreate("ps5_hltb", 64 * 1024, 0);
    if(ctx->mem_id < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceNetPoolCreate=0x%08X", (unsigned int)ctx->mem_id);
        klog_printf("[PS5-HLTB] %s\n", g_last_http_error);
        return -1;
    }
    ctx->ssl_id = sceSslInit(304 * 1024);
    if(ctx->ssl_id < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceSslInit=0x%08X", (unsigned int)ctx->ssl_id);
        klog_printf("[PS5-HLTB] %s\n", g_last_http_error);
        sceNetPoolDestroy(ctx->mem_id);
        ctx->mem_id = -1;
        return -1;
    }

    ctx->http_id = sceHttpInit(ctx->mem_id, ctx->ssl_id, 256 * 1024);
    if(ctx->http_id >= 0) {
        ctx->tmpl_id = sceHttpCreateTemplate(ctx->http_id, ua, SCE_HTTP_VERSION_1_1, 1);
        if(ctx->tmpl_id >= 0) {
            (void)sceHttpsDisableOption(ctx->tmpl_id, SCE_HTTPS_FLAG_ALL);
            (void)sceHttpsSetSslCallback(ctx->tmpl_id, skip_ssl_callback, NULL);
            (void)sceHttpSetAutoRedirect(ctx->tmpl_id, 1);
            (void)sceHttpSetConnectTimeOut(ctx->tmpl_id, 15 * 1000 * 1000U);
            (void)sceHttpSetSendTimeOut(ctx->tmpl_id, 15 * 1000 * 1000U);
            (void)sceHttpSetRecvTimeOut(ctx->tmpl_id, 20 * 1000 * 1000U);
        }
    }

    ctx->http2_id = sceHttp2Init(ctx->mem_id, ctx->ssl_id, 256 * 1024, 4);
    if(ctx->http2_id >= 0) {
        ctx->tmpl2_id = sceHttp2CreateTemplate(ctx->http2_id, ua, SCE_HTTP2_VERSION_2_0, 1);
        if(ctx->tmpl2_id >= 0) {
            (void)sceHttp2SslDisableOption(ctx->tmpl2_id, SCE_HTTPS_FLAG_ALL);
            (void)sceHttp2SetSslCallback(ctx->tmpl2_id, skip_ssl_callback, NULL);
            (void)sceHttp2SetConnectTimeOut(ctx->tmpl2_id, 15 * 1000 * 1000U);
            (void)sceHttp2SetSendTimeOut(ctx->tmpl2_id, 15 * 1000 * 1000U);
            (void)sceHttp2SetRecvTimeOut(ctx->tmpl2_id, 20 * 1000 * 1000U);
        }
    }

    if(ctx->tmpl_id < 0 && ctx->tmpl2_id < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "http=0x%08X/0x%08X http2=0x%08X/0x%08X",
                 (unsigned int)ctx->http_id, (unsigned int)ctx->tmpl_id,
                 (unsigned int)ctx->http2_id, (unsigned int)ctx->tmpl2_id);
        klog_printf("[PS5-HLTB] Init failed: %s\n", g_last_http_error);
        if(ctx->http2_id >= 0) sceHttp2Term(ctx->http2_id);
        if(ctx->http_id >= 0) sceHttpTerm(ctx->http_id);
        sceSslTerm(ctx->ssl_id);
        sceNetPoolDestroy(ctx->mem_id);
        ctx->http2_id = -1;
        ctx->http_id = -1;
        ctx->ssl_id = -1;
        ctx->mem_id = -1;
        return -1;
    }

    return 0;
}

static void http_ctx_fini(http_ctx_t *ctx) {
    if(ctx->tmpl2_id >= 0) {
        sceHttp2DeleteTemplate(ctx->tmpl2_id);
        ctx->tmpl2_id = -1;
    }
    if(ctx->http2_id >= 0) {
        sceHttp2Term(ctx->http2_id);
        ctx->http2_id = -1;
    }
    if(ctx->tmpl_id >= 0) {
        sceHttpDeleteTemplate(ctx->tmpl_id);
        ctx->tmpl_id = -1;
    }
    if(ctx->http_id >= 0) {
        sceHttpTerm(ctx->http_id);
        ctx->http_id = -1;
    }
    if(ctx->ssl_id >= 0) {
        sceSslTerm(ctx->ssl_id);
        ctx->ssl_id = -1;
    }
    if(ctx->mem_id >= 0) {
        sceNetPoolDestroy(ctx->mem_id);
        ctx->mem_id = -1;
    }
}

/* Extracts all Set-Cookie name=value pairs from a raw HTTP response header block. */
static void extract_cookies_from_headers(const char *raw, size_t len,
                                         char *cookie_out, size_t cookie_out_size) {
    const char *p = raw;
    const char *end = raw + len;
    size_t out_pos = 0;

    if(!cookie_out || cookie_out_size == 0) {
        return;
    }
    cookie_out[0] = '\0';
    if(!raw || len == 0) {
        return;
    }

    while(p < end) {
        const char *line_end = p;
        size_t line_len;

        while(line_end < end && *line_end != '\n' && *line_end != '\r' && *line_end != '\0') {
            line_end++;
        }
        line_len = (size_t)(line_end - p);

        if(line_len > 11 && strncasecmp(p, "Set-Cookie:", 11) == 0) {
            const char *val = p + 11;
            const char *val_end = p + line_len;
            const char *semi;
            size_t pair_len;

            while(val < val_end && (*val == ' ' || *val == '\t')) {
                val++;
            }
            semi = (const char *)memchr(val, ';', (size_t)(val_end - val));
            pair_len = semi ? (size_t)(semi - val) : (size_t)(val_end - val);

            while(pair_len > 0 && (val[pair_len - 1] == ' ' || val[pair_len - 1] == '\t')) {
                pair_len--;
            }
            if(pair_len > 0 && memchr(val, '=', pair_len) != NULL &&
               !strstr(val, "=deleted") && !strstr(val, "=;")) {
                if(out_pos > 0 && out_pos + 2 < cookie_out_size) {
                    cookie_out[out_pos++] = ';';
                    cookie_out[out_pos++] = ' ';
                }
                if(out_pos + pair_len < cookie_out_size) {
                    memcpy(cookie_out + out_pos, val, pair_len);
                    out_pos += pair_len;
                    cookie_out[out_pos] = '\0';
                }
            }
        }
        while(line_end < end && (*line_end == '\n' || *line_end == '\r' || *line_end == '\0')) {
            line_end++;
        }
        p = line_end;
    }
}

static int http_ctx_request_v1(http_ctx_t *ctx, int is_post, const char *url,
                               const char *body, const char *cookie,
                               const char *auth_token, const char *hp_key,
                               const char *hp_val, int *status_out,
                               buffer_t *resp_out, char *set_cookie_out,
                               size_t set_cookie_size) {
    int conn_id = -1;
    int req_id = -1;
    size_t body_len = body ? strlen(body) : 0;
    int status = 0;
    char chunk[4096];
    int n;
    int ret;

    if(ctx->tmpl_id < 0) {
        return -1;
    }

    conn_id = sceHttpCreateConnectionWithURL(ctx->tmpl_id, url, 1);
    if(conn_id < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceHttpCreateConnection=0x%08X", (unsigned int)conn_id);
        klog_printf("[PS5-HLTB] %s (%s)\n", g_last_http_error, url);
        return -1;
    }

    req_id = sceHttpCreateRequestWithURL(
        conn_id, is_post ? SCE_HTTP_METHOD_POST : SCE_HTTP_METHOD_GET, url,
        is_post ? (uint64_t)body_len : 0);
    if(req_id < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceHttpCreateRequest=0x%08X", (unsigned int)req_id);
        klog_printf("[PS5-HLTB] %s (%s)\n", g_last_http_error, url);
        sceHttpDeleteConnection(conn_id);
        return -1;
    }

    (void)sceHttpAddRequestHeader(req_id, "Origin", "https://howlongtobeat.com",
                                  SCE_HTTP_HEADER_OVERWRITE);
    (void)sceHttpAddRequestHeader(req_id, "Referer", "https://howlongtobeat.com/",
                                  SCE_HTTP_HEADER_OVERWRITE);
    (void)sceHttpAddRequestHeader(req_id, "Accept", "application/json, text/plain, */*",
                                  SCE_HTTP_HEADER_OVERWRITE);
    if(is_post) {
        (void)sceHttpAddRequestHeader(req_id, "Content-Type", "application/json",
                                      SCE_HTTP_HEADER_OVERWRITE);
    }
    if(cookie && cookie[0]) {
        (void)sceHttpAddRequestHeader(req_id, "Cookie", cookie,
                                      SCE_HTTP_HEADER_OVERWRITE);
    }
    if(auth_token && auth_token[0]) {
        (void)sceHttpAddRequestHeader(req_id, "x-auth-token", auth_token,
                                      SCE_HTTP_HEADER_OVERWRITE);
    }
    if(hp_key && hp_key[0] && hp_val && hp_val[0]) {
        (void)sceHttpAddRequestHeader(req_id, "x-hp-key", hp_key,
                                      SCE_HTTP_HEADER_OVERWRITE);
        (void)sceHttpAddRequestHeader(req_id, "x-hp-val", hp_val,
                                      SCE_HTTP_HEADER_OVERWRITE);
    }

    ret = sceHttpSendRequest(req_id, is_post ? body : NULL, is_post ? body_len : 0);
    if(ret < 0) {
        int ssl_err = 0;
        unsigned int ssl_detail = 0;
        int last_errno = 0;
        (void)sceHttpsGetSslError(req_id, &ssl_err, &ssl_detail);
        (void)sceHttpGetLastErrno(req_id, &last_errno);
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceHttpSendRequest=0x%08X (ssl=0x%08X/0x%X errno=%d)",
                 (unsigned int)ret, (unsigned int)ssl_err, ssl_detail, last_errno);
        klog_printf("[PS5-HLTB] %s (%s)\n", g_last_http_error, url);
        sceHttpDeleteRequest(req_id);
        sceHttpDeleteConnection(conn_id);
        return -1;
    }

    ret = sceHttpGetStatusCode(req_id, &status);
    if(ret < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceHttpGetStatusCode=0x%08X", (unsigned int)ret);
        klog_printf("[PS5-HLTB] %s (%s)\n", g_last_http_error, url);
        sceHttpDeleteRequest(req_id);
        sceHttpDeleteConnection(conn_id);
        return -1;
    }
    if(status_out) {
        *status_out = status;
    }

    if(set_cookie_out && set_cookie_size > 0) {
        char *raw_headers = NULL;
        size_t raw_len = 0;
        if(sceHttpGetAllResponseHeaders(req_id, &raw_headers, &raw_len) >= 0 &&
           raw_headers && raw_len > 0) {
            extract_cookies_from_headers(raw_headers, raw_len, set_cookie_out,
                                         set_cookie_size);
        }
    }

    if(resp_out) {
        while((n = sceHttpReadData(req_id, chunk, sizeof chunk)) > 0) {
            if(buffer_append(resp_out, chunk, (size_t)n) != 0) {
                sceHttpDeleteRequest(req_id);
                sceHttpDeleteConnection(conn_id);
                return -1;
            }
        }
    }

    klog_printf("[PS5-HLTB] HTTP/1.1 %s %s -> %d (%zu bytes)\n",
                is_post ? "POST" : "GET", url, status,
                resp_out ? resp_out->length : 0UL);
    sceHttpDeleteRequest(req_id);
    sceHttpDeleteConnection(conn_id);
    return 0;
}

static int http_ctx_request_v2(http_ctx_t *ctx, int is_post, const char *url,
                               const char *body, const char *cookie,
                               const char *auth_token, const char *hp_key,
                               const char *hp_val, int *status_out,
                               buffer_t *resp_out, char *set_cookie_out,
                               size_t set_cookie_size) {
    int req_id = -1;
    size_t body_len = body ? strlen(body) : 0;
    int status = 0;
    char chunk[4096];
    int n;
    int ret;

    if(ctx->tmpl2_id < 0) {
        return -1;
    }

    req_id = sceHttp2CreateRequestWithURL(
        ctx->tmpl2_id, is_post ? "POST" : "GET", url,
        is_post ? (uint64_t)body_len : 0);
    if(req_id < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceHttp2CreateRequest=0x%08X", (unsigned int)req_id);
        klog_printf("[PS5-HLTB] %s (%s)\n", g_last_http_error, url);
        return -1;
    }

    (void)sceHttp2AddRequestHeader(req_id, "Origin", "https://howlongtobeat.com",
                                   SCE_HTTP_HEADER_OVERWRITE);
    (void)sceHttp2AddRequestHeader(req_id, "Referer", "https://howlongtobeat.com/",
                                   SCE_HTTP_HEADER_OVERWRITE);
    (void)sceHttp2AddRequestHeader(req_id, "Accept", "application/json, text/plain, */*",
                                   SCE_HTTP_HEADER_OVERWRITE);
    if(is_post) {
        (void)sceHttp2AddRequestHeader(req_id, "Content-Type", "application/json",
                                       SCE_HTTP_HEADER_OVERWRITE);
    }
    if(cookie && cookie[0]) {
        (void)sceHttp2AddRequestHeader(req_id, "Cookie", cookie,
                                       SCE_HTTP_HEADER_OVERWRITE);
    }
    if(auth_token && auth_token[0]) {
        (void)sceHttp2AddRequestHeader(req_id, "x-auth-token", auth_token,
                                       SCE_HTTP_HEADER_OVERWRITE);
    }
    if(hp_key && hp_key[0] && hp_val && hp_val[0]) {
        (void)sceHttp2AddRequestHeader(req_id, "x-hp-key", hp_key,
                                       SCE_HTTP_HEADER_OVERWRITE);
        (void)sceHttp2AddRequestHeader(req_id, "x-hp-val", hp_val,
                                       SCE_HTTP_HEADER_OVERWRITE);
    }

    ret = sceHttp2SendRequest(req_id, is_post ? body : NULL, is_post ? body_len : 0);
    if(ret < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceHttp2SendRequest=0x%08X", (unsigned int)ret);
        klog_printf("[PS5-HLTB] %s (%s)\n", g_last_http_error, url);
        sceHttp2DeleteRequest(req_id);
        return -1;
    }

    ret = sceHttp2GetStatusCode(req_id, &status);
    if(ret < 0) {
        snprintf(g_last_http_error, sizeof g_last_http_error,
                 "sceHttp2GetStatusCode=0x%08X", (unsigned int)ret);
        klog_printf("[PS5-HLTB] %s (%s)\n", g_last_http_error, url);
        sceHttp2DeleteRequest(req_id);
        return -1;
    }
    if(status_out) {
        *status_out = status;
    }

    if(set_cookie_out && set_cookie_size > 0) {
        char *raw_headers = NULL;
        size_t raw_len = 0;
        if(sceHttp2GetAllResponseHeaders(req_id, &raw_headers, &raw_len) >= 0 &&
           raw_headers && raw_len > 0) {
            extract_cookies_from_headers(raw_headers, raw_len, set_cookie_out,
                                         set_cookie_size);
        }
    }

    if(resp_out) {
        while((n = sceHttp2ReadData(req_id, chunk, sizeof chunk)) > 0) {
            if(buffer_append(resp_out, chunk, (size_t)n) != 0) {
                sceHttp2DeleteRequest(req_id);
                return -1;
            }
        }
    }

    klog_printf("[PS5-HLTB] HTTP/2 %s %s -> %d (%zu bytes)\n",
                is_post ? "POST" : "GET", url, status,
                resp_out ? resp_out->length : 0UL);
    sceHttp2DeleteRequest(req_id);
    return 0;
}

static int http_ctx_request_ex(http_ctx_t *ctx, int is_post, const char *url,
                               const char *body, const char *cookie,
                               const char *auth_token, const char *hp_key,
                               const char *hp_val, int *status_out,
                               buffer_t *resp_out, char *set_cookie_out,
                               size_t set_cookie_size) {
    if(status_out) {
        *status_out = 0;
    }
    if(set_cookie_out && set_cookie_size > 0) {
        set_cookie_out[0] = '\0';
    }
    if(resp_out && resp_out->data) {
        resp_out->length = 0;
        resp_out->data[0] = '\0';
    }

    if(http_ctx_request_v1(ctx, is_post, url, body, cookie, auth_token, hp_key,
                           hp_val, status_out, resp_out, set_cookie_out,
                           set_cookie_size) == 0) {
        return 0;
    }

    if(resp_out && resp_out->data) {
        resp_out->length = 0;
        resp_out->data[0] = '\0';
    }
    if(set_cookie_out && set_cookie_size > 0) {
        set_cookie_out[0] = '\0';
    }

    return http_ctx_request_v2(ctx, is_post, url, body, cookie, auth_token,
                               hp_key, hp_val, status_out, resp_out,
                               set_cookie_out, set_cookie_size);
}

static int http_ctx_request(http_ctx_t *ctx, int is_post, const char *url,
                            const char *body, const char *cookie,
                            const char *auth_token, int *status_out,
                            buffer_t *resp_out, char *set_cookie_out,
                            size_t set_cookie_size) {
    return http_ctx_request_ex(ctx, is_post, url, body, cookie, auth_token,
                               NULL, NULL, status_out, resp_out, set_cookie_out,
                               set_cookie_size);
}

/* ---- Lightweight JSON Helpers ------------------------------------------- */

static const char *find_json_key(const char *json, const char *key) {
    char pattern[128];
    size_t pat_len;
    const char *p = json;

    if(!json || !key) {
        return NULL;
    }
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    pat_len = strlen(pattern);

    while((p = strstr(p, pattern)) != NULL) {
        const char *after = p + pat_len;
        while(*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') {
            after++;
        }
        if(*after == ':') {
            after++;
            while(*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') {
                after++;
            }
            return after;
        }
        p += pat_len;
    }
    return NULL;
}

static int json_extract_string(const char *json, const char *key, char *out,
                               size_t out_size) {
    const char *val = find_json_key(json, key);
    size_t pos = 0;

    if(!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if(!val || *val != '"') {
        return -1;
    }
    val++;
    while(*val && *val != '"') {
        char c = *val++;
        if(c == '\\' && *val) {
            char esc = *val++;
            switch(esc) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                default:   c = esc; break;
            }
        }
        if(pos + 1 < out_size) {
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
    return 0;
}

static int json_extract_int64(const char *json, const char *key,
                              long long *out_val) {
    const char *val = find_json_key(json, key);
    char *end = NULL;
    long long parsed;

    if(!val) {
        return -1;
    }
    if(*val == '"') {
        val++;
    }
    parsed = strtoll(val, &end, 10);
    if(end == val) {
        return -1;
    }
    *out_val = parsed;
    return 0;
}

static int json_extract_error(const char *json, char *out, size_t out_size) {
    const char *val = find_json_key(json, "error");
    size_t pos = 0;

    if(!val) {
        return -1;
    }
    if(*val == '[') {
        val++;
        while(*val == ' ' || *val == '\t' || *val == '\r' || *val == '\n') {
            val++;
        }
    }
    if(*val != '"') {
        return -1;
    }
    val++;
    while(*val && *val != '"') {
        char c = *val++;
        if(c == '\\' && *val) {
            c = *val++;
        }
        if(pos + 1 < out_size) {
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
    return pos > 0 ? 0 : -1;
}

/* ---- HLTB API Operations ------------------------------------------------ */

static int hltb_api_verify_user(http_ctx_t *hctx, const char *cookie,
                                long long *user_id_out, char *username_out,
                                size_t username_size) {
    buffer_t resp = {0};
    int status = 0;
    long long uid = 0;

    if(http_ctx_request(hctx, 0, "https://howlongtobeat.com/api/user", NULL,
                        cookie, NULL, &status, &resp, NULL, 0) != 0 ||
       status != 200 || !resp.data) {
        free(resp.data);
        return -1;
    }

    if(json_extract_int64(resp.data, "user_id", &uid) != 0 || uid <= 0) {
        free(resp.data);
        return -1;
    }
    if(json_extract_string(resp.data, "user_name", username_out, username_size) != 0 ||
       username_out[0] == '\0') {
        snprintf(username_out, username_size, "User #%lld", uid);
    }

    *user_id_out = uid;
    free(resp.data);
    return 0;
}

static int has_ci_substr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if(!nlen) {
        return 1;
    }
    for(; *haystack; haystack++) {
        if(strncasecmp(haystack, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Cleans a PS5 title name into search terms JSON array, e.g. ["God","of","War"].
 * mode 0: full title (with parenthesized platform tags and special symbols stripped)
 * mode 1: base title (truncate at first ':', '-', '/')
 * mode 2: title with common edition suffix words stripped
 */
static void build_search_terms_json(const char *title, int mode, buffer_t *terms_out) {
    char cleaned[256];
    size_t i = 0;
    size_t out = 0;
    int first = 1;
    const char *p;
    const char *sep = NULL;

    if(mode == 1) {
        sep = strpbrk(title, ":-/");
    }

    while(title[i] && out + 1 < sizeof cleaned) {
        if(sep && &title[i] >= sep) {
            break;
        }

        /* Check for parenthesized / bracketed platform or edition tags:
         * (PlayStation 5), (PlayStation®5), (PS5), (PS4), [PS5], etc. */
        if(title[i] == '(' || title[i] == '[') {
            const char *close_p = strchr(&title[i], title[i] == '(' ? ')' : ']');
            if(close_p) {
                char tag[64] = {0};
                size_t tlen = (size_t)(close_p - &title[i] - 1);
                if(tlen > 0 && tlen < sizeof tag) {
                    memcpy(tag, &title[i] + 1, tlen);
                    if(has_ci_substr(tag, "playstation") || has_ci_substr(tag, "ps4") ||
                       has_ci_substr(tag, "ps5") || has_ci_substr(tag, "edition") ||
                       has_ci_substr(tag, "remaster")) {
                        i = (size_t)(close_p - title) + 1;
                        cleaned[out++] = ' ';
                        continue;
                    }
                }
            }
        }

        unsigned char c = (unsigned char)title[i];
        /* Strip UTF-8 TM (0xE2 0x84 0xA2), (R) (0xC2 0xAE), (C) (0xC2 0xA9) */
        if(c == 0xE2 && (unsigned char)title[i + 1] == 0x84 &&
           (unsigned char)title[i + 2] == 0xA2) {
            i += 3;
            cleaned[out++] = ' ';
            continue;
        }
        if(c == 0xC2 && ((unsigned char)title[i + 1] == 0xAE ||
                         (unsigned char)title[i + 1] == 0xA9)) {
            i += 2;
            cleaned[out++] = ' ';
            continue;
        }
        if(c < 0x80) {
            if(isalnum(c) || c == '\'') {
                cleaned[out++] = (char)c;
            } else {
                cleaned[out++] = ' ';
            }
            i++;
        } else {
            cleaned[out++] = (char)c;
            i++;
        }
    }
    cleaned[out] = '\0';

    (void)buffer_append_str(terms_out, "[");
    p = cleaned;
    while(*p) {
        char word[64];
        size_t wlen = 0;

        while(*p == ' ') {
            p++;
        }
        if(!*p) {
            break;
        }
        while(*p && *p != ' ' && wlen + 1 < sizeof word) {
            word[wlen++] = *p++;
        }
        word[wlen] = '\0';

        /* Ignore console names and platform noise */
        if(strcasecmp(word, "PS4") == 0 || strcasecmp(word, "PS5") == 0 ||
           strcasecmp(word, "PlayStation") == 0 ||
           strcasecmp(word, "PlayStation4") == 0 ||
           strcasecmp(word, "PlayStation5") == 0 ||
           strcmp(word, "-") == 0) {
            continue;
        }
        if(mode == 2) {
            if(strcasecmp(word, "Edition") == 0 || strcasecmp(word, "Remastered") == 0 ||
               strcasecmp(word, "Collection") == 0 || strcasecmp(word, "Bundle") == 0 ||
               strcasecmp(word, "Complete") == 0 || strcasecmp(word, "Deluxe") == 0) {
                continue;
            }
        }
        if(!first) {
            (void)buffer_append_str(terms_out, ",");
        }
        first = 0;
        (void)buffer_append_json_string(terms_out, word);
    }
    (void)buffer_append_str(terms_out, "]");
}

static int hltb_api_fetch_game_by_id(http_ctx_t *hctx, long long game_id,
                                     char *matched_title_out,
                                     size_t matched_title_size,
                                     long long *comp_main_out,
                                     long long *comp_plus_out,
                                     long long *comp_100_out,
                                     long long *comp_all_out) {
    char url[128];
    buffer_t resp = {0};
    int status = 0;
    const char *p;

    if(game_id <= 0) {
        return -1;
    }
    snprintf(url, sizeof url, "https://howlongtobeat.com/game/%lld", game_id);

    if(http_ctx_request(hctx, 0, url, NULL, NULL, NULL, &status, &resp, NULL, 0) != 0 ||
       status != 200 || !resp.data) {
        free(resp.data);
        return -1;
    }

    p = resp.data;
    if(matched_title_out && matched_title_size > 0) {
        if(json_extract_string(p, "game_name", matched_title_out, matched_title_size) != 0) {
            matched_title_out[0] = '\0';
        }
    }
    if(comp_main_out) {
        *comp_main_out = 0;
        (void)json_extract_int64(p, "comp_main", comp_main_out);
    }
    if(comp_plus_out) {
        *comp_plus_out = 0;
        (void)json_extract_int64(p, "comp_plus", comp_plus_out);
    }
    if(comp_100_out) {
        *comp_100_out = 0;
        (void)json_extract_int64(p, "comp_100", comp_100_out);
    }
    if(comp_all_out) {
        *comp_all_out = 0;
        (void)json_extract_int64(p, "comp_all", comp_all_out);
    }
    klog_printf("[PS5-HLTB] Fetched Game #%lld: title='%s' main=%llds plus=%llds 100=%llds all=%llds\n",
                game_id, matched_title_out ? matched_title_out : "",
                comp_main_out ? *comp_main_out : 0,
                comp_plus_out ? *comp_plus_out : 0,
                comp_100_out ? *comp_100_out : 0,
                comp_all_out ? *comp_all_out : 0);
    free(resp.data);
    return 0;
}

static int execute_search(http_ctx_t *hctx, const char *token,
                          const char *hp_key, const char *hp_val,
                          const char *terms_json, long long *gid_out,
                          char *matched_title_out, size_t matched_title_size,
                          long long *comp_main_out, long long *comp_plus_out,
                          long long *comp_100_out, long long *comp_all_out) {
    buffer_t body = {0};
    buffer_t search_resp = {0};
    int status = 0;
    const char *data_arr;
    long long gid = 0;

    (void)buffer_append_str(&body, "{");
    if(hp_key[0] && hp_val[0]) {
        (void)buffer_append_json_string(&body, hp_key);
        (void)buffer_append_str(&body, ":");
        (void)buffer_append_json_string(&body, hp_val);
        (void)buffer_append_str(&body, ",");
    }
    (void)buffer_append_str(&body, "\"searchType\":\"games\",\"searchTerms\":");
    (void)buffer_append_str(&body, terms_json);
    (void)buffer_append_str(
        &body,
        ",\"searchPage\":1,\"size\":20,\"searchOptions\":{\"games\":{\"userId\":0,"
        "\"platform\":\"\",\"sortCategory\":\"popular\",\"rangeCategory\":\"main\","
        "\"rangeTime\":{\"min\":null,\"max\":null},\"gameplay\":{\"perspective\":\"\","
        "\"flow\":\"\",\"genre\":\"\",\"difficulty\":\"\"},\"rangeYear\":{\"min\":\"\","
        "\"max\":\"\"},\"modifier\":\"\"},\"users\":{\"sortCategory\":\"postcount\"},"
        "\"filter\":\"\",\"sort\":0,\"randomizer\":0},\"useCache\":true}");

    if(!body.data ||
       http_ctx_request_ex(hctx, 1, "https://howlongtobeat.com/api/search/site",
                           body.data, NULL, token, hp_key, hp_val, &status,
                           &search_resp, NULL, 0) != 0 ||
       status != 200 || !search_resp.data) {
        klog_printf("[PS5-HLTB] Search %s -> HTTP %d (resp: %s)\n",
                    terms_json, status, search_resp.data ? search_resp.data : "null");
        free(body.data);
        free(search_resp.data);
        return -1;
    }
    free(body.data);

    data_arr = find_json_key(search_resp.data, "data");
    if(!data_arr || json_extract_int64(data_arr, "game_id", &gid) != 0 || gid <= 0) {
        klog_printf("[PS5-HLTB] Search %s -> HTTP 200 but 0 matches (resp len %zu)\n",
                    terms_json, search_resp.length);
        free(search_resp.data);
        return -1;
    }

    *gid_out = gid;
    if(matched_title_out && matched_title_size > 0) {
        if(json_extract_string(data_arr, "game_name", matched_title_out,
                               matched_title_size) != 0) {
            matched_title_out[0] = '\0';
        }
    }
    klog_printf("[PS5-HLTB] Search %s -> Matched HLTB ID %lld ('%s')\n",
                terms_json, gid, matched_title_out ? matched_title_out : "");
    if(comp_main_out) {
        *comp_main_out = 0;
        (void)json_extract_int64(data_arr, "comp_main", comp_main_out);
    }
    if(comp_plus_out) {
        *comp_plus_out = 0;
        (void)json_extract_int64(data_arr, "comp_plus", comp_plus_out);
    }
    if(comp_100_out) {
        *comp_100_out = 0;
        (void)json_extract_int64(data_arr, "comp_100", comp_100_out);
    }
    if(comp_all_out) {
        *comp_all_out = 0;
        (void)json_extract_int64(data_arr, "comp_all", comp_all_out);
    }

    free(search_resp.data);
    return 0;
}

static int hltb_api_search_game(http_ctx_t *hctx, const char *game_name,
                                long long *hltb_id_out, char *matched_title_out,
                                size_t matched_title_size,
                                long long *comp_main_out,
                                long long *comp_plus_out,
                                long long *comp_100_out,
                                long long *comp_all_out) {
    struct timeval tv;
    long long ts_ms;
    char init_url[128];
    buffer_t init_resp = {0};
    char token[1024] = {0};
    char hp_key[64] = {0};
    char hp_val[128] = {0};
    buffer_t terms = {0};
    int status = 0;
    long long gid = 0;

    if(!game_name || !game_name[0]) {
        return -1;
    }

    gettimeofday(&tv, NULL);
    ts_ms = (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
    snprintf(init_url, sizeof init_url,
             "https://howlongtobeat.com/api/search/site/init?t=%lld", ts_ms);

    if(http_ctx_request(hctx, 0, init_url, NULL, NULL, NULL, &status,
                        &init_resp, NULL, 0) != 0 ||
       status != 200 || !init_resp.data ||
       json_extract_string(init_resp.data, "token", token, sizeof token) != 0 ||
       !token[0]) {
        klog_printf("[PS5-HLTB] Init failed: status=%d (resp: %s)\n",
                    status, init_resp.data ? init_resp.data : "null");
        free(init_resp.data);
        return -1;
    }
    (void)json_extract_string(init_resp.data, "hpKey", hp_key, sizeof hp_key);
    (void)json_extract_string(init_resp.data, "hpVal", hp_val, sizeof hp_val);
    free(init_resp.data);

    /* Attempt 1: Full title (platform tags and TM stripped) */
    build_search_terms_json(game_name, 0, &terms);
    if(terms.data && terms.length > 2) {
        if(execute_search(hctx, token, hp_key, hp_val, terms.data, &gid,
                          matched_title_out, matched_title_size,
                          comp_main_out, comp_plus_out, comp_100_out,
                          comp_all_out) == 0 && gid > 0) {
            *hltb_id_out = gid;
            if(matched_title_out && !matched_title_out[0]) {
                snprintf(matched_title_out, matched_title_size, "%s", game_name);
            }
            free(terms.data);
            return 0;
        }
    }
    free(terms.data);
    terms.data = NULL;
    terms.length = terms.capacity = 0;

    /* Attempt 2: Base title (truncate at ':', '-', '/') if present */
    if(strpbrk(game_name, ":-/") != NULL) {
        build_search_terms_json(game_name, 1, &terms);
        if(terms.data && terms.length > 2) {
            if(execute_search(hctx, token, hp_key, hp_val, terms.data, &gid,
                              matched_title_out, matched_title_size,
                              comp_main_out, comp_plus_out, comp_100_out,
                              comp_all_out) == 0 && gid > 0) {
                *hltb_id_out = gid;
                if(matched_title_out && !matched_title_out[0]) {
                    snprintf(matched_title_out, matched_title_size, "%s", game_name);
                }
                free(terms.data);
                return 0;
            }
        }
        free(terms.data);
        terms.data = NULL;
        terms.length = terms.capacity = 0;
    }

    /* Attempt 3: Strip common edition keywords (Edition, Remastered, Collection, etc.) */
    build_search_terms_json(game_name, 2, &terms);
    if(terms.data && terms.length > 2) {
        if(execute_search(hctx, token, hp_key, hp_val, terms.data, &gid,
                          matched_title_out, matched_title_size,
                          comp_main_out, comp_plus_out, comp_100_out,
                          comp_all_out) == 0 && gid > 0) {
            *hltb_id_out = gid;
            if(matched_title_out && !matched_title_out[0]) {
                snprintf(matched_title_out, matched_title_size, "%s", game_name);
            }
            free(terms.data);
            return 0;
        }
    }
    free(terms.data);

    return -1;
}

typedef struct {
    int playing;
    int backlog;
    int replay;
    int custom;
    int custom2;
    int custom3;
    int completed;
    int retired;
} hltb_lists_t;

static void hltb_api_find_user_submission(http_ctx_t *hctx, const char *cookie,
                                          long long hltb_user_id,
                                          long long hltb_game_id,
                                          long long *sub_id_out,
                                          long long *prev_sec_out,
                                          hltb_lists_t *lists_out,
                                          long long *comp_main_out,
                                          long long *comp_plus_out,
                                          long long *comp_100_out,
                                          long long *comp_all_out) {
    char url[128];
    char body[512];
    buffer_t resp = {0};
    int status = 0;
    char needle[64];
    const char *p;

    *sub_id_out = 0;
    *prev_sec_out = 0;
    memset(lists_out, 0, sizeof *lists_out);
    lists_out->playing = 1;

    snprintf(url, sizeof url,
             "https://howlongtobeat.com/api/user/%lld/games/list", hltb_user_id);
    snprintf(body, sizeof body,
             "{\"user_id\":%lld,\"toggleType\":\"Multi List\","
             "\"lists\":[\"playing\",\"backlog\",\"replays\",\"custom\",\"custom2\","
             "\"custom3\",\"completed\",\"retired\"],\"set_playstyle\":\"comp_all\","
             "\"name\":\"\",\"platform\":\"\",\"storefront\":\"\",\"sortBy\":\"\","
             "\"sortFlip\":0,\"view\":\"\",\"random\":0,\"limit\":500,"
             "\"currentUserHome\":true}",
             hltb_user_id);

    if(http_ctx_request(hctx, 1, url, body, cookie, NULL, &status, &resp, NULL,
                        0) != 0 ||
       status != 200 || !resp.data) {
        free(resp.data);
        return;
    }

    snprintf(needle, sizeof needle, "\"game_id\":%lld", hltb_game_id);
    p = strstr(resp.data, needle);
    if(p) {
        /* Walk backwards to the start of this JSON object '{' */
        const char *obj_start = p;
        while(obj_start > resp.data && *obj_start != '{') {
            obj_start--;
        }
        if(*obj_start == '{') {
            long long sid = 0;
            long long inv = 0;
            long long v = 0;

            if(json_extract_int64(obj_start, "id", &sid) == 0 && sid > 0) {
                *sub_id_out = sid;
            }
            if(json_extract_int64(obj_start, "invested_pro", &inv) == 0 && inv >= 0) {
                *prev_sec_out = inv;
            }
            if(json_extract_int64(obj_start, "list_playing", &v) == 0) lists_out->playing = (v != 0);
            if(json_extract_int64(obj_start, "list_backlog", &v) == 0) lists_out->backlog = (v != 0);
            if(json_extract_int64(obj_start, "list_replay", &v) == 0)  lists_out->replay = (v != 0);
            if(json_extract_int64(obj_start, "list_custom", &v) == 0)  lists_out->custom = (v != 0);
            if(json_extract_int64(obj_start, "list_custom2", &v) == 0) lists_out->custom2 = (v != 0);
            if(json_extract_int64(obj_start, "list_custom3", &v) == 0) lists_out->custom3 = (v != 0);
            if(json_extract_int64(obj_start, "list_comp", &v) == 0)    lists_out->completed = (v != 0);
            if(json_extract_int64(obj_start, "list_retired", &v) == 0) lists_out->retired = (v != 0);

            if(comp_main_out && *comp_main_out <= 0) {
                (void)json_extract_int64(obj_start, "comp_main", comp_main_out);
            }
            if(comp_plus_out && *comp_plus_out <= 0) {
                (void)json_extract_int64(obj_start, "comp_plus", comp_plus_out);
            }
            if(comp_100_out && *comp_100_out <= 0) {
                (void)json_extract_int64(obj_start, "comp_100", comp_100_out);
            }
            if(comp_all_out && *comp_all_out <= 0) {
                long long ca = 0;
                if(json_extract_int64(obj_start, "comp_all", &ca) == 0 && ca > 0) {
                    *comp_all_out = ca;
                } else if(json_extract_int64(obj_start, "comp_all_g", &ca) == 0 && ca > 0) {
                    *comp_all_out = ca;
                }
            }

            if(!lists_out->playing && !lists_out->backlog && !lists_out->replay &&
               !lists_out->custom && !lists_out->custom2 && !lists_out->custom3 &&
               !lists_out->completed && !lists_out->retired) {
                lists_out->playing = 1;
            }
        }
    }

    free(resp.data);
}

static int hltb_api_submit_progress(http_ctx_t *hctx, const char *cookie,
                                    long long hltb_user_id,
                                    long long hltb_game_id,
                                    long long submission_id, const char *title,
                                    int is_ps5, long long played_sec,
                                    long long prev_sec,
                                    const hltb_lists_t *lists,
                                    long long *new_sub_id_out,
                                    char *error_message,
                                    size_t error_message_size) {
    buffer_t payload = {0};
    buffer_t resp = {0};
    char chunk[2048];
    int status = 0;
    long long h = played_sec / 3600;
    long long m = (played_sec % 3600) / 60;
    long long s = played_sec % 60;
    long long ph = prev_sec / 3600;
    long long pm = (prev_sec % 3600) / 60;
    long long ps = prev_sec % 60;
    long long updated_id = 0;
    char comp_main_str[160];
    char comp_date_str[64];

    if(lists->completed) {
        time_t now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        snprintf(comp_date_str, sizeof comp_date_str,
                 "\"completionDate\":{\"year\":\"%04d\",\"month\":\"%02d\",\"day\":\"%02d\"}",
                 tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
        snprintf(comp_main_str, sizeof comp_main_str,
                 "\"compMain\":{\"time\":{\"hours\":%lld,\"minutes\":%lld,\"seconds\":%lld},\"notes\":\"\"}",
                 h, m, s);
    } else {
        snprintf(comp_date_str, sizeof comp_date_str,
                 "\"completionDate\":{\"year\":\"0000\",\"month\":\"00\",\"day\":\"00\"}");
        snprintf(comp_main_str, sizeof comp_main_str,
                 "\"compMain\":{\"time\":{\"hours\":null,\"minutes\":null,\"seconds\":null},\"notes\":\"\"}");
    }

    snprintf(chunk, sizeof chunk,
             "{\"submissionId\":%lld,\"userId\":%lld,\"gameId\":%lld,\"title\":",
             submission_id > 1 ? submission_id : 0LL, hltb_user_id, hltb_game_id);
    (void)buffer_append_str(&payload, chunk);
    (void)buffer_append_json_string(&payload, title ? title : "");

    snprintf(
        chunk, sizeof chunk,
        ",\"platform\":\"%s\",\"storefront\":\"\","
        "\"lists\":{\"playing\":%s,\"backlog\":%s,\"replay\":%s,\"custom\":%s,"
        "\"custom2\":%s,\"custom3\":%s,\"completed\":%s,\"retired\":%s},"
        "\"general\":{\"progress\":{\"hours\":%lld,\"minutes\":%lld,\"seconds\":%lld},"
        "\"progressBefore\":{\"hours\":%lld,\"minutes\":%lld,\"seconds\":%lld},"
        "\"retirementNotes\":\"\","
        "%s,"
        "\"startDate\":{\"year\":\"0000\",\"month\":\"00\",\"day\":\"00\"}},"
        "\"singlePlayer\":{\"playCount\":false,\"includesDLC\":false,"
        "%s,"
        "\"compPlus\":{\"time\":{\"hours\":null,\"minutes\":null,\"seconds\":null},\"notes\":\"\"},"
        "\"comp100\":{\"time\":{\"hours\":null,\"minutes\":null,\"seconds\":null},\"notes\":\"\"}},"
        "\"speedRuns\":{"
        "\"percAny\":{\"time\":{\"hours\":null,\"minutes\":null,\"seconds\":null},\"notes\":\"\"},"
        "\"perc100\":{\"time\":{\"hours\":null,\"minutes\":null,\"seconds\":null},\"notes\":\"\"}},"
        "\"multiPlayer\":{"
        "\"coOp\":{\"time\":{\"hours\":null,\"minutes\":null,\"seconds\":null}},"
        "\"vs\":{\"time\":{\"hours\":null,\"minutes\":null,\"seconds\":null}}},"
        "\"review\":{\"score\":0,\"notes\":\"\"},"
        "\"additionals\":{\"notes\":\"\",\"video\":\"\"},"
        "\"adminId\":null,"
        "\"customLabels\":{\"custom\":\"\",\"custom2\":\"\",\"custom3\":\"\"}}",
        is_ps5 ? "PlayStation 5" : "PlayStation 4",
        lists->playing ? "true" : "false",
        lists->backlog ? "true" : "false",
        lists->replay ? "true" : "false",
        lists->custom ? "true" : "false",
        lists->custom2 ? "true" : "false",
        lists->custom3 ? "true" : "false",
        lists->completed ? "true" : "false",
        lists->retired ? "true" : "false",
        h, m, s, ph, pm, ps,
        comp_date_str,
        comp_main_str);
    (void)buffer_append_str(&payload, chunk);

    if(!payload.data ||
       http_ctx_request(hctx, 1, "https://howlongtobeat.com/api/submit",
                        payload.data, cookie, NULL, &status, &resp, NULL, 0) != 0) {
        free(payload.data);
        free(resp.data);
        snprintf(error_message, error_message_size,
                 "Failed to reach howlongtobeat.com/api/submit.");
        return -1;
    }
    free(payload.data);

    if(resp.data && json_extract_error(resp.data, error_message, error_message_size) == 0) {
        free(resp.data);
        return -1;
    }
    if(status < 200 || status >= 300) {
        snprintf(error_message, error_message_size,
                 "HLTB /api/submit returned HTTP %d.", status);
        free(resp.data);
        return -1;
    }

    if(resp.data) {
        if(json_extract_int64(resp.data, "newId", &updated_id) == 0 && updated_id > 1) {
            *new_sub_id_out = updated_id;
        } else if(json_extract_int64(resp.data, "updatedId", &updated_id) == 0 && updated_id > 1) {
            *new_sub_id_out = updated_id;
        }
    }

    free(resp.data);
    return 0;
}

/* ---- SQLite Schema & Persistence ---------------------------------------- */

static void set_db_error(char *message, size_t size, const char *context,
                         sqlite3 *db) {
    snprintf(message, size, "%s: %s", context,
             db ? sqlite3_errmsg(db) : "database error");
}

static int open_hltb_db(const char *path, sqlite3 **out_db, char *error_message,
                        size_t error_message_size) {
    static const char schema_sql[] =
        "CREATE TABLE IF NOT EXISTS hltb_config ("
        "  id INTEGER PRIMARY KEY CHECK (id = 1),"
        "  ps5_user_id TEXT NOT NULL DEFAULT '',"
        "  hltb_user_id INTEGER NOT NULL DEFAULT 0,"
        "  hltb_username TEXT NOT NULL DEFAULT '',"
        "  session_cookie TEXT NOT NULL DEFAULT '',"
        "  auto_sync INTEGER NOT NULL DEFAULT 1,"
        "  last_sync TEXT NOT NULL DEFAULT ''"
        ");"
        "INSERT OR IGNORE INTO hltb_config (id, auto_sync) VALUES (1, 1);"
        "CREATE TABLE IF NOT EXISTS hltb_accounts ("
        "  ps5_user_id TEXT PRIMARY KEY,"
        "  hltb_user_id INTEGER NOT NULL DEFAULT 0,"
        "  hltb_username TEXT NOT NULL DEFAULT '',"
        "  session_cookie TEXT NOT NULL DEFAULT '',"
        "  auto_sync INTEGER NOT NULL DEFAULT 1,"
        "  last_sync TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS hltb_games ("
        "  user_id TEXT NOT NULL,"
        "  title_id TEXT NOT NULL,"
        "  selected INTEGER NOT NULL DEFAULT 0,"
        "  hltb_game_id INTEGER NOT NULL DEFAULT 0,"
        "  hltb_submission_id INTEGER NOT NULL DEFAULT 0,"
        "  hltb_title TEXT NOT NULL DEFAULT '',"
        "  synced_seconds INTEGER NOT NULL DEFAULT 0,"
        "  comp_main INTEGER NOT NULL DEFAULT 0,"
        "  comp_plus INTEGER NOT NULL DEFAULT 0,"
        "  comp_100 INTEGER NOT NULL DEFAULT 0,"
        "  comp_all INTEGER NOT NULL DEFAULT 0,"
        "  list_type TEXT NOT NULL DEFAULT 'playing',"
        "  updated_at TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY (user_id, title_id)"
        ");";
    sqlite3 *db = NULL;
    char *err = NULL;

    if(sqlite3_open_v2(path, &db,
                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                           SQLITE_OPEN_FULLMUTEX,
                       NULL) != SQLITE_OK) {
        set_db_error(error_message, error_message_size, "Open HLTB DB failed", db);
        if(db) {
            sqlite3_close(db);
        }
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);

    if(sqlite3_exec(db, schema_sql, NULL, NULL, &err) != SQLITE_OK) {
        snprintf(error_message, error_message_size, "Init HLTB schema failed: %s",
                 err ? err : "unknown error");
        if(err) {
            sqlite3_free(err);
        }
        sqlite3_close(db);
        return -1;
    }

    /* Seamless migration if an earlier playtime_hltb.db existed on /data */
    (void)sqlite3_exec(db, "ALTER TABLE hltb_config ADD COLUMN session_cookie TEXT NOT NULL DEFAULT '';", NULL, NULL, NULL);
    (void)sqlite3_exec(db, "ALTER TABLE hltb_games ADD COLUMN selected INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);
    (void)sqlite3_exec(db, "ALTER TABLE hltb_games ADD COLUMN comp_main INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);
    (void)sqlite3_exec(db, "ALTER TABLE hltb_games ADD COLUMN comp_plus INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);
    (void)sqlite3_exec(db, "ALTER TABLE hltb_games ADD COLUMN comp_100 INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);
    (void)sqlite3_exec(db, "ALTER TABLE hltb_games ADD COLUMN comp_all INTEGER NOT NULL DEFAULT 0;", NULL, NULL, NULL);
    (void)sqlite3_exec(db, "ALTER TABLE hltb_games ADD COLUMN list_type TEXT NOT NULL DEFAULT 'playing';", NULL, NULL, NULL);

    /* Migrate single-user session from hltb_config into hltb_accounts if not already present */
    (void)sqlite3_exec(
        db,
        "INSERT OR IGNORE INTO hltb_accounts (ps5_user_id, hltb_user_id, hltb_username, session_cookie, auto_sync, last_sync) "
        "SELECT CASE WHEN c.ps5_user_id != '' THEN c.ps5_user_id "
        "            WHEN (SELECT user_id FROM hltb_games WHERE user_id != '' LIMIT 1) IS NOT NULL "
        "            THEN (SELECT user_id FROM hltb_games WHERE user_id != '' LIMIT 1) "
        "            ELSE '0' END, "
        "       c.hltb_user_id, c.hltb_username, c.session_cookie, c.auto_sync, c.last_sync "
        "FROM hltb_config c WHERE c.id = 1 AND c.session_cookie != '';",
        NULL, NULL, NULL);

    /* Back-fill ps5_user_id in hltb_config if it was empty */
    (void)sqlite3_exec(
        db,
        "UPDATE hltb_config SET ps5_user_id = (SELECT ps5_user_id FROM hltb_accounts LIMIT 1) "
        "WHERE id = 1 AND ps5_user_id = '' AND EXISTS (SELECT 1 FROM hltb_accounts);",
        NULL, NULL, NULL);

    /* Verify tables have all required columns & primary keys; recreate if stale */
    {
        sqlite3_stmt *chk = NULL;
        int ok1 = (sqlite3_prepare_v2(
                       db,
                       "SELECT ps5_user_id, hltb_user_id, hltb_username, "
                       "session_cookie, auto_sync, last_sync FROM hltb_config LIMIT 1",
                       -1, &chk, NULL) == SQLITE_OK);
        sqlite3_finalize(chk);
        chk = NULL;
        int ok2 = (sqlite3_prepare_v2(
                       db,
                       "INSERT INTO hltb_games (user_id, title_id, selected) "
                       "VALUES ('', '', 0) ON CONFLICT(user_id, title_id) DO NOTHING",
                       -1, &chk, NULL) == SQLITE_OK);
        sqlite3_finalize(chk);
        chk = NULL;
        int ok3 = (sqlite3_prepare_v2(
                       db,
                       "SELECT ps5_user_id, hltb_user_id, hltb_username, "
                       "session_cookie, auto_sync, last_sync FROM hltb_accounts LIMIT 1",
                       -1, &chk, NULL) == SQLITE_OK);
        sqlite3_finalize(chk);
        if(!ok1) {
            (void)sqlite3_exec(db, "DROP TABLE IF EXISTS hltb_config;", NULL, NULL, NULL);
        }
        if(!ok2) {
            klog_printf("[PS5-HLTB] Recreating stale hltb_games table at %s\n", path);
            (void)sqlite3_exec(db, "DROP TABLE IF EXISTS hltb_games;", NULL, NULL, NULL);
        }
        if(!ok3) {
            (void)sqlite3_exec(db, "DROP TABLE IF EXISTS hltb_accounts;", NULL, NULL, NULL);
        }
        if(!ok1 || !ok2 || !ok3) {
            if(sqlite3_exec(db, schema_sql, NULL, NULL, &err) != SQLITE_OK) {
                snprintf(error_message, error_message_size,
                         "Recreate HLTB schema failed: %s",
                         err ? err : "unknown error");
                if(err) {
                    sqlite3_free(err);
                }
                sqlite3_close(db);
                return -1;
            }
        }
    }

    *out_db = db;
    return 0;
}

char *hltb_state_json(const char *hltb_db_path, size_t *length,
                      char *error_message, size_t error_message_size) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    buffer_t out = {0};
    char chunk[256];
    char cur_ps5_user[64] = {0};
    int rc;
    int first = 1;

    if(open_hltb_db(hltb_db_path, &db, error_message, error_message_size) != 0) {
        return NULL;
    }

    rc = sqlite3_prepare_v2(
        db,
        "SELECT ps5_user_id, hltb_user_id, hltb_username, session_cookie, "
        "auto_sync, last_sync FROM hltb_config WHERE id = 1",
        -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        set_db_error(error_message, error_message_size, "Read HLTB config failed", db);
        sqlite3_close(db);
        return NULL;
    }

    if(sqlite3_step(stmt) == SQLITE_ROW) {
        const char *ps5_user = (const char *)sqlite3_column_text(stmt, 0);
        long long hltb_uid = sqlite3_column_int64(stmt, 1);
        const char *hltb_uname = (const char *)sqlite3_column_text(stmt, 2);
        const char *cookie = (const char *)sqlite3_column_text(stmt, 3);
        int auto_sync = sqlite3_column_int(stmt, 4);
        const char *last_sync = (const char *)sqlite3_column_text(stmt, 5);

        if(ps5_user) {
            snprintf(cur_ps5_user, sizeof cur_ps5_user, "%s", ps5_user);
        }

        /* If cur_ps5_user is set, check if hltb_accounts has specific info for them */
        if(cur_ps5_user[0]) {
            sqlite3_stmt *astmt = NULL;
            if(sqlite3_prepare_v2(
                   db,
                   "SELECT hltb_user_id, hltb_username, session_cookie, auto_sync, last_sync "
                   "FROM hltb_accounts WHERE ps5_user_id = ?1",
                   -1, &astmt, NULL) == SQLITE_OK &&
               sqlite3_bind_text(astmt, 1, cur_ps5_user, -1, SQLITE_STATIC) == SQLITE_OK &&
               sqlite3_step(astmt) == SQLITE_ROW) {
                hltb_uid = sqlite3_column_int64(astmt, 0);
                hltb_uname = (const char *)sqlite3_column_text(astmt, 1);
                cookie = (const char *)sqlite3_column_text(astmt, 2);
                auto_sync = sqlite3_column_int(astmt, 3);
                last_sync = (const char *)sqlite3_column_text(astmt, 4);
            }
            if(astmt) {
                sqlite3_finalize(astmt);
            }
        }

        int logged_in = (cookie && cookie[0] != '\0' && hltb_uid > 0) ? 1 : 0;

        if(buffer_append_str(&out, "{\"ps5User\":") != 0 ||
           buffer_append_json_string(&out, cur_ps5_user) != 0) {
            goto oom;
        }
        snprintf(chunk, sizeof chunk, ",\"loggedIn\":%d,\"hltbUserId\":%lld,\"hltbUsername\":",
                 logged_in, hltb_uid);
        if(buffer_append_str(&out, chunk) != 0 ||
           buffer_append_json_string(&out, hltb_uname ? hltb_uname : "") != 0) {
            goto oom;
        }
        snprintf(chunk, sizeof chunk, ",\"autoSync\":%d,\"lastSync\":",
                 auto_sync ? 1 : 0);
        if(buffer_append_str(&out, chunk) != 0 ||
           buffer_append_json_string(&out, last_sync ? last_sync : "") != 0) {
            goto oom;
        }
    } else {
        if(buffer_append_str(&out,
                             "{\"ps5User\":\"\",\"loggedIn\":0,\"hltbUserId\":0,"
                             "\"hltbUsername\":\"\",\"autoSync\":1,\"lastSync\":\"\"") != 0) {
            goto oom;
        }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    /* Build accounts dictionary mapping each ps5_user_id to their HLTB account status */
    if(buffer_append_str(&out, ",\"accounts\":{") != 0) {
        goto oom;
    }
    rc = sqlite3_prepare_v2(
        db,
        "SELECT ps5_user_id, hltb_user_id, hltb_username, session_cookie, auto_sync, last_sync "
        "FROM hltb_accounts ORDER BY ps5_user_id",
        -1, &stmt, NULL);
    if(rc == SQLITE_OK) {
        int acct_first = 1;
        while(sqlite3_step(stmt) == SQLITE_ROW) {
            const char *auid = (const char *)sqlite3_column_text(stmt, 0);
            long long ah_uid = sqlite3_column_int64(stmt, 1);
            const char *ah_name = (const char *)sqlite3_column_text(stmt, 2);
            const char *ack = (const char *)sqlite3_column_text(stmt, 3);
            int a_sync = sqlite3_column_int(stmt, 4);
            const char *a_last = (const char *)sqlite3_column_text(stmt, 5);
            int a_logged = (ack && ack[0] != '\0' && ah_uid > 0) ? 1 : 0;

            if(!auid || !auid[0]) {
                continue;
            }
            if(!acct_first && buffer_append_str(&out, ",") != 0) {
                goto oom;
            }
            acct_first = 0;
            if(buffer_append_str(&out, "\"") != 0 ||
               buffer_append_str(&out, auid) != 0 ||
               buffer_append_str(&out, "\":{\"loggedIn\":") != 0) {
                goto oom;
            }
            snprintf(chunk, sizeof chunk, "%d,\"hltbUserId\":%lld,\"hltbUsername\":",
                     a_logged, ah_uid);
            if(buffer_append_str(&out, chunk) != 0 ||
               buffer_append_json_string(&out, ah_name ? ah_name : "") != 0) {
                goto oom;
            }
            snprintf(chunk, sizeof chunk, ",\"autoSync\":%d,\"lastSync\":", a_sync ? 1 : 0);
            if(buffer_append_str(&out, chunk) != 0 ||
               buffer_append_json_string(&out, a_last ? a_last : "") != 0 ||
               buffer_append_str(&out, "}") != 0) {
                goto oom;
            }
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if(buffer_append_str(&out, "},\"games\":[") != 0) {
        goto oom;
    }

    rc = sqlite3_prepare_v2(
        db,
        "SELECT user_id, title_id, selected, hltb_game_id, hltb_submission_id, "
        "hltb_title, synced_seconds, comp_main, comp_plus, comp_100, updated_at, "
        "COALESCE(NULLIF(list_type, ''), 'playing'), comp_all "
        "FROM hltb_games ORDER BY title_id",
        -1, &stmt, NULL);
    if(rc != SQLITE_OK) {
        set_db_error(error_message, error_message_size, "Read HLTB games failed", db);
        free(out.data);
        sqlite3_close(db);
        return NULL;
    }

    while((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *user_id = (const char *)sqlite3_column_text(stmt, 0);
        const char *title_id = (const char *)sqlite3_column_text(stmt, 1);
        int selected = sqlite3_column_int(stmt, 2);
        long long hltb_id = sqlite3_column_int64(stmt, 3);
        long long sub_id = sqlite3_column_int64(stmt, 4);
        const char *hltb_title = (const char *)sqlite3_column_text(stmt, 5);
        long long synced_sec = sqlite3_column_int64(stmt, 6);
        long long comp_main = sqlite3_column_int64(stmt, 7);
        long long comp_plus = sqlite3_column_int64(stmt, 8);
        long long comp_100 = sqlite3_column_int64(stmt, 9);
        const char *updated_at = (const char *)sqlite3_column_text(stmt, 10);
        const char *list_type = (const char *)sqlite3_column_text(stmt, 11);
        long long comp_all = sqlite3_column_int64(stmt, 12);

        if(!first && buffer_append_str(&out, ",") != 0) {
            goto oom;
        }
        first = 0;

        if(buffer_append_str(&out, "{\"user\":") != 0 ||
           buffer_append_json_string(&out, user_id ? user_id : "") != 0 ||
           buffer_append_str(&out, ",\"id\":") != 0 ||
           buffer_append_json_string(&out, title_id ? title_id : "") != 0) {
            goto oom;
        }
        snprintf(chunk, sizeof chunk,
                 ",\"selected\":%d,\"hltbId\":%lld,\"subId\":%lld,\"hltbTitle\":",
                 selected ? 1 : 0, hltb_id, sub_id);
        if(buffer_append_str(&out, chunk) != 0 ||
           buffer_append_json_string(&out, hltb_title ? hltb_title : "") != 0) {
            goto oom;
        }
        snprintf(chunk, sizeof chunk,
                 ",\"syncedSec\":%lld,\"compMain\":%lld,\"compPlus\":%lld,"
                 "\"comp100\":%lld,\"compAll\":%lld,\"updatedAt\":",
                 synced_sec, comp_main, comp_plus, comp_100, comp_all);
        if(buffer_append_str(&out, chunk) != 0 ||
           buffer_append_json_string(&out, updated_at ? updated_at : "") != 0 ||
           buffer_append_str(&out, ",\"listType\":") != 0 ||
           buffer_append_json_string(&out, (list_type && list_type[0]) ? list_type : "playing") != 0 ||
           buffer_append_str(&out, "}") != 0) {
            goto oom;
        }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if(buffer_append_str(&out, "]") != 0) {
        goto oom;
    }

    snprintf(chunk, sizeof chunk,
             ",\"syncActive\":%d,\"syncStep\":%d,\"lastSyncedCount\":%d,\"syncMsg\":",
             g_sync_active, g_sync_step, g_last_synced_count);
    if(buffer_append_str(&out, chunk) != 0 ||
       buffer_append_json_string(&out, g_sync_msg[0] ? g_sync_msg : "") != 0 ||
       buffer_append_str(&out, "}") != 0) {
        goto oom;
    }

    sqlite3_close(db);
    *length = out.length;
    return out.data;

oom:
    if(stmt) {
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    free(out.data);
    snprintf(error_message, error_message_size, "Out of memory building HLTB JSON.");
    return NULL;
}

int hltb_save_config(const char *hltb_db_path, const char *ps5_user_id,
                     int auto_sync, char *error_message,
                     size_t error_message_size) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;

    if(open_hltb_db(hltb_db_path, &db, error_message, error_message_size) != 0) {
        return -1;
    }

    /* Update current active ps5_user_id in hltb_config */
    if(sqlite3_prepare_v2(
           db,
           "UPDATE hltb_config SET ps5_user_id = ?1, auto_sync = ?2 WHERE id = 1",
           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ps5_user_id ? ps5_user_id : "", -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, auto_sync ? 1 : 0);
        (void)sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    if(ps5_user_id && ps5_user_id[0]) {
        /* Update auto_sync in hltb_accounts for this user if they exist */
        if(sqlite3_prepare_v2(
               db,
               "UPDATE hltb_accounts SET auto_sync = ?2 WHERE ps5_user_id = ?1",
               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ps5_user_id, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, auto_sync ? 1 : 0);
            (void)sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = NULL;
        }

        /* Keep row 1 in sync with active user's credentials */
        if(sqlite3_prepare_v2(
               db,
               "UPDATE hltb_config SET "
               "hltb_user_id = COALESCE((SELECT hltb_user_id FROM hltb_accounts WHERE ps5_user_id = ?1), 0), "
               "hltb_username = COALESCE((SELECT hltb_username FROM hltb_accounts WHERE ps5_user_id = ?1), ''), "
               "session_cookie = COALESCE((SELECT session_cookie FROM hltb_accounts WHERE ps5_user_id = ?1), ''), "
               "auto_sync = COALESCE((SELECT auto_sync FROM hltb_accounts WHERE ps5_user_id = ?1), ?2), "
               "last_sync = COALESCE((SELECT last_sync FROM hltb_accounts WHERE ps5_user_id = ?1), '') "
               "WHERE id = 1",
               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ps5_user_id, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 2, auto_sync ? 1 : 0);
            (void)sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = NULL;
        }
    }

    sqlite3_close(db);
    return 0;
}

int hltb_login(const char *hltb_db_path, const char *ps5_user_id,
               const char *username, const char *password,
               const char *captcha_token, const char *cookie_override,
               const char *user_agent, char *error_message,
               size_t error_message_size) {
    http_ctx_t hctx;
    char cookie[2048] = {0};
    char resolved_name[128] = {0};
    long long resolved_uid = 0;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_hltb_mutex);

    if(http_ctx_init(&hctx, user_agent) != 0) {
        pthread_mutex_unlock(&g_hltb_mutex);
        snprintf(error_message, error_message_size,
                 "Failed to initialize PS5 HTTPS client (%s).",
                 g_last_http_error[0] ? g_last_http_error : "init error");
        return -1;
    }

    if(cookie_override && cookie_override[0]) {
        const char *trimmed = cookie_override;
        while(*trimmed == ' ' || *trimmed == '\t' || *trimmed == '\r' || *trimmed == '\n') {
            trimmed++;
        }
        if(strchr(trimmed, '=') == NULL) {
            snprintf(cookie, sizeof cookie, "hltb_alive=%s", trimmed);
        } else {
            snprintf(cookie, sizeof cookie, "%s", trimmed);
        }
        if(hltb_api_verify_user(&hctx, cookie, &resolved_uid, resolved_name,
                                sizeof resolved_name) != 0) {
            http_ctx_fini(&hctx);
            pthread_mutex_unlock(&g_hltb_mutex);
            snprintf(error_message, error_message_size,
                     "Invalid or expired HLTB session cookie (%s).",
                     g_last_http_error[0] ? g_last_http_error : "auth failed");
            return -1;
        }
    } else {
        buffer_t body = {0};
        buffer_t resp = {0};
        int status = 0;

        if(!username || !username[0] || !password || !password[0]) {
            http_ctx_fini(&hctx);
            pthread_mutex_unlock(&g_hltb_mutex);
            snprintf(error_message, error_message_size,
                     "Username and password are required.");
            return -1;
        }

        (void)buffer_append_str(&body, "{\"user_name\":");
        (void)buffer_append_json_string(&body, username);
        (void)buffer_append_str(&body, ",\"password\":");
        (void)buffer_append_json_string(&body, password);
        (void)buffer_append_str(&body, ",\"token\":");
        (void)buffer_append_json_string(&body, captcha_token ? captcha_token : "");
        (void)buffer_append_str(&body, "}");

        if(!body.data ||
           http_ctx_request(&hctx, 1, "https://howlongtobeat.com/api/login",
                            body.data, NULL, NULL, &status, &resp, cookie,
                            sizeof cookie) != 0) {
            free(body.data);
            free(resp.data);
            http_ctx_fini(&hctx);
            pthread_mutex_unlock(&g_hltb_mutex);
            snprintf(error_message, error_message_size,
                     "Failed to connect to howlongtobeat.com/api/login (%s).",
                     g_last_http_error[0] ? g_last_http_error : "network error");
            return -1;
        }
        free(body.data);

        if(resp.data &&
           json_extract_error(resp.data, error_message, error_message_size) == 0) {
            free(resp.data);
            http_ctx_fini(&hctx);
            pthread_mutex_unlock(&g_hltb_mutex);
            return -1;
        }

        if(!cookie[0]) {
            free(resp.data);
            http_ctx_fini(&hctx);
            pthread_mutex_unlock(&g_hltb_mutex);
            snprintf(error_message, error_message_size,
                     "HLTB login did not return a session cookie (HTTP %d).",
                     status);
            return -1;
        }

        if(resp.data) {
            (void)json_extract_int64(resp.data, "user_id", &resolved_uid);
            (void)json_extract_string(resp.data, "user_name", resolved_name,
                                      sizeof resolved_name);
        }
        free(resp.data);

        if(resolved_uid <= 0 || !resolved_name[0]) {
            if(hltb_api_verify_user(&hctx, cookie, &resolved_uid, resolved_name,
                                    sizeof resolved_name) != 0) {
                http_ctx_fini(&hctx);
                pthread_mutex_unlock(&g_hltb_mutex);
                snprintf(error_message, error_message_size,
                         "HLTB login cookie verification failed.");
                return -1;
            }
        }
    }

    http_ctx_fini(&hctx);

    if(open_hltb_db(hltb_db_path, &db, error_message, error_message_size) != 0) {
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }

    /* Save / update in hltb_accounts for this PS5 profile */
    if(ps5_user_id && ps5_user_id[0]) {
        if(sqlite3_prepare_v2(
               db,
               "INSERT INTO hltb_accounts (ps5_user_id, hltb_user_id, hltb_username, session_cookie, auto_sync) "
               "VALUES (?1, ?2, ?3, ?4, 1) "
               "ON CONFLICT(ps5_user_id) DO UPDATE SET "
               "hltb_user_id = excluded.hltb_user_id, "
               "hltb_username = excluded.hltb_username, "
               "session_cookie = excluded.session_cookie, "
               "auto_sync = 1",
               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ps5_user_id, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)resolved_uid);
            sqlite3_bind_text(stmt, 3, resolved_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, cookie, -1, SQLITE_STATIC);
            (void)sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = NULL;
        }
    }

    if(sqlite3_prepare_v2(
           db,
           "UPDATE hltb_config SET "
           "ps5_user_id = CASE WHEN ?1 != '' THEN ?1 ELSE ps5_user_id END, "
           "hltb_user_id = ?2, hltb_username = ?3, session_cookie = ?4, "
           "auto_sync = 1 WHERE id = 1",
           -1, &stmt, NULL) != SQLITE_OK ||
       sqlite3_bind_text(stmt, 1, ps5_user_id ? ps5_user_id : "", -1,
                         SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_int64(stmt, 2, (sqlite3_int64)resolved_uid) != SQLITE_OK ||
       sqlite3_bind_text(stmt, 3, resolved_name, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_text(stmt, 4, cookie, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_step(stmt) != SQLITE_DONE) {
        set_db_error(error_message, error_message_size, "Save HLTB login failed", db);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }

    /* Back-fill hltb_accounts if ps5_user_id was empty */
    (void)sqlite3_exec(
        db,
        "INSERT INTO hltb_accounts (ps5_user_id, hltb_user_id, hltb_username, session_cookie, auto_sync) "
        "SELECT ps5_user_id, hltb_user_id, hltb_username, session_cookie, auto_sync FROM hltb_config WHERE id = 1 AND ps5_user_id != '' "
        "ON CONFLICT(ps5_user_id) DO UPDATE SET "
        "hltb_user_id = excluded.hltb_user_id, hltb_username = excluded.hltb_username, session_cookie = excluded.session_cookie;",
        NULL, NULL, NULL);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    pthread_mutex_unlock(&g_hltb_mutex);
    return 0;
}

int hltb_logout(const char *hltb_db_path, const char *ps5_user_id,
                char *error_message, size_t error_message_size) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;

    pthread_mutex_lock(&g_hltb_mutex);
    if(open_hltb_db(hltb_db_path, &db, error_message, error_message_size) != 0) {
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }

    if(ps5_user_id && ps5_user_id[0]) {
        /* Clear in hltb_accounts for this user */
        if(sqlite3_prepare_v2(
               db,
               "UPDATE hltb_accounts SET hltb_user_id = 0, hltb_username = '', session_cookie = '' "
               "WHERE ps5_user_id = ?1",
               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ps5_user_id, -1, SQLITE_STATIC);
            (void)sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = NULL;
        }
        /* Clear row 1 if it matches this user */
        if(sqlite3_prepare_v2(
               db,
               "UPDATE hltb_config SET hltb_user_id = 0, hltb_username = '', session_cookie = '' "
               "WHERE id = 1 AND (ps5_user_id = ?1 OR ps5_user_id = '')",
               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ps5_user_id, -1, SQLITE_STATIC);
            (void)sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            stmt = NULL;
        }
    } else {
        (void)sqlite3_exec(
            db,
            "UPDATE hltb_accounts SET hltb_user_id = 0, hltb_username = '', session_cookie = '' "
            "WHERE ps5_user_id = (SELECT ps5_user_id FROM hltb_config WHERE id = 1);"
            "UPDATE hltb_config SET hltb_user_id = 0, hltb_username = '', session_cookie = '' WHERE id = 1;",
            NULL, NULL, NULL);
    }

    sqlite3_close(db);
    pthread_mutex_unlock(&g_hltb_mutex);
    return 0;
}

int hltb_select_game(const char *hltb_db_path, const char *ps5_user_id,
                     const char *title_id, const char *title, int selected,
                     long long custom_hltb_id, const char *list_type,
                     char *error_message, size_t error_message_size) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    char valid_list[16] = {0};
    long long comp_main = 0, comp_plus = 0, comp_100 = 0, comp_all = 0;
    char fetched_title[256] = {0};

    if(!ps5_user_id || !ps5_user_id[0] || !title_id || !title_id[0]) {
        snprintf(error_message, error_message_size, "Missing user or title ID.");
        return -1;
    }

    /* Homebrew apps are not games and cannot be synced to HowLongToBeat */
    if(strncmp(title_id, "PPSA9", 5) == 0 || strncmp(title_id, "CUSA9", 5) == 0) {
        snprintf(error_message, error_message_size,
                 "Homebrew applications cannot be synced to HowLongToBeat.");
        return -1;
    }

    if(list_type && list_type[0]) {
        if(strcmp(list_type, "playing") == 0 ||
           strcmp(list_type, "backlog") == 0 ||
           strcmp(list_type, "completed") == 0) {
            snprintf(valid_list, sizeof valid_list, "%s", list_type);
        }
    }

    if(custom_hltb_id > 0) {
        http_ctx_t hctx;
        if(http_ctx_init(&hctx, NULL) == 0) {
            (void)hltb_api_fetch_game_by_id(&hctx, custom_hltb_id, fetched_title,
                                            sizeof fetched_title, &comp_main,
                                            &comp_plus, &comp_100, &comp_all);
            http_ctx_fini(&hctx);
        }
    } else if(custom_hltb_id == 0 && title && title[0]) {
        http_ctx_t hctx;
        if(http_ctx_init(&hctx, NULL) == 0) {
            (void)hltb_api_search_game(&hctx, title, &custom_hltb_id, fetched_title,
                                       sizeof fetched_title, &comp_main,
                                       &comp_plus, &comp_100, &comp_all);
            http_ctx_fini(&hctx);
        }
    }

    const char *save_title = (fetched_title[0] != '\0') ? fetched_title : (title ? title : "");

    if(open_hltb_db(hltb_db_path, &db, error_message, error_message_size) != 0) {
        return -1;
    }

    if(sqlite3_prepare_v2(
           db,
           "UPDATE hltb_games SET "
           "selected = ?3, "
           "hltb_title = CASE WHEN ?4 != '' THEN ?4 "
           "                  WHEN hltb_title != '' THEN hltb_title "
           "                  ELSE COALESCE((SELECT hltb_title FROM hltb_games WHERE title_id = ?2 AND hltb_title != '' LIMIT 1), '') END, "
           "hltb_game_id = CASE WHEN ?5 >= 0 THEN ?5 "
           "                    WHEN hltb_game_id > 0 THEN hltb_game_id "
           "                    ELSE COALESCE((SELECT hltb_game_id FROM hltb_games WHERE title_id = ?2 AND hltb_game_id > 0 LIMIT 1), 0) END, "
           "hltb_submission_id = CASE WHEN ?5 >= 0 AND ?5 != hltb_game_id THEN 0 ELSE hltb_submission_id END, "
           "synced_seconds = CASE WHEN ?5 >= 0 AND ?5 != hltb_game_id THEN -1 "
           "                      WHEN ?6 != '' AND ?6 != list_type THEN -1 "
           "                      WHEN selected = 0 AND ?3 = 1 THEN -1 "
           "                      ELSE synced_seconds END, "
           "list_type = CASE WHEN ?6 != '' THEN ?6 ELSE list_type END, "
           "comp_main = CASE WHEN ?7 > 0 THEN ?7 "
           "                 WHEN comp_main > 0 THEN comp_main "
           "                 ELSE COALESCE((SELECT comp_main FROM hltb_games WHERE title_id = ?2 AND comp_main > 0 LIMIT 1), 0) END, "
           "comp_plus = CASE WHEN ?8 > 0 THEN ?8 "
           "                 WHEN comp_plus > 0 THEN comp_plus "
           "                 ELSE COALESCE((SELECT comp_plus FROM hltb_games WHERE title_id = ?2 AND comp_plus > 0 LIMIT 1), 0) END, "
           "comp_100 = CASE WHEN ?9 > 0 THEN ?9 "
           "                WHEN comp_100 > 0 THEN comp_100 "
           "                ELSE COALESCE((SELECT comp_100 FROM hltb_games WHERE title_id = ?2 AND comp_100 > 0 LIMIT 1), 0) END, "
           "comp_all = CASE WHEN ?10 > 0 THEN ?10 "
           "                WHEN comp_all > 0 THEN comp_all "
           "                ELSE COALESCE((SELECT comp_all FROM hltb_games WHERE title_id = ?2 AND comp_all > 0 LIMIT 1), 0) END, "
           "updated_at = datetime('now', 'localtime') "
           "WHERE user_id = ?1 AND title_id = ?2",
           -1, &stmt, NULL) != SQLITE_OK ||
       sqlite3_bind_text(stmt, 1, ps5_user_id, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_int(stmt, 3, selected ? 1 : 0) != SQLITE_OK ||
       sqlite3_bind_text(stmt, 4, save_title, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_int64(stmt, 5, (sqlite3_int64)custom_hltb_id) != SQLITE_OK ||
       sqlite3_bind_text(stmt, 6, valid_list, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_int64(stmt, 7, (sqlite3_int64)comp_main) != SQLITE_OK ||
       sqlite3_bind_int64(stmt, 8, (sqlite3_int64)comp_plus) != SQLITE_OK ||
       sqlite3_bind_int64(stmt, 9, (sqlite3_int64)comp_100) != SQLITE_OK ||
       sqlite3_bind_int64(stmt, 10, (sqlite3_int64)comp_all) != SQLITE_OK ||
       sqlite3_step(stmt) != SQLITE_DONE) {
        set_db_error(error_message, error_message_size,
                     "Update game sync selection failed", db);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if(sqlite3_changes(db) == 0) {
        if(sqlite3_prepare_v2(
               db,
               "INSERT OR REPLACE INTO hltb_games "
               "(user_id, title_id, selected, hltb_title, hltb_game_id, list_type, synced_seconds, "
               "comp_main, comp_plus, comp_100, comp_all, updated_at) "
               "VALUES (?1, ?2, ?3, "
               "CASE WHEN ?4 != '' THEN ?4 ELSE COALESCE((SELECT hltb_title FROM hltb_games WHERE title_id = ?2 AND hltb_title != '' LIMIT 1), '') END, "
               "CASE WHEN ?5 >= 0 THEN ?5 ELSE COALESCE((SELECT hltb_game_id FROM hltb_games WHERE title_id = ?2 AND hltb_game_id > 0 LIMIT 1), 0) END, "
               "CASE WHEN ?6 != '' THEN ?6 ELSE 'playing' END, -1, "
               "CASE WHEN ?7 > 0 THEN ?7 ELSE COALESCE((SELECT comp_main FROM hltb_games WHERE title_id = ?2 AND comp_main > 0 LIMIT 1), 0) END, "
               "CASE WHEN ?8 > 0 THEN ?8 ELSE COALESCE((SELECT comp_plus FROM hltb_games WHERE title_id = ?2 AND comp_plus > 0 LIMIT 1), 0) END, "
               "CASE WHEN ?9 > 0 THEN ?9 ELSE COALESCE((SELECT comp_100 FROM hltb_games WHERE title_id = ?2 AND comp_100 > 0 LIMIT 1), 0) END, "
               "CASE WHEN ?10 > 0 THEN ?10 ELSE COALESCE((SELECT comp_all FROM hltb_games WHERE title_id = ?2 AND comp_all > 0 LIMIT 1), 0) END, "
               "datetime('now', 'localtime'))",
               -1, &stmt, NULL) != SQLITE_OK ||
           sqlite3_bind_text(stmt, 1, ps5_user_id, -1, SQLITE_STATIC) != SQLITE_OK ||
           sqlite3_bind_text(stmt, 2, title_id, -1, SQLITE_STATIC) != SQLITE_OK ||
           sqlite3_bind_int(stmt, 3, selected ? 1 : 0) != SQLITE_OK ||
           sqlite3_bind_text(stmt, 4, save_title, -1, SQLITE_STATIC) != SQLITE_OK ||
           sqlite3_bind_int64(stmt, 5, (sqlite3_int64)custom_hltb_id) != SQLITE_OK ||
           sqlite3_bind_text(stmt, 6, valid_list, -1, SQLITE_STATIC) != SQLITE_OK ||
           sqlite3_bind_int64(stmt, 7, (sqlite3_int64)comp_main) != SQLITE_OK ||
           sqlite3_bind_int64(stmt, 8, (sqlite3_int64)comp_plus) != SQLITE_OK ||
           sqlite3_bind_int64(stmt, 9, (sqlite3_int64)comp_100) != SQLITE_OK ||
           sqlite3_bind_int64(stmt, 10, (sqlite3_int64)comp_all) != SQLITE_OK ||
           sqlite3_step(stmt) != SQLITE_DONE) {
            set_db_error(error_message, error_message_size,
                         "Insert game sync selection failed", db);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return -1;
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return 0;
}

typedef struct {
    char ps5_user_id[32];
    long long hltb_user_id;
    char hltb_username[128];
    char session_cookie[2048];
    int auto_sync;
} hltb_account_entry_t;

typedef struct {
    char user_id[24];
    char title_id[16];
    long long hltb_game_id;
    long long hltb_submission_id;
    char hltb_title[256];
    long long synced_seconds;
    long long comp_main;
    long long comp_plus;
    long long comp_100;
    long long comp_all;
    char list_type[16];
} selected_game_t;

static int read_ps5_game_info(sqlite3 *app_db, const char *user_id,
                              const char *title_id, char *name_out,
                              size_t name_size, long long *played_sec_out) {
    sqlite3_stmt *stmt = NULL;
    char *sql = NULL;
    size_t i;

    name_out[0] = '\0';
    *played_sec_out = 0;

    for(i = 0; user_id[i]; i++) {
        if(!isalnum((unsigned char)user_id[i]) && user_id[i] != '_') {
            return -1;
        }
    }

    if(sqlite3_prepare_v2(
           app_db,
           "SELECT MAX(titleName) FROM tbl_contentinfo "
           "WHERE UPPER(TRIM(titleId)) = UPPER(TRIM(?1)) OR UPPER(TRIM(titleId)) LIKE UPPER(TRIM(?1)) || '%'",
           -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(stmt, 0);
        if(t && t[0]) {
            snprintf(name_out, name_size, "%s", t);
        }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if(!name_out[0]) {
        snprintf(name_out, name_size, "%s", title_id);
    }

    sql = sqlite3_mprintf(
        "SELECT MAX(playedTimeOnConsole) FROM \"tbl_iconinfo_%w\" "
        "WHERE UPPER(TRIM(titleId)) = UPPER(TRIM(?1)) OR UPPER(TRIM(titleId)) LIKE UPPER(TRIM(?1)) || '%%'",
        user_id);
    if(!sql) {
        return -1;
    }
    if(sqlite3_prepare_v2(app_db, sql, -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW) {
        if(sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            long long raw = sqlite3_column_int64(stmt, 0);
            if(raw > 0) {
                *played_sec_out = raw * (long long)PS5_HLTB_UNIT_SECONDS;
            }
        }
    } else {
        klog_printf("[PS5-HLTB] Error querying playtime for %s (%s): %s\n",
                    title_id, user_id, sqlite3_errmsg(app_db));
    }
    sqlite3_finalize(stmt);
    sqlite3_free(sql);
    klog_printf("[PS5-HLTB] Read game info for %s (%s): name='%s' playtime=%lld sec\n",
                title_id, user_id, name_out, *played_sec_out);
    return 0;
}

int hltb_sync_selected(const char *app_db_path, const char *hltb_db_path,
                       int force_all, int *synced_count_out,
                       char *error_message, size_t error_message_size) {
    sqlite3 *hltb_db = NULL;
    sqlite3 *app_db = NULL;
    sqlite3_stmt *stmt = NULL;
    selected_game_t *items = NULL;
    size_t item_count = 0;
    size_t item_cap = 0;
    http_ctx_t hctx;
    int synced_count = 0;
    size_t i;

    if(synced_count_out) {
        *synced_count_out = 0;
    }

    g_sync_active = 1;
    g_sync_step = 1;
    snprintf(g_sync_msg, sizeof g_sync_msg, "Syncing selected games to HowLongToBeat...");

    pthread_mutex_lock(&g_hltb_mutex);

    if(open_hltb_db(hltb_db_path, &hltb_db, error_message, error_message_size) != 0) {
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }

    hltb_account_entry_t accounts[16];
    int num_accounts = 0;

    if(sqlite3_prepare_v2(
           hltb_db,
           "SELECT ps5_user_id, hltb_user_id, hltb_username, session_cookie, auto_sync "
           "FROM hltb_accounts WHERE session_cookie != '' AND hltb_user_id > 0",
           -1, &stmt, NULL) == SQLITE_OK) {
        while(num_accounts < 16 && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *puid = (const char *)sqlite3_column_text(stmt, 0);
            long long huid = sqlite3_column_int64(stmt, 1);
            const char *hname = (const char *)sqlite3_column_text(stmt, 2);
            const char *sck = (const char *)sqlite3_column_text(stmt, 3);
            int async = sqlite3_column_int(stmt, 4);

            if(puid && sck && sck[0] && huid > 0) {
                memset(&accounts[num_accounts], 0, sizeof accounts[num_accounts]);
                snprintf(accounts[num_accounts].ps5_user_id, sizeof accounts[num_accounts].ps5_user_id, "%s", puid);
                accounts[num_accounts].hltb_user_id = huid;
                if(hname) {
                    snprintf(accounts[num_accounts].hltb_username, sizeof accounts[num_accounts].hltb_username, "%s", hname);
                }
                snprintf(accounts[num_accounts].session_cookie, sizeof accounts[num_accounts].session_cookie, "%s", sck);
                accounts[num_accounts].auto_sync = async;
                num_accounts++;
            }
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    if(num_accounts == 0) {
        /* Fallback to row 1 of hltb_config */
        if(sqlite3_prepare_v2(
               hltb_db,
               "SELECT ps5_user_id, hltb_user_id, hltb_username, session_cookie, auto_sync "
               "FROM hltb_config WHERE id = 1 AND session_cookie != '' AND hltb_user_id > 0",
               -1, &stmt, NULL) == SQLITE_OK &&
           sqlite3_step(stmt) == SQLITE_ROW) {
            const char *puid = (const char *)sqlite3_column_text(stmt, 0);
            long long huid = sqlite3_column_int64(stmt, 1);
            const char *hname = (const char *)sqlite3_column_text(stmt, 2);
            const char *sck = (const char *)sqlite3_column_text(stmt, 3);
            int async = sqlite3_column_int(stmt, 4);

            if(sck && sck[0] && huid > 0) {
                memset(&accounts[0], 0, sizeof accounts[0]);
                if(puid) {
                    snprintf(accounts[0].ps5_user_id, sizeof accounts[0].ps5_user_id, "%s", puid);
                }
                accounts[0].hltb_user_id = huid;
                if(hname) {
                    snprintf(accounts[0].hltb_username, sizeof accounts[0].hltb_username, "%s", hname);
                }
                snprintf(accounts[0].session_cookie, sizeof accounts[0].session_cookie, "%s", sck);
                accounts[0].auto_sync = async;
                num_accounts = 1;
            }
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    if(num_accounts == 0) {
        sqlite3_close(hltb_db);
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        snprintf(error_message, error_message_size,
                 "Not logged in to HowLongToBeat.");
        return -1;
    }

    if(sqlite3_prepare_v2(
           hltb_db,
           "SELECT user_id, title_id, hltb_game_id, hltb_submission_id, "
           "hltb_title, synced_seconds, comp_main, comp_plus, comp_100, "
           "COALESCE(NULLIF(list_type, ''), 'playing'), comp_all "
           "FROM hltb_games WHERE selected = 1",
           -1, &stmt, NULL) != SQLITE_OK) {
        set_db_error(error_message, error_message_size,
                     "Query selected games failed", hltb_db);
        sqlite3_close(hltb_db);
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }

    while(sqlite3_step(stmt) == SQLITE_ROW) {
        const char *uid = (const char *)sqlite3_column_text(stmt, 0);
        const char *tid = (const char *)sqlite3_column_text(stmt, 1);
        const char *htitle = (const char *)sqlite3_column_text(stmt, 4);
        const char *lt = (const char *)sqlite3_column_text(stmt, 9);
        selected_game_t *it;

        if(!uid || !tid) {
            continue;
        }
        /* Never sync homebrew titles to HowLongToBeat */
        if(strncmp(tid, "PPSA9", 5) == 0 || strncmp(tid, "CUSA9", 5) == 0) {
            continue;
        }
        if(item_count == item_cap) {
            size_t ncap = item_cap ? item_cap * 2 : 16;
            selected_game_t *grown =
                (selected_game_t *)realloc(items, ncap * sizeof *items);
            if(!grown) {
                break;
            }
            items = grown;
            item_cap = ncap;
        }
        it = &items[item_count++];
        memset(it, 0, sizeof *it);
        snprintf(it->user_id, sizeof it->user_id, "%s", uid);
        snprintf(it->title_id, sizeof it->title_id, "%s", tid);
        it->hltb_game_id = sqlite3_column_int64(stmt, 2);
        it->hltb_submission_id = sqlite3_column_int64(stmt, 3);
        if(htitle) {
            snprintf(it->hltb_title, sizeof it->hltb_title, "%s", htitle);
        }
        it->synced_seconds = sqlite3_column_int64(stmt, 5);
        it->comp_main = sqlite3_column_int64(stmt, 6);
        it->comp_plus = sqlite3_column_int64(stmt, 7);
        it->comp_100 = sqlite3_column_int64(stmt, 8);
        snprintf(it->list_type, sizeof it->list_type, "%s", (lt && lt[0]) ? lt : "playing");
        it->comp_all = sqlite3_column_int64(stmt, 10);
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if(item_count == 0) {
        sqlite3_close(hltb_db);
        free(items);
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        snprintf(error_message, error_message_size,
                 "No games are selected for HLTB sync. Click '+ Select' on games first.");
        return -1;
    }

    if(sqlite3_open_v2(app_db_path, &app_db,
                       SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                       NULL) != SQLITE_OK) {
        set_db_error(error_message, error_message_size, "Open PS5 app.db failed",
                     app_db);
        if(app_db) {
            sqlite3_close(app_db);
        }
        sqlite3_close(hltb_db);
        free(items);
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }
    sqlite3_busy_timeout(app_db, 5000);

    if(http_ctx_init(&hctx, DEFAULT_USER_AGENT) != 0) {
        sqlite3_close(app_db);
        sqlite3_close(hltb_db);
        free(items);
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        snprintf(error_message, error_message_size,
                 "Failed to initialize PS5 HTTPS client.");
        return -1;
    }

    char last_error[256] = {0};

    for(i = 0; i < item_count; i++) {
        selected_game_t *it = &items[i];
        const hltb_account_entry_t *acct = NULL;
        int a_idx;
        char ps5_title[256] = {0};
        long long played_sec = 0;
        long long prev_sec = it->synced_seconds < 0 ? 0 : it->synced_seconds;
        long long new_sub_id = it->hltb_submission_id;
        hltb_lists_t lists;
        char sub_err[256] = {0};
        int is_ps5 = strncmp(it->title_id, "PPSA", 4) == 0;

        for(a_idx = 0; a_idx < num_accounts; a_idx++) {
            if(strcmp(accounts[a_idx].ps5_user_id, it->user_id) == 0) {
                acct = &accounts[a_idx];
                break;
            }
        }
        if(!acct && num_accounts == 1) {
            acct = &accounts[0];
        }
        if(!acct || !acct->session_cookie[0] || acct->hltb_user_id <= 0) {
            klog_printf("[PS5-HLTB] Profile %s has no active HLTB login; skipping %s\n",
                        it->user_id, it->title_id);
            continue;
        }

        if(read_ps5_game_info(app_db, it->user_id, it->title_id, ps5_title,
                              sizeof ps5_title, &played_sec) != 0) {
            continue;
        }
        if(played_sec < 0) {
            played_sec = 0;
        }

        if(!force_all && it->synced_seconds >= 0 && played_sec == it->synced_seconds &&
           it->hltb_game_id > 0 && it->hltb_submission_id > 0) {
            continue;
        }

        const char *search_name = NULL;
        if(it->hltb_title[0] && strncmp(it->hltb_title, "CUSA", 4) != 0 &&
           strncmp(it->hltb_title, "PPSA", 4) != 0) {
            search_name = it->hltb_title;
        } else if(ps5_title[0] && strncmp(ps5_title, "CUSA", 4) != 0 &&
                  strncmp(ps5_title, "PPSA", 4) != 0) {
            search_name = ps5_title;
        } else if(it->hltb_title[0]) {
            search_name = it->hltb_title;
        } else {
            search_name = ps5_title[0] ? ps5_title : it->title_id;
        }

        if(it->hltb_game_id <= 0) {
            if(hltb_api_search_game(&hctx, search_name, &it->hltb_game_id,
                                    it->hltb_title, sizeof it->hltb_title,
                                    &it->comp_main, &it->comp_plus,
                                    &it->comp_100, &it->comp_all) != 0 ||
               it->hltb_game_id <= 0) {
                snprintf(sub_err, sizeof sub_err,
                         "Could not auto-match '%s' on HLTB. Enter custom HLTB ID in details.",
                         search_name);
                snprintf(last_error, sizeof last_error, "%s", sub_err);
                klog_printf("[PS5-HLTB] %s\n", sub_err);
                continue;
            }
        } else if(it->comp_main == 0 && it->comp_all == 0) {
            (void)hltb_api_fetch_game_by_id(&hctx, it->hltb_game_id,
                                            it->hltb_title[0] ? NULL : it->hltb_title,
                                            it->hltb_title[0] ? 0 : sizeof it->hltb_title,
                                            &it->comp_main, &it->comp_plus,
                                            &it->comp_100, &it->comp_all);
        }
        if(!it->hltb_title[0]) {
            snprintf(it->hltb_title, sizeof it->hltb_title, "%s", search_name);
        }

        hltb_api_find_user_submission(&hctx, acct->session_cookie, acct->hltb_user_id,
                                      it->hltb_game_id, &new_sub_id, &prev_sec,
                                      &lists, &it->comp_main, &it->comp_plus,
                                      &it->comp_100, &it->comp_all);
        if(new_sub_id > 1) {
            it->hltb_submission_id = new_sub_id;
        }

        /* Apply user's selected list type ("playing", "backlog", or "completed") */
        memset(&lists, 0, sizeof lists);
        if(strcmp(it->list_type, "backlog") == 0) {
            lists.backlog = 1;
        } else if(strcmp(it->list_type, "completed") == 0) {
            lists.completed = 1;
        } else {
            lists.playing = 1;
        }

        klog_printf("[PS5-HLTB] Submitting %s ('%s', HLTB ID %lld, subId %lld, list %s, profile %s): playtime %llds (prev %llds)\n",
                    it->title_id, it->hltb_title, (long long)it->hltb_game_id,
                    (long long)it->hltb_submission_id, it->list_type, it->user_id, played_sec, prev_sec);

        if(hltb_api_submit_progress(&hctx, acct->session_cookie, acct->hltb_user_id,
                                    it->hltb_game_id, it->hltb_submission_id,
                                    it->hltb_title, is_ps5, played_sec, prev_sec,
                                    &lists, &new_sub_id, sub_err,
                                    sizeof sub_err) == 0) {
            sqlite3_stmt *up = NULL;
            if(new_sub_id > 1) {
                it->hltb_submission_id = new_sub_id;
            }
            it->synced_seconds = played_sec;

            if(sqlite3_prepare_v2(
                   hltb_db,
                   "UPDATE hltb_games SET hltb_game_id = ?1, hltb_submission_id = ?2, "
                   "hltb_title = ?3, synced_seconds = ?4, comp_main = ?5, "
                   "comp_plus = ?6, comp_100 = ?7, comp_all = ?8, "
                   "updated_at = datetime('now', 'localtime') "
                   "WHERE user_id = ?9 AND title_id = ?10",
                   -1, &up, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(up, 1, (sqlite3_int64)it->hltb_game_id);
                sqlite3_bind_int64(up, 2, (sqlite3_int64)it->hltb_submission_id);
                sqlite3_bind_text(up, 3, it->hltb_title, -1, SQLITE_STATIC);
                sqlite3_bind_int64(up, 4, (sqlite3_int64)it->synced_seconds);
                sqlite3_bind_int64(up, 5, (sqlite3_int64)it->comp_main);
                sqlite3_bind_int64(up, 6, (sqlite3_int64)it->comp_plus);
                sqlite3_bind_int64(up, 7, (sqlite3_int64)it->comp_100);
                sqlite3_bind_int64(up, 8, (sqlite3_int64)it->comp_all);
                sqlite3_bind_text(up, 9, it->user_id, -1, SQLITE_STATIC);
                sqlite3_bind_text(up, 10, it->title_id, -1, SQLITE_STATIC);
                (void)sqlite3_step(up);
            }
            sqlite3_finalize(up);
            up = NULL;

            /* Also update last_sync for this PS5 profile */
            if(sqlite3_prepare_v2(
                   hltb_db,
                   "UPDATE hltb_accounts SET last_sync = datetime('now', 'localtime') WHERE ps5_user_id = ?1",
                   -1, &up, NULL) == SQLITE_OK) {
                sqlite3_bind_text(up, 1, it->user_id, -1, SQLITE_STATIC);
                (void)sqlite3_step(up);
            }
            sqlite3_finalize(up);
            synced_count++;
        } else if(sub_err[0]) {
            snprintf(last_error, sizeof last_error, "%s", sub_err);
            if(error_message && error_message_size > 0) {
                snprintf(error_message, error_message_size, "%s", sub_err);
            }
        }
    }

    if(synced_count == 0 && item_count > 0) {
        if(last_error[0] && error_message && error_message_size > 0 && !error_message[0]) {
            snprintf(error_message, error_message_size, "%s", last_error);
        }
    }

    if(synced_count > 0) {
        (void)sqlite3_exec(
            hltb_db,
            "UPDATE hltb_config SET last_sync = datetime('now', 'localtime') WHERE id = 1;",
            NULL, NULL, NULL);
    }

    http_ctx_fini(&hctx);
    sqlite3_close(app_db);
    sqlite3_close(hltb_db);
    free(items);
    pthread_mutex_unlock(&g_hltb_mutex);

    g_sync_active = 0;
    g_sync_step = 0;
    g_last_synced_count = synced_count;
    if(synced_count > 0) {
        snprintf(g_sync_msg, sizeof g_sync_msg, "Synced %d game(s) to HowLongToBeat", synced_count);
    } else {
        snprintf(g_sync_msg, sizeof g_sync_msg, "Selected games up to date");
    }

    if(synced_count_out) {
        *synced_count_out = synced_count;
    }

    return (synced_count == 0 && item_count > 0 && last_error[0]) ? -1 : 0;
}

/* ---- Library Auto-Discovery & Background Auto-Sync Thread --------------- */

int hltb_auto_match_library(const char *app_db_path, const char *hltb_db_path) {
    sqlite3 *app_db = NULL;
    sqlite3 *hltb_db = NULL;
    sqlite3_stmt *stmt = NULL;
    char default_user[24] = {0};
    typedef struct {
        char title_id[16];
        char title_name[256];
        long long hltb_id;
        long long comp_main;
        long long comp_all;
    } cand_t;
    cand_t *cands = NULL;
    size_t cand_count = 0, cand_cap = 0;
    http_ctx_t hctx;
    int hctx_inited = 0;
    size_t i;

    g_sync_active = 1;
    g_sync_step = 2;
    snprintf(g_sync_msg, sizeof g_sync_msg, "Auto-matching games with HowLongToBeat...");

    pthread_mutex_lock(&g_hltb_mutex);

    if(sqlite3_open_v2(hltb_db_path, &hltb_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if(hltb_db) sqlite3_close(hltb_db);
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }
    sqlite3_busy_timeout(hltb_db, 5000);

    /* Get default user */
    if(sqlite3_prepare_v2(hltb_db, "SELECT ps5_user_id FROM hltb_config WHERE id = 1", -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW) {
        const char *u = (const char *)sqlite3_column_text(stmt, 0);
        if(u && u[0]) {
            snprintf(default_user, sizeof default_user, "%s", u);
        }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    if(sqlite3_open_v2(app_db_path, &app_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if(app_db) sqlite3_close(app_db);
        sqlite3_close(hltb_db);
        g_sync_active = 0;
        g_sync_step = 0;
        pthread_mutex_unlock(&g_hltb_mutex);
        return -1;
    }
    sqlite3_busy_timeout(app_db, 5000);

    /* If no default user, pick first user from app_db */
    if(!default_user[0]) {
        if(sqlite3_prepare_v2(app_db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%'", -1, &stmt, NULL) == SQLITE_OK &&
           sqlite3_step(stmt) == SQLITE_ROW) {
            const char *tbl = (const char *)sqlite3_column_text(stmt, 0);
            if(tbl && strlen(tbl) > 13) {
                snprintf(default_user, sizeof default_user, "%s", tbl + 13);
            }
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    /* Query non-homebrew games from tbl_contentinfo */
    if(sqlite3_prepare_v2(app_db,
                          "SELECT DISTINCT TRIM(titleId), TRIM(titleName) FROM tbl_contentinfo "
                          "WHERE titleId NOT LIKE 'PPSA9%' AND titleId NOT LIKE 'CUSA9%' "
                          "AND titleName IS NOT NULL AND titleName != ''",
                          -1, &stmt, NULL) == SQLITE_OK) {
        while(sqlite3_step(stmt) == SQLITE_ROW) {
            const char *tid = (const char *)sqlite3_column_text(stmt, 0);
            const char *tname = (const char *)sqlite3_column_text(stmt, 1);
            if(!tid || !tid[0] || !tname || !tname[0]) continue;

            if(cand_count == cand_cap) {
                size_t ncap = cand_cap ? cand_cap * 2 : 32;
                cand_t *n = (cand_t *)realloc(cands, ncap * sizeof *cands);
                if(!n) break;
                cands = n;
                cand_cap = ncap;
            }
            memset(&cands[cand_count], 0, sizeof cands[cand_count]);
            snprintf(cands[cand_count].title_id, sizeof cands[cand_count].title_id, "%s", tid);
            snprintf(cands[cand_count].title_name, sizeof cands[cand_count].title_name, "%s", tname);
            cand_count++;
        }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    sqlite3_close(app_db);
    app_db = NULL;

    /* For each game, check if already in hltb_db with times */
    for(i = 0; i < cand_count; i++) {
        sqlite3_stmt *chk = NULL;
        if(sqlite3_prepare_v2(hltb_db,
                              "SELECT hltb_game_id, comp_main, comp_all FROM hltb_games WHERE title_id = ?1",
                              -1, &chk, NULL) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, cands[i].title_id, -1, SQLITE_STATIC);
            if(sqlite3_step(chk) == SQLITE_ROW) {
                cands[i].hltb_id = sqlite3_column_int64(chk, 0);
                cands[i].comp_main = sqlite3_column_int64(chk, 1);
                cands[i].comp_all = sqlite3_column_int64(chk, 2);
            }
        }
        sqlite3_finalize(chk);

        /* If already has times, skip */
        if(cands[i].comp_main > 0 || cands[i].comp_all > 0) {
            continue;
        }

        if(!hctx_inited) {
            if(http_ctx_init(&hctx, DEFAULT_USER_AGENT) != 0) {
                break;
            }
            hctx_inited = 1;
        }

        long long gid = cands[i].hltb_id;
        char matched[256] = {0};
        long long c_main = 0, c_plus = 0, c_100 = 0, c_all = 0;

        if(gid > 0) {
            (void)hltb_api_fetch_game_by_id(&hctx, gid, matched, sizeof matched,
                                            &c_main, &c_plus, &c_100, &c_all);
        } else {
            (void)hltb_api_search_game(&hctx, cands[i].title_name, &gid, matched,
                                       sizeof matched, &c_main, &c_plus, &c_100, &c_all);
        }

        if(gid > 0 && (c_main > 0 || c_all > 0)) {
            sqlite3_stmt *ins = NULL;
            if(sqlite3_prepare_v2(
                   hltb_db,
                   "INSERT INTO hltb_games (user_id, title_id, selected, hltb_title, "
                   "hltb_game_id, list_type, synced_seconds, comp_main, comp_plus, comp_100, comp_all, updated_at) "
                   "VALUES (?1, ?2, 0, ?3, ?4, 'playing', -1, ?5, ?6, ?7, ?8, datetime('now', 'localtime')) "
                   "ON CONFLICT(user_id, title_id) DO UPDATE SET "
                   "hltb_title = CASE WHEN excluded.hltb_title != '' THEN excluded.hltb_title ELSE hltb_title END, "
                   "hltb_game_id = CASE WHEN excluded.hltb_game_id > 0 THEN excluded.hltb_game_id ELSE hltb_game_id END, "
                   "comp_main = excluded.comp_main, comp_plus = excluded.comp_plus, "
                   "comp_100 = excluded.comp_100, comp_all = excluded.comp_all, "
                   "updated_at = datetime('now', 'localtime')",
                   -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, default_user[0] ? default_user : "0", -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 2, cands[i].title_id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 3, matched[0] ? matched : cands[i].title_name, -1, SQLITE_STATIC);
                sqlite3_bind_int64(ins, 4, gid);
                sqlite3_bind_int64(ins, 5, c_main);
                sqlite3_bind_int64(ins, 6, c_plus);
                sqlite3_bind_int64(ins, 7, c_100);
                sqlite3_bind_int64(ins, 8, c_all);
                (void)sqlite3_step(ins);
            }
            sqlite3_finalize(ins);
            klog_printf("[PS5-HLTB] Auto-matched %s ('%s') -> HLTB #%lld (%llds)\n",
                        cands[i].title_id, cands[i].title_name, gid, c_main);
        }
    }

    if(hctx_inited) {
        http_ctx_fini(&hctx);
    }
    free(cands);
    sqlite3_close(hltb_db);
    pthread_mutex_unlock(&g_hltb_mutex);

    g_sync_active = 0;
    g_sync_step = 0;
    snprintf(g_sync_msg, sizeof g_sync_msg, "Library auto-match complete");
    return 0;
}

typedef struct {
    char app_db_path[256];
    char hltb_db_path[256];
} auto_sync_args_t;

static int is_auto_sync_enabled(const char *hltb_db_path) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int enabled = 0;

    if(sqlite3_open_v2(hltb_db_path, &db,
                       SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                       NULL) != SQLITE_OK) {
        if(db) {
            sqlite3_close(db);
        }
        return 0;
    }
    sqlite3_busy_timeout(db, 3000);
    if(sqlite3_prepare_v2(
           db,
           "SELECT 1 FROM hltb_accounts WHERE auto_sync = 1 AND hltb_user_id > 0 AND session_cookie != '' "
           "UNION "
           "SELECT 1 FROM hltb_config WHERE id = 1 AND auto_sync = 1 AND hltb_user_id > 0 AND session_cookie != '' "
           "LIMIT 1",
           -1, &stmt, NULL) == SQLITE_OK &&
       sqlite3_step(stmt) == SQLITE_ROW) {
        enabled = 1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return enabled;
}

static void *auto_sync_worker(void *arg) {
    auto_sync_args_t *args = (auto_sync_args_t *)arg;
    char err[256];
    int synced = 0;

    /* Give the network stack and HTTP server a moment to settle on startup */
    sleep(2);

    /* 1. Fast-path: Sync selected games IMMEDIATELY on launch if enabled */
    if(is_auto_sync_enabled(args->hltb_db_path)) {
        err[0] = '\0';
        (void)hltb_sync_selected(args->app_db_path, args->hltb_db_path, 0,
                                 &synced, err, sizeof err);
    }

    /* 2. Discover and fetch HLTB times for all unmatched library games in background */
    (void)hltb_auto_match_library(args->app_db_path, args->hltb_db_path);

    /* 3. If any newly matched games were marked selected, sync them now */
    if(is_auto_sync_enabled(args->hltb_db_path)) {
        err[0] = '\0';
        (void)hltb_sync_selected(args->app_db_path, args->hltb_db_path, 0,
                                 &synced, err, sizeof err);
    }

    for(;;) {
        sleep(180);
        if(is_auto_sync_enabled(args->hltb_db_path)) {
            err[0] = '\0';
            (void)hltb_sync_selected(args->app_db_path, args->hltb_db_path, 0,
                                     &synced, err, sizeof err);
        }
    }

    free(args);
    return NULL;
}

void hltb_start_auto_sync_thread(const char *app_db_path,
                                 const char *hltb_db_path) {
    pthread_t thr;
    auto_sync_args_t *args = (auto_sync_args_t *)calloc(1, sizeof *args);

    if(!args) {
        return;
    }
    snprintf(args->app_db_path, sizeof args->app_db_path, "%s", app_db_path);
    snprintf(args->hltb_db_path, sizeof args->hltb_db_path, "%s", hltb_db_path);

    if(pthread_create(&thr, NULL, auto_sync_worker, args) == 0) {
        pthread_detach(thr);
    } else {
        free(args);
    }
}
