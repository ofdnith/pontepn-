/* Payload Ponte para o PS5.
 *
 * Fala com o Mac (cliente do protocolo, SPEC §3–§8) e serve uma interface web na porta 47102,
 * para abrir no navegador do celular, do computador ou do próprio PS5:
 *     http://<ip-do-ps5>:47102
 * Na interface você vê o que tem no Mac e o que já está no console, escolhe o que copiar e para
 * qual armazenamento. A cópia roda aqui, em segundo plano.
 */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app.h"
#include "diag.h"
#include "http.h"
#include "jobs.h"
#include "platform.h"
#include "ponte.h"
#include "tile.h"
#include "ui_icon.h"
#include "ui_page.h"
#include "util.h"


static int g_ui_port = 47102;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static mac_t g_mac;
static int g_reachable;
static int g_pairing_required = 1;
static cJSON *g_catalog; /* último catálogo anotado */
static mac_t g_found[PONTE_MAX_FOUND];
static int g_found_count;

/* ---------- estado compartilhado (app.h) ---------- */

void app_get_mac(mac_t *out) {
    pthread_mutex_lock(&g_lock);
    *out = g_mac;
    pthread_mutex_unlock(&g_lock);
}

void app_set_mac(const mac_t *mac) {
    pthread_mutex_lock(&g_lock);
    g_mac = *mac;
    pthread_mutex_unlock(&g_lock);
    ponte_save_config(mac);
}

int app_mac_ready(void) {
    pthread_mutex_lock(&g_lock);
    int ok = g_mac.host[0] && g_reachable && (g_mac.token[0] || !g_pairing_required);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/* ---------- ajustes (settings.json) ---------- */

static void settings_path(char *out, size_t cap) { path_join(out, cap, plat_data_dir(), "settings.json"); }

/* Ajustes de desempenho (para testar no console sem recompilar):
 *   rcvbuf_kb  buffer de recepção do TCP pedido ao sistema (0 = padrão do sistema)
 *   chunk_kb   tamanho de cada buffer da cópia (a rede enche um inteiro antes de ir ao disco) */
static void apply_settings(cJSON *j) {
    cJSON *v = cJSON_GetObjectItem(j, "notify_progress");
    if (cJSON_IsBool(v)) jobs_notify_progress = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(j, "rcvbuf_kb");
    if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 32768) http_rcvbuf = v->valueint << 10;
    v = cJSON_GetObjectItem(j, "chunk_kb");
    if (cJSON_IsNumber(v) && v->valueint >= 64 && v->valueint <= 65536) jobs_chunk_bytes = v->valueint << 10;
    v = cJSON_GetObjectItem(j, "mounter_rescan");
    if (cJSON_IsString(v) && strlen(v->valuestring) < sizeof(jobs_mounter_rescan))
        snprintf(jobs_mounter_rescan, sizeof(jobs_mounter_rescan), "%s", v->valuestring);
}

static void load_settings(void) {
    char path[512];
    settings_path(path, sizeof(path));
    char *text = read_file(path, 64 * 1024, NULL);
    if (!text) return;
    cJSON *j = cJSON_Parse(text);
    free(text);
    apply_settings(j);
    cJSON_Delete(j);
}

static cJSON *settings_json(void);

static void save_settings(void) {
    cJSON *j = settings_json();
    char *text = cJSON_Print(j);
    cJSON_Delete(j);
    char path[512];
    settings_path(path, sizeof(path));
    write_file_atomic(path, text, strlen(text));
    free(text);
}

static cJSON *settings_json(void) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "notify_progress", jobs_notify_progress);
    cJSON_AddNumberToObject(j, "rcvbuf_kb", http_rcvbuf >> 10);
    cJSON_AddNumberToObject(j, "chunk_kb", jobs_chunk_bytes >> 10);
    cJSON_AddStringToObject(j, "mounter_rescan", jobs_mounter_rescan);
    return j;
}

