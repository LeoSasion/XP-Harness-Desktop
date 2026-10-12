/* Durable current/last-turn journal; no automatic command replay. */
static int mem_turn_valid(cJSON *turn) {
    cJSON *messages, *tools, *entry, *other, *index, *message, *call;
    const char *state;
    int found;
    if (!turn) return 1; /* Read libraries written before execution journaling. */
    messages = cJSON_GetObjectItemCaseSensitive(turn, "messages");
    tools = cJSON_GetObjectItemCaseSensitive(turn, "tools");
    if (!cJSON_IsObject(turn) || !mem_messages_ex(messages, 1) || !cJSON_IsArray(tools) ||
        cJSON_GetArraySize(tools) > 384) return 0;
    cJSON_ArrayForEach(entry, tools) {
        state = mem_string(entry, "state"); index = cJSON_GetObjectItemCaseSensitive(entry, "index");
        if (!mem_text(entry, "id", 256, 0) || !mem_text(entry, "request", 80, 0) ||
            !state || !cJSON_IsNumber(index) || index->valuedouble < 0 ||
            index->valuedouble >= cJSON_GetArraySize(messages) || index->valuedouble != index->valueint) return 0;
        if (strcmp(state, "pending") && strcmp(state, "running") && strcmp(state, "completed") &&
            strcmp(state, "denied") && strcmp(state, "interrupted") && strcmp(state, "unknown") && strcmp(state, "not_executed")) return 0;
        if (strcmp(state, "pending") && strcmp(state, "running") && !mem_text(entry, "result", LIBRARY_MAX, 1)) return 0;
        message = cJSON_GetArrayItem(messages, index->valueint); found = 0;
        cJSON_ArrayForEach(call, cJSON_GetObjectItemCaseSensitive(message, "tool_calls"))
            if (!strcmp(mem_string(call, "id"), mem_string(entry, "id"))) found = 1;
        if (!found) return 0;
        for (other = entry->next; other; other = other->next)
            if ((mem_string(other, "request") && !strcmp(mem_string(entry, "request"), mem_string(other, "request"))) ||
                (mem_string(other, "id") && !strcmp(mem_string(entry, "id"), mem_string(other, "id")) &&
                 cJSON_GetObjectItemCaseSensitive(other, "index") && cJSON_GetObjectItemCaseSensitive(other, "index")->valuedouble == index->valuedouble)) return 0;
    }
    return 1;
}

static int mem_checkpoint(struct conversation_t *convo) {
    cJSON *turn = cJSON_GetObjectItemCaseSensitive(memory.session, "turn"), *messages, *copy;
    int i, count = cJSON_GetArraySize(convo->messages);
    if (!turn || memory.failed) return 0;
    messages = cJSON_CreateArray();
    if (!messages) goto failed;
    for (i = memory.turn_start; i < count; ++i) {
        copy = cJSON_Duplicate(cJSON_GetArrayItem(convo->messages, i), 1);
        if (!copy || !cJSON_AddItemToArray(messages, copy)) { cJSON_Delete(copy); goto failed; }
    }
    if (!mem_messages_ex(messages, 1) || !cJSON_ReplaceItemInObjectCaseSensitive(turn, "messages", messages)) goto failed;
    return mem_save();
failed:
    cJSON_Delete(messages); memory.failed = 1;
    return mem_fail("Cannot checkpoint this turn; no further command will execute.");
}

static cJSON *mem_execution(cJSON *turn, int index, const char *id) {
    cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(turn, "tools"))
        if (cJSON_GetObjectItemCaseSensitive(entry, "index")->valueint == index && !strcmp(mem_string(entry, "id"), id)) return entry;
    return NULL;
}

static int mem_finish_turn(cJSON *session, const char *reason) {
    cJSON *turn = cJSON_GetObjectItemCaseSensitive(session, "turn"), *messages, *calls = NULL;
    cJSON *message, *call, *result, *entry, *raw, *copy;
    int i, j, index = -1, found;
    const char *id, *state, *text;
    if (turn) {
        messages = cJSON_GetObjectItemCaseSensitive(turn, "messages");
        if (reason) {
            for (i = 0; i < cJSON_GetArraySize(messages); ++i) {
                message = cJSON_GetArrayItem(messages, i);
                if (cJSON_GetObjectItemCaseSensitive(message, "tool_calls")) {
                    calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls"); index = i;
                }
            }
            cJSON_ArrayForEach(call, calls) {
                id = mem_string(call, "id"); found = 0;
                for (j = index + 1; j < cJSON_GetArraySize(messages); ++j) {
                    message = cJSON_GetArrayItem(messages, j);
                    if (mem_string(message, "tool_call_id") && !strcmp(id, mem_string(message, "tool_call_id"))) found = 1;
                }
                if (found) continue;
                entry = mem_execution(turn, index, id); state = entry ? mem_string(entry, "state") : NULL;
                text = entry ? mem_string(entry, "result") : NULL;
                if (!text) {
                    text = state && !strcmp(state, "running") ?
                        "Execution outcome UNKNOWN after interruption. Command may have changed files. Ask the user to verify effects; do not automatically repeat it." :
                        "Not executed: task ended before approval or command launch. No approval carries over to a resumed turn.";
                    if (entry && (!mem_set(entry, "state", state && !strcmp(state, "running") ? "unknown" : "not_executed") || !mem_set(entry, "result", text))) return 0;
                }
                if (!mem_append_bounded(session, "\r\n\r\nRecovered tool result:\r\n", text)) return 0;
                result = cJSON_CreateObject();
                if (!result || !cJSON_AddStringToObject(result, "role", "tool") || !cJSON_AddStringToObject(result, "tool_call_id", id) ||
                    !cJSON_AddStringToObject(result, "content", text) || !cJSON_AddItemToArray(messages, result)) { cJSON_Delete(result); return mem_fail("Cannot recover tool results."); }
            }
            message = cJSON_GetArrayItem(messages, cJSON_GetArraySize(messages) - 1);
            if (message && (strcmp(mem_string(message, "role"), "assistant") || cJSON_GetObjectItemCaseSensitive(message, "tool_calls"))) {
                result = cJSON_CreateObject();
                if (!result || !cJSON_AddStringToObject(result, "role", "assistant") || !cJSON_AddStringToObject(result, "content",
                    "[Task interrupted or failed. Recorded tool outcomes are retained. Commands were not replayed; verify UNKNOWN outcomes before retrying.]") ||
                    !cJSON_AddItemToArray(messages, result)) { cJSON_Delete(result); return mem_fail("Cannot close recovered turn."); }
            }
        }
        if (!mem_messages(messages)) return mem_fail("Recovered turn has an invalid tool chain; original file is preserved.");
        raw = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(session, "messages"), 1);
        if (!raw) return mem_fail("Cannot preserve recovered history.");
        cJSON_ArrayForEach(message, messages) {
            copy = cJSON_Duplicate(message, 1);
            if (!copy || !cJSON_AddItemToArray(raw, copy)) { cJSON_Delete(copy); cJSON_Delete(raw); return mem_fail("Cannot preserve recovered history."); }
        }
        if (!mem_messages(raw)) { cJSON_Delete(raw); return mem_fail("Invalid recovered history."); }
        cJSON_ReplaceItemInObjectCaseSensitive(session, "messages", raw);
    }
    if (reason && !mem_append_bounded(session, "\r\n\r\n[Interrupted]\r\n", reason)) return 0;
    cJSON_ReplaceItemInObjectCaseSensitive(session, "pending", cJSON_CreateBool(0));
    return 1;
}
