#ifndef PONTE_UTIL_H
#define PONTE_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Cria a pasta e as que faltarem no caminho. 0 = ok. */
int mkdirs(const char *path);

/* Apaga arquivo ou pasta inteira. 0 = ok (ou já não existia). */
int rmtree(const char *path);

int path_exists(const char *path);
int is_dir(const char *path);
int64_t file_size(const char *path); /* -1 se não existir */

/* "a" + "b" -> "a/b" em `out`. 0 = coube. */
int path_join(char *out, size_t cap, const char *a, const char *b);

/* Caminho relativo seguro vindo do servidor (SPEC §5.4): sem "/" no início, sem "..", ".",
 * componentes vazios, barra invertida ou zero. 1 = seguro. */
int safe_relpath(const char *p);

/* Nome de arquivo seguro: troca o que não for letra, número ou " ._-[]()" por "_". */
void safe_name(char *out, size_t cap, const char *in);

/* Progresso de uma conta longa (hash de arquivo): `done` bytes até agora. Devolver diferente de
 * zero interrompe a conta (cancelar/pausar). */
typedef int (*progress_cb)(void *arg, int64_t done);

/* sha256 do arquivo em hexadecimal (65 bytes). 0 = ok; -1 = erro; -2 = interrompido pelo `cb`. */
int sha256_file(const char *path, char out_hex[65]);
int sha256_file_cb(const char *path, char out_hex[65], progress_cb cb, void *arg);

/* sha256 aos pedaços (para conferir enquanto baixa, sem ler o arquivo de novo no fim). */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final_hex(sha256_ctx *c, char out_hex[65]);
/* Soma ao contexto os primeiros `n` bytes do arquivo. 0 = ok; -1 = erro; -2 = interrompido. */
int sha256_add_file(sha256_ctx *c, const char *path, int64_t n);
int sha256_add_file_cb(sha256_ctx *c, const char *path, int64_t n, progress_cb cb, void *arg);

/* Implementação do sha256: 1 = instruções SHA do processador (SHA-NI, x86-64), 0 = C portátil.
 * sha256_set_impl força uma delas (para testes); pedir SHA-NI sem suporte cai no C portátil.
 * Devolve a que ficou valendo. */
int sha256_impl(void);
int sha256_set_impl(int ni);

/* Lê um arquivo inteiro (até `max` bytes), terminado em zero. NULL se falhar. */
char *read_file(const char *path, size_t max, size_t *len_out);

/* Grava de forma atômica (arquivo .tmp + fsync + rename). 0 = ok. */
int write_file_atomic(const char *path, const char *data, size_t len);

/* Relógio monotônico em milissegundos: para durações, velocidade, tempo restante e esperas
 * (não anda para trás se o relógio do console mudar). */
uint64_t now_ms(void);
/* Relógio monotônico em microssegundos (para medir tempos). */
uint64_t now_us(void);
/* Hora de verdade (epoch, ms): só para mostrar quando algo começou. */
uint64_t wall_ms(void);

/* Id de item vindo da interface ou do Mac: 1 a 64 caracteres de [A-Za-z0-9_-]. Impede que um id
 * estranho quebre a requisição ao Mac (cabeçalhos, caminho). 1 = válido. */
int valid_id(const char *id);

/* Velocidade como no padrão visual (docs/DESIGN.md): média dos últimos ~5 s; parado há mais de
 * 2 s dá 0. Amostras a cada >= 250 ms (rate_add pode ser chamada mais vezes). */
#define RATE_SAMPLES 24
typedef struct {
    uint64_t t[RATE_SAMPLES];
    int64_t v[RATE_SAMPLES];
    int n;
    uint64_t last_change_ms;
    int64_t last_v;
} rate_t;

void rate_reset(rate_t *r, uint64_t now, int64_t v);
void rate_add(rate_t *r, uint64_t now, int64_t v);
double rate_get(const rate_t *r, uint64_t now); /* por segundo */

/* Title ID de PS4/PS5 no texto (4 letras maiúsculas + 5 dígitos). 1 = achou. */
int find_ps_title_id(const char *text, char out[10]);

#endif