cJSON *app_catalog_item(const char *id) {
    cJSON *out = NULL;
    pthread_mutex_lock(&g_lock);
    cJSON *items = g_catalog ? cJSON_GetObjectItem(g_catalog, "items") : NULL;
    cJSON *it;
    cJSON_ArrayForEach(it, items) {
        cJSON *iid = cJSON_GetObjectItem(it, "id");
        if (cJSON_IsString(iid) && strcmp(iid->valuestring, id) == 0) {
            out = cJSON_Duplicate(it, 1);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return out;
}

static cJSON *storages_json(storage_t *st, int n) {
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", st[i].id);
        cJSON_AddStringToObject(o, "label", st[i].label);
        cJSON_AddNumberToObject(o, "free", (double)st[i].free);
        cJSON_AddNumberToObject(o, "total", (double)st[i].total);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *app_refresh_catalog(char *err, size_t errlen, int *status) {
    storage_t st[PONTE_MAX_STORAGES];
    int n = plat_storages(st, PONTE_MAX_STORAGES);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "platform", "ps5");
    const char *fw = plat_firmware();
    if (fw) cJSON_AddStringToObject(body, "firmware", fw);
    else cJSON_AddNullToObject(body, "firmware");
    cJSON_AddItemToObject(body, "storages", storages_json(st, n));
    cJSON_AddItemToObject(body, "installed", plat_installed(st, n));
    mac_t mac;
    app_get_mac(&mac);
    cJSON *res = ponte_call(&mac, "POST", "/api/v1/console/state", body, status, err, errlen);
    cJSON_Delete(body);
    if (!res || *status != 200) {
        cJSON_Delete(res);
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    cJSON_Delete(g_catalog);
    g_catalog = cJSON_Duplicate(res, 1);
    pthread_mutex_unlock(&g_lock);
    cJSON_AddItemToObject(res, "storages", storages_json(st, n));
    return res;
}

/* ---------- interface web ---------- */

static char *print(cJSON *j) {
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}

static char *error_json(const char *code, const char *message) {
    cJSON *j = cJSON_CreateObject();
    cJSON *e = cJSON_AddObjectToObject(j, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return print(j);
}

static cJSON *mac_json(const mac_t *m) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "host", m->host);
    cJSON_AddNumberToObject(o, "port", m->port);
    cJSON_AddStringToObject(o, "name", m->name);
    cJSON_AddStringToObject(o, "id", m->id);
    return o;
}

static int local_ip(char *out, size_t cap) {
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return -1;
    int found = -1;
    for (struct ifaddrs *i = ifs; i && found < 0; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        struct sockaddr_in *a = (struct sockaddr_in *)i->ifa_addr;
        if ((ntohl(a->sin_addr.s_addr) >> 24) == 127) continue;
        inet_ntop(AF_INET, &a->sin_addr, out, (socklen_t)cap);
        found = 0;
    }
    freeifaddrs(ifs);
    return found;
}

static cJSON *body_json(const http_req_t *req) {
    return req->body ? cJSON_Parse(req->body) : cJSON_CreateObject();
}

static void handle(const http_req_t *req, http_res_t *res) {
    int get = strcmp(req->method, "GET") == 0 || strcmp(req->method, "HEAD") == 0;
    int post = strcmp(req->method, "POST") == 0;
    const char *p = req->path;

    if (get && (strcmp(p, "/") == 0 || strcmp(p, "/index.html") == 0)) {
        res->status = 200;
        res->content_type = "text/html; charset=utf-8";
        res->static_body = UI_PAGE;
        res->body_len = UI_PAGE_LEN;
        return;
    }

    if (get && (strcmp(p, "/icon.png") == 0 || strcmp(p, "/favicon.ico") == 0)) {
        res->status = 200;
        res->content_type = "image/png";
        res->static_body = UI_ICON;
        res->body_len = UI_ICON_LEN;
        snprintf(res->extra_headers, sizeof(res->extra_headers), "Cache-Control: max-age=86400\r\n");
        return;
    }

    if (get && strcmp(p, "/api/status") == 0) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "version", PONTE_VERSION);
        const char *fw = plat_firmware();
        if (fw) cJSON_AddStringToObject(j, "firmware", fw);
        else cJSON_AddNullToObject(j, "firmware");
        char ip[48] = "";
        local_ip(ip, sizeof(ip));
        cJSON_AddStringToObject(j, "console_ip", ip);
        pthread_mutex_lock(&g_lock);
        mac_t m = g_mac;
        int reachable = g_reachable, pr = g_pairing_required;
        pthread_mutex_unlock(&g_lock);
        if (m.host[0]) {
            cJSON *mj = mac_json(&m);
            cJSON_AddBoolToObject(mj, "reachable", reachable);
            cJSON_AddBoolToObject(mj, "paired", m.token[0] != '\0' || !pr);
            cJSON_AddBoolToObject(mj, "pairing_required", pr);
            cJSON_AddItemToObject(j, "mac", mj);
        } else {
            cJSON_AddNullToObject(j, "mac");
        }
        storage_t st[PONTE_MAX_STORAGES];
        int n = plat_storages(st, PONTE_MAX_STORAGES);
        cJSON_AddItemToObject(j, "storages", storages_json(st, n));
        cJSON_AddItemToObject(j, "jobs", jobs_json());
        cJSON_AddItemToObject(j, "settings", settings_json());
        http_res_json(res, 200, print(j));
        return;
    }

    if (post && strcmp(p, "/api/discover") == 0) {
        mac_t found[PONTE_MAX_FOUND];
        mac_t cur;
        app_get_mac(&cur);
        int n = ponte_discover(found, PONTE_MAX_FOUND, cur.host, 1200);
        pthread_mutex_lock(&g_lock);
        memcpy(g_found, found, sizeof(found));
        g_found_count = n;
        pthread_mutex_unlock(&g_lock);
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, mac_json(&found[i]));
        http_res_json(res, 200, print(arr));
        return;
    }

    if (post && strcmp(p, "/api/connect") == 0) {
        cJSON *b = body_json(req);
        cJSON *host = cJSON_GetObjectItem(b, "host");
        cJSON *port = cJSON_GetObjectItem(b, "port");
        if (!cJSON_IsString(host) || !host->valuestring[0]) {
            cJSON_Delete(b);
            http_res_json(res, 400, error_json("bad_request", "informe o endereço do Mac"));
            return;
        }
        mac_t m;
        app_get_mac(&m);
        if (strcmp(m.host, host->valuestring) != 0) memset(&m, 0, sizeof(m));
        snprintf(m.host, sizeof(m.host), "%s", host->valuestring);
        m.port = cJSON_IsNumber(port) ? port->valueint : 47101;
        cJSON_Delete(b);
        int pr = 1;
        if (ponte_info(&m, &pr) != 0) {
            http_res_json(res, 502, error_json("unreachable", "o Ponte não respondeu nesse endereço"));
            return;
        }
        app_set_mac(&m);
        pthread_mutex_lock(&g_lock);
        g_reachable = 1;
        g_pairing_required = pr;
        pthread_mutex_unlock(&g_lock);
        http_res_json(res, 200, print(mac_json(&m)));
        return;
    }

    if (post && strcmp(p, "/api/pair") == 0) {
        cJSON *b = body_json(req);
        cJSON *code = cJSON_GetObjectItem(b, "code");
        char c6[16] = "";
        if (cJSON_IsString(code)) {
            /* aceita "482 913" */
            size_t k = 0;
            for (const char *s = code->valuestring; *s && k < 6; s++)
                if (*s >= '0' && *s <= '9') c6[k++] = *s;
            c6[k] = '\0';
        }
        cJSON_Delete(b);
        mac_t m;
        app_get_mac(&m);
        char err[256] = "";
        if (strlen(c6) != 6 || !m.host[0]) {
            http_res_json(res, 400, error_json("bad_request", "digite os 6 dígitos mostrados no Mac"));
            return;
        }
        if (ponte_pair(&m, c6, err, sizeof(err)) != 0) {
            http_res_json(res, 400, error_json("pair_failed", err[0] ? err : "não foi possível parear"));
            return;
        }
        app_set_mac(&m);
        plat_notify("Ponte: pareado com %s", m.name);
        http_res_json(res, 200, print(mac_json(&m)));
        return;
    }

    if (post && strcmp(p, "/api/forget") == 0) {
        mac_t m;
        memset(&m, 0, sizeof(m));
        app_set_mac(&m);
        http_res_json(res, 200, strdup("{}"));
        return;
    }

    if (get && strcmp(p, "/api/catalog") == 0) {
        char err[256] = "";
        int status = 0;
        cJSON *c = app_refresh_catalog(err, sizeof(err), &status);
        if (!c) {
            if (status == 401) {
                /* o Mac revogou este console */
                mac_t m;
                app_get_mac(&m);
                m.token[0] = '\0';
                app_set_mac(&m);
            }
            http_res_json(res, status == 401 ? 401 : 502,
                          error_json(status == 401 ? "unauthorized" : "unreachable",
                                     err[0] ? err : "o Mac não respondeu"));
            return;
        }
        http_res_json(res, 200, print(c));
        return;
    }

    if (post && strcmp(p, "/api/copy") == 0) {
        cJSON *b = body_json(req);
        cJSON *ids = cJSON_GetObjectItem(b, "ids");
        cJSON *storage = cJSON_GetObjectItem(b, "storage");
        int replace = cJSON_IsTrue(cJSON_GetObjectItem(b, "replace"));
        cJSON *errors = cJSON_CreateArray();
        int added = 0;
        cJSON *id;
        cJSON_ArrayForEach(id, ids) {
            if (!cJSON_IsString(id)) continue;
            char err[200] = "";
            if (!valid_id(id->valuestring)) {
                cJSON_AddItemToArray(errors, cJSON_CreateString("id de item inválido"));
                continue;
            }
            if (jobs_add(id->valuestring, cJSON_IsString(storage) ? storage->valuestring : "internal", replace, err,
                         sizeof(err)) == 0)
                added++;
            else
                cJSON_AddItemToArray(errors, cJSON_CreateString(err));
        }
        cJSON_Delete(b);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "added", added);
        cJSON_AddItemToObject(j, "errors", errors);
        http_res_json(res, 200, print(j));
        return;
    }

    if (post && strcmp(p, "/api/cancel") == 0) {
        cJSON *b = body_json(req);
        cJSON *seq = cJSON_GetObjectItem(b, "seq");
        int ok = cJSON_IsNumber(seq) && jobs_cancel(seq->valueint) == 0;
        cJSON_Delete(b);
        http_res_json(res, ok ? 200 : 404, ok ? strdup("{}") : error_json("not_found", "trabalho não encontrado"));
        return;
    }

    if (post && (strcmp(p, "/api/pause") == 0 || strcmp(p, "/api/resume") == 0)) {
        cJSON *b = body_json(req);
        cJSON *seq = cJSON_GetObjectItem(b, "seq");
        int pause = strcmp(p, "/api/pause") == 0;
        int ok = cJSON_IsNumber(seq) && (pause ? jobs_pause(seq->valueint) : jobs_resume(seq->valueint)) == 0;
        cJSON_Delete(b);
        http_res_json(res, ok ? 200 : 404, ok ? strdup("{}") : error_json("not_found", "trabalho não encontrado"));
        return;
    }

    if (post && strcmp(p, "/api/settings") == 0) {
        cJSON *b = body_json(req);
        apply_settings(b);
        cJSON_Delete(b);
        save_settings();
        http_res_json(res, 200, print(settings_json()));
        return;
    }

    if (post && strcmp(p, "/api/clear") == 0) {
        jobs_clear_finished();
        http_res_json(res, 200, strdup("{}"));
        return;
    }

    if (get && strcmp(p, "/api/icon") == 0) {
        char id[40] = "";
        http_query_param(req->query, "id", id, sizeof(id));
        mac_t m;
        app_get_mac(&m);
        char path[128], headers[160] = "";
        snprintf(path, sizeof(path), "/api/v1/items/%s/icon", id);
        if (m.token[0]) snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", m.token);
        http_resp_t r;
        if (!valid_id(id) || !m.host[0] || http_open(m.host, m.port, "GET", path, headers, NULL, 0, &r, 10) != 0) {
            http_res_json(res, 404, error_json("not_found", "sem ícone"));
            return;
        }
        char *data = NULL;
        size_t len = 0;
        if (r.status == 200 && http_read_all(&r, &data, &len, 8 << 20) == 0) {
            res->status = 200;
            res->content_type = strstr(r.content_type, "png") ? "image/png" : "image/jpeg";
            res->body = data;
            res->body_len = len;
            snprintf(res->extra_headers, sizeof(res->extra_headers), "Cache-Control: max-age=86400\r\n");
        } else {
            http_res_json(res, 404, error_json("not_found", "sem ícone"));
        }
        http_close(&r);
        return;
    }

    /* teste em andamento: a interface mostra barra e velocidade enquanto espera o resultado */
    if (get && strcmp(p, "/api/diag/progress") == 0) {
        http_res_json(res, 200, print(diag_progress_json()));
        return;
    }

    /* diagnóstico de velocidade: um teste por vez */
    if (get && (strcmp(p, "/api/diag/disk") == 0 || strcmp(p, "/api/diag/net") == 0)) {
        static pthread_mutex_t diag_lock = PTHREAD_MUTEX_INITIALIZER;
        if (pthread_mutex_trylock(&diag_lock) != 0) {
            http_res_json(res, 409, error_json("busy", "já tem um teste rodando"));
            return;
        }
        char v[96], err[256] = "";
        int mb = http_query_param(req->query, "mb", v, sizeof(v)) ? atoi(v) : 256;
        int max_s = http_query_param(req->query, "max_s", v, sizeof(v)) ? atoi(v) : 20;
        cJSON *r;
        if (strcmp(p, "/api/diag/disk") == 0) {
            char storage[32] = "internal";
            http_query_param(req->query, "storage", storage, sizeof(storage));
            int block_kb = http_query_param(req->query, "block_kb", v, sizeof(v)) ? atoi(v) : 4096;
            r = diag_disk(storage, mb, block_kb, max_s, err, sizeof(err));
        } else {
            char item[40] = "";
            http_query_param(req->query, "item", item, sizeof(item));
            int read_kb = http_query_param(req->query, "read_kb", v, sizeof(v)) ? atoi(v) : 4096;
            int rcvbuf_kb = http_query_param(req->query, "rcvbuf_kb", v, sizeof(v)) ? atoi(v) : -1;
            r = valid_id(item) ? diag_net(item, mb, read_kb, rcvbuf_kb, max_s, err, sizeof(err)) : NULL;
            if (!valid_id(item)) snprintf(err, sizeof(err), "informe o item (item=...)");
        }
        pthread_mutex_unlock(&diag_lock);
        if (r) http_res_json(res, 200, print(r));
        else http_res_json(res, 400, error_json("diag_failed", err));
        return;
    }

