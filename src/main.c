#define _WIN32_WINNT 0x0500 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#include <ctype.h>
#include <windows.h>
#include <io.h>

#include <curl/curl.h>
#include "cJSON.h"
#include "ipc.h"
#ifndef JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#endif

#pragma comment(lib, "libcurl")

/* Define DEBUG_REQUESTS / DEBUG_TOOLS explicitly for opt-in sensitive logs. */
static HANDLE agent_pipe = INVALID_HANDLE_VALUE, agent_parent = NULL, agent_cancel = NULL;
static int agent_broken;
static void agent_emit(const char *type, const char *text);
static void agent_send(const char *type, const char *text);
static int mem_utf8(const char *text);
struct conversation_t;
struct config_t;
static char *agent_tool_begin(struct conversation_t *convo, struct config_t *config, cJSON *call);
static int agent_tool_end(struct conversation_t *convo, const char *result);

//////////////////////////////////////////////////////////////////////////
//
// DEBUGGING

#ifdef DEBUG_REQUESTS
static FILE *fdbg_reqs;
#define DBG_REQ(fmt, a) dbg(&fdbg_reqs, "requests.log", fmt, a)
#endif

#ifdef DEBUG_TOOLS
static FILE *fdbg_tools;
#define DBG_TOOL(fmt, a, b) dbg(&fdbg_tools, "tool_calls.log", fmt, a, b)
#endif

#if defined(DEBUG_REQUESTS) || defined(DEBUG_TOOLS)
static void dbg(FILE **fp, const char *filename, const char *fmt, ...) {
    va_list args;

    if (!*fp) *fp = fopen(filename, "a");
    if (!*fp) return;
    
    va_start(args, fmt);
    vfprintf(*fp, fmt, args);
    va_end(args);

    fprintf(*fp, "-------------------------------------------------------------------------------\n");
    fflush(*fp);
}
#endif

//////////////////////////////////////////////////////////////////////////
//
// CONFIGURATION

#define APIKEY_MAX 512
#define APIURL_MAX 2048
#define MODEL_MAX 255

struct config_t {
    char cwd[MAX_PATH];

    // Chat Completions configuration
    char api_key[APIKEY_MAX];
    char model[MODEL_MAX];
    char api_url[APIURL_MAX];
    char extra_body[2048]; /* optional provider parameters; never override core fields */

    // Exa config
    char exa_key[APIKEY_MAX];

    int stream; // Incremental output in CLI and desktop modes
    int context; // model's context window
};

const char *ini_file = "vc6ai.ini";

void config_init(struct config_t *config, const char *ini_file) {
    char ini_path[MAX_PATH];
    
    GetCurrentDirectoryA(MAX_PATH, config->cwd);
    _snprintf(ini_path, MAX_PATH, "%s\\%s", config->cwd, ini_file);

    GetPrivateProfileStringA("LLM", "ApiUrl", "", config->api_url, APIURL_MAX, ini_path);
    GetPrivateProfileStringA("LLM", "ApiKey", "", config->api_key, APIKEY_MAX, ini_path);
    GetPrivateProfileStringA("LLM", "Model", "", config->model, MODEL_MAX, ini_path);
    GetPrivateProfileStringA("LLM", "ExtraBody", "", config->extra_body, sizeof(config->extra_body), ini_path);
    GetPrivateProfileStringA("Exa", "ApiKey", "", config->exa_key, APIKEY_MAX, ini_path);
    if (config->exa_key[0]) SetEnvironmentVariableA("EXA_API_KEY", config->exa_key);
    config->stream = GetPrivateProfileIntA("LLM", "Stream", 1, ini_path);
    config->context = GetPrivateProfileIntA("LLM", "ContextWindow", 0, ini_path);
}

//////////////////////////////////////////////////////////////////////////
//
// TERMINAL

enum term_color {
    // foreground
    C_FG_DARK_CYAN   = FOREGROUND_BLUE | FOREGROUND_GREEN,
    C_FG_DARK_YELLOW = FOREGROUND_RED | FOREGROUND_GREEN,
    C_FG_DARK_GRAY   = FOREGROUND_INTENSITY,

    C_FG_GRAY        = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_GREEN,
    C_FG_BLUE        = FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_FG_TURQUOISE   = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    C_FG_RED         = FOREGROUND_RED | FOREGROUND_INTENSITY,
    C_FG_PURPLE      = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    C_FG_YELLOW      = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    C_FG_WHITE       = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,

    // background
    C_BG_DARK_BLUE   = BACKGROUND_BLUE,
    C_BG_DARK_RED    = BACKGROUND_RED,
    C_BG_DARK_PURPLE = BACKGROUND_RED | BACKGROUND_BLUE
};

static HANDLE term_handle;
static WORD term_orig_attrs;

static volatile LONG term_interrupted_flag = 0;

static BOOL WINAPI term_ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT) {
        term_interrupted_flag = 1;
        return TRUE;
    }
    return FALSE;
}

void term_init(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi = {0};
    term_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo(term_handle, &csbi);
    term_orig_attrs = csbi.wAttributes;

    SetConsoleCtrlHandler(term_ctrl_handler, TRUE);
}

int term_interrupted(void) {
    return term_interrupted_flag != 0 || agent_broken ||
        (agent_cancel && WaitForSingleObject(agent_cancel, 0) == WAIT_OBJECT_0) ||
        (agent_parent && WaitForSingleObject(agent_parent, 0) == WAIT_OBJECT_0);
}

void term_interrupt_reset(void) {
    term_interrupted_flag = 0;
}

void term_color(enum term_color color) {
    SetConsoleTextAttribute(term_handle, color);
};

void term_reset(void) {
    SetConsoleTextAttribute(term_handle, term_orig_attrs);
}

/* Write complete UTF-8 fragments through the XP Unicode console API. */
static void term_write_utf8(const char *text) {
    DWORD mode, written;
    int length;
    WCHAR *wide;
    fflush(stdout);
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) { fputs(text, stdout); return; }
    length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wide = malloc(length * sizeof(WCHAR));
    if (!wide) return;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length);
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), wide, length - 1, &written, NULL);
    free(wide);
}

