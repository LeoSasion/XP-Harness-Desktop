/* Private desktop library. Raw completed messages never use the bounded API projection. */
#define LIBRARY_MAX (32UL * 1024 * 1024)
#define TRANSCRIPT_MAX (4UL * 1024 * 1024)
#define NOTE_MAX 16384
#define NAME_MAX_BYTES 160
static struct {
    cJSON *root, *session, *project;
    HANDLE lock;
    WCHAR file[MAX_PATH], temporary[MAX_PATH];
    int failed, context_bytes, recording, reported, turn_start;
    DWORD flushed;
    size_t dirty;
    char kind[24], error[256];
} memory;

static const char *mem_string(cJSON *object, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}
static int mem_fail(const char *error) {
    lstrcpynA(memory.error, error, sizeof(memory.error));
    return 0;
}
static int mem_set(cJSON *object, const char *key, const char *text) {
    cJSON *value = cJSON_CreateString(text);
    if (!value) return mem_fail("Not enough memory for the library.");
    if (cJSON_GetObjectItemCaseSensitive(object, key)) return cJSON_ReplaceItemInObjectCaseSensitive(object, key, value);
    return cJSON_AddItemToObject(object, key, value);
}
static size_t mem_boundary(const char *text, size_t limit) {
    size_t n = strlen(text);
    if (n <= limit) return n;
    n = limit;
    while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
    return n;
}
static int mem_utf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    unsigned int c, n, minimum;
    if (!p) return 0;
    while (*p) {
        c = *p++;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; minimum = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; minimum = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; minimum = 0x10000; c &= 7; }
        else return 0;
        while (n--) { if ((*p & 0xc0) != 0x80) return 0; c = (c << 6) | (*p++ & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}
static int mem_text(cJSON *object, const char *key, size_t max, int empty) {
    const char *text = mem_string(object, key);
    return text && (empty || *text) && strlen(text) <= max && mem_utf8(text);
}
static cJSON *mem_find(const char *list, const char *id) {
    cJSON *item;
    if (!id) return NULL;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(memory.root, list)) {
        const char *candidate = mem_string(item, "id");
        if (candidate && !strcmp(candidate, id)) return item;
    }
    return NULL;
}
static int mem_name(const char *name) {
    return name && *name && strlen(name) <= NAME_MAX_BYTES && mem_utf8(name);
}
static void mem_id(char *id, char kind) {
    static unsigned long sequence;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    sprintf(id, "%c-%08lx%08lx-%lu-%lu", kind, now.dwHighDateTime, now.dwLowDateTime,
            GetCurrentProcessId(), ++sequence);
}

