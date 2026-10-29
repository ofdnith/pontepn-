/* HTTP/1.1 mínimo: cliente (para falar com o Mac) e servidor (para a interface web do payload).
 * Chamadas avulsas (http_open) usam uma conexão por requisição; a cópia usa uma conexão
 * persistente (http_conn_t, keep-alive), reaproveitada por todos os arquivos de um item. */
#ifndef PONTE_HTTP_H
#define PONTE_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---------- cliente ---------- */

typedef struct {
    int status;
    int64_t content_length; /* -1 = até fechar */
    char content_type[96];
    char revision[32];      /* X-Ponte-Revision */
    char sha256[80];        /* X-Ponte-Sha256 */
    int keep_alive;         /* o servidor deixa a conexão aberta depois desta resposta */
    int rcvbuf_eff;         /* SO_RCVBUF que o sistema deu a esta conexão (bytes; -1 = desconhecido) */
    int fd;
    char buf[16384];
    size_t buf_len, buf_pos;
    int64_t body_read;
} http_resp_t;

/* Conecta, envia a requisição e lê os cabeçalhos da resposta. O corpo fica para http_read.
 * `headers`: linhas extras já com "\r\n" (pode ser NULL). 0 = ok; -1 = falha de rede.
 * Chamada de controle: buffer de recepção padrão do sistema, Connection: close. */
int http_open(const char *host, int port, const char *method, const char *path, const char *headers,
              const char *body, size_t body_len, http_resp_t *r, int timeout_s);

/* O mesmo, escolhendo o buffer de recepção do socket: -1 = usa http_rcvbuf; 0 = padrão do sistema. */
int http_open_ex(const char *host, int port, const char *method, const char *path, const char *headers,
                 const char *body, size_t body_len, http_resp_t *r, int timeout_s, int rcvbuf);

/* Buffer de recepção pedido nas conexões de download (bytes; 0 = padrão do sistema). */
extern int http_rcvbuf;

/* ---------- conexão persistente (keep-alive) ---------- */

typedef struct {
    char host[64];
    int port;
    int rcvbuf;     /* pedido (-1 = http_rcvbuf; 0 = padrão do sistema) */
    int timeout_s;
    int fd;         /* conexão ociosa pronta para a próxima requisição, ou -1 */
    int connects;   /* conexões TCP abertas */
    int requests;   /* requisições enviadas */
    int rcvbuf_eff; /* SO_RCVBUF da última conexão aberta (-1 = desconhecido) */
} http_conn_t;

void http_conn_init(http_conn_t *c, const char *host, int port, int rcvbuf, int timeout_s);

/* Envia a requisição pela conexão aberta (ou abre uma). Se a conexão guardada tiver sido fechada
 * pelo servidor enquanto esperava, reconecta e repete uma vez, sem o chamador perceber.
 * Enquanto a resposta está em uso, a conexão pertence a ela; devolva com http_conn_release. */
int http_conn_request(http_conn_t *c, const char *method, const char *path, const char *headers,
                      const char *body, size_t body_len, http_resp_t *r);

/* Termina a resposta: a conexão volta para `c` se o corpo foi lido inteiro e o servidor não pediu
 * para fechar; senão é fechada (a próxima requisição reconecta). */
void http_conn_release(http_conn_t *c, http_resp_t *r);

void http_conn_close(http_conn_t *c);

/* Interrompe uma leitura bloqueada em outra thread (shutdown do socket). Ainda precisa de http_close. */
void http_abort(http_resp_t *r);

/* Lê até n bytes do corpo. 0 = fim; -1 = erro. */
ssize_t http_read(http_resp_t *r, void *dst, size_t n);

/* Lê o corpo inteiro (até `max`), terminado em zero. 0 = ok. */
int http_read_all(http_resp_t *r, char **out, size_t *len, size_t max);

void http_close(http_resp_t *r);

/* ---------- servidor ---------- */

typedef struct {
    char method[8];
    char path[512];   /* sem a query */
    char query[512];
    char remote_ip[48];
    char range[96];   /* cabeçalho Range, se houver */
    char *body;
    size_t body_len;
} http_req_t;

typedef struct {
    int status;
    const char *content_type;
    char *body;          /* malloc; liberado pelo servidor */
    size_t body_len;
    const void *static_body; /* alternativa sem cópia (não é liberado) */
    char extra_headers[256];
} http_res_t;

typedef void (*http_handler_t)(const http_req_t *req, http_res_t *res);

/* Sobe o servidor em 0.0.0.0:port numa thread própria. 0 = ok. */
int http_serve(int port, http_handler_t handler);

/* Valor de um parâmetro da query (decodificado), ou 0 se não houver. */
int http_query_param(const char *query, const char *key, char *out, size_t cap);

/* Utilitários para respostas. */
void http_res_json(http_res_t *res, int status, char *json /* malloc */);
void http_res_text(http_res_t *res, int status, const char *ctype, const char *text);

/* Codifica para usar em URL (query). */
void url_encode(char *out, size_t cap, const char *in);

#endif
