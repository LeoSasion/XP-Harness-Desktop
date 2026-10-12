/* Included by main.c: preserve the existing runtime and add desktop persistence. */
#include "memory.inc"

static int agent_wire(const char *type, const char *text) {
    cJSON *event;
    char *json;
    int ok, too_large;
    if (agent_pipe == INVALID_HANDLE_VALUE || agent_broken) return 0;
    event = cJSON_CreateObject(); cJSON_AddStringToObject(event, "type", type);
    if (text) cJSON_AddStringToObject(event, "text", text);
    json = cJSON_PrintUnformatted(event); cJSON_Delete(event);
    if (!json) { agent_broken = 1; return 0; }
    too_large = strlen(json) > IPC_MAX_FRAME;
    if (too_large) {
        free(json);
        json = strdup("{\"type\":\"error\",\"text\":\"Metadata exceeds the 256 KiB IPC limit. Shorten project notes or names.\"}");
    }
    ok = json && ipc_write(agent_pipe, json, agent_parent, 10000);
    free(json); if (!ok) agent_broken = 1;
    return ok && !too_large;
}
static void agent_send(const char *type, const char *text) {
    char *chunk;
    size_t length;
    if (agent_pipe == INVALID_HANDLE_VALUE) return;
    if (!text || !strcmp(type, "catalog") || !strcmp(type, "session")) { agent_wire(type, text); return; }
    if (!*text) { agent_wire(type, text); return; }
    chunk = malloc(24577);
    if (!chunk) { agent_wire("error", "Not enough memory to send desktop output."); term_interrupted_flag = 1; return; }
    while (*text && !agent_broken) {
        length = mem_boundary(text, 24576);
        if (!length) break;
        memcpy(chunk, text, length); chunk[length] = 0;
        agent_wire(type, chunk); text += length;
    }
    free(chunk);
}
static void agent_emit(const char *type, const char *text) {
    if (agent_pipe == INVALID_HANDLE_VALUE) return;
    if (!mem_archive(type, text)) {
        if (!memory.reported) { memory.reported = 1; agent_wire("error", memory.error); }
        term_interrupted_flag = 1; memory.recording = 0; return;
    }
    agent_send(type, text);
}
static void agent_reasoning(const char *response) {
    cJSON *root = response ? cJSON_Parse(response) : NULL, *message;
    const char *text;
    message = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0), "message");
    text = mem_string(message, "reasoning_content");
    if (!text) text = mem_string(message, "reasoning");
    if (text && *text) agent_emit("reasoning", text);
    cJSON_Delete(root);
}

static cJSON *agent_execution;
static char *agent_tool_begin(struct conversation_t *convo, struct config_t *config, cJSON *call) {
    cJSON *function, *args, *input, *turn, *tools, *event, *reply, *allow;
    const char *id, *name, *arguments, *command, *type, *reply_id;
    char request_id[80], cwd[MAX_PATH * 3], *json, *frame = NULL;
    int index, accepted = 0;
    agent_execution = NULL;
    if (agent_pipe == INVALID_HANDLE_VALUE) return NULL; /* Legacy CLI mode. */
    if (term_interrupted()) return strdup("Not executed: task interrupted.");
    function = cJSON_GetObjectItemCaseSensitive(call, "function");
    id = mem_string(call, "id"); name = mem_string(function, "name"); arguments = mem_string(function, "arguments");
    args = arguments && mem_json_text(arguments, strlen(arguments)) ? cJSON_ParseWithOpts(arguments, NULL, 1) : NULL;
    command = mem_string(args, "command"); input = cJSON_GetObjectItemCaseSensitive(args, "stdin");
    if (!name || strcmp(name, "cmd") || !command || !*command || strlen(command) > 4000 ||
        (input && (!cJSON_IsString(input) || strlen(input->valuestring) > 131072))) {
        cJSON_Delete(args);
        return strdup("Not executed: invalid cmd arguments (command 1..4000 bytes, optional stdin up to 128 KiB).");
    }
    turn = cJSON_GetObjectItemCaseSensitive(memory.session, "turn");
    tools = cJSON_GetObjectItemCaseSensitive(turn, "tools");
    for (index = memory.turn_start; index < cJSON_GetArraySize(convo->messages); ++index)
        if (cJSON_GetArrayItem(convo->messages, index) == convo->pending_tool_msg) break;
    mem_id(request_id, 'a'); agent_execution = cJSON_CreateObject();
    if (!agent_execution || !cJSON_AddStringToObject(agent_execution, "id", id) ||
        !cJSON_AddStringToObject(agent_execution, "request", request_id) ||
        !cJSON_AddNumberToObject(agent_execution, "index", index - memory.turn_start) ||
        !cJSON_AddStringToObject(agent_execution, "state", "pending") || !cJSON_AddItemToArray(tools, agent_execution)) {
        cJSON_Delete(agent_execution); agent_execution = NULL; memory.failed = 1;
        mem_fail("Cannot journal the tool request; command was not run."); goto failed;
    }
    if (!mem_checkpoint(convo)) goto failed;
    agent_emit("tool", command);
    if (input) agent_emit("tool_stdin", input->valuestring);
    if (term_interrupted()) goto cancelled;
    event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "id", request_id); cJSON_AddStringToObject(event, "tool_call_id", id);
    cJSON_AddStringToObject(event, "name", name); cJSON_AddStringToObject(event, "cwd", path_utf8(config->cwd, cwd));
    cJSON_AddStringToObject(event, "command", command); cJSON_AddStringToObject(event, "stdin", input ? input->valuestring : "");
    json = cJSON_PrintUnformatted(event); cJSON_Delete(event);
    if (!json || strlen(json) > IPC_MAX_FRAME - 128 || !agent_wire("permission", json)) {
        free(json); goto cancelled;
    }
    free(json);
    /* UI only writes on this pipe while Core is explicitly waiting for a decision. */
    if (!ipc_read(agent_pipe, &frame, agent_cancel ? agent_cancel : agent_parent, 600000)) {
        term_interrupted_flag = 1; goto cancelled;
    }
    reply = mem_json_text(frame, strlen(frame)) ? cJSON_ParseWithOpts(frame, NULL, 1) : NULL;
    type = mem_string(reply, "type"); reply_id = mem_string(reply, "id"); allow = cJSON_GetObjectItemCaseSensitive(reply, "allow");
    accepted = type && !strcmp(type, "permission") && reply_id && !strcmp(reply_id, request_id) && cJSON_IsTrue(allow);
    cJSON_Delete(reply); free(frame); frame = NULL;
    if (!accepted || term_interrupted()) {
        mem_set(agent_execution, "state", "denied"); cJSON_Delete(args);
        return strdup("Not executed: permission denied or invalid/stale approval. Do not bypass this decision.");
    }
    if (!mem_set(agent_execution, "state", "running") || !mem_save()) goto failed;
    cJSON_Delete(args); return NULL;