/* Also reject escaped U+0000: cJSON stores strings as NUL-terminated C strings. */
static int mem_json_text(const char *text, size_t size) {
    size_t i;
    if (memchr(text, 0, size) || !mem_utf8(text)) return 0;
    for (i = 0; i < size; ++i) if (text[i] == '\\') {
        if (i + 5 < size && text[i + 1] == 'u' && !memcmp(text + i + 2, "0000", 4)) return 0;
        ++i;
    }
    return 1;
}
static int mem_messages_ex(cJSON *messages, int partial) {
    cJSON *message, *calls = NULL, *call, *function;
    const char *role, *id, *ids[16];
    int count = 0, found, i, open_turn = 0;
    if (!cJSON_IsArray(messages)) return 0;
    cJSON_ArrayForEach(message, messages) {
        role = mem_string(message, "role");
        if (!role) return 0;
        if (!strcmp(role, "user")) {
            if (count || open_turn || !mem_text(message, "content", LIBRARY_MAX, 1)) return 0;
            open_turn = 1;
        } else if (!strcmp(role, "assistant")) {
            if (!open_turn || count) return 0;
            calls = cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
            if (calls) {
                if (!cJSON_IsArray(calls) || !cJSON_GetArraySize(calls) || cJSON_GetArraySize(calls) > 16) return 0;
                cJSON_ArrayForEach(call, calls) {
                    function = cJSON_GetObjectItemCaseSensitive(call, "function");
                    id = mem_string(call, "id");
                    if (!mem_text(call, "id", 256, 0) || !mem_text(function, "name", 256, 0) ||
                        !mem_text(function, "arguments", LIBRARY_MAX, 1)) return 0;
                    for (i = 0; i < count; ++i) if (!strcmp(ids[i], id)) return 0;
                    ids[count++] = id;
                }
            } else {
                if (!mem_text(message, "content", LIBRARY_MAX, 1)) return 0;
                open_turn = 0;
            }
        } else if (!strcmp(role, "tool")) {
            id = mem_string(message, "tool_call_id"); found = -1;
            if (!id || !mem_text(message, "content", LIBRARY_MAX, 1)) return 0;
            for (i = 0; i < count; ++i) if (!strcmp(ids[i], id)) found = i;
            if (found < 0) return 0;
            ids[found] = ids[--count];
        } else return 0;
    }
    return partial || (!count && !open_turn);
}
static int mem_messages(cJSON *messages) { return mem_messages_ex(messages, 0); }
static int mem_turn_valid(cJSON *turn);
static int mem_validate(void) {
    cJSON *projects, *sessions, *item, *other, *version, *pending;
    const char *id;
    version = cJSON_GetObjectItemCaseSensitive(memory.root, "version");
    projects = cJSON_GetObjectItemCaseSensitive(memory.root, "projects");
    sessions = cJSON_GetObjectItemCaseSensitive(memory.root, "sessions");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !cJSON_IsArray(projects) ||
        !cJSON_IsArray(sessions) || !cJSON_GetArraySize(projects) || !cJSON_GetArraySize(sessions) ||
        cJSON_GetArraySize(projects) > 256 || cJSON_GetArraySize(sessions) > 2048) return 0;
    cJSON_ArrayForEach(item, projects) {
        if (!mem_text(item, "id", 80, 0) || !mem_text(item, "name", NAME_MAX_BYTES, 0) ||
            !mem_text(item, "path", MAX_PATH * 4, 0) || !mem_text(item, "memory", NOTE_MAX, 1)) return 0;
        id = mem_string(item, "id");
        for (other = item->next; other; other = other->next)
            if (mem_string(other, "id") && !strcmp(id, mem_string(other, "id"))) return 0;
    }
    cJSON_ArrayForEach(item, sessions) {
        pending = cJSON_GetObjectItemCaseSensitive(item, "pending");
        if (!mem_text(item, "id", 80, 0) || !mem_text(item, "title", NAME_MAX_BYTES, 0) ||
            !mem_find("projects", mem_string(item, "project")) || !cJSON_IsBool(pending) ||
            !mem_text(item, "transcript", TRANSCRIPT_MAX, 1) ||
            !mem_messages(cJSON_GetObjectItemCaseSensitive(item, "messages")) ||
            !mem_turn_valid(cJSON_GetObjectItemCaseSensitive(item, "turn"))) return 0;
        id = mem_string(item, "id");
        if (mem_find("projects", id)) return 0;
        for (other = item->next; other; other = other->next)
            if (mem_string(other, "id") && !strcmp(id, mem_string(other, "id"))) return 0;
    }
    return mem_find("sessions", mem_string(memory.root, "active")) != NULL;
}
static int mem_save(void) {
    char *json;
    HANDLE file;
    size_t size;
    DWORD written;
    int ok;
    if (memory.failed) return 0;
    json = cJSON_PrintUnformatted(memory.root);
    if (!json) { memory.failed = 1; return mem_fail("Not enough memory to save the library; restart before continuing."); }
    size = strlen(json);
    if (size > LIBRARY_MAX) {
        free(json); memory.failed = 1;
        return mem_fail("Library exceeds 32 MiB. Existing library was not overwritten; restart before continuing.");
    }
    file = CreateFileW(memory.temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) ok = 0;
    else {
        ok = WriteFile(file, json, (DWORD)size, &written, NULL) && written == size && FlushFileBuffers(file);
        CloseHandle(file);
        if (ok) ok = MoveFileExW(memory.temporary, memory.file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    free(json);
    if (!ok) {
        memory.failed = 1;
        return mem_fail("Cannot save data/library.json. Previous file is preserved; restart before continuing.");
    }
    memory.flushed = GetTickCount(); memory.dirty = 0;
    return 1;
}
static int mem_append_limit(cJSON *session, const char *prefix, const char *text, size_t limit) {
    const char *old = mem_string(session, "transcript");
    size_t a = old ? strlen(old) : 0, b = strlen(prefix), c = strlen(text);
    char *joined;
    int ok;
    if (a + b + c > limit) return mem_fail("Session transcript exceeds 4 MiB. Start a new chat; no history was discarded.");
    joined = malloc(a + b + c + 1);
    if (!joined) return mem_fail("Not enough memory for the session transcript.");
    if (a) memcpy(joined, old, a);
    memcpy(joined + a, prefix, b); memcpy(joined + a + b, text, c + 1);
    ok = mem_set(session, "transcript", joined);
    free(joined); memory.dirty += b + c;
    return ok;
}
static int mem_append(cJSON *session, const char *prefix, const char *text) {
    return mem_append_limit(session, prefix, text, TRANSCRIPT_MAX - 256);
}
/* Recovery and tool results must not be blocked by the display budget.
   Only the display is shortened; raw messages and the journal stay intact. */
static int mem_append_bounded(cJSON *session, const char *prefix, const char *text) {
    static const char marker[] = "\r\n[Display truncated; full tool results remain in message history.]\r\n";
    const char *old = mem_string(session, "transcript");
    size_t a = old ? strlen(old) : 0, b = strlen(prefix), c = strlen(text);
    size_t m = sizeof(marker) - 1, keep, offset;
    char *joined;
    int ok;
    if (a + b + c <= TRANSCRIPT_MAX) return mem_append_limit(session, prefix, text, TRANSCRIPT_MAX);
    c = mem_boundary(text, TRANSCRIPT_MAX - m - b);
    keep = TRANSCRIPT_MAX - m - b - c;
    offset = a > keep ? a - keep : 0;
    while (offset < a && ((unsigned char)old[offset] & 0xc0) == 0x80) ++offset;
    keep = a - offset;
    joined = malloc(m + keep + b + c + 1);
    if (!joined) return mem_fail("Not enough memory for the session transcript.");
    memcpy(joined, marker, m);
    if (keep) memcpy(joined + m, old + offset, keep);
    memcpy(joined + m + keep, prefix, b);
    memcpy(joined + m + keep + b, text, c);
    joined[m + keep + b + c] = 0;
    ok = mem_set(session, "transcript", joined);
    free(joined); memory.dirty += m + b + c;
    return ok;
}
static int mem_archive(const char *type, const char *text) {
    const char *kind = NULL, *title = NULL;
    char prefix[64];
    if (!memory.recording || !memory.session || !text) return 1;
    if (memory.failed) return 0;
    if (!strcmp(type, "answer_delta") || !strcmp(type, "assistant")) { kind = "assistant"; title = "Assistant"; }
    else if (!strcmp(type, "reasoning_delta") || !strcmp(type, "reasoning")) { kind = "reasoning"; title = "Reasoning"; }
    else if (!strcmp(type, "tool")) { kind = "tool"; title = "Tool"; }
    else if (!strcmp(type, "tool_stdin")) { kind = "stdin"; title = "Tool input"; }
    else if (!strcmp(type, "tool_result")) { kind = "result"; title = "Tool result"; }
    else if (!strcmp(type, "error")) { kind = "error"; title = "Error"; }
    if (!kind) return 1;
    prefix[0] = 0;
    if (strcmp(memory.kind, kind)) sprintf(prefix, "\r\n\r\n%s:\r\n", title);
    if (!mem_append(memory.session, prefix, text)) { memory.failed = 1; return 0; }
    lstrcpynA(memory.kind, kind, sizeof(memory.kind));
    return (memory.dirty < 32768 && GetTickCount() - memory.flushed < 1000) || mem_save();
}

#include "recovery.inc"
static int mem_path(const char *utf8, char *ansi, char *canonical) {
    WCHAR wide[MAX_PATH], full[MAX_PATH];
    DWORD length, attributes;
    BOOL substituted = FALSE;
    if (!mem_utf8(utf8) || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_PATH))
        return mem_fail("Project path is invalid UTF-8 or exceeds MAX_PATH.");
    length = GetFullPathNameW(wide, MAX_PATH, full, NULL);
    attributes = length && length < MAX_PATH ? GetFileAttributesW(full) : INVALID_FILE_ATTRIBUTES;
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return mem_fail("Project folder does not exist or cannot be accessed.");
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, full, -1, ansi, MAX_PATH, NULL, &substituted) || substituted)
        return mem_fail("Project path cannot be represented by the Windows ANSI code page used by the existing tools.");
    if (canonical && !WideCharToMultiByte(CP_UTF8, 0, full, -1, canonical, MAX_PATH * 4, NULL, NULL)) return 0;
    return 1;
}
static cJSON *mem_new_session(const char *project, const char *title) {
    cJSON *session = cJSON_CreateObject();
    char id[80];
    mem_id(id, 's');
    cJSON_AddStringToObject(session, "id", id); cJSON_AddStringToObject(session, "project", project);
    cJSON_AddStringToObject(session, "title", title); cJSON_AddStringToObject(session, "transcript", "");
    cJSON_AddBoolToObject(session, "pending", 0); cJSON_AddArrayToObject(session, "messages");
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(memory.root, "sessions"), session);
    return session;
}
static int mem_init(void) {
    WCHAR directory[MAX_PATH], lockfile[MAX_PATH], ini[MAX_PATH];
    char initial[MAX_PATH * 4], *json;
    DWORD n, size, read, attrs;
    HANDLE file;
    cJSON *project, *session;
    char id[80];
    int recovery = 0;
    memset(&memory, 0, sizeof(memory)); memory.lock = INVALID_HANDLE_VALUE;
    n = GetModuleFileNameW(NULL, directory, MAX_PATH);
    if (!n || n >= MAX_PATH) return mem_fail("Executable path exceeds MAX_PATH.");
    while (n && directory[n - 1] != L'\\') --n;
    directory[n] = 0;
    if (n + 24 >= MAX_PATH) return mem_fail("Executable directory is too long for the data library.");
    wcscpy(ini, directory); wcscat(ini, L"vc6ai.ini");
    memory.context_bytes = GetPrivateProfileIntW(L"LLM", L"ContextBytes", 65536, ini);
    if (memory.context_bytes < 16384) memory.context_bytes = 16384;
    if (memory.context_bytes > 262144) memory.context_bytes = 262144;
    if (!WideCharToMultiByte(CP_UTF8, 0, directory, -1, initial, sizeof(initial), NULL, NULL)) return mem_fail("Cannot convert executable path.");
    wcscat(directory, L"data");
    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return mem_fail("Cannot create data directory beside AgentCore.exe.");
    wcscpy(memory.file, directory); wcscat(memory.file, L"\\library.json");
    wcscpy(memory.temporary, directory); wcscat(memory.temporary, L"\\library.tmp");
    wcscpy(lockfile, directory); wcscat(lockfile, L"\\library.lock");
    memory.lock = CreateFileW(lockfile, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (memory.lock == INVALID_HANDLE_VALUE) return mem_fail("Library is already open in another desktop or cannot be locked.");
    attrs = GetFileAttributesW(memory.file);
    if (attrs == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        if (GetFileAttributesW(memory.temporary) != INVALID_FILE_ATTRIBUTES)
            return mem_fail("library.json is missing but library.tmp exists. Preserve and recover the temporary file before continuing.");
        memory.root = cJSON_Parse("{\"version\":1,\"projects\":[],\"sessions\":[],\"active\":\"\"}");
        project = cJSON_CreateObject(); mem_id(id, 'p');
        cJSON_AddStringToObject(project, "id", id); cJSON_AddStringToObject(project, "name", "Default");
        cJSON_AddStringToObject(project, "path", initial); cJSON_AddStringToObject(project, "memory", "");
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(memory.root, "projects"), project);
        session = mem_new_session(id, "New chat");
        mem_set(memory.root, "active", mem_string(session, "id"));
        return mem_save();
    }
    file = CreateFileW(memory.file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return mem_fail("Cannot read existing library. It was not replaced.");
    size = GetFileSize(file, &n);
    if (n || !size || size > LIBRARY_MAX) { CloseHandle(file); return mem_fail("Library is empty or exceeds 32 MiB. It was not replaced."); }
    json = malloc(size + 1);
    if (!json) { CloseHandle(file); return mem_fail("Not enough memory to read the library."); }
    if (!ReadFile(file, json, size, &read, NULL) || read != size) { free(json); CloseHandle(file); return mem_fail("Cannot read complete library."); }
    CloseHandle(file); json[size] = 0;
    if (mem_json_text(json, size)) memory.root = cJSON_ParseWithLengthOpts(json, size + 1, NULL, 1);
    free(json);
    if (!memory.root || !mem_validate()) return mem_fail("Library is corrupt or has an unsupported format. It was not replaced; repair or restore data/library.json.");
    cJSON_ArrayForEach(session, cJSON_GetObjectItemCaseSensitive(memory.root, "sessions")) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(session, "pending"))) {
            if (!mem_finish_turn(session, "Previous run ended before the turn completed.")) return 0;
            recovery = 1;
        }
    }
    return !recovery || mem_save();
}
static void mem_close(void) {
    cJSON_Delete(memory.root);
    if (memory.lock != INVALID_HANDLE_VALUE) CloseHandle(memory.lock);
    memset(&memory, 0, sizeof(memory)); memory.lock = INVALID_HANDLE_VALUE;
}

