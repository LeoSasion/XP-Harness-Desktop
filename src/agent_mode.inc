/* Included by main.c to reuse the existing CLI runtime without an abstraction layer. */
static void agent_emit(const char *type, const char *text) {
    cJSON *event;
    char *json;
    if (agent_pipe == INVALID_HANDLE_VALUE || agent_broken) return;
    event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", type);
    if (text) cJSON_AddStringToObject(event, "text", text);
    json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (!json) { agent_broken = 1; return; }
    if (strlen(json) > IPC_MAX_FRAME) {
        free(json);
        json = strdup("{\"type\":\"error\",\"text\":\"Reply exceeds the 256 KiB IPC limit.\"}");
    }
    if (!json || !ipc_write(agent_pipe, json, agent_parent, 10000)) agent_broken = 1;
    free(json);
}

static int agent_main(int argc, char **argv) {
    struct config_t config = {0};
    struct http_context_t http = {0};
    struct conversation_t convo = {0};
    char *frame = NULL, *end;
    cJSON *request, *type, *text;
    DWORD pid;
    int round, status, before, success, initialized = 0;
    if (argc != 5 || strcmp(argv[3], "--parent")) return 2;
    pid = strtoul(argv[4], &end, 10);
    if (!pid || *end) return 2;
    agent_parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!agent_parent) return 2;
    agent_pipe = ipc_create_server(argv[2]);
    if (agent_pipe == INVALID_HANDLE_VALUE || !ipc_connect(agent_pipe, agent_parent, 15000)) goto cleanup;
    config_init(&config, ini_file);
    convo_init(&convo, &config, tools);
    convo_add_sysprompt(&convo, &config);
    if (http_init(&http, 120L)) {
        agent_emit("error", "Cannot initialize libcurl.");
        agent_emit("done", NULL);
        goto cleanup;
    }
    initialized = 1;
    openrouter_init(&http, config.api_key);
    /* No model metadata fetch: starting the desktop must not issue a request. */
    while (!term_interrupted() && ipc_read(agent_pipe, &frame, agent_parent, INFINITE)) {
        request = cJSON_ParseWithOpts(frame, NULL, 1);
        free(frame); frame = NULL;
        type = cJSON_GetObjectItemCaseSensitive(request, "type");
        text = cJSON_GetObjectItemCaseSensitive(request, "text");
        if (!cJSON_IsObject(request) || !cJSON_IsString(type)) {
            agent_emit("error", "Invalid JSON request: type is required.");
        } else if (!strcmp(type->valuestring, "shutdown")) {
            cJSON_Delete(request); agent_emit("done", NULL); break;
        } else if (!strcmp(type->valuestring, "reset")) {
            convo_clear(&convo);
            agent_emit("status", "Conversation cleared.");
        } else if (strcmp(type->valuestring, "chat")) {
            agent_emit("error", "Unknown request type.");
        } else if (!cJSON_IsString(text) || !text->valuestring[0] || strlen(text->valuestring) > 65536) {
            agent_emit("error", "Chat text must contain 1 to 65536 UTF-8 bytes.");
        } else if (!config.api_key[0]) {
            agent_emit("error", "OpenRouter API key is missing. Edit vc6ai.ini, then click Stop before sending again.");
        } else {
            before = cJSON_GetArraySize(convo.messages);
            success = 0;
            convo_compact(&convo);
            convo_add_text_message(&convo, "user", text->valuestring);
            agent_emit("status", "Thinking...");
            for (round = 0; round < 24 && !term_interrupted(); ++round) {
                if (openrouter_request(&http, &convo)) break;
                status = convo_add_response(&convo, http.res);
                if (status < 0) break;
                if (!status) { agent_emit("assistant", convo.content); success = 1; break; }
                convo_handle_tool_calls(&convo, &config);
            }
            if (round == 24) agent_emit("error", "Stopped after 24 model requests in this turn.");
            if (!success) {
                while (cJSON_GetArraySize(convo.messages) > before)
                    cJSON_DeleteItemFromArray(convo.messages, before);
                convo.pending_tool_msg = NULL; convo.content = NULL;
            }
        }
        cJSON_Delete(request);
        agent_emit("done", NULL);
    }
cleanup:
    free(frame);
    convo_cleanup(&convo);
    if (initialized || http.curl) http_cleanup(&http);
    /* Closing lets the peer drain queued events; DisconnectNamedPipe would discard them. */
    if (agent_pipe != INVALID_HANDLE_VALUE) CloseHandle(agent_pipe);
    CloseHandle(agent_parent);
    agent_pipe = INVALID_HANDLE_VALUE; agent_parent = NULL;
    return 0;
}