cancelled:
    free(frame); mem_set(agent_execution, "state", "not_executed");
    cJSON_Delete(args); return strdup("Not executed: approval was interrupted or unavailable.");
failed:
    agent_emit("error", memory.error); term_interrupted_flag = 1;
    cJSON_Delete(args); return strdup("Not executed: durable tool state could not be saved.");
}

static int agent_tool_end(struct conversation_t *convo, const char *result) {
    if (agent_pipe == INVALID_HANDLE_VALUE) return 1;
    if (agent_execution) {
        if (!strcmp(mem_string(agent_execution, "state"), "running"))
            mem_set(agent_execution, "state", term_interrupted() ? "interrupted" : "completed");
        if (!mem_set(agent_execution, "result", result)) memory.failed = 1;
    }
    /* Persist the display alongside the raw result before notifying the UI. */
    if (mem_append_bounded(memory.session, "\r\n\r\nTool result:\r\n", result)) {
        lstrcpynA(memory.kind, "result", sizeof(memory.kind));
        if (mem_checkpoint(convo)) return 1;
    }
    memory.failed = 1;
    agent_wire("error", memory.error); term_interrupted_flag = 1; return 0;
}
static void agent_chat(cJSON *request, struct config_t *config, struct http_context_t *http, struct conversation_t *convo) {
    const char *id = mem_string(request, "id"), *text = mem_string(request, "text");
    cJSON *projection, *full;
    CURLcode result;
    int round = 0, status, success = 0;
    char progress[80];
    if (!text || !*text || !mem_utf8(text) || strlen(text) > 65536) {
        agent_emit("error", "Chat text must contain 1 to 65536 UTF-8 bytes."); return;
    }
    if (!id) id = mem_string(memory.root, "active");
    if (!memory.session || strcmp(mem_string(memory.session, "id"), id)) {
        if (!mem_open(id, config, convo, 1)) { agent_emit("error", memory.error); return; }
    }
    agent_execution = NULL;
    if (!mem_turn_begin(convo, text)) { memory.failed = 1; agent_emit("error", memory.error); return; }
    memory.recording = 1;
    http->stream = config->stream && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "stream"));
    if (!llm_configured(config)) {
        agent_emit("error", "Check [LLM] ApiUrl, Model and optional ExtraBody JSON. Edit vc6ai.ini and restart the session connection.");
        goto finished;
    }
    for (round = 0; round < 24 && !term_interrupted(); ++round) {
        projection = mem_project_context(convo);
        if (!projection) { agent_emit("error", memory.error); break; }
        sprintf(progress, "Request %d", round + 1); agent_emit("status", progress);
        full = cJSON_DetachItemFromObjectCaseSensitive(convo->root, "messages");
        if (!cJSON_AddItemToObject(convo->root, "messages", projection)) {
            cJSON_Delete(projection); cJSON_AddItemToObject(convo->root, "messages", full);
            agent_emit("error", "Not enough memory for request context."); break;
        }
        result = llm_request(http, convo);
        cJSON_DeleteItemFromObjectCaseSensitive(convo->root, "messages");
        cJSON_AddItemToObject(convo->root, "messages", full);
        if (result || term_interrupted()) break;
        status = convo_add_response(convo, http->res);
        if (status < 0) break;
        if (!mem_checkpoint(convo)) { agent_emit("error", memory.error); break; }
        if (!http->streamed) agent_reasoning(http->res);
        if (!status) {
            if (!http->streamed) agent_emit("assistant", convo->content);
            success = !term_interrupted(); break;
        }
        convo_handle_tool_calls(convo, config);
    }