static char *mem_catalog_json(void) {
    cJSON *catalog = cJSON_CreateObject(), *sessions, *source, *entry;
    char *json;
    cJSON_AddItemToObject(catalog, "projects", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(memory.root, "projects"), 1));
    sessions = cJSON_AddArrayToObject(catalog, "sessions");
    cJSON_ArrayForEach(source, cJSON_GetObjectItemCaseSensitive(memory.root, "sessions")) {
        entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", mem_string(source, "id"));
        cJSON_AddStringToObject(entry, "project", mem_string(source, "project"));
        cJSON_AddStringToObject(entry, "title", mem_string(source, "title"));
        cJSON_AddItemToArray(sessions, entry);
    }
    cJSON_AddStringToObject(catalog, "active", mem_string(memory.root, "active"));
    json = cJSON_PrintUnformatted(catalog); cJSON_Delete(catalog);
    return json;
}
static int mem_catalog_fits(void) {
    char *json = mem_catalog_json(), *wire;
    cJSON *event = cJSON_CreateObject();
    int ok;
    if (!json || !event) { free(json); cJSON_Delete(event); return mem_fail("Not enough memory for project catalog."); }
    cJSON_AddStringToObject(event, "type", "catalog"); cJSON_AddStringToObject(event, "text", json);
    wire = cJSON_PrintUnformatted(event); cJSON_Delete(event); free(json);
    ok = wire && strlen(wire) <= IPC_MAX_FRAME;
    free(wire);
    return ok || mem_fail("Project catalog exceeds the 256 KiB IPC limit. Shorten names or shared notes before adding more.");
}
static void mem_catalog(void) {
    char *json = mem_catalog_json();
    if (json) { agent_emit("catalog", json); free(json); }
    else agent_emit("error", "Cannot allocate project catalog.");
}
static void mem_metadata(void) {
    cJSON *event = cJSON_CreateObject();
    char *json;
    if (!memory.session || !memory.project) { cJSON_Delete(event); return; }
    cJSON_AddStringToObject(event, "id", mem_string(memory.session, "id"));
    cJSON_AddStringToObject(event, "project", mem_string(memory.session, "project"));
    cJSON_AddStringToObject(event, "title", mem_string(memory.session, "title"));
    cJSON_AddStringToObject(event, "path", mem_string(memory.project, "path"));
    cJSON_AddStringToObject(event, "memory", mem_string(memory.project, "memory"));
    cJSON_AddNumberToObject(event, "context_bytes", memory.context_bytes);
    json = cJSON_PrintUnformatted(event); cJSON_Delete(event);
    if (json) { agent_emit("session", json); free(json); }
}
static int mem_open(const char *id, struct config_t *config, struct conversation_t *convo, int display) {
    cJSON *session = mem_find("sessions", id), *project, *message, *copy;
    char path[MAX_PATH];
    if (!session) return mem_fail("Session does not exist.");
    project = mem_find("projects", mem_string(session, "project"));
    if (!project || !mem_path(mem_string(project, "path"), path, NULL)) return 0;
    memory.session = session; memory.project = project; memory.kind[0] = 0;
    lstrcpynA(config->cwd, path, sizeof(config->cwd));
    convo_cleanup(convo); convo_init(convo, config, tools); convo_add_sysprompt(convo, config);
    cJSON_ArrayForEach(message, cJSON_GetObjectItemCaseSensitive(session, "messages")) {
        copy = cJSON_Duplicate(message, 1);
        if (!copy || !cJSON_AddItemToArray(convo->messages, copy)) {
            cJSON_Delete(copy); return mem_fail("Not enough memory to open conversation.");
        }
    }
    convo->content = NULL; convo->pending_tool_msg = NULL;
    if (strcmp(mem_string(memory.root, "active"), id)) {
        if (!mem_set(memory.root, "active", id) || !mem_save()) return 0;
    }
    if (display) {
        mem_metadata(); agent_emit("clear", NULL);
        agent_emit("replay", mem_string(session, "transcript")); mem_catalog();
    }
    return 1;
}
static int mem_create_project(const char *name, const char *path, struct config_t *config, struct conversation_t *convo) {
    cJSON *projects = cJSON_GetObjectItemCaseSensitive(memory.root, "projects"), *project, *session;
    cJSON *sessions = cJSON_GetObjectItemCaseSensitive(memory.root, "sessions");
    char ansi[MAX_PATH], canonical[MAX_PATH * 4], id[80], session_id[80];
    if (!mem_name(name)) return mem_fail("Project name must contain 1 to 160 UTF-8 bytes.");
    if (cJSON_GetArraySize(projects) >= 256 || cJSON_GetArraySize(sessions) >= 2048) return mem_fail("Project or session count limit reached.");
    if (!path || !mem_path(path, ansi, canonical)) return 0;
    project = cJSON_CreateObject(); mem_id(id, 'p');
    cJSON_AddStringToObject(project, "id", id); cJSON_AddStringToObject(project, "name", name);
    cJSON_AddStringToObject(project, "path", canonical); cJSON_AddStringToObject(project, "memory", "");
    cJSON_AddItemToArray(projects, project);
    session = mem_new_session(id, "New chat"); lstrcpynA(session_id, mem_string(session, "id"), sizeof(session_id));
    if (!mem_catalog_fits()) {
        cJSON_DeleteItemFromArray(projects, cJSON_GetArraySize(projects) - 1);
        cJSON_DeleteItemFromArray(sessions, cJSON_GetArraySize(sessions) - 1); return 0;
    }
    return mem_open(session_id, config, convo, 1);
}
static int mem_create_chat(const char *project, const char *title, struct config_t *config, struct conversation_t *convo) {
    cJSON *sessions = cJSON_GetObjectItemCaseSensitive(memory.root, "sessions"), *session, *parent;
    char id[80], path[MAX_PATH];
    if (!project && memory.project) project = mem_string(memory.project, "id");
    parent = mem_find("projects", project);
    if (!parent) return mem_fail("Project does not exist.");
    if (!mem_path(mem_string(parent, "path"), path, NULL)) return 0;
    if (!title || !*title) title = "New chat";
    if (!mem_name(title)) return mem_fail("Session title must contain 1 to 160 UTF-8 bytes.");
    if (cJSON_GetArraySize(sessions) >= 2048) return mem_fail("Session count limit reached.");
    session = mem_new_session(project, title); lstrcpynA(id, mem_string(session, "id"), sizeof(id));
    if (!mem_catalog_fits()) { cJSON_DeleteItemFromArray(sessions, cJSON_GetArraySize(sessions) - 1); return 0; }
    return mem_open(id, config, convo, 1);
}
static int mem_change(const char *id, const char *text, int note) {
    cJSON *object;
    const char *key;
    char *old;
    int ok;
    if (note) {
        object = memory.project; key = "memory";
        if (!object || !text || !mem_utf8(text) || strlen(text) > NOTE_MAX) return mem_fail("Shared project notes are limited to 16 KiB of UTF-8.");
    } else {
        if (!mem_name(text)) return mem_fail("Name must contain 1 to 160 UTF-8 bytes.");
        object = mem_find("projects", id); key = "name";
        if (!object) { object = mem_find("sessions", id); key = "title"; }
        if (!object) return mem_fail("Project or session does not exist.");
    }
    old = strdup(mem_string(object, key));
    if (!old) return mem_fail("Not enough memory to update the library.");
    ok = mem_set(object, key, text) && mem_catalog_fits();
    if (!ok) mem_set(object, key, old);
    free(old);
    if (ok && !note && !strcmp(key, "title")) {
        if (cJSON_GetObjectItemCaseSensitive(object, "renamed"))
            cJSON_ReplaceItemInObjectCaseSensitive(object, "renamed", cJSON_CreateBool(1));
        else cJSON_AddBoolToObject(object, "renamed", 1);
    }
    if (!ok || !mem_save()) return 0;
    mem_metadata(); mem_catalog(); return 1;
}
static int mem_reset(struct conversation_t *convo) {
    if (!memory.session) return mem_fail("No session is open.");
    cJSON_ReplaceItemInObjectCaseSensitive(memory.session, "messages", cJSON_CreateArray());
    mem_set(memory.session, "transcript", "");
    cJSON_ReplaceItemInObjectCaseSensitive(memory.session, "pending", cJSON_CreateBool(0));
    cJSON_DeleteItemFromObjectCaseSensitive(memory.session, "turn");
    convo_clear(convo); memory.kind[0] = 0;
    if (!mem_save()) return 0;
    agent_emit("clear", NULL); agent_emit("status", "Conversation cleared."); mem_metadata(); return 1;
}

