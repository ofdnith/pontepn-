#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "platform.h"

static void set_timeouts(int fd, int seconds) {
    struct timeval tv = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int send_all(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n > 0) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, p, n, 0);
#endif
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int http_rcvbuf = 4 << 20;

/* Pede o buffer de recepção; se o sistema recusar (no FreeBSD, acima de kern.ipc.maxsockbuf a chamada
 * falha e nada muda), tenta pela metade até 256 KB. Devolve o que ficou valendo, para o diagnóstico. */
static int set_rcvbuf(int fd, int want) {
    static int last_logged = -2;
    int asked = want;
    while (want >= (256 << 10)) {
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want)) == 0) break;
        want /= 2;
    }
    int eff = -1;
    socklen_t len = sizeof(eff);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &eff, &len) != 0) eff = -1;
    if (eff != __atomic_load_n(&last_logged, __ATOMIC_RELAXED)) {
        __atomic_store_n(&last_logged, eff, __ATOMIC_RELAXED);
        plat_log("SO_RCVBUF: pedido %d KB, valendo %d KB", asked >> 10, eff >= 0 ? eff >> 10 : -1);
    }
    return eff;
}

/* Conecta. `rcvbuf`: -1 = http_rcvbuf; 0 = padrão do sistema. `eff_out` recebe o buffer que valeu. */
static int tcp_connect(const char *host, int port, int timeout_s, int rcvbuf, int *eff_out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    /* buffer de recepção maior (antes de conectar, para valer na janela do TCP):
     * rende mais em Wi-Fi e em downloads longos. 0 = deixa o padrão do sistema (com autoajuste). */
    if (rcvbuf < 0) rcvbuf = http_rcvbuf;
    int eff = -1;
    if (rcvbuf > 0) {
        eff = set_rcvbuf(fd, rcvbuf);
    } else {
        socklen_t len = sizeof(eff);
        if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &eff, &len) != 0) eff = -1;
    }
    if (eff_out) *eff_out = eff;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        struct pollfd p = {fd, POLLOUT, 0};
        if (poll(&p, 1, timeout_s * 1000) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);
    set_timeouts(fd, timeout_s);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

