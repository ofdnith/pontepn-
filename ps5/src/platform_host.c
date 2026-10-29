/* Plataforma de teste (Linux/macOS): o "console" é uma pasta.
 *
 *   $PONTE_ROOT/data/ponte/   configuração (config.json)
 *   $PONTE_ROOT/internal/     armazenamento "internal" (homebrew/, ponte/tmp/)
 *   $PONTE_ROOT/usb0/         armazenamento "usb0", se a pasta existir
 *   $PONTE_ROOT/installed/    pacotes "instalados" (o instalador falso baixa a URL para cá)
 *   $PONTE_ROOT/installed.json  lista extra de instalados, no formato do console/state
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "http.h"
#include "platform.h"
#include "platform_common.h"
#include "util.h"

static char g_root[512];
static char g_data[600];

int plat_init(void) {
    const char *r = getenv("PONTE_ROOT");
    snprintf(g_root, sizeof(g_root), "%s", r && *r ? r : "./ponte-host");
    snprintf(g_data, sizeof(g_data), "%s/data/ponte", g_root);
    char internal[600];
    snprintf(internal, sizeof(internal), "%s/internal", g_root);
    if (mkdirs(internal) != 0 || mkdirs(g_data) != 0) {
        fprintf(stderr, "não foi possível criar %s\n", g_root);
        return -1;
    }
    return 0;
}

const char *plat_data_dir(void) { return g_data; }

void plat_notify(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[aviso] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

int plat_storages(storage_t *out, int max) {
    int n = 0;
    char p[600];
    snprintf(p, sizeof(p), "%s/internal", g_root);
    if (n < max && storage_fill(&out[n], "internal", "Interno", p) == 0) n++;
    snprintf(p, sizeof(p), "%s/usb0", g_root);
    if (n < max && is_dir(p) && storage_fill(&out[n], "usb0", "USB 1", p) == 0) n++;
    return n;
}

cJSON *plat_installed(const storage_t *storages, int n) {
    cJSON *arr = cJSON_CreateArray();
    scan_homebrew(arr, storages, n);
    char p[600];
    snprintf(p, sizeof(p), "%s/installed.json", g_root);
    char *text = read_file(p, 1 << 20, NULL);
    if (text) {
        cJSON *extra = cJSON_Parse(text);
        free(text);
        cJSON *e;
        cJSON_ArrayForEach(e, extra) cJSON_AddItemToArray(arr, cJSON_Duplicate(e, 1));
        cJSON_Delete(extra);
    }
    return arr;
}

const char *plat_firmware(void) {
    const char *fw = getenv("PONTE_FIRMWARE");
    return fw && *fw ? fw : "13.60";
}

const char *plat_console_name(void) { return "PS5 (teste)"; }

/* Instalador falso: baixa a URL inteira, como o instalador do PS5 faria. */
int plat_install_pkg(const char *url, const char *name, char *err, size_t errlen) {
    (void)name;
    char host[128];
    int port = 80;
    const char *p = strncmp(url, "http://", 7) == 0 ? url + 7 : NULL;
    if (!p) {
        snprintf(err, errlen, "URL inválida");
        return -1;
    }
    const char *slash = strchr(p, '/');
    const char *colon = memchr(p, ':', slash ? (size_t)(slash - p) : strlen(p));
    size_t hl = (size_t)((colon ? colon : slash) - p);
    if (!slash || hl >= sizeof(host)) {
        snprintf(err, errlen, "URL inválida");
        return -1;
    }
    memcpy(host, p, hl);
    host[hl] = '\0';
    if (colon) port = atoi(colon + 1);
    /* nome do arquivo: último segmento antes da query */
    char fname[256];
    const char *q = strchr(slash, '?');
    const char *last = slash;
    for (const char *s = slash; *s && (!q || s < q); s++)
        if (*s == '/') last = s + 1;
    size_t fl = (size_t)((q ? q : last + strlen(last)) - last);
    if (fl == 0 || fl >= sizeof(fname)) fl = 0;
    memcpy(fname, last, fl);
    fname[fl] = '\0';
    if (!fname[0]) snprintf(fname, sizeof(fname), "pacote.pkg");

    http_resp_t r;
    if (http_open(host, port, "GET", slash, NULL, NULL, 0, &r, 30) != 0) {
        snprintf(err, errlen, "sem conexão com %s:%d", host, port);
        return -1;
    }
    if (r.status != 200) {
        snprintf(err, errlen, "o servidor respondeu %d", r.status);
        http_close(&r);
        return -1;
    }
    char dir[700], dest[1024];
    snprintf(dir, sizeof(dir), "%s/installed", g_root);
    mkdirs(dir);
    path_join(dest, sizeof(dest), dir, fname);
    FILE *f = fopen(dest, "wb");
    if (!f) {
        http_close(&r);
        snprintf(err, errlen, "não foi possível gravar");
        return -1;
    }
    char buf[65536];
    ssize_t n;
    while ((n = http_read(&r, buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    http_close(&r);
    if (n < 0) {
        snprintf(err, errlen, "download interrompido");
        return -1;
    }
    return 0;
}

/* No teste não há sistema para registrar: os arquivos gravados bastam. */
int plat_register_tile(const char *dir, const char *title_id, int files_changed, char *err, size_t errlen) {
    (void)dir, (void)title_id, (void)err, (void)errlen;
    return files_changed ? 1 : 0;
}
