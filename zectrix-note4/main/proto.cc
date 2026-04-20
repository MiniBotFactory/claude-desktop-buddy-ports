// Hardware Buddy protocol parser. Uses cJSON (bundled with ESP-IDF).

#include "proto.h"

#include <string.h>
#include <stdio.h>
#include <cJSON.h>
#include <esp_timer.h>
#include <limits.h>

static uint32_t now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

bool proto_apply(proto_snapshot_t *s, const char *line) {
    if (!line || line[0] != '{') return false;
    cJSON *doc = cJSON_Parse(line);
    if (!doc) return false;

    // Determine if this is a full snapshot vs a ping (only cmd/time).
    // Same rule as the other ports: a snapshot has at least one of total,
    // running, waiting, entries, msg, or prompt.
    bool is_snapshot =
        cJSON_IsNumber(cJSON_GetObjectItem(doc, "total"))   ||
        cJSON_IsNumber(cJSON_GetObjectItem(doc, "running")) ||
        cJSON_IsNumber(cJSON_GetObjectItem(doc, "waiting")) ||
        cJSON_HasObjectItem(doc, "entries")                 ||
        cJSON_HasObjectItem(doc, "msg")                     ||
        cJSON_HasObjectItem(doc, "prompt");

    if (!is_snapshot) { cJSON_Delete(doc); return false; }

    cJSON *it;
    if ((it = cJSON_GetObjectItem(doc, "total"))   && cJSON_IsNumber(it)) s->total   = (uint8_t)it->valueint;
    if ((it = cJSON_GetObjectItem(doc, "running")) && cJSON_IsNumber(it)) s->running = (uint8_t)it->valueint;
    if ((it = cJSON_GetObjectItem(doc, "waiting")) && cJSON_IsNumber(it)) s->waiting = (uint8_t)it->valueint;
    if ((it = cJSON_GetObjectItem(doc, "tokens_today")) && cJSON_IsNumber(it))
        s->tokens_today = (uint32_t)it->valuedouble;

    if ((it = cJSON_GetObjectItem(doc, "msg")) && cJSON_IsString(it))
        copy_str(s->msg, sizeof(s->msg), it->valuestring);

    cJSON *pr = cJSON_GetObjectItem(doc, "prompt");
    if (pr && cJSON_IsObject(pr)) {
        cJSON *pid = cJSON_GetObjectItem(pr, "id");
        cJSON *pt  = cJSON_GetObjectItem(pr, "tool");
        cJSON *ph  = cJSON_GetObjectItem(pr, "hint");
        copy_str(s->prompt_id,   sizeof(s->prompt_id),   pid && cJSON_IsString(pid) ? pid->valuestring : "");
        copy_str(s->prompt_tool, sizeof(s->prompt_tool), pt  && cJSON_IsString(pt)  ? pt->valuestring  : "");
        copy_str(s->prompt_hint, sizeof(s->prompt_hint), ph  && cJSON_IsString(ph)  ? ph->valuestring  : "");
    } else {
        s->prompt_id[0] = 0; s->prompt_tool[0] = 0; s->prompt_hint[0] = 0;
    }

    s->last_update_ms = now_ms();
    cJSON_Delete(doc);
    return true;
}

persona_t proto_persona(const proto_snapshot_t *s) {
    uint32_t t = now_ms();
    uint32_t since = (s->last_update_ms == 0) ? UINT32_MAX : (t - s->last_update_ms);
    bool online = since < 30000;
    if (s->waiting > 0 || s->prompt_id[0]) return PERSONA_ATTENTION;
    if (s->running > 0)                    return PERSONA_BUSY;
    if (!online)                           return PERSONA_SLEEP;
    return PERSONA_IDLE;
}

int proto_fmt_permission(char *buf, size_t buflen, const char *id, const char *decision) {
    return snprintf(buf, buflen,
        "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}\n", id, decision);
}