/* Lê do socket até achar o fim dos cabeçalhos. Devolve o tamanho do cabeçalho (com \r\n\r\n). */
static int read_head(int fd, char *buf, size_t cap, size_t *got) {
    *got = 0;
    while (*got < cap) {
        ssize_t n = recv(fd, buf + *got, cap - *got, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        *got += (size_t)n;
        for (size_t i = 3; i < *got; i++) {
            if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') return (int)(i + 1);
        }
    }
    return -1;
}

/* `tok` aparece em `s` (sem diferenciar maiúsculas)? strcasestr não é padrão em todo lugar. */
static int has_token(const char *s, const char *tok) {
    size_t n = strlen(tok);
    for (; *s; s++)
        if (strncasecmp(s, tok, n) == 0) return 1;
    return 0;
}

static void copy_value(char *dst, size_t cap, const char *v, const char *end) {
    while (v < end && (*v == ' ' || *v == '\t')) v++;
    size_t n = (size_t)(end - v);
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\r')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, v, n);
    dst[n] = '\0';
}

/* Envia a requisição em `fd` e lê os cabeçalhos da resposta para `r`. Não fecha `fd`.
 * 0 = ok; -1 = falha ao enviar; -2 = a conexão fechou antes de chegar qualquer byte da resposta
 * (numa conexão reaproveitada, quer dizer que o servidor a fechou enquanto estava ociosa); -3 = erro depois. */
static int send_and_parse(int fd, const char *host, int port, const char *method, const char *path,
                          const char *headers, const char *body, size_t body_len, int keep_alive, http_resp_t *r) {
    int rcv = r->rcvbuf_eff;
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->content_length = -1;
    r->rcvbuf_eff = rcv;
    char head[2048];
    int n = snprintf(head, sizeof(head),
                     "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: %s\r\nX-Ponte-Protocol: 1\r\n"
                     "User-Agent: ponte-ps5/0.1\r\n%s",
                     method, path, host, port, keep_alive ? "keep-alive" : "close", headers ? headers : "");
    if (n < 0 || (size_t)n >= sizeof(head)) return -1;
    char cl[96];
    if (body) {
        snprintf(cl, sizeof(cl), "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n", body_len);
    } else {
        snprintf(cl, sizeof(cl), "\r\n");
    }
    if (send_all(fd, head, (size_t)n) != 0 || send_all(fd, cl, strlen(cl)) != 0 ||
        (body && body_len && send_all(fd, body, body_len) != 0))
        return -1;
    size_t got = 0;
    int hlen = read_head(fd, r->buf, sizeof(r->buf), &got);
    if (hlen < 0) return got == 0 ? -2 : -3;
    /* linha de status */
    int minor = 1;
    if (sscanf(r->buf, "HTTP/1.%d %d", &minor, &r->status) != 2) return -3;
    int conn_close = minor == 0, conn_keep = 0;
    const char *line = strstr(r->buf, "\r\n");
    const char *end_head = r->buf + hlen - 2;
    while (line && line < end_head) {
        line += 2;
        const char *eol = strstr(line, "\r\n");
        if (!eol || eol > end_head) break;
        const char *colon = memchr(line, ':', (size_t)(eol - line));
        if (colon) {
            size_t klen = (size_t)(colon - line);
            const char *v = colon + 1;
            if (klen == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
                char tmp[32];
                copy_value(tmp, sizeof(tmp), v, eol);
                r->content_length = strtoll(tmp, NULL, 10);
            } else if (klen == 12 && strncasecmp(line, "Content-Type", 12) == 0) {
                copy_value(r->content_type, sizeof(r->content_type), v, eol);
            } else if (klen == 16 && strncasecmp(line, "X-Ponte-Revision", 16) == 0) {
                copy_value(r->revision, sizeof(r->revision), v, eol);
            } else if (klen == 14 && strncasecmp(line, "X-Ponte-Sha256", 14) == 0) {
                copy_value(r->sha256, sizeof(r->sha256), v, eol);
            } else if (klen == 10 && strncasecmp(line, "Connection", 10) == 0) {
                char tmp[64];
                copy_value(tmp, sizeof(tmp), v, eol);
                if (has_token(tmp, "close")) conn_close = 1;
                if (has_token(tmp, "keep-alive")) conn_keep = 1;
            }
        }
        line = eol;
    }
    /* HTTP/1.1 é persistente por padrão; HTTP/1.0 só com keep-alive explícito */
    r->keep_alive = keep_alive && !conn_close && (minor >= 1 || conn_keep) && r->content_length >= 0;
    r->fd = fd;
    /* o que veio junto com o cabeçalho já é corpo */
    r->buf_pos = (size_t)hlen;
    r->buf_len = got;
    return 0;
}

int http_open(const char *host, int port, const char *method, const char *path, const char *headers,
              const char *body, size_t body_len, http_resp_t *r, int timeout_s) {
    /* chamadas de controle (vigia, catálogo, ícones, report) não pedem o buffer grande */
    return http_open_ex(host, port, method, path, headers, body, body_len, r, timeout_s, 0);
}

int http_open_ex(const char *host, int port, const char *method, const char *path, const char *headers,
                 const char *body, size_t body_len, http_resp_t *r, int timeout_s, int rcvbuf) {
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->content_length = -1;
    int eff = -1;
    int fd = tcp_connect(host, port, timeout_s, rcvbuf, &eff);
    if (fd < 0) return -1;
    r->rcvbuf_eff = eff;
    if (send_and_parse(fd, host, port, method, path, headers, body, body_len, 0, r) != 0) {
        close(fd);
        r->fd = -1;
        return -1;
    }
    return 0;
}

void http_conn_init(http_conn_t *c, const char *host, int port, int rcvbuf, int timeout_s) {
    memset(c, 0, sizeof(*c));
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->port = port;
    c->rcvbuf = rcvbuf;
    c->timeout_s = timeout_s;
    c->fd = -1;
    c->rcvbuf_eff = -1;
}

int http_conn_request(http_conn_t *c, const char *method, const char *path, const char *headers,
                      const char *body, size_t body_len, http_resp_t *r) {
    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = c->fd >= 0;
        int fd = c->fd;
        c->fd = -1;
        if (!reused) {
            int eff = -1;
            fd = tcp_connect(c->host, c->port, c->timeout_s, c->rcvbuf, &eff);
            if (fd < 0) return -1;
            c->connects++;
            c->rcvbuf_eff = eff;
        }
        c->requests++;
        r->rcvbuf_eff = c->rcvbuf_eff;
        int rc = send_and_parse(fd, c->host, c->port, method, path, headers, body, body_len, 1, r);
        if (rc == 0) return 0;
        close(fd);
        r->fd = -1;
        /* conexão guardada que o servidor fechou enquanto esperava: tenta uma vez numa nova */
        if (!(reused && (rc == -1 || rc == -2))) return -1;
    }
    return -1;
}