static char *term_read_utf8(char *buffer, int size) {
    WCHAR wide[4096];
    DWORD mode, count;
    if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) return fgets(buffer, size, stdin);
    if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), wide, 4095, &count, NULL) || !count) return NULL;
    wide[count] = 0;
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, buffer, size, NULL, NULL)) buffer[0] = 0;
    return buffer;
}

//////////////////////////////////////////////////////////////////////////
//
// HTTP

struct http_context_t {
    CURL *curl;
    struct curl_slist *headers;
    char *res;
    size_t reslen;
    int stream, streamed;
};

static size_t http_write_callback(char *data, size_t size, size_t nmemb, void *userdata) {
    struct http_context_t *ctx = (struct http_context_t *)userdata;
    size_t realsize;
    char *ptr;
    if (size && nmemb > (size_t)-1 / size) return 0;
    realsize = size * nmemb;
    if (realsize > 4 * 1024 * 1024UL || ctx->reslen > 4 * 1024 * 1024UL - realsize) return 0;
    ptr = realloc(ctx->res, ctx->reslen + realsize + 1);
    if (!ptr) return 0;

    ctx->res = ptr;
    memcpy(&(ctx->res[ctx->reslen]), data, realsize);
    ctx->reslen += realsize;
    ctx->res[ctx->reslen] = '\0'; // null terminate string as res buffer might not be empty

    return realsize;
}

static int http_progress_callback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return term_interrupted();
}

