#include "jobs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/statfs.h>
#endif

#include "app.h"
#include "http.h"
#include "platform.h"
#include "util.h"

#define MAX_JOBS 64
#define NBUF 4 /* buffers em trânsito entre a thread de rede e a de disco */

/* PAUSED fica antes de DONE: state >= JOB_DONE quer dizer "terminou" */
typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_PAUSED, JOB_DONE, JOB_FAILED, JOB_CANCELLED } job_state_t;

#define RETRY_MS 15000 /* pausa automática (Mac fora do ar): tenta de novo depois disso */

/* Etapa do trabalho em andamento (a interface mostra o texto de cada uma). */
typedef enum { PH_NONE, PH_QUERY, PH_VERIFY_PARTIAL, PH_DOWNLOAD, PH_VERIFY, PH_MOVE, PH_INSTALL } phase_t;

static const char *phase_name(phase_t p) {
    switch (p) {
    case PH_QUERY: return "query";
    case PH_VERIFY_PARTIAL: return "verify_partial";
    case PH_DOWNLOAD: return "download";
    case PH_VERIFY: return "verify";
    case PH_MOVE: return "move";
    case PH_INSTALL: return "install";
    default: return NULL;
    }
}

typedef struct {
    int used; /* posição fixa: o trabalho em andamento nunca muda de lugar */
    int seq;
    char item_id[20];
    char title[192];
    char kind[16];
    char content[12];
    char title_id[24];
    char storage[16];
    int replace;
    job_state_t state;
    int64_t done_bytes;
    int64_t total_bytes;
    char message[256];
    char error[48];
    int cancel;
    uint64_t started_ms;   /* relógio monotônico (durações) */
    uint64_t started_wall; /* hora de verdade, para mostrar */
    rate_t rate;           /* velocidade do total (done_bytes) */
    phase_t phase;
    int64_t phase_done, phase_total;
    rate_t phase_rate;     /* velocidade da etapa (hash na conferência, rede no download) */
    char file[256];        /* arquivo em andamento */
    int file_index, file_count;
    int connections, requests; /* conexões TCP e requisições ao Mac nesta cópia (keep-alive) */
    int64_t session_start; /* done_bytes quando esta cópia começou (o resto já estava no console) */
    int manual_pause;      /* pausado por você: só volta quando você mandar continuar */
    int pause_req;         /* pedido de pausa durante a cópia */
    uint64_t next_try_ms;  /* pausa automática: quando tentar de novo */
    int quarter;           /* último aviso de progresso (1 = 25%, 2 = 50%, 3 = 75%) */
    char stats[300];       /* medições da cópia (rede, disco, hash); para achar o gargalo */
} job_t;

static job_t g_jobs[MAX_JOBS];
static const int g_count = MAX_JOBS;
static int g_seq;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
/* queue.json é gravado pelas threads da interface e pela do trabalho: uma de cada vez */
static pthread_mutex_t g_save_lock = PTHREAD_MUTEX_INITIALIZER;

int64_t jobs_test_fail_after = 0;
int jobs_test_corrupt = 0;
int jobs_test_slow_ms = 0;
int jobs_notify_progress = 1;
int jobs_chunk_bytes = 4 << 20;
char jobs_mounter_rescan[160] = "127.0.0.1:10101/api/v1/scan";

static void save_queue(void);

static const char *FOLDER_KINDS[] = {"folder", "folder_ps4", NULL};

static int in_list(const char *s, const char **list) {
    for (int i = 0; list[i]; i++)
        if (strcmp(s, list[i]) == 0) return 1;
    return 0;
}

static int is_pkg(const char *kind) { return strncmp(kind, "pkg_", 4) == 0; }

static const char *state_name(job_state_t s) {
    switch (s) {
    case JOB_QUEUED: return "queued";
    case JOB_RUNNING: return "running";
    case JOB_PAUSED: return "paused";
    case JOB_DONE: return "done";
    case JOB_FAILED: return "failed";
    case JOB_CANCELLED: return "cancelled";
    }
    return "?";
}

static int content_rank(const char *c) {
    if (strcmp(c, "base") == 0) return 0;
    if (strcmp(c, "update") == 0) return 1;
    if (strcmp(c, "dlc") == 0) return 2;
    return 3;
}

/* ---------- fila ---------- */

int jobs_add(const char *item_id, const char *storage, int replace, char *err, size_t errlen) {
    cJSON *it = app_catalog_item(item_id);
    if (!it) {
        snprintf(err, errlen, "item não está no catálogo; atualize a lista");
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    /* já na fila ou copiando: não duplica */
    for (int i = 0; i < g_count; i++) {
        if (g_jobs[i].used && strcmp(g_jobs[i].item_id, item_id) == 0 && g_jobs[i].state < JOB_DONE) {
            /* já está na fila: se estava pausado, volta a andar */
            if (g_jobs[i].state == JOB_PAUSED) {
                g_jobs[i].state = JOB_QUEUED;
                g_jobs[i].manual_pause = 0;
                snprintf(g_jobs[i].message, sizeof(g_jobs[i].message), "na fila");
                pthread_cond_signal(&g_cond);
            }
            pthread_mutex_unlock(&g_lock);
            cJSON_Delete(it);
            save_queue();
            return 0;
        }
    }
    /* posição livre; sem nenhuma, reaproveita o trabalho terminado mais antigo */
    int k = -1;
    for (int i = 0; i < g_count && k < 0; i++)
        if (!g_jobs[i].used) k = i;
    for (int i = 0; i < g_count && k < 0; i++)
        if (g_jobs[i].state >= JOB_DONE && (k < 0 || g_jobs[i].seq < g_jobs[k].seq)) k = i;
    if (k < 0) {
        pthread_mutex_unlock(&g_lock);
        cJSON_Delete(it);
        snprintf(err, errlen, "fila cheia");
        return -1;
    }
    job_t *j = &g_jobs[k];
    memset(j, 0, sizeof(*j));
    j->used = 1;
    j->seq = ++g_seq;
    snprintf(j->item_id, sizeof(j->item_id), "%s", item_id);
    cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItem(it, "title"))) snprintf(j->title, sizeof(j->title), "%s", v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(it, "kind"))) snprintf(j->kind, sizeof(j->kind), "%s", v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(it, "content"))) snprintf(j->content, sizeof(j->content), "%s", v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItem(it, "title_id"))) snprintf(j->title_id, sizeof(j->title_id), "%s", v->valuestring);
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(it, "size"))) j->total_bytes = (int64_t)v->valuedouble;
    snprintf(j->storage, sizeof(j->storage), "%s", storage ? storage : "");
    j->replace = replace;
    j->state = JOB_QUEUED;
    snprintf(j->message, sizeof(j->message), "na fila");
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    cJSON_Delete(it);
    save_queue();
    return 0;
}

