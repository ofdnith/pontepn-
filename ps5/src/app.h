/* Estado compartilhado do payload: o Mac escolhido e o último catálogo. */
#ifndef PONTE_APP_H
#define PONTE_APP_H

#include "ponte.h"
#include "vendor/cJSON.h"

void app_get_mac(mac_t *out);
void app_set_mac(const mac_t *mac); /* também grava a configuração */

/* O Mac está no ar e pareado (dá para copiar agora). */
int app_mac_ready(void);

/* Cópia do item do último catálogo carregado (liberar com cJSON_Delete), ou NULL. */
cJSON *app_catalog_item(const char *id);

/* Envia o estado do console ao Mac e guarda o catálogo anotado (SPEC §5.7).
 * Devolve uma cópia ({generated_at, items}) ou NULL; em erro, `err` e `status`. */
cJSON *app_refresh_catalog(char *err, size_t errlen, int *status);

#endif
