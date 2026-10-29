/* Tudo o que depende do console. Duas implementações:
 *   platform_ps5.c  — o payload de verdade (SDK de payloads do PS5)
 *   platform_host.c — Linux/macOS, para testar o payload contra o servidor sem console
 */
#ifndef PONTE_PLATFORM_H
#define PONTE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "vendor/cJSON.h"

#define PONTE_MAX_STORAGES 12

typedef struct {
    char id[16];      /* "internal", "ext0", "usb0"… (SPEC §5.7) */
    char label[32];   /* para mostrar */
    char root[256];   /* raiz do armazenamento: /data, /mnt/ext0, /mnt/usb0… */
    int64_t free;
    int64_t total;
} storage_t;

int plat_init(void);

/* Pasta do Ponte no console: configuração e estado. */
const char *plat_data_dir(void);

/* Mensagem curta na tela do console (e no log). */
void plat_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Linha de log (stderr / klog). */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Armazenamentos montados agora. Retorna quantos. */
int plat_storages(storage_t *out, int max);

/* O que já está no console: lista de {title_id, version, content, storage} (SPEC §5.7). */
cJSON *plat_installed(const storage_t *storages, int n);

/* Firmware no formato "X.YY", ou NULL. */
const char *plat_firmware(void);

/* Entrega um pacote ao instalador do sistema pela URL. 0 = aceito. */
int plat_install_pkg(const char *url, const char *name, char *err, size_t errlen);

/* Registra o ícone da tela inicial a partir da pasta `dir` (ver tile.h).
 * 1 = registrou agora, 0 = já estava registrado, -1 = falhou (motivo em err). */
int plat_register_tile(const char *dir, const char *title_id, int files_changed, char *err, size_t errlen);

/* Nome do console para o pareamento. */
const char *plat_console_name(void);

#endif