cJSON *jobs_json(void) {
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    /* em ordem de chegada */
    int last = 0;
    while (1) {
        job_t *j = NULL;
        for (int i = 0; i < g_count; i++)
            if (g_jobs[i].used && g_jobs[i].seq > last && (!j || g_jobs[i].seq < j->seq)) j = &g_jobs[i];
        if (!j) break;
        last = j->seq;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", j->seq);
        cJSON_AddStringToObject(o, "item_id", j->item_id);
        cJSON_AddStringToObject(o, "title", j->title);
        cJSON_AddStringToObject(o, "kind", j->kind);
        cJSON_AddStringToObject(o, "content", j->content);
        cJSON_AddStringToObject(o, "storage", j->storage);
        cJSON_AddStringToObject(o, "state", state_name(j->state));
        cJSON_AddNumberToObject(o, "done", (double)j->done_bytes);
        cJSON_AddNumberToObject(o, "total", (double)j->total_bytes);
        cJSON_AddStringToObject(o, "message", j->message);
        uint64_t now = now_ms();
        int running = j->state == JOB_RUNNING;
        double speed = running ? rate_get(&j->rate, now) : 0;
        cJSON_AddNumberToObject(o, "speed", speed);
        if (speed > 0 && j->total_bytes > j->done_bytes)
            cJSON_AddNumberToObject(o, "eta_s", (double)(j->total_bytes - j->done_bytes) / speed);
        else
            cJSON_AddNullToObject(o, "eta_s");
        const char *ph = running ? phase_name(j->phase) : NULL;
        if (ph) cJSON_AddStringToObject(o, "phase", ph);
        else cJSON_AddNullToObject(o, "phase");
        if (ph && j->phase == PH_DOWNLOAD) {
            /* no download, a etapa é a cópia inteira */
            cJSON_AddNumberToObject(o, "phase_done", (double)j->done_bytes);
            cJSON_AddNumberToObject(o, "phase_total", (double)j->total_bytes);
            cJSON_AddNumberToObject(o, "phase_speed", speed);
        } else if (ph && j->phase_total > 0) {
            cJSON_AddNumberToObject(o, "phase_done", (double)j->phase_done);
            cJSON_AddNumberToObject(o, "phase_total", (double)j->phase_total);
            cJSON_AddNumberToObject(o, "phase_speed", rate_get(&j->phase_rate, now));
        } else {
            cJSON_AddNullToObject(o, "phase_done");
            cJSON_AddNullToObject(o, "phase_total");
            cJSON_AddNumberToObject(o, "phase_speed", 0);
        }
        if (j->file[0]) cJSON_AddStringToObject(o, "file", j->file);
        else cJSON_AddNullToObject(o, "file");
        cJSON_AddNumberToObject(o, "file_index", j->file_index);
        cJSON_AddNumberToObject(o, "file_count", j->file_count);
        if (j->started_wall) cJSON_AddNumberToObject(o, "started_ms", (double)j->started_wall);
        else cJSON_AddNullToObject(o, "started_ms");
        cJSON_AddNumberToObject(o, "connections", j->connections);
        cJSON_AddNumberToObject(o, "requests", j->requests);
        cJSON_AddBoolToObject(o, "manual_pause", j->manual_pause);
        if (j->stats[0]) cJSON_AddStringToObject(o, "stats", j->stats);
        else cJSON_AddNullToObject(o, "stats");
        if (j->error[0]) cJSON_AddStringToObject(o, "error", j->error);
        else cJSON_AddNullToObject(o, "error");
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&g_lock);
    return arr;
}

int jobs_cancel(int seq) {
    int found = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        if (!g_jobs[i].used || g_jobs[i].seq != seq) continue;
        found = 0;
        if (g_jobs[i].state == JOB_QUEUED || g_jobs[i].state == JOB_PAUSED) {
            g_jobs[i].state = JOB_CANCELLED;
            snprintf(g_jobs[i].message, sizeof(g_jobs[i].message), "cancelado (o que já veio fica guardado para continuar)");
        } else if (g_jobs[i].state == JOB_RUNNING) {
            g_jobs[i].cancel = 1;
            g_jobs[i].pause_req = 0;
        }
    }
    pthread_mutex_unlock(&g_lock);
    save_queue();
    return found;
}

int jobs_pause(int seq) {
    int found = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        job_t *j = &g_jobs[i];
        if (!j->used || j->seq != seq) continue;
        found = 0;
        if (j->state == JOB_QUEUED || j->state == JOB_PAUSED) {
            j->state = JOB_PAUSED;
            j->manual_pause = 1;
            snprintf(j->message, sizeof(j->message), "pausado");
        } else if (j->state == JOB_RUNNING) {
            j->cancel = 1;
            j->pause_req = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    save_queue();
    return found;
}

int jobs_resume(int seq) {
    int found = -1;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        job_t *j = &g_jobs[i];
        if (!j->used || j->seq != seq) continue;
        found = 0;
        if (j->state == JOB_PAUSED) {
            j->state = JOB_QUEUED;
            j->manual_pause = 0;
            j->next_try_ms = 0;
            snprintf(j->message, sizeof(j->message), "na fila");
            pthread_cond_signal(&g_cond);
        }
    }
    pthread_mutex_unlock(&g_lock);
    save_queue();
    return found;
}

void jobs_clear_finished(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_count; i++)
        if (g_jobs[i].used && g_jobs[i].state >= JOB_DONE) g_jobs[i].used = 0;
    pthread_mutex_unlock(&g_lock);
}