/* Clone before truncating tool output. IDs, arguments and reasoning remain exact. */
static int mem_turn_begin(struct conversation_t *convo, const char *text) {
    char title[81];
    cJSON *turn;
    size_t length;
    int automatic = 0;
    if (!mem_append(memory.session, "\r\n\r\nYou:\r\n", text)) return 0;
    if (!strcmp(mem_string(memory.session, "title"), "New chat") &&
        !cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(memory.session, "messages")) &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(memory.session, "renamed"))) {
        length = mem_boundary(text, 60); memcpy(title, text, length); title[length] = 0;
        if (length) { mem_set(memory.session, "title", title); automatic = 1; }
    }
    cJSON_ReplaceItemInObjectCaseSensitive(memory.session, "pending", cJSON_CreateBool(1));
    cJSON_DeleteItemFromObjectCaseSensitive(memory.session, "turn");
    turn = cJSON_AddObjectToObject(memory.session, "turn");
    if (!turn || !cJSON_AddArrayToObject(turn, "messages") || !cJSON_AddArrayToObject(turn, "tools"))
        return mem_fail("Not enough memory to begin a durable turn.");
    memory.turn_start = cJSON_GetArraySize(convo->messages);
    convo_add_text_message(convo, "user", text);
    memory.kind[0] = 0;
    if (!mem_catalog_fits()) {
        if (automatic) mem_set(memory.session, "title", "New chat");
        if (!mem_catalog_fits()) return 0;
    }
    if (!mem_checkpoint(convo)) return 0;
    mem_metadata(); mem_catalog(); return 1;
}
static int mem_turn_end(struct conversation_t *convo, int success) {
    if (memory.failed) return 0;
    return mem_checkpoint(convo) && mem_finish_turn(memory.session,
        success ? NULL : "Task stopped or failed. Recorded tool results were retained.") && mem_save();
}
static cJSON *mem_projection_copy(cJSON *source) {
    cJSON *copy = cJSON_Duplicate(source, 1);
    const char *role = mem_string(copy, "role"), *content = mem_string(copy, "content");
    char excerpt[4300];
    size_t n;
    if (role && !strcmp(role, "tool") && content && strlen(content) > 4096) {
        n = mem_boundary(content, 4096); memcpy(excerpt, content, n); excerpt[n] = 0;
        strcat(excerpt, "\n[Tool output excerpt; full original remains in session history.]");
        if (!mem_set(copy, "content", excerpt)) { cJSON_Delete(copy); return NULL; }
    }
    return copy;
}
static cJSON *mem_system(const char *text) {
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "system"); cJSON_AddStringToObject(message, "content", text);
    return message;
}
static size_t mem_size(cJSON *value) {
    char *json = cJSON_PrintUnformatted(value);
    size_t size = json ? strlen(json) : LIBRARY_MAX + 1;
    free(json); return size;
}
static void mem_excerpt(char *buffer, size_t capacity, const char *label, const char *text) {
    size_t at = strlen(buffer), label_size = strlen(label), n;
    if (!text || at + label_size + 8 >= capacity) return;
    n = mem_boundary(text, 120);
    if (at + label_size + n + 5 >= capacity) return;
    memcpy(buffer + at, label, label_size); at += label_size;
    memcpy(buffer + at, text, n); at += n;
    if (text[n]) { memcpy(buffer + at, "...", 3); at += 3; }
    buffer[at++] = '\n'; buffer[at] = 0;
}
static cJSON *mem_project_context(struct conversation_t *convo) {
    cJSON *copies = cJSON_CreateArray(), *source, *copy, *projection = NULL, *note, *excerpts;
    size_t *sums = NULL, used, base, extra_size, budget = memory.context_bytes;
    int *starts = NULL, count = cJSON_GetArraySize(convo->messages), turns = 0, first, i, begin, excerpt_count = 0;
    const char *role, *notes = memory.project ? mem_string(memory.project, "memory") : "";
    char excerpt[2300], stats[256], *note_text;
    if (!copies || count < 2) goto failed;
    sums = malloc((count + 1) * sizeof(size_t)); starts = malloc(count * sizeof(int));
    if (!sums || !starts) goto failed;
    sums[0] = 0; i = 0;
    cJSON_ArrayForEach(source, convo->messages) {
        copy = mem_projection_copy(source); if (!copy) goto failed;
        cJSON_AddItemToArray(copies, copy);
        sums[i + 1] = sums[i] + mem_size(copy) + 1;
        role = mem_string(copy, "role");
        if (role && !strcmp(role, "user")) starts[turns++] = i;
        ++i;
    }
    if (!turns) goto failed;
    projection = cJSON_CreateArray();
    cJSON_AddItemToArray(projection, cJSON_Duplicate(cJSON_GetArrayItem(copies, 0), 1));
    if (notes && *notes) {
        note_text = malloc(strlen(notes) + 100);
        if (!note_text) goto failed;
        strcpy(note_text, "Explicit shared notes for this project (provided by the user):\n"); strcat(note_text, notes);
        note = mem_system(note_text); free(note_text); cJSON_AddItemToArray(projection, note);
    }
    base = mem_size(projection); first = turns - 1;
    used = base + sums[count] - sums[starts[first]];
    if (used > budget) { mem_fail("Current turn, project notes and system instructions exceed ContextBytes. Shorten the message/notes or start a new chat; no tool chain was split."); goto fail_message; }
    while (first > 0) {
        size_t candidate = base + sums[count] - sums[starts[first - 1]];
        if (candidate > budget || (first > 1 && candidate + 400 > budget)) break;
        --first; used = candidate;
    }
    if (first > 0) {
        strcpy(excerpt, "Historical excerpts only, not complete memory. Earlier full turns are omitted from this request; original history is retained locally.\n");
        begin = first > 6 ? first - 6 : 0;
        for (i = begin; i < first; ++i) {
            int index;
            mem_excerpt(excerpt, sizeof(excerpt), "User excerpt: ", mem_string(cJSON_GetArrayItem(copies, starts[i]), "content"));
            for (index = starts[i + 1] - 1; index > starts[i]; --index) {
                source = cJSON_GetArrayItem(copies, index); role = mem_string(source, "role");
                if (role && !strcmp(role, "assistant") && mem_string(source, "content")) {
                    mem_excerpt(excerpt, sizeof(excerpt), "Assistant excerpt: ", mem_string(source, "content")); break;
                }
            }
            ++excerpt_count;
        }
        excerpts = mem_system(excerpt); extra_size = mem_size(excerpts) + 1;
        while (used + extra_size > budget && strlen(excerpt) > 160) {
            excerpt[mem_boundary(excerpt, strlen(excerpt) / 2)] = 0;
            cJSON_Delete(excerpts); excerpts = mem_system(excerpt); extra_size = mem_size(excerpts) + 1;
        }
        if (used + extra_size <= budget) cJSON_AddItemToArray(projection, excerpts);
        else { cJSON_Delete(excerpts); excerpt_count = 0; }
    }
    for (i = starts[first]; i < count; ++i) {
        copy = cJSON_Duplicate(cJSON_GetArrayItem(copies, i), 1);
        if (!copy || !cJSON_AddItemToArray(projection, copy)) { cJSON_Delete(copy); goto failed; }
    }
    used = mem_size(projection);
    if (used > budget) { mem_fail("Context projection exceeds ContextBytes; no history was discarded."); goto fail_message; }
    sprintf(stats, "%d/%d complete-or-current turns retained; %lu/%d message JSON bytes; %d older turns excerpted. Full history stays on disk.",
            turns - first, turns, (unsigned long)used, memory.context_bytes, excerpt_count);
    agent_emit("context", stats);
    cJSON_Delete(copies); free(sums); free(starts); return projection;
failed:
    mem_fail("Not enough memory to build request context.");
fail_message:
    cJSON_Delete(projection); cJSON_Delete(copies); free(sums); free(starts); return NULL;
}