int http_init(struct http_context_t *ctx, long timeout) {
    CURLcode res;

    res = curl_global_init(CURL_GLOBAL_ALL);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }

    ctx->curl = curl_easy_init();
    if (!ctx->curl) {
        fprintf(stderr, "curl_easy_init failed\n");
        return 2;
    }

    ctx->headers = NULL;
    ctx->res = NULL;
    ctx->reslen = 0;

    curl_easy_setopt(ctx->curl, CURLOPT_CAINFO, "curl-ca-bundle.crt");
    curl_easy_setopt(ctx->curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(ctx->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEFUNCTION, http_write_callback);
    curl_easy_setopt(ctx->curl, CURLOPT_WRITEDATA, ctx);
    curl_easy_setopt(ctx->curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(ctx->curl, CURLOPT_PROGRESSFUNCTION, http_progress_callback);

    return 0;
}

void http_cleanup(struct http_context_t *ctx) {
    if (ctx->curl) curl_easy_cleanup(ctx->curl);    
    curl_global_cleanup();
    if (ctx->headers) curl_slist_free_all(ctx->headers);
    if (ctx->res) free(ctx->res);

    memset(ctx, 0, sizeof(struct http_context_t));
}

//////////////////////////////////////////////////////////////////////////
//
// TOOLS

struct tool_params_t {
    const char *name;
    const char *type;
    const char *desc;
    int required;
};

struct tool_t {
    const char *name;
    const char *desc;
    const struct tool_params_t *params;
};

const struct tool_params_t run_cmd_params[] = {
    {
        "command",
        "string",
        "Command passed to Windows XP cmd.exe /D /C, limited to about 4000 characters. "
        "Use one command per call and join dependent steps with &&; calls execute sequentially. "
        "Use call \"file.bat\" when running a batch file before another command. "
        "Use only Windows XP-compatible tools; do not use PowerShell, Python, Perl, package managers, or newer Windows commands. "
        "When detected in the system environment profile, busybox.exe provides sh, find, grep, sed, awk, cat, head, tail, diff, patch, tee, and unix2dos. "
        "Use forward-slash paths and short POSIX options with BusyBox; GNU long options such as --include are unsupported. Scope searches and cap their output. "
        "For pipes, regexes, or difficult quoting, run busybox.exe sh and pass the script in stdin. "
        "For edits, pass a unified diff to busybox.exe patch -p1 in stdin and verify the result; malformed hunks can exit successfully without applying. "
        "Do not retry a failed patch unchanged or convert CRLF before patching. "
        "patch, tee, and sed -i write LF, so restore existing CRLF files with unix2dos. "
        "Use tee and stdin for whole-file writes. "
        "When detected, use jq.exe for JSON and curl.exe for networking; these are bundled tools, not XP system commands. "
        "If EXA_API_KEY is set, use Exa to search and fetch web pages. "
        "Post JSON on stdin to https://api.exa.ai/search or https://api.exa.ai/contents with curl.exe -s, Content-Type and x-api-key headers, and -d @-. "
        "Use /search to discover pages; nest highlights or text under contents. Use /contents to fetch known URLs; highlights and text are top-level. "
        "Prefer highlights for lookups, or text with maxCharacters for deeper reading. Set maxAgeHours to 0 only when fresh content matters.",
        1
    },
    {
        "stdin",
        "string",
        "Optional verbatim standard input. Use it for shell scripts, patches, JSON, and whole-file writes instead of command-line quoting.",
        0
    },
    {NULL, NULL, NULL, 0}
};
const struct tool_t tools[] = {
    {
        "cmd",
        "Run one non-interactive command with Windows XP cmd.exe /D /C. "
        "Stdout and stderr are captured and [exit N] is appended. "
        "Output over about 12 KB is trimmed; the full output is saved under C:\\Temp\\vc6ai. "
        "Commands are killed after 3 minutes (exit 124) and must not wait for input; exit 130 means Ctrl-C.",
        run_cmd_params
    },
    {NULL, NULL, NULL}
};

/* Drain output while stdin is written: a full pipe must not block Stop. */
struct cmd_input_t { HANDLE pipe; const char *text; DWORD size; };
static DWORD WINAPI cmd_write_input(void *data) {
    struct cmd_input_t *input = data;
    DWORD written, at = 0;
    while (at < input->size && WriteFile(input->pipe, input->text + at, input->size - at, &written, NULL) && written)
        at += written;
    CloseHandle(input->pipe);
    return 0;
}

char *tool_handle_run_cmd(const char *cwd, const char *cmd, const char *input) {
    // rough idea is that we create pipes to read cmd.exe's stdout/err and pipe in stdin.
    // we start cmd.exe tool call suspended, add it to the job, resume and poll it.
    // we terminate the job on timeout or interrupt (Ctrl-C).

#define CMD_TIMEOUT_MS 3*60*1000
#define EXIT_TIMEOUT   124
#define EXIT_INTERRUPT 130

    // hacky macro to check if there's anything on the pipe, read if so.
    // we only read once so a noisy command won't block
#define DRAIN_OUTPUT(outread, out, outsize, outlen) do {                               \
    DWORD drained;                                                                     \
    char *grown;                                                                       \
    if (exitcode != STILL_ACTIVE) break;                                               \
    if (PeekNamedPipe((outread), NULL, 0, NULL, &drained, NULL) && drained) {          \
        if (drained > 4UL * 1024 * 1024 - (outlen) - 32) {                             \
            exitcode = 122; TerminateJobObject(job, exitcode); break;                  \
        }                                                                             \
        if ((outlen) + drained + 32 > (outsize)) {                                     \
            while ((outlen) + drained + 32 > (outsize)) (outsize) *= 2;                \
            grown = realloc((out), (outsize));                                         \
            if (!grown) { exitcode = 8; TerminateJobObject(job, exitcode); break; }     \
            (out) = grown;                                                             \
        }                                                                              \
        if (ReadFile((outread), (out) + (outlen), drained, &drained, NULL) && drained) \
            (outlen) += drained;                                                       \
    }                                                                                  \
} while (0)

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE outread = NULL, outwrite = NULL, inread = NULL, inwrite = NULL, job, writer = NULL;
    struct cmd_input_t input_data;
    STARTUPINFO si = {0};
    PROCESS_INFORMATION pi = {0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    char cmdpath[MAX_PATH];
    char *cmdline, buf[1024], *out;
    size_t outsize = 4096, outlen = 0;
    DWORD exitcode = STILL_ACTIVE, started;
    BOOL ok;

    if (!CreatePipe(&outread, &outwrite, &sa, 0)) return strdup("Cannot create output pipe.");
    SetHandleInformation(outread, HANDLE_FLAG_INHERIT, 0);

    if (input) {
        if (!CreatePipe(&inread, &inwrite, &sa, 4096)) {
            CloseHandle(outread); CloseHandle(outwrite);
            return strdup("Cannot create input pipe.");
        }
        SetHandleInformation(inwrite, HANDLE_FLAG_INHERIT, 0);
    }

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = input ? inread : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outwrite;
    si.hStdError = outwrite;

    cmdline = malloc(strlen(cmd) + 16);
    if (!cmdline) {
        CloseHandle(outread); CloseHandle(outwrite);
        if (input) { CloseHandle(inread); CloseHandle(inwrite); }
        return strdup("Not enough memory for command.");
    }
    sprintf(cmdline, "cmd.exe /D /C %s", cmd);

    started = GetTickCount();

    job = CreateJobObjectA(NULL, NULL);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ok = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    if (ok) {
        DWORD n = GetSystemDirectoryA(cmdpath, MAX_PATH);
        ok = n && n < MAX_PATH - 9;
        if (ok) { strcat(cmdpath, "\\cmd.exe");
            ok = CreateProcessA(cmdpath, cmdline, NULL, NULL, TRUE,
                CREATE_SUSPENDED | (agent_pipe != INVALID_HANDLE_VALUE ? CREATE_NO_WINDOW : 0), NULL, cwd, &si, &pi);
        }
    }
    free(cmdline);
    CloseHandle(outwrite);
    if (input) CloseHandle(inread);

    if (!ok) {
        if (input) CloseHandle(inwrite);
        if (job) CloseHandle(job);
        CloseHandle(outread);

        sprintf(buf, "CreateProcessA failed: %lu\n", GetLastError());
        return strdup(buf);
    }

    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(job); CloseHandle(outread);
        if (input) CloseHandle(inwrite);
        return strdup("Cannot contain command in a job; command was not run.");
    }
    ResumeThread(pi.hThread);

    if (input) {
        input_data.pipe = inwrite; input_data.text = input; input_data.size = (DWORD)strlen(input);
        writer = CreateThread(NULL, 0, cmd_write_input, &input_data, 0, NULL);
        if (!writer) { CloseHandle(inwrite); TerminateJobObject(job, 8); }
    }

    out = malloc(outsize);
    if (!out) {
        TerminateJobObject(job, 8);
        CloseHandle(job); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(outread);
        if (writer) { WaitForSingleObject(writer, INFINITE); CloseHandle(writer); }
        return strdup("Not enough memory for command output.");
    }
    for (;;) {
        DRAIN_OUTPUT(outread, out, outsize, outlen);

        // check if cmd.exe exited
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;

        if (GetTickCount() - started >= CMD_TIMEOUT_MS) {
            exitcode = EXIT_TIMEOUT;
        } else if (term_interrupted()) {
            if (agent_pipe == INVALID_HANDLE_VALUE) term_interrupt_reset();
            exitcode = EXIT_INTERRUPT;
        }
        if (exitcode != STILL_ACTIVE) {
            // interrupt or timeout: terminate cmd.exe and all child processes 
            TerminateJobObject(job, exitcode);
            break;
        }

        Sleep(10);
    }

    DRAIN_OUTPUT(outread, out, outsize, outlen);
    CloseHandle(outread);

    if (exitcode == STILL_ACTIVE) GetExitCodeProcess(pi.hProcess, &exitcode);

    CloseHandle(job);
    if (writer) { WaitForSingleObject(writer, INFINITE); CloseHandle(writer); }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // cap huge output: keep head + tail, save the full output to a file
#define CAP_HEAD 4096
#define CAP_TAIL 8192
#define CAP_LIMIT (CAP_HEAD + CAP_TAIL + 512)
    
    if (outlen > CAP_LIMIT) {
        static int seq = 0;
        char path[MAX_PATH], mark[MAX_PATH + 80];
        FILE *f;
        size_t mlen;

        CreateDirectoryA("C:\\Temp", NULL);
        CreateDirectoryA("C:\\Temp\\vc6ai", NULL);
        sprintf(path, "C:\\Temp\\vc6ai\\call_%lu_%d.txt", GetCurrentProcessId(), ++seq);

        f = fopen(path, "wb");
        if (f) { fwrite(out, 1, outlen, f); fclose(f); }

        sprintf(mark, "\n[... %u of %u bytes omitted, full output: %s ...]\n", outlen - CAP_HEAD - CAP_TAIL, outlen, path);
        mlen = strlen(mark);
        
        memmove(out + CAP_HEAD + mlen, out + outlen - CAP_TAIL, CAP_TAIL);
        memcpy(out + CAP_HEAD, mark, mlen);
        
        outlen = CAP_HEAD + mlen + CAP_TAIL;
    }

    sprintf(out + outlen, "\n[exit %lu]", exitcode);

    return out;
}