/* Atualiza o trabalho em andamento (com trava). */
static void job_update(job_t *j, int64_t done, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void job_update(job_t *j, int64_t done, const char *fmt, ...) {
    int notify_q = 0;
    double notify_speed = 0;
    pthread_mutex_lock(&g_lock);
    char title[192] = "";
    if (done >= 0) {
        uint64_t now = now_ms();
        rate_add(&j->rate, now, done);
        j->done_bytes = done;
        if (j->total_bytes > 0) {
            int q = (int)(done * 4 / j->total_bytes);
            if (q > j->quarter && q >= 1 && q <= 3) {
                j->quarter = q;
                notify_q = q;
                notify_speed = rate_get(&j->rate, now);
                snprintf(title, sizeof(title), "%s", j->title);
            }
        }
    }
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(j->message, sizeof(j->message), fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&g_lock);
    if (notify_q && jobs_notify_progress) {
        if (notify_speed > 0)
            plat_notify("Ponte: %s %d%% · %.0f MB/s", title, notify_q * 25, notify_speed / 1e6);
        else
            plat_notify("Ponte: %s %d%%", title, notify_q * 25);
    }
}

/* Muda de etapa. `total` = bytes da etapa (0 = sem tamanho: barra indeterminada). */
static void job_phase(job_t *j, phase_t ph, int64_t total, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void job_phase(job_t *j, phase_t ph, int64_t total, const char *fmt, ...) {
    pthread_mutex_lock(&g_lock);
    j->phase = ph;
    j->phase_total = total;
    j->phase_done = 0;
    rate_reset(&j->phase_rate, now_ms(), 0);
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(j->message, sizeof(j->message), fmt, ap);
        va_end(ap);
    }
    pthread_mutex_unlock(&g_lock);
}

static void job_phase_done(job_t *j, int64_t done) {
    pthread_mutex_lock(&g_lock);
    j->phase_done = done;
    rate_add(&j->phase_rate, now_ms(), done);
    pthread_mutex_unlock(&g_lock);
}

static void job_file(job_t *j, const char *rel, int index, int count) {
    pthread_mutex_lock(&g_lock);
    snprintf(j->file, sizeof(j->file), "%s", rel);
    j->file_index = index;
    j->file_count = count;
    pthread_mutex_unlock(&g_lock);
}

static int job_cancelled(job_t *j) {
    pthread_mutex_lock(&g_lock);
    int c = j->cancel;
    pthread_mutex_unlock(&g_lock);
    return c;
}

/* ---------- cópia (SPEC §8) ---------- */

/* RES_HASH: o arquivo chegou mas o sha256 não confere (já foi apagado; dá para baixar de novo) */
typedef enum { RES_OK = 0, RES_FAIL, RES_CANCEL, RES_REVISION, RES_HASH } result_t;

/* Medições do lado do disco, somando todos os arquivos. Tempos em microssegundos. */
typedef struct {
    uint64_t hash_us;
    uint64_t write_us;     /* dentro de write */
    uint64_t write_calls;
    int64_t write_bytes;
    uint64_t wait_net_us;  /* o disco parado esperando dados da rede */
    uint64_t start_us;
} xstats_t;

/* ---------- rede e disco em paralelo ----------
 * Uma thread lê do socket e enche buffers inteiros (jobs_chunk_bytes); a do trabalho confere o
 * sha256 e grava. Assim a rede não para enquanto o disco grava, e o disco recebe escritas grandes
 * em vez de um write() por recv(). A thread de rede vive a cópia inteira: cada arquivo é só uma
 * nova resposta entregue a ela (na mesma conexão, com keep-alive). */

typedef struct {
    http_resp_t *r; /* resposta em leitura (vale enquanto active) */
    int active;     /* a thread de rede está lendo o corpo de r */
    int abort;      /* pare de ler r (a conexão vai ser fechada) */
    int stop;       /* fim da cópia: a thread termina */
    char **bufs;
    size_t cap;
    size_t len[NBUF];
    int flag[NBUF]; /* 0 = dados; 1 = fim do corpo; -1 = a conexão caiu */
    int rpos, wpos, count;
    pthread_mutex_t m;
    pthread_cond_t cv;
    /* escritos só pela thread de rede; lidos com __atomic para as medições ao vivo */
    int64_t inflight; /* recebido e ainda não gravado */
    uint64_t recv_us, recv_calls, wait_disk_us;
    int64_t recv_bytes;
} xpipe_t;

#define AADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_RELAXED)
#define ALOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)

typedef struct {
    job_t *job;
    mac_t mac;
    int64_t done;        /* bytes já no console, somando todos os arquivos */
    int64_t fail_budget; /* teste: bytes até a falha simulada; 0 = desligado */
    int corrupt_next;    /* teste: estraga um byte do próximo arquivo */
    char err[256];
    char code[48];
    char *bufs[NBUF];
    size_t chunk;
    xstats_t st;
    http_conn_t conn;    /* conexão persistente com o Mac, para todos os arquivos */
    xpipe_t pipe;
    int pipe_on;
    pthread_t pipe_th;
    char dirs[8][1024];  /* pastas já criadas (evita mkdirs a cada arquivo) */
    int ndirs;
} ctx_t;

static void set_err(ctx_t *c, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_err(ctx_t *c, const char *code, const char *fmt, ...) {
    snprintf(c->code, sizeof(c->code), "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof(c->err), fmt, ap);
    va_end(ap);
}

static void *net_reader(void *arg) {
    xpipe_t *p = arg;
    while (1) {
        pthread_mutex_lock(&p->m);
        while (!p->stop && !p->active) pthread_cond_wait(&p->cv, &p->m);
        if (p->stop) {
            pthread_mutex_unlock(&p->m);
            return NULL;
        }
        http_resp_t *r = p->r;
        pthread_mutex_unlock(&p->m);

        int flag = 0;
        while (flag == 0) {
            pthread_mutex_lock(&p->m);
            uint64_t t0 = now_us();
            while (p->count == NBUF && !p->stop && !p->abort) pthread_cond_wait(&p->cv, &p->m);
            AADD(&p->wait_disk_us, now_us() - t0);
            if (p->stop || p->abort) {
                pthread_mutex_unlock(&p->m);
                break;
            }
            int k = p->wpos;
            pthread_mutex_unlock(&p->m);

            char *b = p->bufs[k];
            size_t len = 0;
            int fl = 0;
            while (len < p->cap) {
                if (ALOAD(&p->abort) || ALOAD(&p->stop)) break;
                uint64_t a = now_us();
                ssize_t n = http_read(r, b + len, p->cap - len);
                AADD(&p->recv_us, now_us() - a);
                AADD(&p->recv_calls, 1);
                if (n < 0) {
                    fl = -1;
                    break;
                }
                if (n == 0) {
                    fl = 1;
                    break;
                }
                len += (size_t)n;
                AADD(&p->recv_bytes, (int64_t)n);
                AADD(&p->inflight, (int64_t)n);
            }
            pthread_mutex_lock(&p->m);
            if (p->stop || p->abort) {
                pthread_mutex_unlock(&p->m);
                break;
            }
            p->len[k] = len;
            p->flag[k] = fl;
            p->wpos = (k + 1) % NBUF;
            p->count++;
            pthread_cond_broadcast(&p->cv);
            pthread_mutex_unlock(&p->m);
            flag = fl;
        }
        pthread_mutex_lock(&p->m);
        p->active = 0;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->m);
    }
}

static int pipe_start(ctx_t *c) {
    xpipe_t *p = &c->pipe;
    memset(p, 0, sizeof(*p));
    p->bufs = c->bufs;
    p->cap = c->chunk;
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->cv, NULL);
    if (pthread_create(&c->pipe_th, NULL, net_reader, p) != 0) {
        pthread_mutex_destroy(&p->m);
        pthread_cond_destroy(&p->cv);
        return -1;
    }
    c->pipe_on = 1;
    return 0;
}

static void pipe_stop(ctx_t *c) {
    if (!c->pipe_on) return;
    xpipe_t *p = &c->pipe;
    pthread_mutex_lock(&p->m);
    p->stop = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->m);
    pthread_join(c->pipe_th, NULL);
    pthread_mutex_destroy(&p->m);
    pthread_cond_destroy(&p->cv);
    c->pipe_on = 0;
}

static double mbps(int64_t bytes, uint64_t us) { return us > 0 ? (double)bytes / (double)us : 0; /* bytes/µs = MB/s */ }

/* Texto curto com as medições. */
static void stats_text(ctx_t *c, char *out, size_t cap) {
    xstats_t s = c->st;
    xpipe_t *p = &c->pipe;
    uint64_t recv_us = ALOAD(&p->recv_us), recv_calls = ALOAD(&p->recv_calls), wait_disk_us = ALOAD(&p->wait_disk_us);
    int64_t recv_bytes = ALOAD(&p->recv_bytes);
    double wall = s.start_us ? (double)(now_us() - s.start_us) / 1e6 : 0;
    const char *neck = recv_bytes == 0 ? "?" : (wait_disk_us > s.wait_net_us ? "disco" : "rede");
    snprintf(out, cap,
             "rede %.1f MB/s (recv médio %lld KB, buffer TCP %d KB) · disco %.1f MB/s (%lld escritas de %lld KB) · "
             "hash %.0f MB/s%s · esperando rede %.0f s, esperando disco %.0f s de %.0f s · gargalo: %s · bloco %lld KB · "
             "%d conexões, %d requisições",
             mbps(recv_bytes, recv_us), (long long)(recv_calls ? recv_bytes / (int64_t)recv_calls / 1024 : 0),
             c->conn.rcvbuf_eff >= 0 ? c->conn.rcvbuf_eff >> 10 : -1, mbps(s.write_bytes, s.write_us),
             (long long)s.write_calls, (long long)(s.write_calls ? s.write_bytes / (int64_t)s.write_calls / 1024 : 0),
             mbps(s.write_bytes, s.hash_us), sha256_impl() ? " SHA-NI" : "", (double)s.wait_net_us / 1e6,
             (double)wait_disk_us / 1e6, wall, neck, (long long)(c->chunk >> 10), c->conn.connects, c->conn.requests);
}

