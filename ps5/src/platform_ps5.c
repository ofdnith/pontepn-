/* Plataforma do PS5 (SDK de payloads da comunidade). */
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include "platform.h"
#include "platform_common.h"
#include "util.h"

/* ---------- avisos na tela ---------- */

typedef struct {
    char reserved[45];
    char message[3075];
} notify_request_t;

typedef int (*notify_fn)(int device, notify_request_t *req, size_t size, int blocking);

/* Log em /data/ponte/ponte.log (além do stderr), para dar para ver o que houve no console. */
static FILE *g_logf;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_line(const char *text) {
    pthread_mutex_lock(&g_log_lock);
    fprintf(stderr, "[ponte] %s\n", text);
    if (g_logf) {
        fprintf(g_logf, "%s\n", text);
        fflush(g_logf);
    }
    pthread_mutex_unlock(&g_log_lock);
}

void plat_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(buf);
}

void plat_notify(const char *fmt, ...) {
    /* resolvido na hora: ligar o símbolo na compilação faria o carregador recusar o ELF se ele
     * não existir nesta versão do sistema */
    static notify_fn send;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        send = (notify_fn)dlsym(RTLD_DEFAULT, "sceKernelSendNotificationRequest");
        if (!send) log_line("sceKernelSendNotificationRequest não encontrado; avisos só no log");
    }
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    if (send) send(0, &req, sizeof(req), 0);
    log_line(req.message);
}

static void on_crash(int sig) {
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "Ponte: erro fatal (sinal %d)\n", sig);
    if (g_logf) {
        fwrite(msg, 1, (size_t)n, g_logf);
        fflush(g_logf);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

int plat_init(void) {
    mkdirs("/data/ponte");
    g_logf = fopen("/data/ponte/ponte.log", "w");
    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGILL, on_crash);
    signal(SIGABRT, on_crash);
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

const char *plat_data_dir(void) { return "/data/ponte"; }

const char *plat_console_name(void) { return "PS5"; }

/* ---------- firmware ---------- */

static char g_fw[16];

/* "13.60" a partir de kern.version (o primeiro número no formato NN.NN). */
const char *plat_firmware(void) {
    if (g_fw[0]) return g_fw;
    char buf[256];
    size_t sz = sizeof(buf) - 1;
    if (sysctlbyname("kern.version", buf, &sz, NULL, 0) != 0) return NULL;
    buf[sz] = '\0';
    for (char *p = buf; *p; p++) {
        if (*p < '0' || *p > '9' || (p > buf && p[-1] >= '0' && p[-1] <= '9')) continue;
        char *q = p;
        int major = 0;
        while (*q >= '0' && *q <= '9') major = major * 10 + (*q++ - '0');
        if (*q == '.' && q[1] >= '0' && q[1] <= '9' && q[2] >= '0' && q[2] <= '9' && (q[3] < '0' || q[3] > '9') &&
            major > 0 && major < 100) {
            snprintf(g_fw, sizeof(g_fw), "%d.%c%c", major, q[1], q[2]);
            return g_fw;
        }
    }
    return NULL;
}

/* ---------- armazenamentos ---------- */

static int is_mount_point(const char *path) {
    struct statfs fs;
    if (statfs(path, &fs) != 0) return 0;
    return strcmp(fs.f_mntonname, path) == 0;
}

int plat_storages(storage_t *out, int max) {
    int n = 0;
    if (n < max && storage_fill(&out[n], "internal", "Interno", "/data") == 0) n++;
    static const struct {
        const char *id, *label, *root;
    } others[] = {
        {"ext0", "M.2 / estendido", "/mnt/ext0"}, {"ext1", "Estendido 2", "/mnt/ext1"},
        {"usb0", "USB 1", "/mnt/usb0"},           {"usb1", "USB 2", "/mnt/usb1"},
        {"usb2", "USB 3", "/mnt/usb2"},           {"usb3", "USB 4", "/mnt/usb3"},
        {"usb4", "USB 5", "/mnt/usb4"},           {"usb5", "USB 6", "/mnt/usb5"},
        {"usb6", "USB 7", "/mnt/usb6"},           {"usb7", "USB 8", "/mnt/usb7"},
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]) && n < max; i++) {
        if (is_mount_point(others[i].root) && storage_fill(&out[n], others[i].id, others[i].label, others[i].root) == 0)
            n++;
    }
    return n;
}

/* ---------- o que já está instalado ---------- */

