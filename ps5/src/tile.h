/* Ícone do Ponte na tela inicial do PS5.
 *
 * Uma "pasta de app" mínima em /data/ponte/tile/PNTE00001/ (sce_sys/param.json, ícone e um
 * eboot.bin de mentira) que o próprio payload registra no sistema, como fazem outros payloads
 * de 13.60: copia sce_sys para /user/app e /user/appmeta, liga a pasta em /system_ex/app por
 * nullfs e chama o instalador do sistema. O param.json tem deeplinkUri apontando para a
 * interface, então abrir o ícone abre o navegador do PS5 em http://127.0.0.1:47102/ (o eboot
 * nunca roda). */
#ifndef PONTE_TILE_H
#define PONTE_TILE_H

#define PONTE_TILE_ID "PNTE00001"

/* Grava os arquivos do ícone em `dir`. 1 = gravou algo, 0 = já estava igual, -1 = falhou. */
int tile_write_files(const char *dir, int ui_port);

/* Grava os arquivos e registra o ícone (só no console). Avisa na tela o que aconteceu. */
void tile_ensure(int ui_port);

#endif