static void job_set_stats(ctx_t *c) {
    char text[sizeof(c->job->stats)];
    stats_text(c, text, sizeof(text));
    pthread_mutex_lock(&g_lock);
    snprintf(c->job->stats, sizeof(c->job->stats), "%s", text);
    c->job->connections = c->conn.connects;
    c->job->requests = c->conn.requests;
    pthread_mutex_unlock(&g_lock);
}

/* Baixa o corpo de `r` para `fd`. Devolve `r` à conexão persistente (se deu para ler o corpo
 * inteiro) ou o fecha, em qualquer caso. */
static result_t pipe_download(ctx_t *c, http_resp_t *r, int fd, const char *rel, sha256_ctx *hctx, int hashing) {
    xpipe_t *p = &c->pipe;
    pthread_mutex_lock(&p->m);
    p->r = r;
    p->rpos = p->wpos = p->count = 0;
    p->abort = 0;
    p->inflight = 0;
    p->active = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->m);

    result_t res = RES_OK;
    int body_end = 0;
    uint64_t last = 0, last_stats = 0;
    while (1) {
        pthread_mutex_lock(&p->m);
        uint64_t t0 = now_us();
        if (p->count == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts); /* pthread_cond_timedwait usa o relógio de verdade */
            ts.tv_nsec += 300 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&p->cv, &p->m, &ts);
        }
        c->st.wait_net_us += now_us() - t0;
        int have = p->count > 0;
        int k = p->rpos;
        size_t len = have ? p->len[k] : 0;
        int flag = have ? p->flag[k] : 0;
        pthread_mutex_unlock(&p->m);

        if (job_cancelled(c->job)) {
            res = RES_CANCEL;
            break;
        }
        if (have) {
            size_t wlen = len;
            if (c->fail_budget > 0 && (int64_t)wlen > c->fail_budget) wlen = (size_t)c->fail_budget;
            char *b = c->bufs[k];
            if (jobs_test_slow_ms > 0) usleep((useconds_t)jobs_test_slow_ms * 1000); /* teste: disco lento */
            if (c->corrupt_next && wlen) {
                b[0] ^= 0x5a; /* teste: dado estragado no caminho */
                c->corrupt_next = 0;
            }
            if (hashing && wlen) {
                uint64_t a = now_us();
                sha256_update(hctx, b, wlen);
                c->st.hash_us += now_us() - a;
            }
            size_t off = 0;
            int werr = 0;
            uint64_t a = now_us();
            while (off < wlen) {
                ssize_t w = write(fd, b + off, wlen - off);
                c->st.write_calls++;
                if (w < 0 && errno == EINTR) continue;
                if (w < 0) {
                    werr = errno;
                    break;
                }
                off += (size_t)w;
            }
            c->st.write_us += now_us() - a;
            c->st.write_bytes += (int64_t)off;
            c->done += (int64_t)off;
            AADD(&p->inflight, -(int64_t)len);
            if (werr) {
                set_err(c, werr == ENOSPC ? "no_space" : "io", "erro ao gravar %s: %s", rel, strerror(werr));
                res = RES_FAIL;
                break;
            }
            if (c->fail_budget > 0) {
                c->fail_budget -= (int64_t)wlen;
                if (c->fail_budget <= 0) {
                    set_err(c, "network", "interrupção simulada (teste)");
                    res = RES_FAIL;
                    break;
                }
            }
            pthread_mutex_lock(&p->m);
            p->rpos = (k + 1) % NBUF;
            p->count--;
            pthread_cond_broadcast(&p->cv);
            pthread_mutex_unlock(&p->m);
            if (flag == -1) {
                set_err(c, "network", "a conexão com o Mac caiu");
                res = RES_FAIL;
                break;
            }
            if (flag == 1) {
                body_end = 1;
                break;
            }
        }
        uint64_t now = now_ms();
        if (now - last > 300) {
            /* progresso conta o que já chegou (inclusive o que ainda está no buffer) */
            job_update(c->job, c->done + ALOAD(&p->inflight), NULL);
            last = now;
        }
        if (now - last_stats > 2000) {
            job_set_stats(c);
            last_stats = now;
        }
    }

    if (res == RES_OK && body_end) {
        /* a thread de rede já terminou este corpo; a conexão pode servir ao próximo arquivo */
        pthread_mutex_lock(&p->m);
        while (p->active) pthread_cond_wait(&p->cv, &p->m);
        pthread_mutex_unlock(&p->m);
        http_conn_release(&c->conn, r);
    } else {
        /* interrompe a thread de rede (pode estar parada num recv) e descarta a conexão */
        pthread_mutex_lock(&p->m);
        p->abort = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->m);
        http_abort(r);
        pthread_mutex_lock(&p->m);
        while (p->active) pthread_cond_wait(&p->cv, &p->m);
        pthread_mutex_unlock(&p->m);
        http_close(r);
    }
    AADD(&p->inflight, -ALOAD(&p->inflight));
    return res;
}

/* Conta longa de hash (conferência): mostra progresso e atende cancelar/pausar. */
typedef struct {
    ctx_t *c;
    uint64_t last;
} hash_prog_t;

static int hash_progress(void *arg, int64_t done) {
    hash_prog_t *h = arg;
    uint64_t now = now_ms();
    if (now - h->last >= 250) {
        job_phase_done(h->c->job, done);
        h->last = now;
    }
    return job_cancelled(h->c->job);
}

