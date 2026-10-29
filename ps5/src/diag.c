#include "diag.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "http.h"
#include "platform.h"
#include "ponte.h"
#include "util.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---------- progresso ao vivo (um teste por vez) ---------- */

static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int running;
    char kind[8];
    char label[160];
    int64_t done, total;
    uint64_t start_ms;
    rate_t rate;
} g_prog;

static void prog_start(const char *kind, const char *label, int64_t total) {
    pthread_mutex_lock(&g_plock);
    memset(&g_prog, 0, sizeof(g_prog));
    g_prog.running = 1;
    snprintf(g_prog.kind, sizeof(g_prog.kind), "%s", kind);
    snprintf(g_prog.label, sizeof(g_prog.label), "%s", label);
    g_prog.total = total;
    g_prog.start_ms = now_ms();
    rate_reset(&g_prog.rate, g_prog.start_ms, 0);
    pthread_mutex_unlock(&g_plock);
}

static void prog_set(int64_t done) {
    uint64_t now = now_ms();
    pthread_mutex_lock(&g_plock);
    g_prog.done = done;
    rate_add(&g_prog.rate, now, done);
    pthread_mutex_unlock(&g_plock);
}

static void prog_end(void) {
    pthread_mutex_lock(&g_plock);
    g_prog.running = 0;
    pthread_mutex_unlock(&g_plock);
}

cJSON *diag_progress_json(void) {
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&g_plock);
    cJSON_AddBoolToObject(o, "running", g_prog.running);
    if (g_prog.running) {
        uint64_t now = now_ms();
        double speed = rate_get(&g_prog.rate, now);
        cJSON_AddStringToObject(o, "kind", g_prog.kind);
        cJSON_AddStringToObject(o, "label", g_prog.label);
        cJSON_AddNumberToObject(o, "done", (double)g_prog.done);
        cJSON_AddNumberToObject(o, "total", (double)g_prog.total);
        cJSON_AddNumberToObject(o, "speed", speed);
        cJSON_AddNumberToObject(o, "elapsed_s", (double)(now - g_prog.start_ms) / 1000.0);
    }
    pthread_mutex_unlock(&g_plock);
    return o;
}

cJSON *diag_disk(const char *storage_id, int mb, int block_kb, int max_s, char *err, size_t errlen) {
    mb = clampi(mb, 1, 4096);
    block_kb = clampi(block_kb, 1, 64 * 1024);
    max_s = clampi(max_s, 1, 120);
    storage_t st[PONTE_MAX_STORAGES];
    int n = plat_storages(st, PONTE_MAX_STORAGES);
    const storage_t *s = NULL;
    for (int i = 0; i < n; i++)
        if (strcmp(st[i].id, storage_id) == 0) s = &st[i];
    if (!s) {
        snprintf(err, errlen, "armazenamento \"%s\" não encontrado", storage_id);
        return NULL;
    }
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s/ponte/tmp", s->root);
    snprintf(path, sizeof(path), "%s/diag.bin", dir);
    if (mkdirs(dir) != 0) {
        snprintf(err, errlen, "não foi possível criar %s", dir);
        return NULL;
    }
    size_t block = (size_t)block_kb << 10;
    char *buf = malloc(block);
    if (!buf) {
        snprintf(err, errlen, "sem memória para um bloco de %d KB", block_kb);
        return NULL;
    }
    /* dados que não são só zeros, para nenhuma camada pular a escrita */
    for (size_t i = 0; i < block; i++) buf[i] = (char)(i * 131 + 7);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        snprintf(err, errlen, "não foi possível criar %s: %s", path, strerror(errno));
        free(buf);
        return NULL;
    }
    int64_t want = (int64_t)mb << 20, done = 0;
    char label[160];
    snprintf(label, sizeof(label), "disco %s, blocos de %d KB", s->label, block_kb);
    prog_start("disk", label, want);
    uint64_t calls = 0, max_us = 0;
    uint64_t t0 = now_us(), deadline = t0 + (uint64_t)max_s * 1000000u;
    int werr = 0;
    while (done < want && now_us() < deadline) {
        size_t len = want - done < (int64_t)block ? (size_t)(want - done) : block;
        uint64_t a = now_us();
        ssize_t w = write(fd, buf, len);
        uint64_t d = now_us() - a;
        if (d > max_us) max_us = d;
        calls++;
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) {
            werr = errno;
            break;
        }
        done += w;
        prog_set(done);
    }
    uint64_t t1 = now_us();
    fsync(fd);
    prog_end();
    uint64_t t2 = now_us();
    close(fd);
    unlink(path);
    free(buf);
    if (werr && done == 0) {
        snprintf(err, errlen, "erro ao gravar: %s", strerror(werr));
        return NULL;
    }
    double secs = (double)(t2 - t0) / 1e6;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "storage", s->id);
    cJSON_AddStringToObject(j, "path", path);
    cJSON_AddNumberToObject(j, "block_kb", block_kb);
    cJSON_AddNumberToObject(j, "bytes", (double)done);
    cJSON_AddNumberToObject(j, "seconds", secs);
    cJSON_AddNumberToObject(j, "mb_s", secs > 0 ? (double)done / secs / 1e6 : 0);
    cJSON_AddNumberToObject(j, "writes", (double)calls);
    cJSON_AddNumberToObject(j, "avg_write_ms", calls ? (double)(t1 - t0) / (double)calls / 1000.0 : 0);
    cJSON_AddNumberToObject(j, "max_write_ms", (double)max_us / 1000.0);
    cJSON_AddNumberToObject(j, "fsync_ms", (double)(t2 - t1) / 1000.0);
    if (werr) cJSON_AddStringToObject(j, "error", strerror(werr));
    plat_log("[diag] disco %s bloco %d KB: %.1f MB/s (%lld MB em %.1f s, escrita mais lenta %.0f ms, fsync %.0f ms)",
             s->id, block_kb, secs > 0 ? (double)done / secs / 1e6 : 0, (long long)(done >> 20), secs,
             (double)max_us / 1000.0, (double)(t2 - t1) / 1000.0);
    return j;
}