char *tool_dispatch(struct config_t *config, const char *name, const char *args_json) {
    const char *cmd, *input = NULL;
    char *out = NULL;
    cJSON *command_json, *stdin_json, *args = cJSON_Parse(args_json);
    if (!name || strcmp(name, "cmd")) { cJSON_Delete(args); return strdup("Unknown tool."); }
    command_json = cJSON_GetObjectItemCaseSensitive(args, "command");
    if (!cJSON_IsObject(args) || !cJSON_IsString(command_json) || !command_json->valuestring[0] ||
        strlen(command_json->valuestring) > 4000) {
        cJSON_Delete(args); return strdup("Invalid cmd arguments: command must be a string of 1 to 4000 bytes.");
    }

    term_color(C_FG_DARK_CYAN);
    if (agent_pipe == INVALID_HANDLE_VALUE) printf("\n[Tool: %s]\n", name);
    else printf("  $ %s (", name);

    if (!strcmp(name, "cmd")) {
        cmd = cJSON_GetObjectItemCaseSensitive(args, "command")->valuestring;

        stdin_json = cJSON_GetObjectItemCaseSensitive(args, "stdin");
        if (cJSON_IsString(stdin_json)) input = stdin_json->valuestring;
        if (term_interrupted()) { cJSON_Delete(args); return strdup("Execution interrupted."); }

#ifdef DEBUG_TOOLS
        DBG_TOOL("Tool call (%s): %s\n", name, cmd);
#endif

        if (agent_pipe == INVALID_HANDLE_VALUE) {
            term_write_utf8(cmd); printf("\n");
            if (input) { printf("[stdin]\n"); term_write_utf8(input); printf("\n"); }
        }
        else if (strlen(cmd) > 62) printf("%.62s...", cmd);
        else fputs(cmd, stdout);
        fflush(stdout);
        out = tool_handle_run_cmd(config->cwd, cmd, input);
    }

    if (agent_pipe != INVALID_HANDLE_VALUE) printf(")\n");
    term_reset();

    cJSON_Delete(args);
    return out;
}

//////////////////////////////////////////////////////////////////////////
//
// CONVERSATION HANDLING

struct conversation_t {
    cJSON *root;
    cJSON *messages;

    const char *content; // last content
    cJSON *pending_tool_msg; // message with pending tool calls

    // session usage stats
    int tokens_in;
    int tokens_out;
    int tokens_cached;
    int tokens_ctx;
    int requests;
    double cost;
};

void convo_init(struct conversation_t *convo, struct config_t *config, const struct tool_t *tooldefs) {
    const struct tool_t *tooldef;
    const struct tool_params_t *paramdef;
    cJSON *messages, *options, *option;
    cJSON *tools, *tool, *func, *params, *props, *prop, *reqs;
    cJSON *root = cJSON_CreateObject();
    int i, j;

    tools = cJSON_CreateArray();

    printf("  available tools: ");
    for (i = 0; tooldefs[i].name; ++i) {
        tooldef = &tooldefs[i];

        tool = cJSON_CreateObject();
        func = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddItemToObject(tool, "function", func);

        params = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", tooldef->name);
        cJSON_AddStringToObject(func, "description", tooldef->desc);
        cJSON_AddItemToObject(func, "parameters", params);

        cJSON_AddStringToObject(params, "type", "object");
        props = cJSON_CreateObject();
        reqs = cJSON_CreateArray();

        for (j = 0; tooldef->params[j].name; ++j) {
            paramdef = &tooldef->params[j];

            prop = cJSON_CreateObject();
            cJSON_AddStringToObject(prop, "type", paramdef->type);
            cJSON_AddStringToObject(prop, "description", paramdef->desc);

            cJSON_AddItemToObject(props, paramdef->name, prop);
            if (paramdef->required) cJSON_AddItemToArray(reqs, cJSON_CreateString(paramdef->name));
        }
        cJSON_AddItemToObject(params, "properties", props);
        cJSON_AddItemToObject(params, "required", reqs);

        cJSON_AddItemToArray(tools, tool);

        fputs(tooldef->name, stdout);
    }
    printf("\n\n");

    cJSON_AddStringToObject(root, "model", config->model);
    cJSON_AddItemToObject(root, "tools", tools);

    messages = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "messages", messages);
    options = cJSON_Parse(config->extra_body);
    if (cJSON_IsObject(options)) cJSON_ArrayForEach(option, options) {
        if (strcmp(option->string, "stream") && !cJSON_GetObjectItemCaseSensitive(root, option->string))
            cJSON_AddItemToObject(root, option->string, cJSON_Duplicate(option, 1));
    }
    cJSON_Delete(options);

    memset(convo, 0, sizeof(struct conversation_t));
    convo->root = root;
    convo->messages = messages;
}

void convo_clear(struct conversation_t *convo) { 
    cJSON *root = convo->root, *messages, *system;

    system = cJSON_DetachItemFromArray(convo->messages, 0);
    cJSON_DeleteItemFromObject(root, "messages");
    
    messages = cJSON_CreateArray();
    cJSON_AddItemToArray(messages, system);
    cJSON_AddItemToObject(root, "messages", messages);

    memset(convo, 0, sizeof(struct conversation_t));
    convo->root = root;
    convo->messages = messages;
}

void convo_add_text_message(struct conversation_t *convo, const char *role, const char *content_str) {
    cJSON *msg, *content;
    
    msg = cJSON_CreateObject();
    content = cJSON_CreateString(content_str);
    
    cJSON_AddStringToObject(msg, "role", role);
    cJSON_AddItemToObject(msg, "content", content);
    
    cJSON_AddItemToArray(convo->messages, msg);

    convo->content = content->valuestring;
}

