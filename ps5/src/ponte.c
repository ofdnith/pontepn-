#include "ponte.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "platform.h"
#include "util.h"

int ponte_discovery_port = 47100;
const char *ponte_discovery_extra = NULL;

static int add_found(mac_t *found, int count, int max, const mac_t *m) {
    for (int i = 0; i < count; i++) {
        if ((m->id[0] && strcmp(found[i].id, m->id) == 0) ||
            (strcmp(found[i].host, m->host) == 0 && found[i].port == m->port))
            return count;
    }
    if (count < max) found[count++] = *m;
    return count;
}

int ponte_discover(mac_t *found, int max, const char *extra_host, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    const char *q = "PONTE?1";
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)ponte_discovery_port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, q, strlen(q), 0, (struct sockaddr *)&to, sizeof(to));
    const char *extras[2] = {extra_host, ponte_discovery_extra};
    for (int i = 0; i < 2; i++)
        if (extras[i] && *extras[i] && inet_pton(AF_INET, extras[i], &to.sin_addr) == 1)
            sendto(fd, q, strlen(q), 0, (struct sockaddr *)&to, sizeof(to));

    int count = 0;
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    while (now_ms() < deadline) {
        struct pollfd p = {fd, POLLIN, 0};
        int left = (int)(deadline - now_ms());
        if (poll(&p, 1, left > 0 ? left : 0) <= 0) continue;
        char buf[1024];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        buf[n] = '\0';
        cJSON *j = cJSON_Parse(buf);
        if (!j) continue;
        cJSON *ver = cJSON_GetObjectItem(j, "ponte");
        cJSON *port = cJSON_GetObjectItem(j, "port");
        if (cJSON_IsNumber(ver) && ver->valueint == 1 && cJSON_IsNumber(port)) {
            mac_t m;
            memset(&m, 0, sizeof(m));
            inet_ntop(AF_INET, &from.sin_addr, m.host, sizeof(m.host));
            m.port = port->valueint;
            cJSON *id = cJSON_GetObjectItem(j, "id");
            cJSON *name = cJSON_GetObjectItem(j, "name");
            if (cJSON_IsString(id)) snprintf(m.id, sizeof(m.id), "%s", id->valuestring);
            if (cJSON_IsString(name)) snprintf(m.name, sizeof(m.name), "%s", name->valuestring);
            count = add_found(found, count, max, &m);
        }
        cJSON_Delete(j);
    }
    close(fd);
    return count;
}

cJSON *ponte_call(const mac_t *mac, const char *method, const char *path, cJSON *body, int *status,
                  char *err, size_t errlen) {
    char headers[256] = "";
    if (mac->token[0]) snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", mac->token);
    char *json = body ? cJSON_PrintUnformatted(body) : NULL;
    http_resp_t r;
    int rc = http_open(mac->host, mac->port, method, path, headers, json, json ? strlen(json) : 0, &r, 20);
    free(json);
    if (status) *status = 0;
    if (rc != 0) {
        if (err) snprintf(err, errlen, "sem resposta do Mac em %s:%d", mac->host, mac->port);
        return NULL;
    }
    if (status) *status = r.status;
    char *text = NULL;
    size_t len = 0;
    cJSON *out = NULL;
    if (http_read_all(&r, &text, &len, 32 << 20) == 0) {
        out = len ? cJSON_Parse(text) : NULL;
        free(text);
    }
    http_close(&r);
    if (r.status >= 400 && err) {
        cJSON *e = out ? cJSON_GetObjectItem(out, "error") : NULL;
        cJSON *code = e ? cJSON_GetObjectItem(e, "code") : NULL;
        cJSON *msg = e ? cJSON_GetObjectItem(e, "message") : NULL;
        snprintf(err, errlen, "%s: %s", cJSON_IsString(code) ? code->valuestring : "http",
                 cJSON_IsString(msg) ? msg->valuestring : "erro");
    }
    return out;
}