/* Valor de texto do param.sfo (formato em reference/ponte_ref/formats/sfo.py). */
static int sfo_get(const unsigned char *d, size_t len, const char *key, char *out, size_t cap) {
    if (len < 20 || memcmp(d, "\0PSF", 4) != 0) return 0;
    uint32_t key_start = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
    uint32_t data_start = d[12] | d[13] << 8 | d[14] << 16 | (uint32_t)d[15] << 24;
    uint32_t count = d[16] | d[17] << 8 | d[18] << 16 | (uint32_t)d[19] << 24;
    if (count > 1024 || 20 + (size_t)count * 16 > len) return 0;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *e = d + 20 + i * 16;
        uint32_t koff = e[0] | e[1] << 8;
        uint32_t fmt = e[2] | e[3] << 8;
        uint32_t vlen = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
        uint32_t doff = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        size_t k = (size_t)key_start + koff;
        if (k >= len || strncmp((const char *)d + k, key, len - k) != 0) continue;
        size_t v = (size_t)data_start + doff;
        if (fmt != 0x0204 && fmt != 0x0004) return 0;
        if (v + vlen > len) return 0;
        size_t n = strnlen((const char *)d + v, vlen);
        if (n >= cap) n = cap - 1;
        memcpy(out, d + v, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}

/* Versão instalada do título: param.json (PS5) ou APP_VER do param.sfo (PS4). */
static void installed_version(const char *meta_dir, char *out, size_t cap) {
    out[0] = '\0';
    char path[512];
    snprintf(path, sizeof(path), "%s/param.json", meta_dir);
    char *text = read_file(path, 1 << 20, NULL);
    if (text) {
        cJSON *j = cJSON_Parse(text);
        free(text);
        cJSON *v = j ? cJSON_GetObjectItem(j, "contentVersion") : NULL;
        if (cJSON_IsString(v)) snprintf(out, cap, "%s", v->valuestring);
        cJSON_Delete(j);
        if (out[0]) return;
    }
    snprintf(path, sizeof(path), "%s/param.sfo", meta_dir);
    size_t len = 0;
    char *sfo = read_file(path, 1 << 20, &len);
    if (sfo) {
        if (!sfo_get((unsigned char *)sfo, len, "APP_VER", out, cap)) sfo_get((unsigned char *)sfo, len, "VERSION", out, cap);
        free(sfo);
    }
}

static void scan_apps(cJSON *arr, const char *base, const char *storage) {
    char apps[256];
    snprintf(apps, sizeof(apps), "%s/user/app", base);
    DIR *d = opendir(apps);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char tid[10];
        if (strlen(e->d_name) != 9 || !find_ps_title_id(e->d_name, tid)) continue;
        char meta[512], ver[64];
        snprintf(meta, sizeof(meta), "/user/appmeta/%s", tid);
        installed_version(meta, ver, sizeof(ver));
        if (!ver[0]) {
            snprintf(meta, sizeof(meta), "%s/%s/sce_sys", apps, tid);
            installed_version(meta, ver, sizeof(ver));
        }
        add_installed(arr, tid, ver[0] ? ver : NULL, "base", storage);
        /* atualização instalada: a versão em appmeta já é a da atualização */
        char patch[512];
        snprintf(patch, sizeof(patch), "%s/user/patch/%s", base, tid);
        if (is_dir(patch)) add_installed(arr, tid, ver[0] ? ver : NULL, "update", storage);
        /* DLC: não dá para saber a versão de cada um; marca o título */
        char addcont[512];
        snprintf(addcont, sizeof(addcont), "%s/user/addcont/%s", base, tid);
        if (is_dir(addcont)) add_installed(arr, tid, NULL, "dlc", storage);
    }
    closedir(d);
}

cJSON *plat_installed(const storage_t *storages, int n) {
    cJSON *arr = cJSON_CreateArray();
    scan_apps(arr, "", "internal");
    for (int i = 0; i < n; i++)
        if (strncmp(storages[i].id, "ext", 3) == 0) scan_apps(arr, storages[i].root, storages[i].id);
    scan_homebrew(arr, storages, n);
    return arr;
}

/* ---------- instalação de pacotes ---------- */

/* ABI do sceAppInstUtil (a mesma usada pela comunidade; ver ps5upload/payload/include/sceAppInstUtil.h).
 * O sistema lê 8 bytes depois dos seis ponteiros: precisam ser zero para uma instalação normal. */
typedef struct {
    char content_id[0x30];
    int content_type;
    int content_platform;
} SceAppInstallPkgInfo;

typedef struct {
    const char *uri;
    const char *ex_uri;
    const char *playgo_scenario_id;
    const char *content_id;
    const char *content_name;
    const char *icon_url;
    uint64_t unknown_30;
    uint64_t reserved[7];
} MetaInfo;

typedef struct {
    char languages[30][8];
    char playgo_scenario_ids[64][3];
    char content_ids[64][0x30];
    unsigned char unknown[6480];
} PlayGoInfo;