cJSON *diag_net(const char *item_id, int mb, int read_kb, int rcvbuf_kb, int max_s, char *err, size_t errlen) {
    mb = clampi(mb, 1, 8192);
    read_kb = clampi(read_kb, 1, 64 * 1024);
    max_s = clampi(max_s, 1, 120);
    if (!valid_id(item_id)) {
        snprintf(err, errlen, "id de item inválido");
        return NULL;
    }
    if (!app_mac_ready()) {
        snprintf(err, errlen, "o Mac não está conectado e pareado");
        return NULL;
    }
    mac_t mac;
    app_get_mac(&mac);
    char path[160];
    snprintf(path, sizeof(path), "/api/v1/items/%s", item_id);
    int status = 0;
    char e[256] = "";
    cJSON *detail = ponte_call(&mac, "GET", path, NULL, &status, e, sizeof(e));
    if (!detail || status != 200) {
        snprintf(err, errlen, "o Mac não deu os arquivos do item: %s", e[0] ? e : "sem resposta");
        cJSON_Delete(detail);
        return NULL;
    }
    /* o maior arquivo do item */
    int index = -1;
    double best = -1;
    char name[256] = "";
    cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(detail, "files")) {
        cJSON *sz = cJSON_GetObjectItem(f, "size"), *ix = cJSON_GetObjectItem(f, "index"),
              *p = cJSON_GetObjectItem(f, "path");
        if (cJSON_IsNumber(sz) && cJSON_IsNumber(ix) && sz->valuedouble > best) {
            best = sz->valuedouble;
            index = ix->valueint;
            snprintf(name, sizeof(name), "%s", cJSON_IsString(p) ? p->valuestring : "");
        }
    }
    cJSON_Delete(detail);
    if (index < 0) {
        snprintf(err, errlen, "o item não tem arquivos");
        return NULL;
    }

    size_t cap = (size_t)read_kb << 10;
    char *buf = malloc(cap);
    if (!buf) {
        snprintf(err, errlen, "sem memória");
        return NULL;
    }
    char headers[256];
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", mac.token);
    snprintf(path, sizeof(path), "/api/v1/items/%s/files/%d", item_id, index);
    int rcvbuf = rcvbuf_kb < 0 ? -1 : rcvbuf_kb << 10;
    http_resp_t r;
    uint64_t t0 = now_us();
    if (http_open_ex(mac.host, mac.port, "GET", path, headers, NULL, 0, &r, 30, rcvbuf) != 0) {
        free(buf);
        snprintf(err, errlen, "não foi possível conectar ao Mac");
        return NULL;
    }
    uint64_t t_head = now_us();
    if (r.status != 200) {
        http_close(&r);
        free(buf);
        snprintf(err, errlen, "o Mac respondeu %d", r.status);
        return NULL;
    }
    int64_t want = (int64_t)mb << 20, done = 0;
    if (r.content_length >= 0 && r.content_length < want) want = r.content_length;
    char label[160];
    snprintf(label, sizeof(label), "rede, leituras de %d KB", read_kb);
    prog_start("net", label, want);
    uint64_t calls = 0, deadline = t_head + (uint64_t)max_s * 1000000u, max_gap = 0;
    int neterr = 0;
    while (done < want && now_us() < deadline) {
        uint64_t a = now_us();
        ssize_t n = http_read(&r, buf, cap);
        uint64_t d = now_us() - a;
        if (d > max_gap) max_gap = d;
        if (n < 0) {
            neterr = 1;
            break;
        }
        if (n == 0) break;
        calls++;
        done += n;
        prog_set(done);
    }
    prog_end();
    uint64_t t1 = now_us();
    int eff = r.rcvbuf_eff;
    http_close(&r);
    free(buf);
    double secs = (double)(t1 - t_head) / 1e6;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "file", name);
    cJSON_AddNumberToObject(j, "bytes", (double)done);
    cJSON_AddNumberToObject(j, "seconds", secs);
    cJSON_AddNumberToObject(j, "mb_s", secs > 0 ? (double)done / secs / 1e6 : 0);
    cJSON_AddNumberToObject(j, "recv_calls", (double)calls);
    cJSON_AddNumberToObject(j, "avg_recv_kb", calls ? (double)done / (double)calls / 1024.0 : 0);
    cJSON_AddNumberToObject(j, "max_wait_ms", (double)max_gap / 1000.0);
    cJSON_AddNumberToObject(j, "read_kb", read_kb);
    cJSON_AddNumberToObject(j, "rcvbuf_asked_kb", rcvbuf < 0 ? http_rcvbuf >> 10 : rcvbuf >> 10);
    cJSON_AddNumberToObject(j, "rcvbuf_kb", eff >= 0 ? eff >> 10 : -1);
    cJSON_AddNumberToObject(j, "connect_ms", (double)(t_head - t0) / 1000.0);
    if (neterr) cJSON_AddStringToObject(j, "error", "a conexão caiu no meio");
    plat_log("[diag] rede: %.1f MB/s (%lld MB em %.1f s, recv médio %.0f KB, maior espera %.0f ms, buffer TCP %d KB)",
             secs > 0 ? (double)done / secs / 1e6 : 0, (long long)(done >> 20), secs,
             calls ? (double)done / (double)calls / 1024.0 : 0, (double)max_gap / 1000.0, eff >= 0 ? eff >> 10 : -1);
    return j;
}
