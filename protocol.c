#include "protocol.h"
#include "vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *json_to_alloced(cJSON *j)
{
    /* Compact (PrintUnformatted) keeps responses small. The returned buffer
     * is owned by cJSON's allocator (malloc by default) so free() is fine. */
    if (!j) return NULL;
    return cJSON_PrintUnformatted(j);
}

char *proto_encode_lease_response(const proto_lease_response_args *a)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    cJSON *file0 = cJSON_CreateObject();
    cJSON *out   = cJSON_CreateObject();
    if (!root || !files || !file0 || !out) goto fail;

    char side_str[2] = { a->side, 0 };

    cJSON_AddStringToObject(root, "workunit_id",      a->workunit_id);
    cJSON_AddNumberToObject(root, "q_start",          (double)a->q_start);
    cJSON_AddNumberToObject(root, "q_range",          (double)a->q_range);
    cJSON_AddStringToObject(root, "side",             side_str);
    cJSON_AddNumberToObject(root, "lease_seconds",    (double)a->lease_seconds);
    cJSON_AddStringToObject(root, "siever",           a->siever);
    cJSON_AddStringToObject(root, "command_template", a->command_template);
    cJSON_AddStringToObject(root, "siever_args",      a->siever_args ? a->siever_args : "");

    cJSON_AddStringToObject(file0, "name",   a->file_name);
    cJSON_AddStringToObject(file0, "sha256", a->file_sha256_hex);
    cJSON_AddStringToObject(file0, "url",    a->file_url);
    cJSON_AddItemToArray(files, file0); file0 = NULL; /* now owned by files */
    cJSON_AddItemToObject(root, "files", files); files = NULL;

    cJSON_AddStringToObject(out, "name",      a->output_name);
    cJSON_AddNumberToObject(out, "max_bytes", (double)a->output_max_bytes);
    cJSON_AddItemToObject(root, "output", out); out = NULL;

    char *s = json_to_alloced(root);
    cJSON_Delete(root);
    return s;

fail:
    if (file0) cJSON_Delete(file0);
    if (files) cJSON_Delete(files);
    if (out)   cJSON_Delete(out);
    if (root)  cJSON_Delete(root);
    return NULL;
}

static void copy_str_field(cJSON *root, const char *name,
                           char *buf, size_t buf_n)
{
    if (!buf || buf_n == 0) return;
    buf[0] = '\0';
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (v && cJSON_IsString(v) && v->valuestring) {
        snprintf(buf, buf_n, "%s", v->valuestring);
    }
}

int proto_decode_lease_request(const char *body, size_t body_len,
                               char *client_id_buf,      size_t client_id_buf_n,
                               char *client_version_buf, size_t client_version_buf_n)
{
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (!root) return -1;
    copy_str_field(root, "client_id",      client_id_buf,      client_id_buf_n);
    copy_str_field(root, "client_version", client_version_buf, client_version_buf_n);
    cJSON_Delete(root);
    return 0;
}

char *proto_encode_lease_request(const char *client_id, const char *client_version)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "client_id",      client_id      ? client_id      : "");
    cJSON_AddStringToObject(root, "client_version", client_version ? client_version : "");
    char *s = json_to_alloced(root);
    cJSON_Delete(root);
    return s;
}

static void copy_str_field_into(cJSON *root, const char *name,
                                char *buf, size_t buf_n)
{
    if (!buf || buf_n == 0) return;
    buf[0] = '\0';
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (v && cJSON_IsString(v) && v->valuestring) {
        snprintf(buf, buf_n, "%s", v->valuestring);
    }
}

static int64_t copy_int_field(cJSON *root, const char *name, int64_t fallback)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, name);
    if (v && cJSON_IsNumber(v)) return (int64_t)v->valuedouble;
    return fallback;
}

int proto_decode_lease_response(const char *body, size_t body_len,
                                proto_lease_response_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (!root) return -1;

    copy_str_field_into(root, "workunit_id",      out->workunit_id,      sizeof(out->workunit_id));
    copy_str_field_into(root, "siever",           out->siever,           sizeof(out->siever));
    copy_str_field_into(root, "command_template", out->command_template, sizeof(out->command_template));
    copy_str_field_into(root, "siever_args",      out->siever_args,      sizeof(out->siever_args));

    out->q_start       = copy_int_field(root, "q_start",       0);
    out->q_range       = copy_int_field(root, "q_range",       0);
    out->lease_seconds = copy_int_field(root, "lease_seconds", 0);

    /* side is "a" or "r" */
    cJSON *side = cJSON_GetObjectItemCaseSensitive(root, "side");
    out->side = (side && cJSON_IsString(side) && side->valuestring && side->valuestring[0])
                ? side->valuestring[0] : '?';

    /* files[0] */
    cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (files && cJSON_IsArray(files)) {
        cJSON *f0 = cJSON_GetArrayItem(files, 0);
        if (f0 && cJSON_IsObject(f0)) {
            copy_str_field_into(f0, "name",   out->file_name,        sizeof(out->file_name));
            copy_str_field_into(f0, "sha256", out->file_sha256_hex,  sizeof(out->file_sha256_hex));
            copy_str_field_into(f0, "url",    out->file_url,         sizeof(out->file_url));
        }
    }

    cJSON *output = cJSON_GetObjectItemCaseSensitive(root, "output");
    if (output && cJSON_IsObject(output)) {
        copy_str_field_into(output, "name", out->output_name, sizeof(out->output_name));
        out->output_max_bytes = copy_int_field(output, "max_bytes", 0);
    }

    cJSON_Delete(root);

    /* Required-field sanity check — caller can decide what to do. */
    if (out->workunit_id[0] == '\0' ||
        out->siever[0]      == '\0' ||
        out->file_sha256_hex[0] == '\0' ||
        out->file_url[0]    == '\0' ||
        out->output_name[0] == '\0') {
        return -1;
    }
    return 0;
}

char *proto_encode_submit_response(int accepted,
                                   const char *verified_status,
                                   int64_t num_relations)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject  (root, "accepted",      accepted ? 1 : 0);
    cJSON_AddStringToObject(root, "verified",      verified_status ? verified_status : "skipped");
    cJSON_AddNumberToObject(root, "num_relations", (double)num_relations);
    char *s = json_to_alloced(root);
    cJSON_Delete(root);
    return s;
}

char *proto_encode_health_response(int ok, const char *job_id,
                                   int64_t uptime_seconds)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject  (root, "ok",             ok ? 1 : 0);
    cJSON_AddStringToObject(root, "job_id",         job_id ? job_id : "");
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)uptime_seconds);
    char *s = json_to_alloced(root);
    cJSON_Delete(root);
    return s;
}
