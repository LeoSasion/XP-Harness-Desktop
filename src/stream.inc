/* Chat Completions SSE, included by main.c; no additional library or process. */
struct stream_t {
    struct http_context_t *http;
    cJSON *reply, *choice, *message;
    size_t parsed;
    int done, seen, section;
};

static int stream_append(cJSON *object, const char *key, cJSON *delta) {
    cJSON *value;
    size_t oldlen, addlen;
    char *joined;
    if (!cJSON_IsString(delta)) return cJSON_IsNull(delta) || !delta;
    value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value) return cJSON_AddStringToObject(object, key, delta->valuestring) != NULL;
    if (!cJSON_IsString(value)) return 0;
    oldlen = strlen(value->valuestring); addlen = strlen(delta->valuestring);
    if (oldlen + addlen > 4 * 1024 * 1024UL) return 0;
    joined = realloc(value->valuestring, oldlen + addlen + 1);
    if (!joined) return 0;
    memcpy(joined + oldlen, delta->valuestring, addlen + 1);
    value->valuestring = joined;
    return 1;
}

static void stream_print(struct stream_t *s, cJSON *text, int section) {
    if (!cJSON_IsString(text) || !text->valuestring[0]) return;
    if (agent_pipe != INVALID_HANDLE_VALUE) {
        agent_emit(section == 1 ? "reasoning_delta" : "answer_delta", text->valuestring);
        return;
    }
    if (s->section != section) {
        term_reset();
        printf("\n[%s]\n", section == 1 ? "Thinking" : "Answer");
        s->section = section;
    }
    if (section == 1) term_color(C_FG_DARK_GRAY); else term_reset();
    term_write_utf8(text->valuestring);
    fflush(stdout);
}

static int stream_line(struct stream_t *s, char *line) {
    cJSON *chunk, *choice, *delta, *text, *calls, *call, *target, *function, *index, *finish;
    int ok = 1, n;
    if (strncmp(line, "data:", 5)) return 1; /* comments / keep-alive */
    line += 5; while (*line == ' ') ++line;
    if (!strcmp(line, "[DONE]")) { s->done = 1; return 1; }
    if (s->done) return 0;
    chunk = cJSON_ParseWithOpts(line, NULL, 1);
    if (!cJSON_IsObject(chunk)) { cJSON_Delete(chunk); return 0; }
    if (cJSON_GetObjectItemCaseSensitive(chunk, "error")) {
        text = cJSON_GetObjectItemCaseSensitive(chunk, "error");
        text = cJSON_GetObjectItemCaseSensitive(text, "message");
        fprintf(stderr, "\nAPI stream error: %s\n", cJSON_IsString(text) ? text->valuestring : "Unknown error");
        agent_emit("error", cJSON_IsString(text) ? text->valuestring : "Unknown API stream error");
        cJSON_Delete(chunk); return 0;
    }
    s->seen = 1;
    text = cJSON_GetObjectItemCaseSensitive(chunk, "usage");
    if (cJSON_IsObject(text)) {
        cJSON_DeleteItemFromObject(s->reply, "usage");
        cJSON_AddItemToObject(s->reply, "usage", cJSON_Duplicate(text, 1));
    }
    choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(chunk, "choices"), 0);
    if (!choice) { ok = cJSON_IsObject(text); cJSON_Delete(chunk); return ok; }
    delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
    if (!cJSON_IsObject(delta)) ok = 0;
    if (cJSON_IsString(finish))
        cJSON_ReplaceItemInObject(s->choice, "finish_reason", cJSON_Duplicate(finish, 1));
    text = cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
    if (!text) text = cJSON_GetObjectItemCaseSensitive(delta, "reasoning");
    if (text) {
        ok = ok && stream_append(s->message, "reasoning_content", text);
        stream_print(s, text, 1);
    }
    text = cJSON_GetObjectItemCaseSensitive(delta, "content");
    ok = ok && stream_append(s->message, "content", text);
    stream_print(s, text, 2);
    calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
    if (calls && !cJSON_IsArray(calls)) ok = 0;
    cJSON_ArrayForEach(call, calls) {
        index = cJSON_GetObjectItemCaseSensitive(call, "index");
        if (!cJSON_IsNumber(index) || index->valuedouble != index->valueint ||
            index->valueint < 0 || index->valueint >= 16) { ok = 0; break; }
        target = cJSON_GetObjectItemCaseSensitive(s->message, "tool_calls");
        if (!target) target = cJSON_AddArrayToObject(s->message, "tool_calls");
        n = index->valueint;
        while (target && cJSON_GetArraySize(target) <= n) {
            cJSON *entry = cJSON_CreateObject();
            if (!entry) { ok = 0; break; }
            cJSON_AddStringToObject(entry, "type", "function");
            cJSON_AddObjectToObject(entry, "function");
            cJSON_AddItemToArray(target, entry);
        }
        target = cJSON_GetArrayItem(target, n);
        function = cJSON_GetObjectItemCaseSensitive(call, "function");
        if (!target || !cJSON_IsObject(function)) { ok = 0; break; }
        ok = ok && stream_append(target, "id", cJSON_GetObjectItemCaseSensitive(call, "id"));
        target = cJSON_GetObjectItemCaseSensitive(target, "function");
        ok = ok && stream_append(target, "name", cJSON_GetObjectItemCaseSensitive(function, "name"));
        ok = ok && stream_append(target, "arguments", cJSON_GetObjectItemCaseSensitive(function, "arguments"));
    }
    cJSON_Delete(chunk);
    return ok;
}