static result_t fetch_file(ctx_t *c, const char *revision, int index, int nth, int nfiles, const char *rel, int64_t size,
                           const char *sha, const char *target) {
    job_t *j = c->job;
    int64_t have = file_size(target);
    if (have < 0) have = 0;
    if (have > size) {
        /* maior que o esperado: não serve; run_copy contou só `size` dele */
        unlink(target);
        c->done -= size;
        have = 0;
    }
    job_file(j, rel, nth, nfiles);
    char header_sha[80] = "";
    /* confere o sha256 enquanto baixa; numa retomada, soma antes o que já estava no console.
     * Só quando o Mac informa o hash (a conferência é opcional lá): sem ele não há com o que
     * comparar, e a conta e a releitura na retomada seriam tempo perdido. */
    const int want_hash = sha && *sha;
    sha256_ctx hctx;
    int hashing = 0;
    if (have < size) {
        sha256_init(&hctx);
        hashing = want_hash;
        if (have > 0 && hashing) {
            /* antes de pedir ao Mac: a conta pode levar um tempo e a conexão ficaria parada */
            job_phase(j, PH_VERIFY_PARTIAL, have, "conferindo o que já tinha de %s", rel);
            hash_prog_t hp = {c, 0};
            int rc = sha256_add_file_cb(&hctx, target, have, hash_progress, &hp);
            if (rc == -2) return RES_CANCEL;
            if (rc != 0) hashing = 0; /* confere no fim, lendo o arquivo */
        }
        job_phase(j, PH_DOWNLOAD, j->total_bytes, "copiando %s", rel);
        job_update(j, c->done, NULL);
        char headers[512];
        int n = snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\nX-Ponte-Revision: %s\r\n",
                         c->mac.token, revision);
        if (have > 0) snprintf(headers + n, sizeof(headers) - (size_t)n, "Range: bytes=%lld-\r\n", (long long)have);
        char path[160];
        snprintf(path, sizeof(path), "/api/v1/items/%s/files/%d", j->item_id, index);
        http_resp_t *r = malloc(sizeof(*r)); /* 16 KB de buffer: fora da pilha */
        if (!r) {
            set_err(c, "io", "sem memória");
            return RES_FAIL;
        }
        if (http_conn_request(&c->conn, "GET", path, headers, NULL, 0, r) != 0) {
            free(r);
            set_err(c, "network", "a conexão com o Mac caiu");
            return RES_FAIL;
        }
        if (r->status == 412) {
            http_close(r);
            free(r);
            return RES_REVISION;
        }
        if (r->status != 200 && r->status != 206) {
            char *body = NULL;
            int status = r->status;
            if (http_read_all(r, &body, NULL, 65536) == 0) http_conn_release(&c->conn, r);
            else http_close(r);
            free(r);
            set_err(c, "http", "%s: o Mac respondeu %d %s", rel, status, body ? body : "");
            free(body);
            return RES_FAIL;
        }
        int flags = O_WRONLY | O_CREAT;
        if (r->status == 200) {
            /* sem Range (ou o servidor ignorou): recomeça este arquivo */
            flags |= O_TRUNC;
            c->done -= have;
            have = 0;
            sha256_init(&hctx);
            hashing = want_hash;
        } else {
            flags |= O_APPEND;
        }
        snprintf(header_sha, sizeof(header_sha), "%s", r->sha256);
        int fd = open(target, flags, 0666);
        if (fd < 0) {
            http_close(r);
            free(r);
            set_err(c, "io", "não foi possível gravar %s: %s", rel, strerror(errno));
            return RES_FAIL;
        }
        result_t dres = pipe_download(c, r, fd, rel, &hctx, hashing);
        free(r);
        /* um fsync por arquivo: o que foi conferido está mesmo no disco antes de ir para homebrew/ */
        if (dres == RES_OK && fsync(fd) != 0 && errno != EINVAL) {
            set_err(c, errno == ENOSPC ? "no_space" : "io", "erro ao gravar %s: %s", rel, strerror(errno));
            dres = RES_FAIL;
        }
        close(fd);
        if (dres != RES_OK) return dres;
    }
    if (size == 0 && file_size(target) < 0) {
        /* arquivo vazio: não há o que baixar, só criar */
        int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            set_err(c, "io", "não foi possível gravar %s: %s", rel, strerror(errno));
            return RES_FAIL;
        }
        close(fd);
    }
    int64_t final_size = file_size(target);
    if (final_size != size) {
        set_err(c, "size_mismatch", "%s: tamanho %lld, esperado %lld", rel, (long long)final_size, (long long)size);
        return RES_FAIL;
    }
    const char *expected = (sha && *sha) ? sha : (header_sha[0] ? header_sha : NULL);
    if (expected) {
        char hex[65];
        int ok;
        if (hashing) {
            sha256_final_hex(&hctx, hex); /* já calculado durante o download */
            ok = 1;
        } else {
            job_phase(j, PH_VERIFY, size, "conferindo %s", rel);
            hash_prog_t hp = {c, 0};
            int rc = sha256_file_cb(target, hex, hash_progress, &hp);
            if (rc == -2) return RES_CANCEL;
            ok = rc == 0;
        }
        if (!ok || strcmp(hex, expected) != 0) {
            unlink(target);
            c->done -= size;
            job_update(j, c->done, NULL);
            set_err(c, "hash_mismatch", "%s: o arquivo chegou diferente do que está no Mac (sha256 não confere)", rel);
            return RES_HASH;
        }
    }
    return RES_OK;
}

static int storage_by_id(const char *id, storage_t *out) {
    storage_t st[PONTE_MAX_STORAGES];
    int n = plat_storages(st, PONTE_MAX_STORAGES);
    for (int i = 0; i < n; i++) {
        if (strcmp(st[i].id, id) == 0) {
            *out = st[i];
            return 0;
        }
    }
    return -1;
}

static int64_t free_bytes(const char *path) {
    struct statfs s;
    if (statfs(path, &s) != 0) return -1;
    return (int64_t)s.f_bavail * (int64_t)s.f_bsize;
}

/* Dados do trabalho copiados sob a trava: depois que ele termina, a posição pode ser reaproveitada. */
typedef struct {
    int seq;
    char item_id[20];
    char title[192];
    char kind[16];
    char storage[16];
    char message[256];
    char state[16];
    uint64_t started_ms;
} job_snap_t;

static void send_report(const mac_t *mac, const job_snap_t *s, const char *result, const char *detail) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "result", result);
    if (s->storage[0] && !is_pkg(s->kind)) cJSON_AddStringToObject(b, "storage", s->storage);
    else cJSON_AddNullToObject(b, "storage");
    if (detail) cJSON_AddStringToObject(b, "detail", detail);
    else cJSON_AddNullToObject(b, "detail");
    cJSON_AddNumberToObject(b, "duration_s", (double)((now_ms() - s->started_ms) / 1000));
    char path[128];
    snprintf(path, sizeof(path), "/api/v1/items/%s/report", s->item_id);
    cJSON *r = ponte_call(mac, "POST", path, b, NULL, NULL, 0);
    cJSON_Delete(r);
    cJSON_Delete(b);
}

/* Pede ao montador uma varredura nova de homebrew/ (ShadowMountPlus: POST /api/v1/scan).
 * Sozinho ele já varre a cada ~15 s; isto só adianta e zera as tentativas que falharam antes
 * (reset_attempts), para um jogo substituído não ficar esperando. Falhar aqui não muda o
 * resultado da cópia: o jogo já está em homebrew/. */
static void mounter_rescan(int seq) {
    char spec[sizeof(jobs_mounter_rescan)];
    pthread_mutex_lock(&g_lock);
    snprintf(spec, sizeof(spec), "%s", jobs_mounter_rescan);
    pthread_mutex_unlock(&g_lock);
    if (!spec[0]) return;

    char host[64] = "", path[128] = "/";
    int port = 80;
    const char *slash = strchr(spec, '/');
    size_t hostlen = slash ? (size_t)(slash - spec) : strlen(spec);
    if (slash) snprintf(path, sizeof(path), "%s", slash);
    char hostport[sizeof(host)];
    if (hostlen >= sizeof(hostport)) hostlen = sizeof(hostport) - 1;
    memcpy(hostport, spec, hostlen);
    hostport[hostlen] = 0;
    char *colon = strrchr(hostport, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    memcpy(host, hostport, sizeof(host));
    if (!host[0] || port <= 0 || port > 65535) {
        plat_log("[job %d] montador: endereço inválido nos ajustes (%s)", seq, spec);
        return;
    }

    static const char body[] = "{\"reset_attempts\":true}";
    http_resp_t r;
    if (http_open(host, port, "POST", path, "Content-Type: application/json\r\n", body, sizeof(body) - 1, &r, 5) != 0) {
        plat_log("[job %d] montador: sem resposta em %s:%d (ShadowMountPlus rodando com a API ligada?)", seq, host, port);
        return;
    }
    char *text = NULL;
    size_t len = 0;
    http_read_all(&r, &text, &len, 4096);
    plat_log("[job %d] montador: varredura pedida, HTTP %d %s", seq, r.status, text ? text : "");
    free(text);
    http_close(&r);
}

/* Instala um .pkg: o instalador do sistema baixa direto do Mac (SPEC §8 item 7). */
static result_t run_pkg(ctx_t *c, cJSON *detail) {
    cJSON *files = cJSON_GetObjectItem(detail, "files");
    cJSON *f0 = cJSON_GetArrayItem(files, 0);
    cJSON *p = f0 ? cJSON_GetObjectItem(f0, "path") : NULL;
    char name[160];
    safe_name(name, sizeof(name), cJSON_IsString(p) ? p->valuestring : "pacote.pkg");
    char ename[480], etoken[200];
    url_encode(ename, sizeof(ename), name);
    url_encode(etoken, sizeof(etoken), c->mac.token);
    char url[1024];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/items/%s/files/0/%s?token=%s", c->mac.host, c->mac.port,
             c->job->item_id, ename, etoken);
    job_file(c->job, name, 1, 1);
    job_phase(c->job, PH_INSTALL, 0, "entregando ao instalador do sistema");
    char title[192];
    pthread_mutex_lock(&g_lock);
    snprintf(title, sizeof(title), "%s", c->job->title);
    pthread_mutex_unlock(&g_lock);
    char err[200] = "";
    if (plat_install_pkg(url, title, err, sizeof(err)) != 0) {
        set_err(c, "install_refused", "o instalador recusou: %s", err);
        return RES_FAIL;
    }
    return RES_OK;
}