#ifdef PONTE_HOST
    /* só na versão de teste: simula a conexão caindo depois de N bytes */
    if (post && strcmp(p, "/api/debug/fail_after") == 0) {
        cJSON *b = body_json(req);
        cJSON *n = cJSON_GetObjectItem(b, "bytes");
        jobs_test_fail_after = cJSON_IsNumber(n) ? (int64_t)n->valuedouble : 0;
        cJSON_Delete(b);
        http_res_json(res, 200, strdup("{}"));
        return;
    }
    /* só na versão de teste: estraga um byte dos próximos N arquivos (sha256 não confere) */
    if (post && strcmp(p, "/api/debug/corrupt") == 0) {
        cJSON *b = body_json(req);
        cJSON *n = cJSON_GetObjectItem(b, "files");
        __atomic_store_n(&jobs_test_corrupt, cJSON_IsNumber(n) ? n->valueint : 0, __ATOMIC_RELAXED);
        cJSON_Delete(b);
        http_res_json(res, 200, strdup("{}"));
        return;
    }
    if (post && strcmp(p, "/api/debug/slow") == 0) {
        cJSON *b = body_json(req);
        cJSON *n = cJSON_GetObjectItem(b, "ms");
        jobs_test_slow_ms = cJSON_IsNumber(n) ? n->valueint : 0;
        cJSON_Delete(b);
        http_res_json(res, 200, strdup("{}"));
        return;
    }