void convo_add_tool_message(struct conversation_t *convo, const char *id, const char *content_str) {
    cJSON *msg, *content;
    
    msg = cJSON_CreateObject();
    content = cJSON_CreateString(content_str);
    
    cJSON_AddStringToObject(msg, "role", "tool");
    cJSON_AddStringToObject(msg, "tool_call_id", id);
    cJSON_AddItemToObject(msg, "content", content);

    cJSON_AddItemToArray(convo->messages, msg);
    
    convo->content = content->valuestring;
}

void convo_compact(struct conversation_t *convo) {
#define TOOL_KEEP 1024
    cJSON *msg, *role, *content;
    char buf[TOOL_KEEP + 64];
    
    cJSON_ArrayForEach(msg, convo->messages) {
        role = cJSON_GetObjectItemCaseSensitive(msg, "role");
        if (!cJSON_IsString(role)) continue;

        if (!strcmp(role->valuestring, "assistant")) {
            cJSON_DeleteItemFromObject(msg, "reasoning");
            cJSON_DeleteItemFromObject(msg, "reasoning_details");
            cJSON_DeleteItemFromObject(msg, "refusal");
        } else if (!strcmp(role->valuestring, "tool")) {
            content = cJSON_GetObjectItemCaseSensitive(msg, "content");
            if (!cJSON_IsString(content)) continue;
            if (strlen(content->valuestring) <= TOOL_KEEP + 64) continue;

            lstrcpynA(buf, content->valuestring, TOOL_KEEP + 1);
            strcat(buf, "\n[... output pruned from history ...]");

            cJSON_ReplaceItemInObject(msg, "content", cJSON_CreateString(buf));
        }
    }
}

static double optional_number(cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(value) ? value->valuedouble : 0;
}

int convo_add_response(struct conversation_t *convo, const char *json) {
    cJSON *res, *choice, *msg, *err, *usage, *details, *finish;
    cJSON *role, *content, *tc, *tcs, *func, *prior;
    const char *error = "Malformed model response.";
    int has_tools = 0, ti, to;
    res = json ? cJSON_ParseWithOpts(json, NULL, 1) : NULL;
    err = cJSON_GetObjectItemCaseSensitive(res, "error");
    if (err) {
        content = cJSON_GetObjectItemCaseSensitive(err, "message");
        if (cJSON_IsString(content)) error = content->valuestring;
        goto invalid;
    }
    choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(res, "choices"), 0);
    finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
    msg = cJSON_GetObjectItemCaseSensitive(choice, "message");
    role = cJSON_GetObjectItemCaseSensitive(msg, "role");
    content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    if (!cJSON_IsString(finish) || !finish->valuestring[0] || !cJSON_IsObject(msg) ||
        !cJSON_IsString(role) || strcmp(role->valuestring, "assistant")) goto invalid;
    has_tools = !strcmp(finish->valuestring, "tool_calls");
    if (has_tools) {
        tcs = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
        if (!cJSON_IsArray(tcs) || !cJSON_GetArraySize(tcs) || cJSON_GetArraySize(tcs) > 16) goto invalid;
        cJSON_ArrayForEach(tc, tcs) {
            func = cJSON_GetObjectItemCaseSensitive(tc, "function");
            role = cJSON_GetObjectItemCaseSensitive(tc, "id");
            content = cJSON_GetObjectItemCaseSensitive(func, "name");
            details = cJSON_GetObjectItemCaseSensitive(func, "arguments");
            if (!cJSON_IsString(role) || !role->valuestring[0] || strlen(role->valuestring) > 256 || !cJSON_IsString(content) ||
                !content->valuestring[0] || !cJSON_IsString(details)) goto invalid;
            for (prior = tcs->child; prior != tc; prior = prior->next)
                if (!strcmp(cJSON_GetObjectItemCaseSensitive(prior, "id")->valuestring, role->valuestring)) goto invalid;
        }
        msg = cJSON_Duplicate(msg, cJSON_True);
        if (!msg) goto invalid;
        cJSON_DeleteItemFromObject(msg, "refusal");
        cJSON_DeleteItemFromObject(msg, "annotations");
        tcs = cJSON_GetObjectItemCaseSensitive(msg, "tool_calls");
        cJSON_ArrayForEach(tc, tcs) cJSON_DeleteItemFromObject(tc, "index");
        cJSON_AddItemToArray(convo->messages, msg);
        convo->pending_tool_msg = msg;
    } else {
        if (!cJSON_IsString(content)) goto invalid;
        msg = cJSON_Duplicate(msg, cJSON_True);
        if (!msg) goto invalid;
        cJSON_DeleteItemFromObject(msg, "refusal");
        cJSON_DeleteItemFromObject(msg, "annotations");
        cJSON_AddItemToArray(convo->messages, msg);
        convo->content = cJSON_GetObjectItemCaseSensitive(msg, "content")->valuestring;
    }
    usage = cJSON_GetObjectItemCaseSensitive(res, "usage");
    ti = (int)optional_number(usage, "prompt_tokens");
    to = (int)optional_number(usage, "completion_tokens");
    convo->tokens_in += ti; convo->tokens_out += to; convo->tokens_ctx = ti + to;
    convo->cost += optional_number(usage, "cost");
    details = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens_details");
    convo->tokens_cached += (int)optional_number(details, "cached_tokens");
    ++convo->requests;
#ifdef DEBUG_REQUESTS
    DBG_REQ("Response: %s\n", json);
#endif
    cJSON_Delete(res);
    return has_tools;