void http_conn_release(http_conn_t *c, http_resp_t *r) {
    if (r->fd < 0) return;
    int reusable = r->keep_alive && r->content_length >= 0 && r->body_read == r->content_length &&
                   r->buf_pos >= r->buf_len; /* nada além do corpo chegou */
    if (reusable && c->fd < 0) {
        c->fd = r->fd;
    } else {
        close(r->fd);
    }
    r->fd = -1;
}

void http_conn_close(http_conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

ssize_t http_read(http_resp_t *r, void *dst, size_t n) {
    if (r->content_length >= 0) {
        int64_t left = r->content_length - r->body_read;
        if (left <= 0) return 0;
        if ((int64_t)n > left) n = (size_t)left;
    }
    if (r->buf_pos < r->buf_len) {
        size_t avail = r->buf_len - r->buf_pos;
        if (n > avail) n = avail;
        memcpy(dst, r->buf + r->buf_pos, n);
        r->buf_pos += n;
        r->body_read += (int64_t)n;
        return (ssize_t)n;
    }
    while (1) {
        ssize_t got = recv(r->fd, dst, n, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (got == 0) return r->content_length >= 0 ? -1 : 0; /* fechou antes do fim */
        r->body_read += got;
        return got;
    }
}

int http_read_all(http_resp_t *r, char **out, size_t *len, size_t max) {
    size_t cap = r->content_length >= 0 ? (size_t)r->content_length : 65536;
    if (cap > max) return -1;
    char *buf = malloc(cap + 1);
    if (!buf) return -1;
    size_t used = 0;
    while (1) {
        if (used == cap) {
            if (r->content_length >= 0) break;
            if (cap >= max) {
                free(buf);
                return -1;
            }
            cap = cap * 2 > max ? max : cap * 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
        }
        ssize_t n = http_read(r, buf + used, cap - used);
        if (n < 0) {
            free(buf);
            return -1;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    *out = buf;
    if (len) *len = used;
    return 0;
}

void http_abort(http_resp_t *r) {
    if (r->fd >= 0) shutdown(r->fd, SHUT_RDWR);
}

void http_close(http_resp_t *r) {
    if (r->fd >= 0) close(r->fd);
    r->fd = -1;
}

void url_encode(char *out, size_t cap, const char *in) {
    static const char hx[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hx[c >> 4];
            out[j++] = hx[c & 15];
        }
    }
    out[j] = '\0';
}

/* ---------- servidor ---------- */

#define SERVER_MAX_BODY (256 * 1024)

typedef struct {
    int fd;
    char ip[48];
    http_handler_t handler;
} conn_arg_t;

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    default: return "Status";
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *o++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

int http_query_param(const char *query, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vl = len - kl - 1;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = '\0';
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

void http_res_json(http_res_t *res, int status, char *json) {
    res->status = status;
    res->content_type = "application/json; charset=utf-8";
    res->body = json;
    res->body_len = json ? strlen(json) : 0;
}

void http_res_text(http_res_t *res, int status, const char *ctype, const char *text) {
    res->status = status;
    res->content_type = ctype;
    res->body = strdup(text);
    res->body_len = strlen(text);
}

static void *serve_conn(void *arg) {
    conn_arg_t *c = arg;
    char head[8192];
    size_t got = 0;
    int hlen = read_head(c->fd, head, sizeof(head) - 1, &got);
    if (hlen < 0) goto done;
    head[got] = '\0';

    http_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.remote_ip, sizeof(req.remote_ip), "%s", c->ip);
    char target[1024];
    if (sscanf(head, "%7s %1023s", req.method, target) != 2) goto done;
    char *q = strchr(target, '?');
    if (q) {
        *q = '\0';
        snprintf(req.query, sizeof(req.query), "%s", q + 1);
    }
    if (strlen(target) >= sizeof(req.path)) goto done;
    memcpy(req.path, target, strlen(target) + 1);
    url_decode(req.path);

    size_t content_length = 0;
    char *line = strstr(head, "\r\n");
    while (line && line < head + hlen - 2) {
        line += 2;
        char *eol = strstr(line, "\r\n");
        if (!eol) break;
        char *colon = memchr(line, ':', (size_t)(eol - line));
        if (colon) {
            size_t klen = (size_t)(colon - line);
            if (klen == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
                content_length = (size_t)strtoull(colon + 1, NULL, 10);
            } else if (klen == 5 && strncasecmp(line, "Range", 5) == 0) {
                copy_value(req.range, sizeof(req.range), colon + 1, eol);
            }
        }
        line = eol;
    }
    if (content_length > SERVER_MAX_BODY) goto done;
    if (content_length > 0) {
        req.body = malloc(content_length + 1);
        if (!req.body) goto done;
        size_t have = got - (size_t)hlen;
        if (have > content_length) have = content_length;
        memcpy(req.body, head + hlen, have);
        while (have < content_length) {
            ssize_t n = recv(c->fd, req.body + have, content_length - have, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                free(req.body);
                goto done;
            }
            have += (size_t)n;
        }
        req.body[content_length] = '\0';
        req.body_len = content_length;
    }

    http_res_t res;
    memset(&res, 0, sizeof(res));
    res.status = 404;
    c->handler(&req, &res);
    free(req.body);

    const void *body = res.body ? (const void *)res.body : res.static_body;
    char out[1024];
    int n = snprintf(out, sizeof(out),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n%s\r\n",
                     res.status, reason(res.status), res.content_type ? res.content_type : "text/plain",
                     res.body_len, res.extra_headers);
    if (n > 0 && send_all(c->fd, out, (size_t)n) == 0 && strcmp(req.method, "HEAD") != 0 && body && res.body_len)
        send_all(c->fd, body, res.body_len);
    free(res.body);
done:
    close(c->fd);
    free(c);
    return NULL;
}

typedef struct {
    int fd;
    http_handler_t handler;
} server_arg_t;

static void *accept_loop(void *arg) {
    server_arg_t *s = arg;
    while (1) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = accept(s->fd, (struct sockaddr *)&addr, &len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            usleep(100000);
            continue;
        }
        set_timeouts(fd, 30);
        int one = 1;
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        conn_arg_t *c = calloc(1, sizeof(*c));
        if (!c) {
            close(fd);
            continue;
        }
        c->fd = fd;
        c->handler = s->handler;
        inet_ntop(AF_INET, &addr.sin_addr, c->ip, sizeof(c->ip));
        pthread_t t;
        if (pthread_create(&t, NULL, serve_conn, c) != 0) {
            close(fd);
            free(c);
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

int http_serve(int port, http_handler_t handler) {
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    server_arg_t *s = calloc(1, sizeof(*s));
    s->fd = fd;
    s->handler = handler;
    pthread_t t;
    if (pthread_create(&t, NULL, accept_loop, s) != 0) {
        close(fd);
        free(s);
        return -1;
    }
    pthread_detach(t);
    return 0;
}