/* mkdirs com memória das pastas já criadas nesta cópia. */
static int ensure_dir(ctx_t *c, const char *dir) {
    for (int i = 0; i < c->ndirs; i++)
        if (strcmp(c->dirs[i], dir) == 0) return 0;
    if (mkdirs(dir) != 0) return -1;
    int k = c->ndirs < 8 ? c->ndirs++ : (int)(now_us() % 8);
    snprintf(c->dirs[k], sizeof(c->dirs[k]), "%s", dir);
    return 0;
}

static result_t run_copy(ctx_t *c, cJSON *detail, char *final_out, size_t final_cap) {
    job_t *j = c->job;
    storage_t st;
    if (storage_by_id(j->storage, &st) != 0) {
        set_err(c, "no_storage", "armazenamento \"%s\" não encontrado", j->storage);
        return RES_FAIL;
    }
    cJSON *rev = cJSON_GetObjectItem(detail, "revision");
    cJSON *files = cJSON_GetObjectItem(detail, "files");
    if (!cJSON_IsString(rev) || !cJSON_IsArray(files) || cJSON_GetArraySize(files) == 0) {
        set_err(c, "bad_item", "resposta do Mac sem arquivos");
        return RES_FAIL;
    }
    int nfiles = cJSON_GetArraySize(files);
    for (int i = 0; i < nfiles; i++) {
        cJSON *f = cJSON_GetArrayItem(files, i);
        cJSON *p = cJSON_GetObjectItem(f, "path");
        cJSON *sz = cJSON_GetObjectItem(f, "size");
        if (!cJSON_IsString(p) || !safe_relpath(p->valuestring)) {
            set_err(c, "unsafe_path", "caminho recusado vindo do Mac");
            return RES_FAIL;
        }
        if (!cJSON_IsNumber(sz) || sz->valuedouble < 0) {
            set_err(c, "bad_item", "resposta do Mac sem o tamanho de %s", p->valuestring);
            return RES_FAIL;
        }
    }

    /* área temporária fora de homebrew/ (o montador não vigia) */
    char tmp[512], data[600], revfile[600], homebrew[512];
    snprintf(tmp, sizeof(tmp), "%s/ponte/tmp/%s", st.root, j->item_id);
    snprintf(data, sizeof(data), "%s/data", tmp);
    snprintf(revfile, sizeof(revfile), "%s/.revision", tmp);
    snprintf(homebrew, sizeof(homebrew), "%s/homebrew", st.root);

    char *old = read_file(revfile, 64, NULL);
    if (is_dir(tmp) && (!old || strcmp(old, rev->valuestring) != 0)) rmtree(tmp);
    free(old);
    if (ensure_dir(c, data) != 0 || write_file_atomic(revfile, rev->valuestring, strlen(rev->valuestring)) != 0) {
        set_err(c, "io", "não foi possível criar %s", tmp);
        return RES_FAIL;
    }

    /* destino final */
    char name[256];
    int folder = in_list(j->kind, FOLDER_KINDS);
    if (folder) {
        safe_name(name, sizeof(name), j->title_id[0] ? j->title_id : j->title);
    } else {
        const char *p0 = cJSON_GetObjectItem(cJSON_GetArrayItem(files, 0), "path")->valuestring;
        const char *base = strrchr(p0, '/');
        safe_name(name, sizeof(name), base ? base + 1 : p0);
    }
    if (path_join(final_out, final_cap, homebrew, name) != 0) {
        set_err(c, "unsafe_path", "caminho longo demais: %s", name);
        return RES_FAIL;
    }
    if (path_exists(final_out) && !j->replace) {
        set_err(c, "destination_exists", "já existe %s/homebrew/%s", st.label, name);
        return RES_FAIL;
    }

    /* espaço: o que falta + 5% */
    int64_t have = 0, total = 0;
    for (int i = 0; i < nfiles; i++) {
        cJSON *f = cJSON_GetArrayItem(files, i);
        int64_t size = (int64_t)cJSON_GetObjectItem(f, "size")->valuedouble;
        char target[1024];
        if (path_join(target, sizeof(target), data, cJSON_GetObjectItem(f, "path")->valuestring) != 0) {
            set_err(c, "unsafe_path", "caminho longo demais vindo do Mac");
            return RES_FAIL;
        }
        int64_t s = file_size(target);
        if (s > 0) have += s < size ? s : size;
        total += size;
    }
    int64_t free_b = free_bytes(st.root);
    int64_t need = (total - have) + (total - have) / 20;
    if (free_b >= 0 && free_b < need) {
        set_err(c, "no_space", "espaço insuficiente em %s: precisa de %lld MB, livre %lld MB", st.label,
                (long long)(need / 1000000), (long long)(free_b / 1000000));
        return RES_FAIL;
    }
    c->done = have;
    pthread_mutex_lock(&g_lock);
    j->total_bytes = total;
    j->done_bytes = have;
    j->session_start = have;
    rate_reset(&j->rate, now_ms(), have);
    j->quarter = (int)(have * 4 / (total > 0 ? total : 1)); /* numa retomada, não repete avisos */
    pthread_mutex_unlock(&g_lock);

    /* buffers da cópia; sem memória para o tamanho pedido, tenta menores */
    size_t chunk = jobs_chunk_bytes > 0 ? (size_t)jobs_chunk_bytes : (4u << 20);
    while (1) {
        int ok = 1;
        for (int i = 0; i < NBUF; i++)
            if (!(c->bufs[i] = malloc(chunk))) ok = 0;
        if (ok) break;
        for (int i = 0; i < NBUF; i++) {
            free(c->bufs[i]);
            c->bufs[i] = NULL;
        }
        if (chunk <= (256u << 10)) {
            set_err(c, "io", "sem memória");
            return RES_FAIL;
        }
        chunk /= 2;
    }
    c->chunk = chunk;
    memset(&c->st, 0, sizeof(c->st));
    c->st.start_us = now_us();
    pthread_mutex_lock(&g_lock);
    j->stats[0] = '\0';
    pthread_mutex_unlock(&g_lock);
    result_t res = RES_OK;
    if (pipe_start(c) != 0) {
        set_err(c, "io", "não foi possível criar a thread de rede");
        res = RES_FAIL;
    }
    for (int i = 0; i < nfiles && res == RES_OK; i++) {
        cJSON *f = cJSON_GetArrayItem(files, i);
        const char *rel = cJSON_GetObjectItem(f, "path")->valuestring;
        cJSON *idx = cJSON_GetObjectItem(f, "index");
        cJSON *sha = cJSON_GetObjectItem(f, "sha256");
        int64_t size = (int64_t)cJSON_GetObjectItem(f, "size")->valuedouble;
        char target[1024], parent[1024];
        path_join(target, sizeof(target), data, rel); /* já conferido acima */
        snprintf(parent, sizeof(parent), "%s", target);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = '\0';
        if (ensure_dir(c, parent) != 0) {
            set_err(c, "io", "não foi possível criar a pasta de %s", rel);
            res = RES_FAIL;
            break;
        }
        int index = cJSON_IsNumber(idx) ? idx->valueint : i;
        const char *want_sha = cJSON_IsString(sha) ? sha->valuestring : NULL;
        if (jobs_test_corrupt > 0 && size > 0) {
            AADD(&jobs_test_corrupt, -1);
            c->corrupt_next = 1;
        }
        res = fetch_file(c, rev->valuestring, index, i + 1, nfiles, rel, size, want_sha, target);
        if (res == RES_HASH) {
            /* o arquivo chegou estragado (rede ou disco): baixa de novo, do zero, uma vez */
            plat_log("[job %d] %s; baixando de novo", j->seq, c->err);
            job_update(j, c->done, "%s não conferiu; baixando de novo", rel);
            if (jobs_test_corrupt > 0) {
                AADD(&jobs_test_corrupt, -1);
                c->corrupt_next = 1;
            }
            res = fetch_file(c, rev->valuestring, index, i + 1, nfiles, rel, size, want_sha, target);
            if (res == RES_HASH) {
                set_err(c, "hash_mismatch",
                        "%s chegou diferente do que está no Mac duas vezes seguidas; o arquivo foi apagado. "
                        "Confira a rede e o disco de destino e tente de novo", rel);
                res = RES_FAIL;
            }
        }
    }
    pipe_stop(c);
    http_conn_close(&c->conn);
    if (c->conn.requests) {
        job_set_stats(c);
        plat_log("[job %d] %s", j->seq, j->stats);
    }
    for (int i = 0; i < NBUF; i++) {
        free(c->bufs[i]);
        c->bufs[i] = NULL;
    }
    if (res == RES_REVISION) rmtree(tmp); /* o item mudou no Mac: recomeça do zero */
    if (res != RES_OK) return res;

    /* tudo conferido: move para homebrew/ (renomeação no mesmo disco) */
    job_update(j, total, NULL);
    job_file(j, name, nfiles, nfiles);
    if (mkdirs(homebrew) != 0) {
        set_err(c, "io", "não foi possível criar %s", homebrew);
        return RES_FAIL;
    }
    if (path_exists(final_out)) {
        job_phase(j, PH_MOVE, 0, "apagando a cópia antiga em homebrew/%s", name);
        if (rmtree(final_out) != 0) {
            set_err(c, "io", "não foi possível substituir %s", final_out);
            return RES_FAIL;
        }
    }
    job_phase(j, PH_MOVE, 0, "movendo para homebrew/");
    char src[1024];
    if (folder) {
        snprintf(src, sizeof(src), "%s", data);
    } else if (path_join(src, sizeof(src), data,
                         cJSON_GetObjectItem(cJSON_GetArrayItem(files, 0), "path")->valuestring) != 0) {
        set_err(c, "unsafe_path", "caminho longo demais vindo do Mac");
        return RES_FAIL;
    }
    if (rename(src, final_out) != 0) {
        set_err(c, "io", "não foi possível mover para %s: %s", final_out, strerror(errno));
        return RES_FAIL;
    }
    rmtree(tmp);
    return RES_OK;
}

