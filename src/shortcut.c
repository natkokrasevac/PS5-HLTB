#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "shortcut.h"

/* PS5 AppInstUtil symbols */
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *arg);
int sceAppInstUtilAppUnInstall(const char *title_id);

#define APP_DIR "/user/app"
#define HLTB_APP_DIR "/user/app/" SHORTCUT_TITLE_ID
#define HLTB_SYS_DIR "/user/app/" SHORTCUT_TITLE_ID "/sce_sys"
#define HLTB_PARAM_JSON HLTB_SYS_DIR "/param.json"
#define HLTB_ICON_PNG   HLTB_SYS_DIR "/icon0.png"

/* Ensure process has root vnode and ucred privileges to write under /user/app */
static void ensure_root_privileges(void) {
    pid_t pid = getpid();
    intptr_t rootvnode = kernel_get_root_vnode();
    if(rootvnode) {
        kernel_set_proc_rootdir(pid, rootvnode);
        kernel_set_proc_jaildir(pid, 0);
    }
    kernel_set_ucred_uid(pid, 0);
    kernel_set_ucred_ruid(pid, 0);
    kernel_set_ucred_svuid(pid, 0);
    kernel_set_ucred_rgid(pid, 0);
    kernel_set_ucred_svgid(pid, 0);
}

int shortcut_is_installed(void) {
    ensure_root_privileges();
    struct stat st;
    if(stat(HLTB_PARAM_JSON, &st) == 0 && S_ISREG(st.st_mode)) {
        return 1;
    }
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0) {
        return -1;
    }
    while(size > 0) {
        ssize_t n = write(fd, data, size);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        data += n;
        size -= (size_t)n;
    }
    return close(fd);
}

int shortcut_install(int port, const void *icon_data, size_t icon_size,
                     char *err_buf, size_t err_size) {
    ensure_root_privileges();

    mkdir(APP_DIR, 0755);
    mkdir(HLTB_APP_DIR, 0755);
    mkdir(HLTB_SYS_DIR, 0755);

    /* Generate param.json: applicationCategoryType 65536 registers the tile under the Media tab */
    char param_json[512];
    int len = snprintf(param_json, sizeof param_json,
        "{\n"
        "    \"titleId\": \"" SHORTCUT_TITLE_ID "\",\n"
        "    \"applicationCategoryType\": 65536,\n"
        "    \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
        "    \"localizedParameters\": {\n"
        "        \"defaultLanguage\": \"en-US\",\n"
        "        \"en-US\": {\n"
        "            \"titleName\": \"PS5-HLTB\"\n"
        "        }\n"
        "    }\n"
        "}\n",
        port > 0 ? port : 7788
    );

    if(write_file(HLTB_PARAM_JSON, (const uint8_t *)param_json, (size_t)len) != 0) {
        klog_printf("[PS5-HLTB] Failed to write %s: %s\n", HLTB_PARAM_JSON, strerror(errno));
        if(err_buf && err_size > 0) {
            snprintf(err_buf, err_size, "Failed to write param.json: %s", strerror(errno));
        }
        return -1;
    }

    if(icon_data && icon_size > 0) {
        if(write_file(HLTB_ICON_PNG, (const uint8_t *)icon_data, icon_size) != 0) {
            klog_printf("[PS5-HLTB] Failed to write %s: %s\n", HLTB_ICON_PNG, strerror(errno));
            if(err_buf && err_size > 0) {
                snprintf(err_buf, err_size, "Failed to write icon0.png: %s", strerror(errno));
            }
            return -1;
        }
    }

    int err = sceAppInstUtilInitialize();
    if(err) {
        klog_printf("[PS5-HLTB] sceAppInstUtilInitialize failed: 0x%08x\n", err);
        if(err_buf && err_size > 0) {
            snprintf(err_buf, err_size, "sceAppInstUtilInitialize failed: 0x%08x", err);
        }
        return -1;
    }

    uint32_t handle = 0;
    int (*install_title_dir)(const char *, const char *, void *) = NULL;
    if(!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) && handle) {
        install_title_dir = (void *)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
    }

    if(install_title_dir) {
        err = install_title_dir(SHORTCUT_TITLE_ID, APP_DIR "/", 0);
        klog_printf("[PS5-HLTB] install_title_dir returned: 0x%08x\n", err);
    } else {
        err = sceAppInstUtilAppInstallAll(0);
        klog_printf("[PS5-HLTB] sceAppInstUtilAppInstallAll returned: 0x%08x\n", err);
    }

    sceAppInstUtilTerminate();

    if(err) {
        klog_printf("[PS5-HLTB] Registering shortcut failed: 0x%08x\n", err);
        if(err_buf && err_size > 0) {
            snprintf(err_buf, err_size, "Registering shortcut failed: 0x%08x", err);
        }
        return -1;
    }

    klog_printf("[PS5-HLTB] Shortcut %s registered in Media tab successfully\n", SHORTCUT_TITLE_ID);
    return 0;
}

int shortcut_uninstall(char *err_buf, size_t err_size) {
    ensure_root_privileges();

    int err = sceAppInstUtilInitialize();
    if(err) {
        klog_printf("[PS5-HLTB] sceAppInstUtilInitialize failed: 0x%08x\n", err);
        if(err_buf && err_size > 0) {
            snprintf(err_buf, err_size, "sceAppInstUtilInitialize failed: 0x%08x", err);
        }
        return -1;
    }

    err = sceAppInstUtilAppUnInstall(SHORTCUT_TITLE_ID);
    klog_printf("[PS5-HLTB] sceAppInstUtilAppUnInstall returned: 0x%08x\n", err);
    sceAppInstUtilTerminate();

    unlink(HLTB_PARAM_JSON);
    unlink(HLTB_ICON_PNG);
    rmdir(HLTB_SYS_DIR);
    rmdir(HLTB_APP_DIR);

    if(err) {
        klog_printf("[PS5-HLTB] Removing shortcut failed: 0x%08x\n", err);
        if(err_buf && err_size > 0) {
            snprintf(err_buf, err_size, "Removing shortcut failed: 0x%08x", err);
        }
        return -1;
    }

    klog_printf("[PS5-HLTB] Shortcut %s uninstalled successfully\n", SHORTCUT_TITLE_ID);
    return 0;
}

