#ifndef PONTE_PLATFORM_COMMON_H
#define PONTE_PLATFORM_COMMON_H

#include "platform.h"

/* Preenche `s` com espaço livre/total de `root`. 0 = montado. */
int storage_fill(storage_t *s, const char *id, const char *label, const char *root);

void add_installed(cJSON *arr, const char *title_id, const char *version, const char *content, const char *storage);

/* Jogos em <armazenamento>/homebrew/ (pastas e imagens que o montador registra). */
void scan_homebrew(cJSON *arr, const storage_t *st, int n);

#endif