static void run_job(job_t *j) {
    ctx_t *c = calloc(1, sizeof(ctx_t)); /* buffers grandes: fora da pilha da thread */
    if (!c) {
        pthread_mutex_lock(&g_lock);
        j->state = JOB_PAUSED;
        j->next_try_ms = now_ms() + RETRY_MS;
        snprintf(j->message, sizeof(j->message), "pausado: sem memória; continua sozinho");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    c->job = j;
    app_get_mac(&c->mac);
    /* downloads: buffer TCP dos ajustes, 20 s sem dados = o Mac sumiu (pausa e tenta de novo) */
    http_conn_init(&c->conn, c->mac.host, c->mac.port, -1, 20);
    if (jobs_test_fail_after > 0) {
        c->fail_budget = jobs_test_fail_after;
        jobs_test_fail_after = 0;
    }
    char item_id[20];
    pthread_mutex_lock(&g_lock);
    snprintf(item_id, sizeof(item_id), "%s", j->item_id);
    pthread_mutex_unlock(&g_lock);
    job_phase(j, PH_QUERY, 0, "consultando o Mac");

    result_t res = RES_FAIL;
    char final[1024] = "";
    for (int attempt = 0; attempt < 2; attempt++) {
        char path[96];
        snprintf(path, sizeof(path), "/api/v1/items/%s", item_id);
        int status = 0;
        char err[256] = "";
        cJSON *detail = ponte_call(&c->mac, "GET", path, NULL, &status, err, sizeof(err));
        if (!detail || status != 200) {
            set_err(c, status == 409 ? "item_not_ready" : (status == 0 ? "network" : "http"), "%s",
                    err[0] ? err : "o Mac não respondeu");
            cJSON_Delete(detail);
            res = RES_FAIL;
            break;
        }
        res = is_pkg(j->kind) ? run_pkg(c, detail) : run_copy(c, detail, final, sizeof(final));
        cJSON_Delete(detail);
        if (res != RES_REVISION) break;
        job_update(j, 0, "o item mudou no Mac; recomeçando");
        if (attempt == 1) {
            set_err(c, "revision_changed", "o item mudou no Mac durante a cópia");
            res = RES_FAIL;
        }
    }
    if (res == RES_HASH) res = RES_FAIL;

    /* o Mac sumiu (desligou, dormiu, rede caiu) ou o item ainda está sendo preparado:
     * pausa e tenta de novo sozinho; o que já veio fica em ponte/tmp/ */
    int auto_pause = res == RES_FAIL && (strcmp(c->code, "network") == 0 || strcmp(c->code, "item_not_ready") == 0 ||
                                        strcmp(c->code, "no_storage") == 0); /* USB ainda não montado */
    job_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    pthread_mutex_lock(&g_lock);
    if (res != RES_OK && c->done > 0) j->done_bytes = c->done; /* o que já está no console */
    int paused_by_you = res == RES_CANCEL && j->pause_req;
    j->cancel = 0;
    j->pause_req = 0;
    j->phase = PH_NONE;
    j->connections = c->conn.connects;
    j->requests = c->conn.requests;
    if (paused_by_you || auto_pause) {
        j->state = JOB_PAUSED;
        j->manual_pause = paused_by_you;
        j->next_try_ms = now_ms() + RETRY_MS;
        if (paused_by_you) snprintf(j->message, sizeof(j->message), "pausado");
        else snprintf(j->message, sizeof(j->message), "pausado: %.200s; continua sozinho", c->err[0] ? c->err : "sem resposta do Mac");
        snprintf(snap.message, sizeof(snap.message), "%s", j->message);
        snap.seq = j->seq;
        pthread_mutex_unlock(&g_lock);
        save_queue();
        plat_log("[job %d] pausado: %s", snap.seq, snap.message);
        free(c);
        return;
    }
    switch (res) {
    case RES_OK:
        j->state = JOB_DONE;
        j->done_bytes = j->total_bytes;
        if (is_pkg(j->kind)) snprintf(j->message, sizeof(j->message), "instalação iniciada no console");
        else {
            double secs = (double)(now_ms() - j->started_ms) / 1000.0;
            double mbs = secs > 0.5 ? (double)(j->total_bytes - j->session_start) / secs / 1e6 : 0;
            if (mbs > 0) snprintf(j->message, sizeof(j->message), "pronto em %s (média %.1f MB/s)", final, mbs);
            else snprintf(j->message, sizeof(j->message), "pronto em %s", final);
        }
        break;
    case RES_CANCEL:
        j->state = JOB_CANCELLED;
        snprintf(j->message, sizeof(j->message), "cancelado (o que já veio fica guardado para continuar)");
        break;
    default:
        j->state = JOB_FAILED;
        snprintf(j->message, sizeof(j->message), "%s", c->err[0] ? c->err : "falhou");
        snprintf(j->error, sizeof(j->error), "%s", c->code[0] ? c->code : "io");
        break;
    }
    /* daqui em diante a posição do trabalho pode ser reaproveitada: só a cópia `snap` vale */
    snap.seq = j->seq;
    snprintf(snap.item_id, sizeof(snap.item_id), "%s", j->item_id);
    snprintf(snap.title, sizeof(snap.title), "%s", j->title);
    snprintf(snap.kind, sizeof(snap.kind), "%s", j->kind);
    snprintf(snap.storage, sizeof(snap.storage), "%s", j->storage);
    snprintf(snap.message, sizeof(snap.message), "%s", j->message);
    snprintf(snap.state, sizeof(snap.state), "%s", state_name(j->state));
    snap.started_ms = j->started_ms;
    pthread_mutex_unlock(&g_lock);
    save_queue();

    if (res == RES_OK) {
        send_report(&c->mac, &snap, is_pkg(snap.kind) ? "installed" : "copied", NULL);
        plat_notify(is_pkg(snap.kind) ? "Ponte: instalando %s" : "Ponte: %s copiado", snap.title);
        if (!is_pkg(snap.kind)) mounter_rescan(snap.seq);
    } else if (res == RES_CANCEL) {
        send_report(&c->mac, &snap, "cancelled", NULL);
    } else {
        char detail[320];
        snprintf(detail, sizeof(detail), "%s: %s", c->code, c->err);
        send_report(&c->mac, &snap, "failed", detail);
        plat_notify("Ponte: falha em %s", snap.title);
    }
    plat_log("[job %d] %s: %s", snap.seq, snap.state, snap.message);
    free(c);
}

static int runnable(const job_t *j, uint64_t now) {
    if (!j->used) return 0;
    if (j->state == JOB_QUEUED) return 1;
    return j->state == JOB_PAUSED && !j->manual_pause && now >= j->next_try_ms;
}

static void *worker(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_lock);
        job_t *next = NULL;
        while (!next) {
            uint64_t now = now_ms();
            /* só começa com o Mac no ar e pareado */
            if (app_mac_ready()) {
                /* o mais antigo da fila; dentro do mesmo jogo, base → atualização → DLC (SPEC §8 item 8) */
                for (int i = 0; i < g_count; i++)
                    if (runnable(&g_jobs[i], now) && (!next || g_jobs[i].seq < next->seq)) next = &g_jobs[i];
                if (next && next->title_id[0]) {
                    job_t *first = next;
                    for (int i = 0; i < g_count; i++) {
                        job_t *c = &g_jobs[i];
                        if (!runnable(c, now) || strcmp(c->title_id, first->title_id) != 0) continue;
                        int rc = content_rank(c->content), rn = content_rank(next->content);
                        if (rc < rn || (rc == rn && c->seq < next->seq)) next = c;
                    }
                }
            }
            if (!next) {
                /* pthread_cond_timedwait usa o relógio de verdade; a espera em si é curta */
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 2;
                pthread_cond_timedwait(&g_cond, &g_lock, &ts);
            }
        }
        next->state = JOB_RUNNING;
        next->started_ms = now_ms();
        next->started_wall = wall_ms();
        next->error[0] = '\0';
        next->phase = PH_NONE;
        next->file[0] = '\0';
        next->file_index = next->file_count = 0;
        next->connections = next->requests = 0;
        rate_reset(&next->rate, next->started_ms, next->done_bytes);
        pthread_mutex_unlock(&g_lock);
        save_queue();
        run_job(next);
    }
    return NULL;
}

