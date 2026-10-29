/* Fila de cópias (SPEC §8): uma de cada vez, com retomada e conferência. */
#ifndef PONTE_JOBS_H
#define PONTE_JOBS_H

#include <stdint.h>

#include "vendor/cJSON.h"

/* Traz de volta a fila gravada no console e começa a copiar. */
void jobs_init(void);

/* Põe um item na fila. `storage` = id do armazenamento (ignorado para .pkg). 0 = ok. */
int jobs_add(const char *item_id, const char *storage, int replace, char *err, size_t errlen);

/* Lista para a interface: [{seq, item_id, title, kind, storage, state, done, total, speed, eta_s,
 * message, error, phase, phase_done, phase_total, phase_speed, file, file_index, file_count,
 * started_ms, connections, requests, ...}] (ver ps5/README.md). */
cJSON *jobs_json(void);

int jobs_cancel(int seq);
/* Pausa (o que já veio fica guardado) e continua. 0 = achou o trabalho. */
int jobs_pause(int seq);
int jobs_resume(int seq);
void jobs_clear_finished(void);

/* Avisos na tela a cada 25% (ligado por padrão; ajuste na interface). */
extern int jobs_notify_progress;

/* Tamanho de cada buffer da cópia (bytes): a rede enche um buffer inteiro antes de ele ir ao disco. */
extern int jobs_chunk_bytes;

/* Para testes: falha a cópia depois de N bytes (simula a conexão caindo). */
extern int64_t jobs_test_fail_after;
/* Para testes: estraga um byte dos próximos N arquivos baixados (simula dado corrompido). */
extern int jobs_test_corrupt;
/* Para testes: espera N ms a cada buffer gravado (cópia lenta, para ver as etapas ao vivo). */
extern int jobs_test_slow_ms;

#endif