invalid:
    term_color(C_FG_RED);
    fprintf(stderr, "  ! api: %s\n\n", error);
    agent_emit("error", error);
    term_reset();
    cJSON_Delete(res);
    return -1;
}
void convo_handle_tool_calls(struct conversation_t *convo, struct config_t *config) {
    cJSON *tcs, *tc, *func;
    const char *id, *name, *args_json;
    char *content;
    WCHAR *wide;
    char *converted;
    int count, bytes;

    if (!convo->pending_tool_msg) return;

    tcs = cJSON_GetObjectItemCaseSensitive(convo->pending_tool_msg, "tool_calls");
    cJSON_ArrayForEach(tc, tcs) {
        func = cJSON_GetObjectItemCaseSensitive(tc, "function");

        id = cJSON_GetObjectItemCaseSensitive(tc, "id")->valuestring;
        name = cJSON_GetObjectItemCaseSensitive(func, "name")->valuestring;
        args_json = cJSON_GetObjectItemCaseSensitive(func, "arguments")->valuestring;
 
        content = agent_tool_begin(convo, config, tc);
        if (!content && !term_interrupted()) content = tool_dispatch(config, name, args_json);
        if (!content) content = strdup("Not executed: task interrupted before command launch.");
        /* cmd utilities may return OEM bytes. Keep desktop history valid UTF-8. */
        if (agent_pipe != INVALID_HANDLE_VALUE && content && !mem_utf8(content)) {
            count = MultiByteToWideChar(CP_OEMCP, 0, content, -1, NULL, 0);
            wide = count ? malloc(count * sizeof(WCHAR)) : NULL;
            converted = NULL;
            if (wide && MultiByteToWideChar(CP_OEMCP, 0, content, -1, wide, count)) {
                bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                converted = bytes ? malloc(bytes) : NULL;
                if (converted && !WideCharToMultiByte(CP_UTF8, 0, wide, -1, converted, bytes, NULL, NULL)) {
                    free(converted); converted = NULL;
                }
            }
            free(wide); free(content); content = converted;
        }
        if (!content) content = strdup("Tool failed: out of memory.");
        if (!content) { agent_emit("error", "Tool result allocation failed."); agent_broken = 1; break; }
        if (agent_pipe == INVALID_HANDLE_VALUE && content) {
            printf("[Tool result: %s]\n", id); term_write_utf8(content); printf("\n"); fflush(stdout);
        }
        convo_add_tool_message(convo, id, content);
        if (!agent_tool_end(convo, content)) { free(content); break; }
        agent_send("tool_result", content); /* Already archived by agent_tool_end. */
        free(content);
        if (term_interrupted()) break;
    }

    convo->pending_tool_msg = NULL;
}

void convo_cleanup(struct conversation_t *convo) {
    if (convo->root) cJSON_Delete(convo->root);
    convo->root = NULL;
    convo->messages = NULL;
    convo->content = NULL;
    convo->pending_tool_msg = NULL;
}

//////////////////////////////////////////////////////////////////////////
//
// CHAT COMPLETIONS HTTP

void llm_init(struct http_context_t *ctx, const struct config_t *config) {
    char buf[APIKEY_MAX + 32];
    
    if (config->api_key[0]) {
        _snprintf(buf, sizeof(buf), "Authorization: Bearer %s", config->api_key);
        ctx->headers = curl_slist_append(ctx->headers, buf);
    }
    ctx->headers = curl_slist_append(ctx->headers, "Content-Type: application/json");
    curl_easy_setopt(ctx->curl, CURLOPT_URL, config->api_url);
    curl_easy_setopt(ctx->curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(ctx->curl, CURLOPT_FOLLOWLOCATION, 0L);

    ctx->stream = config->stream;
    curl_easy_setopt(ctx->curl, CURLOPT_HTTPHEADER, ctx->headers);
}

static int llm_configured(const struct config_t *config) {
    cJSON *options;
    int valid;
    if (config->extra_body[0]) {
        options = cJSON_ParseWithOpts(config->extra_body, NULL, 1);
        valid = cJSON_IsObject(options); cJSON_Delete(options);
        if (!valid) return 0;
    }
    return config->model[0] &&
        (!strncmp(config->api_url, "https://", 8) || !strncmp(config->api_url, "http://", 7));
}

static const char *llm_error_hint(CURLcode result, long status) {
    if (status == 401) return "Authentication failed. Check ApiKey in Settings (vc6ai.ini).";
    if (status == 403) return "Access denied. Check API key and model permissions in Settings.";
    if (status == 429) return "Rate limit or quota reached. Check provider limits; wait before sending Continue.";
    if (status == 404 || status == 405 || (status >= 300 && status < 400))
        return "Wrong API endpoint. Set ApiUrl to the final Chat Completions URL in Settings.";
    if (status >= 500) return "Provider unavailable. Try Continue later; tools are not automatically replayed.";
    if (status >= 400) return "Request rejected. Check Model, ExtraBody and context size in Settings.";
    if (result == CURLE_COULDNT_RESOLVE_HOST || result == CURLE_COULDNT_RESOLVE_PROXY)
        return "DNS lookup failed. Check the API hostname, proxy and network connection.";
    if (result == CURLE_COULDNT_CONNECT) return "Connection failed. Check ApiUrl, network/proxy and whether the server is running.";
    if (result == CURLE_OPERATION_TIMEDOUT) return "Request timed out. Check the connection; send Continue when ready.";
    if (result == CURLE_PEER_FAILED_VERIFICATION || result == CURLE_SSL_CONNECT_ERROR)
        return "TLS/certificate failure. Check the system clock, CA bundle and endpoint; keep certificate verification enabled.";
    if (result == CURLE_PARTIAL_FILE || result == CURLE_RECV_ERROR || result == CURLE_GOT_NOTHING)
        return "Response interrupted. Check the connection; send Continue when ready.";
    if (result == CURLE_WRITE_ERROR) return "Invalid or oversized response. Check the API endpoint and model; incomplete tool calls were not executed.";
    return "Check the API settings and connection before trying again.";
}
static CURLcode llm_finish(struct http_context_t *http, CURLcode result) {
    long status = 0;
    char error[512];
    if (result == CURLE_ABORTED_BY_CALLBACK && term_interrupted()) return result;
    curl_easy_getinfo(http->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status >= 300) result = CURLE_HTTP_RETURNED_ERROR;
    if (result == CURLE_OK) return result;
    if (status >= 300) sprintf(error, "HTTP %ld: %s", status, llm_error_hint(result, status));
    else _snprintf(error, sizeof(error), "%s: %s", curl_easy_strerror(result), llm_error_hint(result, 0));
    error[sizeof(error) - 1] = 0;
    fprintf(stderr, "%s\n", error); agent_emit("error", error);
    return result;
}

#include "stream.inc"

CURLcode llm_request(struct http_context_t *ctx, struct conversation_t *convo) {
    CURLcode res;
    char *payload;
    ctx->streamed = 0;
    if (ctx->stream) return stream_request(ctx, convo);
    payload = cJSON_PrintUnformatted(convo->root);
    if (!payload) return CURLE_OUT_OF_MEMORY;

#ifdef DEBUG_REQUESTS
    {
        char *json = cJSON_Print(convo->root);
        DBG_REQ("Request: %s\n", json);
        free(json);
    }
#endif

    ctx->reslen = 0;
    if (ctx->res) ctx->res[0] = '\0';

    curl_easy_setopt(ctx->curl, CURLOPT_POSTFIELDS, payload);
    res = curl_easy_perform(ctx->curl);
    free(payload);

    return llm_finish(ctx, res);
}

//////////////////////////////////////////////////////////////////////////
//
// STRING UTILS

char *ltrim(char *str) {
    while (isspace(*str)) ++str;
    return str;
}

char *rtrim(char *str) {
    char *last = str + strlen(str);
    while (last > str && isspace(*(last-1))) --last;
    *last = '\0';
    return str;
}

char *trim(char *str) {
    return ltrim(rtrim(str));
}

//////////////////////////////////////////////////////////////////////////
//
// MAIN AGENT LOOP

#define SYSPROMPT_MAX 32768

const char *sysprompt =
    "You are XP-Harness-Desktop, an AI agent that gets things done on this machine. "
    "Reply in the user's language. "
    "Be concise and practical, using short paragraphs and lists only when useful. "
    "Compatibility target: Windows XP SP3 x86, even when the actual host is newer. "
    "Use only XP-compatible commands and methods through cmd.exe /D /C; calls run sequentially. "
    "Do not assume PowerShell, Python, Perl, winget, WSL, package managers, or modern Windows commands exist. "
    "curl, jq and sh are not XP built-ins. Use only detected bundled tools, using their quoted absolute paths below. "
    "If a required tool is missing, explain the limitation instead of silently installing software. "
    "Desktop commands require user approval; a rejection is a tool result, not permission to try a bypass. "
    "Do not re-run commands whose results are already recorded. Interrupted commands can have partial effects. "
    "An unknown execution outcome requires user verification before any retry; never automatically repeat it.";

static int prompt_append(char *buf, int pos, const char *fmt, ...) {
    int written, available = SYSPROMPT_MAX - pos;
    va_list args;
    if (available <= 1) return pos;
    va_start(args, fmt);
    written = _vsnprintf(buf + pos, available, fmt, args);
    va_end(args);
    if (written < 0 || written >= available) written = available - 1;
    buf[pos + written] = 0;
    return pos + written;
}

static const char *path_utf8(const char *path, char *utf8) {
    WCHAR wide[MAX_PATH];
    utf8[0] = 0;
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH))
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, MAX_PATH * 3, NULL, NULL);
    return utf8;
}