static size_t stream_write(char *data, size_t size, size_t nmemb, void *userdata) {
    struct stream_t *s = userdata;
    size_t count = http_write_callback(data, size, nmemb, s->http), end;
    long status = 0;
    char saved;
    if (!count) return 0;
    curl_easy_getinfo(s->http->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 300) return count; /* Never render or execute an HTTP error body as SSE. */
    while (s->parsed < s->http->reslen) {
        for (end = s->parsed; end < s->http->reslen && s->http->res[end] != '\n'; ++end) {}
        if (end == s->http->reslen) break; /* wait for the rest of this SSE line */
        saved = s->http->res[end]; s->http->res[end] = 0;
        if (end > s->parsed && s->http->res[end - 1] == '\r') s->http->res[end - 1] = 0;
        if (!stream_line(s, s->http->res + s->parsed)) return 0;
        s->http->res[end] = saved; s->parsed = end + 1;
    }
    return count;
}

static CURLcode stream_request(struct http_context_t *http, struct conversation_t *convo) {
    struct stream_t s;
    CURLcode result;
    char *payload, *json;
    memset(&s, 0, sizeof(s)); s.http = http;
    s.reply = cJSON_Parse("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}");
    if (!s.reply) return CURLE_OUT_OF_MEMORY;
    s.choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(s.reply, "choices"), 0);
    s.message = cJSON_GetObjectItemCaseSensitive(s.choice, "message");
    cJSON_AddBoolToObject(convo->root, "stream", 1);
    payload = cJSON_PrintUnformatted(convo->root);
    cJSON_DeleteItemFromObject(convo->root, "stream");
    if (!payload) { cJSON_Delete(s.reply); return CURLE_OUT_OF_MEMORY; }
    http->reslen = 0; if (http->res) http->res[0] = 0;
    curl_easy_setopt(http->curl, CURLOPT_WRITEFUNCTION, stream_write);
    curl_easy_setopt(http->curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(http->curl, CURLOPT_POSTFIELDS, payload);
    result = curl_easy_perform(http->curl);
    curl_easy_setopt(http->curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(http->curl, CURLOPT_WRITEDATA, http);
    curl_easy_setopt(http->curl, CURLOPT_POSTFIELDS, NULL);
    free(payload);
    if (agent_pipe == INVALID_HANDLE_VALUE) { term_reset(); printf("\n"); fflush(stdout); }
    if (result == CURLE_OK && s.seen) {
        if (!s.done || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(s.choice, "finish_reason")))
            result = CURLE_PARTIAL_FILE;
        else {
            json = cJSON_PrintUnformatted(s.reply);
            if (!json) result = CURLE_OUT_OF_MEMORY;
            else { free(http->res); http->res = json; http->reslen = strlen(json); }
        }
    }
    result = llm_finish(http, result);
    http->streamed = s.seen;
    cJSON_Delete(s.reply);
    return result;
}