typedef int (*inst_init_fn)(void);
typedef int (*inst_pkg_fn)(MetaInfo *meta, SceAppInstallPkgInfo *pkg_info, PlayGoInfo *playgo);
static inst_init_fn p_inst_init;
static inst_pkg_fn p_inst_pkg;

/* Carrega o instalador do sistema só quando precisa. libSceIpmi tem de vir antes de
 * libSceAppInstUtil (ps5-payload-dev/sdk#26). */
static int load_appinstutil(char *err, size_t errlen) {
    if (p_inst_pkg) return 0;
    dlopen("/system/common/lib/libSceIpmi.sprx", RTLD_LAZY | RTLD_GLOBAL);
    void *h = dlopen("/system/common/lib/libSceAppInstUtil.sprx", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        const char *e = dlerror();
        snprintf(err, errlen, "não foi possível carregar libSceAppInstUtil: %s", e ? e : "?");
        return -1;
    }
    p_inst_init = (inst_init_fn)dlsym(h, "sceAppInstUtilInitialize");
    p_inst_pkg = (inst_pkg_fn)dlsym(h, "sceAppInstUtilInstallByPackage");
    if (!p_inst_init || !p_inst_pkg) {
        p_inst_pkg = NULL;
        snprintf(err, errlen, "instalador do sistema sem as funções esperadas");
        return -1;
    }
    return 0;
}

static pthread_mutex_t g_inst_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inst_ready;

static const char *install_hint(unsigned code) {
    switch (code) {
    case 0x80A30002u: return "sem espaço no console";
    case 0x80A3000Cu: return "o jogo está aberto; feche e tente de novo";
    case 0x80A3000Du: return "exige um firmware mais novo";
    case 0x80A30011u: return "versão do aplicativo incompatível";
    case 0x80A30017u: return "instale o jogo base antes do DLC";
    case 0x80A30019u: return "atualização inválida para o jogo instalado";
    default: return "código do sistema";
    }
}

int plat_install_pkg(const char *url, const char *name, char *err, size_t errlen) {
    static MetaInfo meta;        /* grandes demais para a pilha (PlayGoInfo), e o acesso é serializado */
    static SceAppInstallPkgInfo info;
    static PlayGoInfo playgo;
    pthread_mutex_lock(&g_inst_lock);
    if (load_appinstutil(err, errlen) != 0) {
        pthread_mutex_unlock(&g_inst_lock);
        plat_log("%s", err);
        return -1;
    }
    int rc = 0;
    if (!g_inst_ready) {
        rc = p_inst_init();
        if (rc == 0) g_inst_ready = 1;
    }
    if (rc == 0) {
        memset(&meta, 0, sizeof(meta));
        memset(&info, 0, sizeof(info));
        memset(&playgo, 0, sizeof(playgo));
        meta.uri = url;
        meta.ex_uri = "";
        meta.playgo_scenario_id = "";
        meta.content_id = "";
        meta.content_name = name ? name : "";
        meta.icon_url = "";
        rc = p_inst_pkg(&meta, &info, &playgo);
    }
    pthread_mutex_unlock(&g_inst_lock);
    if (rc != 0) {
        snprintf(err, errlen, "0x%08X (%s)", (unsigned)rc, install_hint((unsigned)rc));
        plat_log("InstallByPackage %s -> 0x%08X", url, (unsigned)rc);
        return -1;
    }
    return 0;
}

/* ---------- ícone da tela inicial (tile.h) ---------- */

typedef int (*title_dir_fn)(const char *title_id, const char *base, void *unused);
typedef int (*install_all_fn)(void *unused);

static int copy_file(const char *src, const char *dst) {
    size_t len = 0;
    char *data = read_file(src, 8 << 20, &len);
    if (!data) return -1;
    int rc = write_file_atomic(dst, data, len);
    free(data);
    return rc;
}

/* Copia os arquivos de sce_sys (param.json, imagens) para `dst`. */
static int copy_sce_sys(const char *src, const char *dst) {
    if (mkdirs(dst) != 0) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char a[700], b[700];
        snprintf(a, sizeof(a), "%s/%s", src, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", dst, e->d_name);
        if (copy_file(a, b) != 0) rc = -1;
    }
    closedir(d);
    return rc;
}

static int mounted_at(const char *path) {
    struct statfs fs;
    return statfs(path, &fs) == 0 && strcmp(fs.f_mntonname, path) == 0;
}

static int remount_rw(const char *mnt) {
    struct statfs fs;
    if (statfs(mnt, &fs) != 0 || strcmp(fs.f_mntonname, mnt) != 0) return -1;
    struct iovec iov[6] = {
        {"fstype", 7}, {fs.f_fstypename, strlen(fs.f_fstypename) + 1},
        {"fspath", 7}, {(void *)mnt, strlen(mnt) + 1},
        {"from", 5},   {fs.f_mntfromname, strlen(fs.f_mntfromname) + 1},
    };
    return nmount(iov, 6, MNT_UPDATE);
}