static int prompt_environment(char *buf, int n) {
    OSVERSIONINFOA version;
    char directory[MAX_PATH], path[MAX_PATH], utf8[MAX_PATH * 3], *slash;
    const char *names[] = {"busybox.exe", "jq.exe", "curl.exe"};
    DWORD length, attributes;
    int i;
    memset(&version, 0, sizeof(version)); version.dwOSVersionInfoSize = sizeof(version);
    if (GetVersionExA(&version)) n = prompt_append(buf, n, "\nReported host Windows version: %lu.%lu build %lu (does not relax the XP compatibility target).",
        version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    n = prompt_append(buf, n, "\nCommand/input encoding: Windows ANSI code page %u; output fallback: OEM code page %u. Prefer ASCII commands.\nDetected bundled tools (file presence only; versions not verified):", GetACP(), GetOEMCP());
    length = GetModuleFileNameA(NULL, directory, MAX_PATH);
    slash = length && length < MAX_PATH ? strrchr(directory, '\\') : NULL;
    if (slash) *slash = 0;
    for (i = 0; i < 3; ++i) {
        attributes = INVALID_FILE_ATTRIBUTES;
        if (slash && strlen(directory) + strlen(names[i]) + 2 < MAX_PATH) {
            sprintf(path, "%s\\%s", directory, names[i]); attributes = GetFileAttributesA(path);
        }
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            n = prompt_append(buf, n, "\n%s: \"%s\"", names[i], path_utf8(path, utf8));
        else n = prompt_append(buf, n, "\n%s: not detected beside AgentCore.exe; do not assume it is available.", names[i]);
    }
    length = GetSystemDirectoryA(path, MAX_PATH);
    if (length && length < MAX_PATH - 9) { strcat(path, "\\cmd.exe"); n = prompt_append(buf, n, "\nCommand interpreter: \"%s\" /D /C\n", path_utf8(path, utf8)); }
    return n;
}

void convo_add_sysprompt(struct conversation_t *convo, struct config_t *config) {
    char buf[SYSPROMPT_MAX];
    char utf8[MAX_PATH * 3];
    char path[MAX_PATH + 16];
    struct _finddata_t fd;
    long h;
    int n;
    FILE *f;

    // add base sysprompt + cwd
    n = prompt_environment(buf, prompt_append(buf, 0, "%s", sysprompt));
    n = prompt_append(buf, n, "\nCurrent directory: %s\nCurrent directory contents:", path_utf8(config->cwd, utf8));
    
    // add cwd content
    _snprintf(path, sizeof(path), "%s\\*", config->cwd);
    path[sizeof(path) - 1] = 0;
    h = _findfirst(path, &fd);
    if (h != -1) { do {
        if (fd.name[0] == '.') continue;
        n = prompt_append(buf, n, " %s%s", path_utf8(fd.name, utf8), (fd.attrib & _A_SUBDIR) ? "/" : "");
    } while (_findnext(h, &fd) == 0);
    _findclose(h); }

    // add AGENTS.md contents if exists
    _snprintf(path, sizeof(path), "%s\\AGENTS.md", config->cwd);
    path[sizeof(path) - 1] = 0;
    f = fopen(path, "rb");
    if (f) {
        n = prompt_append(buf, n, "\n\nInstructions from AGENTS.md:\n---\n");
        if (n < SYSPROMPT_MAX - 8) n += fread(buf + n, 1, SYSPROMPT_MAX - n - 8, f);
        n = prompt_append(buf, n, "\n---");
        fclose(f);
    }
    buf[n] = '\0';
    
    convo_add_text_message(convo, "system", buf);
}

#include "agent_mode.inc"

int main(int argc, char **argv) {
    char buf[16384], *prompt, *out;
    int before = 0, round = 0;
    struct config_t config = {0};
    struct http_context_t http = {0};
    struct conversation_t convo = {0};
    
    if (argc > 1 && !strcmp(argv[1], "--pipe")) return agent_main(argc, argv);
    term_init();
    config_init(&config, ini_file);
    if (!llm_configured(&config)) {
        fprintf(stderr, "Check [LLM] ApiUrl, Model and optional ExtraBody JSON in vc6ai.ini.\n");
        return 1;
    }

    // fancy title with background grad
    printf("\n ");
    term_color(C_FG_WHITE | C_BG_DARK_BLUE);   printf(" XP-Harness-");
    term_color(C_FG_WHITE | C_BG_DARK_PURPLE); printf("Desktop");
    term_color(C_FG_WHITE | C_BG_DARK_RED);    printf(" ");
    term_reset();
    printf("\n\n");

    printf("  current model: %s\n\n", config.model);

    term_color(C_FG_DARK_GRAY);
    printf("  available commands: /model, /stats, /new, /exit\n");
    
    convo_init(&convo, &config, tools);
    convo_add_sysprompt(&convo, &config);

    if (http_init(&http, 600L)) return 1; // allow longer thinking sessions
    llm_init(&http, &config);

    for (;;) {
prompt:
        term_reset();
        printf("> ");
        term_color(C_FG_TURQUOISE);
        fflush(stdout);
        if (!term_read_utf8(buf, sizeof(buf))) {
            // handle Ctrl-C abort
            if (term_interrupted()) {
                term_interrupt_reset();
                clearerr(stdin);
                putchar('\n');
                goto prompt;
            }
            break;
        }
        prompt = trim(buf);
        if (strlen(prompt) == 0) goto prompt;

        putchar('\n');

        if (prompt[0] == '/') {
            // handle slash commands
            if (!strncmp(&prompt[1], "exit", 4)) {
                goto cleanup;
            } else if (!strncmp(&prompt[1], "new", 3)) {
                term_reset();
                printf("  \257 conversation cleared\n\n");
                convo_clear(&convo);
                goto prompt;
            } else if (!strncmp(&prompt[1], "model", 5)) {
                char *arg = trim(&prompt[6]);
                if (*arg) {
                    // switch model
                    lstrcpynA(config.model, arg, MODEL_MAX);
                    cJSON_ReplaceItemInObject(convo.root, "model", cJSON_CreateString(config.model));
                    config.context = 0;
                }
                term_reset();
                printf("  \257 current model: %s\n", config.model);
                if (config.context > 0) printf("    context window: %d\n", config.context);
                goto prompt;
            } else if (!strncmp(&prompt[1], "stats", 5)) {
                term_reset();
                printf("  \257 session stats (%d requests)\n", convo.requests);
                printf("    tokens in:  %d\n", convo.tokens_in);
                printf("    tokens out: %d\n", convo.tokens_out);
                printf("    cached in:  %d\n", convo.tokens_cached);
                if (config.context > 0) {
                    int pct = (int)(100.0 * convo.tokens_ctx / config.context + 0.5);
                    printf("    context:    %d / %d (%d%% used)\n",
                           convo.tokens_ctx, config.context, pct);
                } else {
                    printf("    context:    %d\n", convo.tokens_ctx);
                }
                printf("    cost:       $%.4f\n\n", convo.cost);
                goto prompt;
            }

            term_color(C_FG_RED);
            fprintf(stderr, "  \257 unrecognised slash command: %s\n\n", prompt);
            goto prompt;
        } else if (prompt[0] == '!') {
            // execute cmd directly
            out = tool_handle_run_cmd(config.cwd, &prompt[1], NULL);
            if (strlen(out)) {
                term_reset();
                printf("%s\n\n", out);
            }
            free(out);
            goto prompt;
        }

        // compact earlier turns to shring payload and save tokens:
        // - drop reasoning traces
        // - truncate old tool outputs
        // TODO: compact convo if at 80% of context
        convo_compact(&convo);

        // store user message
        before = cJSON_GetArraySize(convo.messages); round = 0;
        convo_add_text_message(&convo, "user", prompt);

        term_color(C_FG_DARK_CYAN);
        printf("  ~ thinking ...\n");
        term_reset();

request:
        if (++round > 24) {
            fprintf(stderr, "Stopped after 24 model requests in this turn.\n");
            goto turn_failed;
        }
        printf("[Request %d]\n", round); fflush(stdout);
        // send user message to LLM
        if (llm_request(&http, &convo)) {
            if (term_interrupted()) {
                term_interrupt_reset();
                printf("  ~ interrupted\n\n");
            }
            goto turn_failed;
        }

        // parse LLM (assistant) response
        //   -1: some API error
        //    0: ok
        //    1: ok + has tool calls 
        switch (convo_add_response(&convo, http.res)) {
            case -1: goto turn_failed;
            case 1:
                convo_handle_tool_calls(&convo, &config);
                // send tool call result to LLM
                goto request;
        }

        // print LLM response
        putchar('\n');
        if (!http.streamed) term_write_utf8(convo.content);
        printf("\n\n");
        continue;
turn_failed:
        while (cJSON_GetArraySize(convo.messages) > before)
            cJSON_DeleteItemFromArray(convo.messages, before);
        convo.pending_tool_msg = NULL; convo.content = NULL;
    }

cleanup:
    convo_cleanup(&convo);
    http_cleanup(&http);
    term_reset();

    return EXIT_SUCCESS;
}
