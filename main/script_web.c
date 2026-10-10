#include "script_web.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "console.h"
#include "script.h"

/*
 * GET  /api/scripts              status: running script, error, params, file list
 * POST /api/scripts              {"action":"run","name":"fan-circle"} | {"action":"stop"} |
 *                                {"action":"delete","name":..} | {"params":{"speed":0.3}}
 *                                -> the status plus {"ok":..,"error":"request error"}
 * GET  /api/scripts/log?since=N  {"end":N2,"text":"..."}: log written after offset N
 * GET  /scripts/<name>.js        source
 * PUT  /scripts/<name>.js        save (a running script restarts with it)
 */

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!js) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, js);
    free(js);
    return err;
}

static esp_err_t send_result(httpd_req_t *req, const char *err)
{
    cJSON *r = script_status_json();
    cJSON_AddBoolToObject(r, "ok", err == NULL);
    if (err) {
        cJSON_AddStringToObject(r, "error", err);
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return send_json(req, r);
}

static char *read_body(httpd_req_t *req, size_t max_len)
{
    if (req->content_len > max_len) {
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += n;
    }
    body[got] = '\0';
    return body;
}

static esp_err_t status_get(httpd_req_t *req)
{
    return send_json(req, script_status_json());
}

static esp_err_t action_post(httpd_req_t *req)
{
    char *body = read_body(req, 2048);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_result(req, "invalid JSON body");
    }
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *namej = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const char *name = cJSON_IsString(namej) ? namej->valuestring : "";
    const char *err = NULL;

    if (cJSON_IsObject(params)) {
        const cJSON *p;
        cJSON_ArrayForEach(p, params) {
            if (!cJSON_IsNumber(p)) {
                err = "param values must be numbers";
            } else if (script_set_param(p->string, p->valuedouble)) {
                err = "no such parameter";
            }
        }
    }
    if (cJSON_IsString(action)) {
        const char *a = action->valuestring;
        if (strcmp(a, "run") == 0) {
            err = script_run(name);
        } else if (strcmp(a, "stop") == 0) {
            script_stop();
        } else if (strcmp(a, "delete") == 0) {
            err = script_delete(name);
        } else {
            err = "unknown action";
        }
    } else if (!cJSON_IsObject(params)) {
        err = "missing action";
    }
    cJSON_Delete(root);
    return send_result(req, err);
}

static esp_err_t log_get(httpd_req_t *req)
{
    char query[32], val[16];
    size_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
        since = strtoul(val, NULL, 10);
    }
    char *text = NULL;
    size_t end = script_log_read(since, &text);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "end", end);
    cJSON_AddStringToObject(r, "text", text ? text : "");
    free(text);
    return send_json(req, r);
}

// "/scripts/fan-circle.js" -> "fan-circle"; false if it isn't a valid script name.
static bool name_from_uri(const char *uri, char *name)
{
    const char *p = uri + strlen("/scripts/");
    const char *q = strchr(p, '?');
    size_t n = q ? (size_t)(q - p) : strlen(p);
    if (n <= 3 || n - 3 > SCRIPT_NAME_MAX || strncmp(p + n - 3, ".js", 3) != 0) {
        return false;
    }
    memcpy(name, p, n - 3);
    name[n - 3] = '\0';
    return script_name_valid(name);
}

static esp_err_t source_get(httpd_req_t *req)
{
    char name[SCRIPT_NAME_MAX + 1];
    size_t len;
    char *src = name_from_uri(req->uri, name) ? script_read(name, &len) : NULL;
    if (!src) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such script");
    }
    httpd_resp_set_type(req, "text/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, src, len);
    free(src);
    return err;
}

static esp_err_t source_put(httpd_req_t *req)
{
    char name[SCRIPT_NAME_MAX + 1];
    if (!name_from_uri(req->uri, name)) {
        return send_result(req, "name: 1-24 letters, digits, - or _, ending in .js");
    }
    if (req->content_len > SCRIPT_MAX_SIZE) {
        return send_result(req, "script too large (max 32 KB)");
    }
    char *src = read_body(req, SCRIPT_MAX_SIZE);
    if (!src) {
        return send_result(req, "upload failed");
    }
    const char *err = script_write(name, src, req->content_len);
    free(src);
    return send_result(req, err);
}

static void scripts_changed(void)
{
    console_broadcast("scripts");
}

esp_err_t script_web_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/api/scripts/log", .method = HTTP_GET,  .handler = log_get },
        { .uri = "/api/scripts",     .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/scripts",     .method = HTTP_POST, .handler = action_post },
        { .uri = "/scripts/*",       .method = HTTP_GET,  .handler = source_get },
        { .uri = "/scripts/*",       .method = HTTP_PUT,  .handler = source_put },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    script_set_change_cb(scripts_changed);
    return ESP_OK;
}