/* ---------- fila guardada no console ---------- */

static void queue_path(char *out, size_t cap) { path_join(out, cap, plat_data_dir(), "queue.json"); }

/* Grava o que ainda não terminou, para retomar depois de desligar o console. */
static void save_queue(void) {
    pthread_mutex_lock(&g_save_lock);
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_count; i++) {
        job_t *j = &g_jobs[i];
        if (!j->used || j->state >= JOB_DONE) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", j->seq);
        cJSON_AddStringToObject(o, "item_id", j->item_id);
        cJSON_AddStringToObject(o, "title", j->title);
        cJSON_AddStringToObject(o, "kind", j->kind);
        cJSON_AddStringToObject(o, "content", j->content);
        cJSON_AddStringToObject(o, "title_id", j->title_id);
        cJSON_AddStringToObject(o, "storage", j->storage);
        cJSON_AddBoolToObject(o, "replace", j->replace);
        cJSON_AddNumberToObject(o, "total", (double)j->total_bytes);
        cJSON_AddNumberToObject(o, "done", (double)j->done_bytes);
        cJSON_AddBoolToObject(o, "manual_pause", j->state == JOB_PAUSED && j->manual_pause);
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&g_lock);
    char *text = cJSON_Print(arr);
    cJSON_Delete(arr);
    char path[512];
    queue_path(path, sizeof(path));
    if (text) write_file_atomic(path, text, strlen(text));
    free(text);
    pthread_mutex_unlock(&g_save_lock);
}

/* Traz de volta a fila gravada: o que estava copiando ou na fila volta a andar sozinho;
 * o que você pausou continua pausado. */
static int load_queue(void) {
    char path[512];
    queue_path(path, sizeof(path));
    char *text = read_file(path, 1 << 20, NULL);
    if (!text) return 0;
    cJSON *arr = cJSON_Parse(text);
    free(text);
    int n = 0;
    cJSON *o;
    pthread_mutex_lock(&g_lock);
    cJSON_ArrayForEach(o, arr) {
        if (n >= g_count) break;
        cJSON *v;
        job_t *j = &g_jobs[n];
        memset(j, 0, sizeof(*j));
        if (!cJSON_IsString(v = cJSON_GetObjectItem(o, "item_id")) || !valid_id(v->valuestring)) continue;
        snprintf(j->item_id, sizeof(j->item_id), "%s", v->valuestring);
#define STR(field, key) if (cJSON_IsString(v = cJSON_GetObjectItem(o, key))) snprintf(j->field, sizeof(j->field), "%s", v->valuestring)
        STR(title, "title");
        STR(kind, "kind");
        STR(content, "content");
        STR(title_id, "title_id");
        STR(storage, "storage");
#undef STR
        j->replace = cJSON_IsTrue(cJSON_GetObjectItem(o, "replace"));
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "total"))) j->total_bytes = (int64_t)v->valuedouble;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(o, "done"))) j->done_bytes = (int64_t)v->valuedouble;
        j->used = 1;
        j->seq = ++g_seq;
        j->manual_pause = cJSON_IsTrue(cJSON_GetObjectItem(o, "manual_pause"));
        j->state = j->manual_pause ? JOB_PAUSED : JOB_QUEUED;
        snprintf(j->message, sizeof(j->message), j->manual_pause ? "pausado" : "retomando de onde parou");
        n++;
    }
    pthread_mutex_unlock(&g_lock);
    cJSON_Delete(arr);
    return n;
}

void jobs_init(void) {
    int restored = load_queue();
    if (restored > 0) {
        plat_log("fila restaurada: %d item(ns)", restored);
        plat_notify("Ponte: retomando %d cópia(s) pendente(s)", restored);
    }
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    pthread_detach(t);
}
