/* Partes de plataforma que valem para o console e para o teste. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#if defined(__linux__)
#include <sys/statfs.h>
#endif

#include "platform.h"
#include "platform_common.h"
#include "tile.h"
#include "util.h"

int storage_fill(storage_t *s, const char *id, const char *label, const char *root) {
    struct statfs fs;
    if (statfs(root, &fs) != 0) return -1;
    memset(s, 0, sizeof(*s));
    snprintf(s->id, sizeof(s->id), "%s", id);
    snprintf(s->label, sizeof(s->label), "%s", label);
    snprintf(s->root, sizeof(s->root), "%s", root);
    s->free = (int64_t)fs.f_bavail * (int64_t)fs.f_bsize;
    s->total = (int64_t)fs.f_blocks * (int64_t)fs.f_bsize;
    return 0;
}

void add_installed(cJSON *arr, const char *title_id, const char *version, const char *content, const char *storage) {
    /* evita repetir o mesmo título/conteúdo/armazenamento */
    cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        cJSON *t = cJSON_GetObjectItem(e, "title_id"), *c = cJSON_GetObjectItem(e, "content");
        cJSON *s = cJSON_GetObjectItem(e, "storage");
        if (cJSON_IsString(t) && strcmp(t->valuestring, title_id) == 0 && cJSON_IsString(c) &&
            strcmp(c->valuestring, content) == 0 && cJSON_IsString(s) && strcmp(s->valuestring, storage) == 0)
            return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title_id", title_id);
    if (version) cJSON_AddStringToObject(o, "version", version);
    else cJSON_AddNullToObject(o, "version");
    cJSON_AddStringToObject(o, "content", content);
    cJSON_AddStringToObject(o, "storage", storage);
    cJSON_AddItemToArray(arr, o);
}

/* Versão de uma imagem ou pasta já copiada: o arquivo lateral não vem junto, então só a pasta
 * tem como dizer (sce_sys/param.json). Imagens ficam sem versão (conta como instalada). */
static void folder_version(const char *dir, char *out, size_t cap) {
    out[0] = '\0';
    char path[1024];
    path_join(path, sizeof(path), dir, "sce_sys/param.json");
    char *text = read_file(path, 1 << 20, NULL);
    if (!text) return;
    cJSON *j = cJSON_Parse(text);
    free(text);
    cJSON *v = j ? cJSON_GetObjectItem(j, "contentVersion") : NULL;
    if (cJSON_IsString(v)) snprintf(out, cap, "%s", v->valuestring);
    cJSON_Delete(j);
}

void scan_homebrew(cJSON *arr, const storage_t *st, int n) {
    for (int i = 0; i < n; i++) {
        char hb[512];
        path_join(hb, sizeof(hb), st[i].root, "homebrew");
        DIR *d = opendir(hb);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char tid[10];
            if (!find_ps_title_id(e->d_name, tid)) continue;
            if (strcmp(tid, PONTE_TILE_ID) == 0) continue; /* o ícone do próprio Ponte */
            char full[1024], ver[64] = "";
            path_join(full, sizeof(full), hb, e->d_name);
            if (is_dir(full)) folder_version(full, ver, sizeof(ver));
            add_installed(arr, tid, ver[0] ? ver : NULL, "base", st[i].id);
        }
        closedir(d);
    }
}