#endif

    http_res_json(res, 404, error_json("not_found", "rota não encontrada"));
}

/* ---------- vigia: o Mac está no ar? ---------- */

static void *watcher(void *arg) {
    (void)arg;
    int announced = 0;
    while (1) {
        mac_t m;
        app_get_mac(&m);
        if (!m.host[0]) {
            /* sem Mac escolhido: procura; com um só na rede, escolhe sozinho */
            mac_t found[PONTE_MAX_FOUND];
            int n = ponte_discover(found, PONTE_MAX_FOUND, NULL, 1500);
            pthread_mutex_lock(&g_lock);
            memcpy(g_found, found, sizeof(found));
            g_found_count = n;
            pthread_mutex_unlock(&g_lock);
            if (n == 1) {
                int pr = 1;
                if (ponte_info(&found[0], &pr) == 0) {
                    app_set_mac(&found[0]);
                    pthread_mutex_lock(&g_lock);
                    g_pairing_required = pr;
                    pthread_mutex_unlock(&g_lock);
                    plat_notify("Ponte: Mac encontrado (%s)", found[0].name);
                }
            }
        } else {
            mac_t probe = m;
            int pr = 1;
            int ok = ponte_info(&probe, &pr) == 0;
            pthread_mutex_lock(&g_lock);
            g_reachable = ok;
            if (ok) g_pairing_required = pr;
            /* o Mac é outro (id mudou): o token antigo não vale */
            int changed = ok && strcmp(probe.id, g_mac.id) != 0;
            pthread_mutex_unlock(&g_lock);
            if (changed) app_set_mac(&probe);
            if (ok && !announced) {
                plat_notify("Ponte: conectado ao %s", probe.name);
                announced = 1;
            }
        }
        sleep(5);
    }
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    const char *env;
    if ((env = getenv("PONTE_UI_PORT"))) g_ui_port = atoi(env);
    if ((env = getenv("PONTE_DISCOVERY_PORT"))) ponte_discovery_port = atoi(env);
    if ((env = getenv("PONTE_DISCOVERY_HOST"))) ponte_discovery_extra = env;

    if (plat_init() != 0) return 1;
    plat_notify("Ponte %s iniciando…", PONTE_VERSION);
    mkdirs(plat_data_dir());
    ponte_load_config(&g_mac);
    load_settings();

    if (http_serve(g_ui_port, handle) != 0) {
        plat_notify("Ponte já está rodando (porta %d ocupada)", g_ui_port);
        return 1;
    }
    jobs_init();
    /* ícone na tela inicial (o ShadowMountPlus registra em alguns segundos) */
    tile_ensure(g_ui_port);
    pthread_t t;
    pthread_create(&t, NULL, watcher, NULL);
    pthread_detach(t);

    char ip[48] = "";
    if (local_ip(ip, sizeof(ip)) == 0) plat_notify("Ponte pronto: abra http://%s:%d", ip, g_ui_port);
    else plat_notify("Ponte pronto na porta %d", g_ui_port);
    plat_log("Ponte %s, interface em http://%s:%d", PONTE_VERSION, ip[0] ? ip : "0.0.0.0", g_ui_port);

    while (1) sleep(3600);
    return 0;
}
