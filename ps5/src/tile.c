#include "tile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "ui_icon.h"
#include "util.h"

static const char EBOOT[] = "Ponte: icone da tela inicial. Abre o navegador na interface do payload.\n";

/* Grava só quando o conteúdo mudou. 1 = gravou, 0 = igual, -1 = erro. */
static int sync_file(const char *path, const char *data, size_t len) {
    size_t have_len = 0;
    char *have = read_file(path, len + 1, &have_len);
    int same = have && have_len == len && memcmp(have, data, len) == 0;
    free(have);
    if (same) return 0;
    return write_file_atomic(path, data, len) == 0 ? 1 : -1;
}

int tile_write_files(const char *dir, int ui_port) {
    char sce_sys[600], path[700];
    snprintf(sce_sys, sizeof(sce_sys), "%s/sce_sys", dir);
    if (mkdirs(sce_sys) != 0) return -1;

    char param[1024];
    int plen = snprintf(param, sizeof(param),
                        "{\n"
                        "  \"applicationCategoryType\": 66048,\n"
                        "  \"titleId\": \"%s\",\n"
                        "  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
                        "  \"localizedParameters\": {\n"
                        "    \"defaultLanguage\": \"en-US\",\n"
                        "    \"en-US\": {\"titleName\": \"Ponte\"},\n"
                        "    \"pt-BR\": {\"titleName\": \"Ponte\"}\n"
                        "  }\n"
                        "}\n",
                        PONTE_TILE_ID, ui_port);
    int changed = 0, r;
    snprintf(path, sizeof(path), "%s/param.json", sce_sys);
    if ((r = sync_file(path, param, (size_t)plen)) < 0) return -1;
    changed |= r;
    snprintf(path, sizeof(path), "%s/icon0.png", sce_sys);
    if ((r = sync_file(path, UI_ICON, UI_ICON_LEN)) < 0) return -1;
    changed |= r;
    snprintf(path, sizeof(path), "%s/eboot.bin", dir);
    if ((r = sync_file(path, EBOOT, sizeof(EBOOT) - 1)) < 0) return -1;
    changed |= r;
    return changed;
}

void tile_ensure(int ui_port) {
    /* versão anterior punha o ícone em homebrew/; tira de lá para o montador não registrar duas vezes */
    storage_t st[PONTE_MAX_STORAGES];
    int n = plat_storages(st, PONTE_MAX_STORAGES);
    for (int i = 0; i < n; i++) {
        if (strcmp(st[i].id, "internal") != 0) continue;
        char old[600];
        snprintf(old, sizeof(old), "%s/homebrew/%s", st[i].root, PONTE_TILE_ID);
        if (is_dir(old)) rmtree(old);
    }

    char dir[600];
    snprintf(dir, sizeof(dir), "%s/tile/%s", plat_data_dir(), PONTE_TILE_ID);
    int changed = tile_write_files(dir, ui_port);
    if (changed < 0) {
        plat_log("não foi possível gravar os arquivos do ícone em %s", dir);
        return;
    }
    char err[256] = "";
    int rc = plat_register_tile(dir, PONTE_TILE_ID, changed, err, sizeof(err));
    if (rc < 0) plat_notify("Ponte: não deu para criar o ícone (%s)", err);
    else if (rc > 0) plat_notify("Ponte: ícone adicionado à tela inicial");
}