int ponte_info(mac_t *mac, int *pairing_required) {
    int status = 0;
    mac_t anon = *mac;
    anon.token[0] = '\0';
    cJSON *j = ponte_call(&anon, "GET", "/api/v1/info", NULL, &status, NULL, 0);
    if (!j || status != 200) {
        cJSON_Delete(j);
        return -1;
    }
    cJSON *id = cJSON_GetObjectItem(j, "id");
    cJSON *name = cJSON_GetObjectItem(j, "name");
    cJSON *pr = cJSON_GetObjectItem(j, "pairing_required");
    if (cJSON_IsString(id)) {
        /* outro Mac no mesmo endereço: o token antigo não vale */
        if (mac->id[0] && strcmp(mac->id, id->valuestring) != 0) mac->token[0] = '\0';
        snprintf(mac->id, sizeof(mac->id), "%s", id->valuestring);
    }
    if (cJSON_IsString(name)) snprintf(mac->name, sizeof(mac->name), "%s", name->valuestring);
    if (pairing_required) *pairing_required = cJSON_IsTrue(pr);
    cJSON_Delete(j);
    return 0;
}

int ponte_pair(mac_t *mac, const char *code, char *err, size_t errlen) {
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "code", code);
    cJSON *client = cJSON_AddObjectToObject(body, "client");
    cJSON_AddStringToObject(client, "platform", "ps5");
    cJSON_AddStringToObject(client, "name", plat_console_name());
    cJSON_AddStringToObject(client, "version", PONTE_VERSION);
    const char *fw = plat_firmware();
    if (fw) cJSON_AddStringToObject(client, "firmware", fw);
    mac_t anon = *mac;
    anon.token[0] = '\0';
    int status = 0;
    cJSON *j = ponte_call(&anon, "POST", "/api/v1/pair", body, &status, err, errlen);
    cJSON_Delete(body);
    int rc = -1;
    if (j && status == 200) {
        cJSON *t = cJSON_GetObjectItem(j, "client_token");
        if (cJSON_IsString(t)) {
            snprintf(mac->token, sizeof(mac->token), "%s", t->valuestring);
            rc = 0;
        }
    }
    cJSON_Delete(j);
    return rc;
}

static void config_path(char *out, size_t cap) {
    path_join(out, cap, plat_data_dir(), "config.json");
}

int ponte_load_config(mac_t *mac) {
    memset(mac, 0, sizeof(*mac));
    char path[512];
    config_path(path, sizeof(path));
    char *text = read_file(path, 64 * 1024, NULL);
    if (!text) return -1;
    cJSON *j = cJSON_Parse(text);
    free(text);
    if (!j) return -1;
    cJSON *m = cJSON_GetObjectItem(j, "mac");
    if (cJSON_IsObject(m)) {
        cJSON *v;
        if (cJSON_IsString(v = cJSON_GetObjectItem(m, "host"))) snprintf(mac->host, sizeof(mac->host), "%s", v->valuestring);
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(m, "port"))) mac->port = v->valueint;
        if (cJSON_IsString(v = cJSON_GetObjectItem(m, "id"))) snprintf(mac->id, sizeof(mac->id), "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItem(m, "name"))) snprintf(mac->name, sizeof(mac->name), "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItem(m, "token"))) snprintf(mac->token, sizeof(mac->token), "%s", v->valuestring);
    }
    cJSON_Delete(j);
    if (!mac->port) mac->port = 47101;
    return mac->host[0] ? 0 : -1;
}

int ponte_save_config(const mac_t *mac) {
    cJSON *j = cJSON_CreateObject();
    cJSON *m = cJSON_AddObjectToObject(j, "mac");
    cJSON_AddStringToObject(m, "host", mac->host);
    cJSON_AddNumberToObject(m, "port", mac->port);
    cJSON_AddStringToObject(m, "id", mac->id);
    cJSON_AddStringToObject(m, "name", mac->name);
    cJSON_AddStringToObject(m, "token", mac->token);
    char *text = cJSON_Print(j);
    cJSON_Delete(j);
    mkdirs(plat_data_dir());
    char path[512];
    config_path(path, sizeof(path));
    int rc = write_file_atomic(path, text, strlen(text));
    free(text);
    return rc;
}