finished:
    if (round == 24) agent_emit("error", "Stopped after 24 model requests in this turn.");
    if (!success) {
        if (term_interrupted()) agent_emit("error", "Turn interrupted. Recorded tool outcomes are retained; commands will not be replayed.");
    }
    memory.recording = 0;
    if (!mem_turn_end(convo, success)) {
        agent_wire("error", memory.error);
        memory.failed = 1;
    }
    if (!memory.failed && !success) mem_open(mem_string(memory.session, "id"), config, convo, 0);
    agent_execution = NULL;
    mem_metadata();
}

static int agent_main(int argc, char **argv) {
    struct config_t config = {0};
    struct http_context_t http = {0};
    struct conversation_t convo = {0};
    char *frame = NULL, *end;
    cJSON *request;
    const char *type, *text, *id;
    DWORD pid;
    int initialized = 0, loaded = 0, ok;
    if ((argc != 5 && argc != 7) || strcmp(argv[3], "--parent")) return 2;
    pid = strtoul(argv[4], &end, 10);
    if (!pid || *end) return 2;
    agent_parent = OpenProcess(SYNCHRONIZE | (argc == 7 ? PROCESS_DUP_HANDLE : 0), FALSE, pid);
    if (!agent_parent) return 2;
    if (argc == 7) {
        DWORD handle = strtoul(argv[6], &end, 10);
        if (strcmp(argv[5], "--cancel") || !handle || *end ||
            !DuplicateHandle(agent_parent, (HANDLE)(ULONG_PTR)handle, GetCurrentProcess(), &agent_cancel, SYNCHRONIZE, FALSE, 0)) goto cleanup;
    }
    agent_pipe = ipc_create_server(argv[2]);
    if (agent_pipe == INVALID_HANDLE_VALUE || !ipc_connect(agent_pipe, agent_parent, 15000)) goto cleanup;
    config_init(&config, ini_file);
    loaded = mem_init();
    if (loaded) mem_open(mem_string(memory.root, "active"), &config, &convo, 0);
    if (!http_init(&http, 120L)) { initialized = 1; llm_init(&http, &config); }
    while (!term_interrupted() && ipc_read(agent_pipe, &frame, agent_parent, INFINITE)) {
        request = mem_json_text(frame, strlen(frame)) ? cJSON_ParseWithOpts(frame, NULL, 1) : NULL;
        free(frame); frame = NULL;
        type = mem_string(request, "type"); text = mem_string(request, "text"); id = mem_string(request, "id");
        ok = 1;
        if (!cJSON_IsObject(request) || !type) agent_emit("error", "Invalid JSON request: type is required and text must be UTF-8 without NUL.");
        else if (!strcmp(type, "shutdown")) { cJSON_Delete(request); agent_emit("done", NULL); break; }
        else if (!loaded || memory.failed) agent_emit("error", memory.error);
        else if (!strcmp(type, "library")) mem_catalog();
        else if (!strcmp(type, "open")) ok = mem_open(id, &config, &convo, 1);
        else if (!strcmp(type, "new_project")) ok = mem_create_project(text, mem_string(request, "path"), &config, &convo);
        else if (!strcmp(type, "new_chat")) ok = mem_create_chat(mem_string(request, "project"), text, &config, &convo);
        else if (!strcmp(type, "delete_chat")) ok = mem_delete_chat(id, &config, &convo);
        else if (!strcmp(type, "export_chat")) ok = mem_export_chat(id, mem_string(request, "path"));
        else if (!strcmp(type, "rename")) ok = mem_change(id, text, 0);
        else if (!strcmp(type, "memory")) ok = mem_change(NULL, text, 1);
        else if (!strcmp(type, "reset")) ok = mem_reset(&convo);
        else if (!strcmp(type, "chat")) {
            if (initialized) agent_chat(request, &config, &http, &convo);
            else agent_emit("error", "Cannot initialize libcurl.");
        } else agent_emit("error", "Unknown request type.");
        if (!ok) agent_emit("error", memory.error);
        cJSON_Delete(request); agent_emit("done", NULL);
    }
cleanup:
    free(frame); convo_cleanup(&convo);
    if (initialized || http.curl) http_cleanup(&http);
    mem_close();
    if (agent_pipe != INVALID_HANDLE_VALUE) CloseHandle(agent_pipe);
    CloseHandle(agent_parent); agent_pipe = INVALID_HANDLE_VALUE; agent_parent = NULL;
    if (agent_cancel) CloseHandle(agent_cancel);
    agent_cancel = NULL;
    return 0;
}
