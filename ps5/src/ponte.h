/* Cliente do protocolo Ponte (SPEC §3 a §6): descoberta, pareamento e rotas do Mac. */
#ifndef PONTE_PONTE_H
#define PONTE_PONTE_H

/* Versão do payload (aparece na interface e no pareamento). */
#define PONTE_VERSION "1.0.2"

#include "vendor/cJSON.h"

typedef struct {
    char host[64];
    int port;
    char id[40];
    char name[128];
    char token[96];
} mac_t;

#define PONTE_MAX_FOUND 8

/* Porta da descoberta (47100; trocável nos testes). */
extern int ponte_discovery_port;
/* Endereço extra para perguntar por unicast (além do broadcast), ou NULL. */
extern const char *ponte_discovery_extra;

/* Broadcast "PONTE?1" (e unicast para `extra_host`, se dado). Retorna quantos Macs responderam. */
int ponte_discover(mac_t *found, int max, const char *extra_host, int timeout_ms);

/* Faz uma requisição JSON ao Mac. Devolve o JSON da resposta (ou NULL) e o status HTTP.
 * Em erro da API, `err` recebe "codigo: mensagem". */
cJSON *ponte_call(const mac_t *mac, const char *method, const char *path, cJSON *body, int *status,
                  char *err, size_t errlen);

/* GET /api/v1/info em host:port; preenche id e nome. 0 = respondeu. */
int ponte_info(mac_t *mac, int *pairing_required);

/* POST /api/v1/pair. 0 = pareado (token em mac->token). */
int ponte_pair(mac_t *mac, const char *code, char *err, size_t errlen);

/* Configuração salva no console (Mac escolhido e token). */
int ponte_load_config(mac_t *mac);
int ponte_save_config(const mac_t *mac);

#endif