/* Liga `src` em /system_ex/app/<id> (nullfs). Não sobrevive a reinícios: refeito a cada boot. */
static int bind_app(const char *src, const char *title_id, char *err, size_t errlen) {
    char dst[256];
    snprintf(dst, sizeof(dst), "/system_ex/app/%s", title_id);
    char eboot[300];
    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", dst);
    if (mounted_at(dst) && path_exists(eboot)) return 0;
    if (mkdir(dst, 0755) != 0 && errno == EROFS) {
        remount_rw("/system_ex");
        mkdir(dst, 0755);
    }
    if (!is_dir(dst)) {
        snprintf(err, errlen, "não foi possível criar %s: %s", dst, strerror(errno));
        return -1;
    }
    if (mounted_at(dst)) unmount(dst, MNT_FORCE);
    struct iovec iov[6] = {
        {"fstype", 7}, {"nullfs", 7},
        {"from", 5},   {(void *)src, strlen(src) + 1},
        {"fspath", 7}, {dst, strlen(dst) + 1},
    };
    if (nmount(iov, 6, 0) != 0) {
        snprintf(err, errlen, "nullfs em %s: %s", dst, strerror(errno));
        return -1;
    }
    return 0;
}

int plat_register_tile(const char *dir, const char *title_id, int files_changed, char *err, size_t errlen) {
    char src_sys[600], user_app[256], user_sys[300], appmeta[256], icon_src[700], icon_dst[300];
    snprintf(src_sys, sizeof(src_sys), "%s/sce_sys", dir);
    snprintf(user_app, sizeof(user_app), "/user/app/%s", title_id);
    snprintf(user_sys, sizeof(user_sys), "%s/sce_sys", user_app);
    snprintf(appmeta, sizeof(appmeta), "/user/appmeta/%s", title_id);
    char meta_param[300];
    snprintf(meta_param, sizeof(meta_param), "%s/param.json", appmeta);
    int registered_before = path_exists(meta_param) && is_dir(user_sys);

    /* 1. sce_sys em /user/app/<id> e /user/appmeta/<id> (o que a tela inicial lê) */
    if (files_changed || !registered_before) {
        if (copy_sce_sys(src_sys, user_sys) != 0 || copy_sce_sys(src_sys, appmeta) != 0) {
            snprintf(err, errlen, "não foi possível copiar para /user/app (%s)", strerror(errno));
            return -1;
        }
        snprintf(icon_src, sizeof(icon_src), "%s/icon0.png", src_sys);
        snprintf(icon_dst, sizeof(icon_dst), "%s/icon0.png", user_app);
        copy_file(icon_src, icon_dst);
    }

    /* 2. a pasta em /system_ex/app/<id> (refeito a cada boot) */
    if (bind_app(dir, title_id, err, errlen) != 0) return -1;

    /* 3. instalador do sistema, só quando ainda não está registrado ou os arquivos mudaram */
    if (registered_before && !files_changed) return 0;
    pthread_mutex_lock(&g_inst_lock);
    dlopen("/system/common/lib/libSceIpmi.sprx", RTLD_LAZY | RTLD_GLOBAL);
    void *h = dlopen("/system/common/lib/libSceAppInstUtil.sprx", RTLD_LAZY | RTLD_GLOBAL);
    inst_init_fn init = h ? (inst_init_fn)dlsym(h, "sceAppInstUtilInitialize") : NULL;
    title_dir_fn title_dir = h ? (title_dir_fn)dlsym(h, "sceAppInstUtilAppInstallTitleDir") : NULL;
    install_all_fn install_all = h ? (install_all_fn)dlsym(h, "sceAppInstUtilAppInstallAll") : NULL;
    if (!init || (!title_dir && !install_all)) {
        pthread_mutex_unlock(&g_inst_lock);
        snprintf(err, errlen, "instalador do sistema indisponível");
        return -1;
    }
    init();
    /* no 13.x a chamada por título sumiu; "instalar tudo" registra o que está em /user/app */
    int rc = title_dir ? title_dir(title_id, "/user/app/", NULL) : install_all(NULL);
    usleep(200000);
    pthread_mutex_unlock(&g_inst_lock);
    plat_log("registro do ícone %s: %s -> 0x%08X", title_id, title_dir ? "AppInstallTitleDir" : "AppInstallAll",
             (unsigned)rc);
    if (rc != 0 && (unsigned)rc != 0x80990002u) { /* 0x80990002 = já registrado */
        snprintf(err, errlen, "o sistema recusou: 0x%08X", (unsigned)rc);
        return -1;
    }
    return 1;
}
