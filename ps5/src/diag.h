/* Diagnóstico de velocidade: mede a rede e o disco separados, para achar o gargalo de uma cópia. */
#ifndef PONTE_DIAG_H
#define PONTE_DIAG_H

#include <stddef.h>

#include "vendor/cJSON.h"

/* Grava `mb` MB de dados em blocos de `block_kb` KB em <armazenamento>/ponte/tmp/ (no máximo
 * `max_s` segundos), com fsync no fim, e apaga. NULL = erro (motivo em err). */
cJSON *diag_disk(const char *storage_id, int mb, int block_kb, int max_s, char *err, size_t errlen);

/* Baixa até `mb` MB do maior arquivo de `item_id` do Mac e descarta (sem hash, sem disco), lendo
 * em pedaços de `read_kb` KB, com buffer TCP de `rcvbuf_kb` KB (-1 = o dos ajustes; 0 = padrão do
 * sistema). NULL = erro. */
cJSON *diag_net(const char *item_id, int mb, int read_kb, int rcvbuf_kb, int max_s, char *err, size_t errlen);

/* Teste em andamento, para a interface mostrar barra e velocidade ao vivo:
 * {running, kind ("net"|"disk"), label, done, total, speed, elapsed_s}. Sem teste: {running:false}. */
cJSON *diag_progress_json(void);

#endif
