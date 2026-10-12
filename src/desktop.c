/* Native XP desktop: Win32 controls, one IPC worker, storage owned by AgentCore. */
#define _WIN32_WINNT 0x0501
#define _WIN32_IE 0x0500
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <process.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "cJSON.h"
#include "ipc.h"

#define ID_SEND 101
#define ID_NEW 102
#define ID_STOP 103
#define ID_SETTINGS 104
#define ID_PROJECT 105
#define ID_RENAME 106
#define ID_MEMORY 107
#define ID_TREE 108
#define ID_EXPORT 109
#define ID_DELETE 118
#define WM_EVENT (WM_APP + 1)
#define EV_PROTOCOL 1
#define EV_FINISH 2
#define EV_CANCELLED 3
#define EV_FAILED 4
#define INPUT_LIMIT 16384
#define NAME_BYTES 160
#define MEMORY_BYTES 16384

typedef struct { LONG serial; int kind; char text[1]; } UI_EVENT;
typedef struct {
    const WCHAR *label, *initial;
    HWND edit;
    WCHAR *result;
    int multiline, limit, done, approval, decision;
} PROMPT;
static HWND window, transcript, input, send_button, new_button, stop_button, tree;
static HWND settings_button, project_button, rename_button, memory_button;
static HWND status_label, input_label, title_label, path_label, context_label, tree_label;
static HANDLE quit_event, cancel_event, work_event, worker;
static HANDLE core_cancel, force_event, permission_event;
static HWND permission_window;
static char *permission_reply;
static HANDLE core_process, pipe_handle = INVALID_HANDLE_VALUE;
static WCHAR directory[MAX_PATH], last_character;
static char *pending_request, *active_id, *selected_id;
static char section[24];
static cJSON *catalog, *session;
static LONG serial, pending_serial;
static int busy, stopping, closing, rebuilding, bootstrap, request_failed;
static int reload_settings, pending_reload, request_is_chat, request_is_export;
static void show_permission(const char *text);

static const char *string_value(const cJSON *object, const char *key) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static void replace_string(char **target, const char *value) {
    char *copy = (char *)malloc(strlen(value) + 1);
    if (copy) { strcpy(copy, value); free(*target); *target = copy; }
}

static WCHAR *to_wide(const char *text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *wide = count ? (WCHAR *)malloc(count * sizeof(WCHAR)) : NULL;
    if (wide) MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, count);
    return wide;
}

static char *to_utf8(const WCHAR *wide) {
    int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = count ? (char *)malloc(count) : NULL;
    if (text) WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, count, NULL, NULL);
    return text;
}

static void set_text(HWND control, const char *text) {
    WCHAR *wide = to_wide(text);
    if (wide) { SetWindowTextW(control, wide); free(wide); }
}

static void post_event(int kind, LONG id, const char *text) {
    UI_EVENT *event;
    if (WaitForSingleObject(quit_event, 0) == WAIT_OBJECT_0) return;
    event = (UI_EVENT *)malloc(sizeof(UI_EVENT) + strlen(text));
    if (!event) return;
    event->serial = id; event->kind = kind; strcpy(event->text, text);
    if (!PostMessageW(window, WM_EVENT, 0, (LPARAM)event)) free(event);
}

static void stop_core(void) {
    if (pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_handle); pipe_handle = INVALID_HANDLE_VALUE;
    }
    if (core_process) {
        /* Never terminate by executable name: this handle identifies our child. */
        if (WaitForSingleObject(core_process, 100) == WAIT_TIMEOUT)
            TerminateProcess(core_process, 1);
        WaitForSingleObject(core_process, 1000);
        CloseHandle(core_process); core_process = NULL;
    }
}

static int start_core(void) {
    WCHAR executable[MAX_PATH], command[MAX_PATH + 160], name[100];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HCRYPTPROV crypto;
    DWORD random[2], started, error;
    if (core_process && WaitForSingleObject(core_process, 0) == WAIT_TIMEOUT)
        return pipe_handle != INVALID_HANDLE_VALUE;
    stop_core();
    if (!CryptAcquireContextW(&crypto, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return 0;
    error = CryptGenRandom(crypto, sizeof(random), (BYTE *)random); CryptReleaseContext(crypto, 0);
    if (!error) return 0;
    wsprintfW(name, L"\\\\.\\pipe\\vc6agent-%lu-%08lx%08lx", GetCurrentProcessId(), random[0], random[1]);
    wsprintfW(executable, L"%s\\AgentCore.exe", directory);
    ResetEvent(core_cancel); ResetEvent(force_event);
    wsprintfW(command, L"\"%s\" --pipe %s --parent %lu --cancel %lu", executable, name, GetCurrentProcessId(), (DWORD)(ULONG_PTR)core_cancel);
    ZeroMemory(&startup, sizeof(startup)); startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, directory, &startup, &process)) return 0;
    core_process = process.hProcess; CloseHandle(process.hThread); started = GetTickCount();
    while (GetTickCount() - started < 10000) {
        if (WaitForSingleObject(cancel_event, 30) == WAIT_OBJECT_0 ||
            WaitForSingleObject(quit_event, 0) == WAIT_OBJECT_0 ||
            WaitForSingleObject(core_process, 0) != WAIT_TIMEOUT) break;
        pipe_handle = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (pipe_handle != INVALID_HANDLE_VALUE) return 1;
        error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) break;
    }
    stop_core(); return 0;
}

static unsigned __stdcall worker_main(void *unused) {
    HANDLE events[3], permission_waits[3];
    DWORD wait;
    LONG id;
    char *request, *reply;
    cJSON *message;
    const char *type;
    int ok, finished;
    (void)unused;
    events[0] = quit_event; events[1] = cancel_event; events[2] = work_event;
    permission_waits[0] = quit_event; permission_waits[1] = cancel_event; permission_waits[2] = permission_event;
    for (;;) {
        wait = WaitForMultipleObjects(3, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        if (wait == WAIT_OBJECT_0 + 1) {
            stop_core(); free(pending_request); pending_request = NULL;
            ResetEvent(work_event); ResetEvent(cancel_event);
            post_event(EV_CANCELLED, InterlockedCompareExchange(&serial, 0, 0), ""); continue;
        }
        request = pending_request; pending_request = NULL; id = pending_serial;
        if (!request) continue;
        if (pending_reload) stop_core();
        ok = start_core() && ipc_write(pipe_handle, request, force_event, 10000);
        free(request); finished = 0;
        while (ok && !finished) {
            reply = NULL;
            ok = ipc_read(pipe_handle, &reply, force_event, 600000);
            if (!ok) { free(reply); break; }
            message = cJSON_Parse(reply); type = string_value(message, "type");
            if (!*type) ok = 0;
            else if (!strcmp(type, "done")) finished = 1;
            else if (!strcmp(type, "permission")) {
                free(permission_reply); permission_reply = NULL; ResetEvent(permission_event);
                post_event(EV_PROTOCOL, id, reply);
                wait = WaitForMultipleObjects(3, permission_waits, FALSE, 600000);
                if (wait == WAIT_OBJECT_0 + 2 && permission_reply)
                    ok = ipc_write(pipe_handle, permission_reply, force_event, 10000);
                else if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1) ok = 0;
                free(permission_reply); permission_reply = NULL;
            } else post_event(EV_PROTOCOL, id, reply);
            cJSON_Delete(message); free(reply);
        }
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            stop_core(); ResetEvent(cancel_event);
            post_event(EV_CANCELLED, InterlockedCompareExchange(&serial, 0, 0), "");
        } else {
            if (!ok) { stop_core(); post_event(EV_FAILED, id,
                "Connection lost. Saved sessions are retained. Check AgentCore.exe and vc6ai.ini, then try again."); }
            post_event(EV_FINISH, id, "");
        }
    }
    stop_core(); free(pending_request); pending_request = NULL; return 0;
}

/* Preserve delta boundaries; only adapt LF to the native EDIT control's CRLF. */
static void append_text(const WCHAR *text) {
    WCHAR *normalized;
    int i, length = lstrlenW(text), used = 0;
    normalized = (WCHAR *)malloc((length * 2 + 1) * sizeof(WCHAR));
    if (!normalized) return;
    for (i = 0; i < length; ++i) {
        if (text[i] == L'\n' && last_character != L'\r') normalized[used++] = L'\r';
        normalized[used++] = text[i]; last_character = text[i];
    }
    normalized[used] = 0;
    if (GetWindowTextLengthW(transcript) + used > 1000000)
        SetWindowTextW(transcript, L"[Earlier display text omitted; saved history is retained.]\r\n");
    SendMessageW(transcript, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(transcript, EM_REPLACESEL, FALSE, (LPARAM)normalized);
    SendMessageW(transcript, EM_SCROLLCARET, 0, 0); free(normalized);
}

static void append_utf8(const char *text) {
    WCHAR *wide = to_wide(text);
    if (wide) { append_text(wide); free(wide); }
}

static void begin_section(const char *name) {
    if (!strcmp(section, name)) return;
    append_text(L"\r\n\r\n["); append_utf8(name); append_text(L"]\r\n");
    lstrcpynA(section, name, sizeof(section));
}

static cJSON *find_item(const char *collection, const char *id) {
    cJSON *item, *array = cJSON_GetObjectItemCaseSensitive(catalog, collection);
    for (item = array ? array->child : NULL; item; item = item->next)
        if (!strcmp(string_value(item, "id"), id ? id : "")) return item;
    return NULL;
}

static cJSON *selection(void) {
    TVITEMW item;
    ZeroMemory(&item, sizeof(item)); item.mask = TVIF_PARAM; item.hItem = TreeView_GetSelection(tree);
    return item.hItem && SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&item) ? (cJSON *)item.lParam : NULL;
}

static const char *selected_project(void) {
    cJSON *item = selection();
    const char *project = string_value(item, "project");
    return *project ? project : string_value(item, "id");
}

static void enable_controls(void) {
    int ready = !busy && !closing, loaded = *string_value(session, "id") != 0;
    EnableWindow(send_button, ready && loaded); EnableWindow(new_button, ready && *selected_project());
    EnableWindow(project_button, ready); EnableWindow(rename_button, ready && selection() != NULL);
    EnableWindow(memory_button, ready && loaded && !strcmp(selected_project(), string_value(session, "project")));
    EnableWindow(settings_button, ready); EnableWindow(tree, ready);
    EnableWindow(stop_button, busy && request_is_chat && !stopping && !closing);
}

static int queue_request(cJSON *message) {
    char *request;
    int is_chat;
    if (!message) return 0;
    is_chat = !strcmp(string_value(message, "type"), "chat");
    request_is_export = !strcmp(string_value(message, "type"), "export_chat");
    request = cJSON_PrintUnformatted(message); cJSON_Delete(message);
    if (!request) return 0;
    if (busy || closing) { free(request); return 0; }
    pending_request = request; pending_serial = InterlockedIncrement(&serial);
    pending_reload = reload_settings; reload_settings = 0;
    request_failed = 0; request_is_chat = is_chat; busy = 1; enable_controls();
    SetWindowTextW(status_label, L"Working..."); SetEvent(work_event); return 1;
}

static cJSON *request_type(const char *type) {
    cJSON *message = cJSON_CreateObject();
    if (message) cJSON_AddStringToObject(message, "type", type);
    return message;
}

static void open_session(const char *id) {
    cJSON *message = request_type("open");
    cJSON_AddStringToObject(message, "id", id); queue_request(message);
}

static void update_header(void) {
    cJSON *item = find_item("sessions", string_value(session, "id"));
    WCHAR *path = to_wide(string_value(session, "path"));
    const char *title = item ? string_value(item, "title") : string_value(session, "title");
    set_text(title_label, *title ? title : "XP-Harness-Desktop");
    if (path) { SetWindowTextW(path_label, path); free(path); }
}

static HTREEITEM insert_item(HTREEITEM parent, const char *label, cJSON *data) {
    TVINSERTSTRUCTW insert;
    WCHAR *wide = to_wide(label);
    HTREEITEM item;
    if (!wide) return NULL;
    ZeroMemory(&insert, sizeof(insert)); insert.hParent = parent; insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM; insert.item.pszText = wide; insert.item.lParam = (LPARAM)data;
    item = (HTREEITEM)SendMessageW(tree, TVM_INSERTITEMW, 0, (LPARAM)&insert); free(wide); return item;
}

static void show_catalog(const char *text) {
    cJSON *next = cJSON_Parse(text), *projects, *sessions, *project, *chat;
    HTREEITEM parent, child, selected = NULL, active = NULL;
    const char *id;
    if (!cJSON_IsObject(next)) { cJSON_Delete(next); return; }
    rebuilding = 1; SendMessageW(tree, WM_SETREDRAW, FALSE, 0); TreeView_DeleteAllItems(tree);
    cJSON_Delete(catalog); catalog = next; replace_string(&active_id, string_value(catalog, "active"));
    projects = cJSON_GetObjectItemCaseSensitive(catalog, "projects");
    sessions = cJSON_GetObjectItemCaseSensitive(catalog, "sessions");
    for (project = projects ? projects->child : NULL; project; project = project->next) {
        id = string_value(project, "id"); parent = insert_item(TVI_ROOT, string_value(project, "name"), project);
        if (selected_id && !strcmp(id, selected_id)) selected = parent;
        for (chat = sessions ? sessions->child : NULL; chat; chat = chat->next) {
            if (strcmp(string_value(chat, "project"), id)) continue;
            child = insert_item(parent, string_value(chat, "title"), chat);
            if (selected_id && !strcmp(string_value(chat, "id"), selected_id)) selected = child;
            if (active_id && !strcmp(string_value(chat, "id"), active_id)) active = child;
        }
        TreeView_Expand(tree, parent, TVE_EXPAND);
    }
    TreeView_SelectItem(tree, selected ? selected : active);
    SendMessageW(tree, WM_SETREDRAW, TRUE, 0); InvalidateRect(tree, NULL, TRUE);
    rebuilding = 0; update_header(); enable_controls();
}

static void protocol_event(const char *json) {
    cJSON *message = cJSON_Parse(json), *next, *context;
    const char *type = string_value(message, "type"), *text = string_value(message, "text");
    WCHAR info[120];
    if (!strcmp(type, "permission")) show_permission(text);
    else if (!strcmp(type, "catalog")) show_catalog(text);
    else if (!strcmp(type, "session")) {
        next = cJSON_Parse(text);
        if (cJSON_IsObject(next)) {
            cJSON_Delete(session); session = next; replace_string(&active_id, string_value(session, "id"));
            replace_string(&selected_id, string_value(session, "id")); update_header();
            context = cJSON_GetObjectItemCaseSensitive(session, "context_bytes");
            wsprintfW(info, L"Context budget: %lu bytes  |  Project memory is shared by its chats.",
                      cJSON_IsNumber(context) ? (DWORD)context->valuedouble : 0);
            SetWindowTextW(context_label, info);
        } else cJSON_Delete(next);
    } else if (!strcmp(type, "clear")) {
        SetWindowTextW(transcript, L""); section[0] = 0; last_character = 0;
    } else if (!strcmp(type, "replay")) { append_utf8(text); section[0] = 0; }
    else if (!strcmp(type, "status")) set_text(status_label, text);
    else if (!strcmp(type, "context")) set_text(context_label, text);
    else if (!strcmp(type, "reasoning_delta") || !strcmp(type, "reasoning")) {
        begin_section("Thinking"); append_utf8(text);
    } else if (!strcmp(type, "answer_delta") || !strcmp(type, "assistant")) {
        begin_section("Answer"); append_utf8(text);
    } else if (!strcmp(type, "tool") || !strcmp(type, "tool_stdin") || !strcmp(type, "tool_result")) {
        begin_section(!strcmp(type, "tool_result") ? "Result" : !strcmp(type, "tool_stdin") ? "Tool input" : "Tool");
        append_utf8(text);
    } else if (!strcmp(type, "error")) { request_failed = 1; begin_section("Error"); append_utf8(text); }
    cJSON_Delete(message);
}

static HWND control(HWND parent, LPCWSTR cls, LPCWSTR label, DWORD style, int id) {
    HWND child = CreateWindowExW(!lstrcmpW(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0,
        cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, parent, (HMENU)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(child, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE); return child;
}

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PROMPT *prompt = (PROMPT *)GetWindowLongW(hwnd, GWL_USERDATA);
    WCHAR *wide;
    char *utf8;
    RECT rect;
    HWND child;
    int length;
    switch (msg) {
    case WM_CREATE:
        prompt = (PROMPT *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongW(hwnd, GWL_USERDATA, (LONG)prompt);
        control(hwnd, L"STATIC", prompt->label, 0, 10);
        prompt->edit = control(hwnd, L"EDIT", prompt->initial, WS_TABSTOP | ES_AUTOVSCROLL | (prompt->approval ? ES_READONLY : 0) |
            (prompt->multiline ? ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL : ES_AUTOHSCROLL), 11);
        control(hwnd, L"BUTTON", prompt->approval ? L"&Allow once" : L"&Save", WS_TABSTOP | (prompt->approval ? 0 : BS_DEFPUSHBUTTON), IDOK);
        control(hwnd, L"BUTTON", prompt->approval ? L"&Deny" : L"Cancel", WS_TABSTOP | (prompt->approval ? BS_DEFPUSHBUTTON : 0), IDCANCEL);
        if (prompt->approval) control(hwnd, L"BUTTON", L"&Stop task", WS_TABSTOP, ID_STOP);
        SendMessageW(prompt->edit, EM_LIMITTEXT, prompt->limit, 0); return 0;
    case WM_SIZE:
        GetClientRect(hwnd, &rect);
        MoveWindow(GetDlgItem(hwnd, 10), 12, 12, rect.right - 24, 36, TRUE);
        MoveWindow(prompt->edit, 12, 50, rect.right - 24, rect.bottom - 102, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDOK), rect.right - (prompt->approval ? 300 : 188), rect.bottom - 38, prompt->approval ? 100 : 80, 26, TRUE);
        if (prompt->approval) MoveWindow(GetDlgItem(hwnd, ID_STOP), rect.right - 192, rect.bottom - 38, 88, 26, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDCANCEL), rect.right - 96, rect.bottom - 38, 84, 26, TRUE); return 0;
    case WM_COMMAND:
        if (prompt->approval && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL || LOWORD(wp) == ID_STOP)) {
            prompt->decision = LOWORD(wp) == IDOK;
            if (LOWORD(wp) == ID_STOP) SendMessageW(window, WM_COMMAND, ID_STOP, 0);
            DestroyWindow(hwnd); return 0;
        }
        if (LOWORD(wp) == IDOK) {
            length = GetWindowTextLengthW(prompt->edit); wide = (WCHAR *)malloc((length + 1) * sizeof(WCHAR));
            if (!wide) return 0;
            GetWindowTextW(prompt->edit, wide, length + 1); utf8 = to_utf8(wide);
            if (!utf8 || strlen(utf8) > (size_t)prompt->limit || (!prompt->multiline && !*utf8)) {
                free(wide); free(utf8);
                MessageBoxW(hwnd, prompt->multiline ? L"Notes must fit within 16 KiB of UTF-8 text." :
                    L"Enter a name using at most 160 UTF-8 bytes.", L"Check text", MB_ICONINFORMATION); return 0;
            }
            free(utf8); prompt->result = wide; DestroyWindow(hwnd);
        } else if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_GETMINMAXINFO:
        if (prompt && prompt->approval) {
            ((MINMAXINFO *)lp)->ptMinTrackSize.x = 500;
            ((MINMAXINFO *)lp)->ptMinTrackSize.y = 300;
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        child = (HWND)lp; SetTextColor((HDC)wp, GetSysColor(child == prompt->edit ? COLOR_WINDOWTEXT : COLOR_BTNTEXT));
        SetBkColor((HDC)wp, GetSysColor(child == prompt->edit ? COLOR_WINDOW : COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(child == prompt->edit ? COLOR_WINDOW : COLOR_BTNFACE);
    case WM_DESTROY: prompt->done = 1; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_permission(const char *text) {
    cJSON *request = cJSON_Parse(text), *reply;
    char *details;
    WCHAR *wide;
    PROMPT prompt;
    MSG msg;
    RECT owner;
    size_t size;
    if (closing || stopping || !busy || !*string_value(request, "id")) { cJSON_Delete(request); return; }
    size = strlen(text) + 256; details = (char *)malloc(size);
    if (!details) { cJSON_Delete(request); SendMessageW(window, WM_COMMAND, ID_STOP, 0); return; }
    _snprintf(details, size, "Working directory:\r\n%s\r\n\r\nCommand (cmd.exe /D /C):\r\n%s\r\n\r\nStandard input (verbatim):\r\n%s",
        string_value(request, "cwd"), string_value(request, "command"), string_value(request, "stdin"));
    details[size - 1] = 0; wide = to_wide(details); free(details);
    ZeroMemory(&prompt, sizeof(prompt)); prompt.approval = prompt.multiline = 1;
    prompt.limit = IPC_MAX_FRAME; prompt.initial = wide ? wide : L"";
    prompt.label = L"Allow this command once? Review the full command and input below.";
    GetWindowRect(window, &owner); EnableWindow(window, FALSE);
    permission_window = wide ? CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"XPHarnessPrompt", L"Tool permission",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, owner.left + 70, owner.top + 70, 680, 470,
        window, NULL, GetModuleHandleW(NULL), &prompt) : NULL;
    if (permission_window) {
        ShowWindow(permission_window, SW_SHOW); SetFocus(GetDlgItem(permission_window, IDCANCEL));
        while (!prompt.done && GetMessageW(&msg, NULL, 0, 0) > 0)
            if (!IsDialogMessageW(permission_window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (!prompt.done) { DestroyWindow(permission_window); PostQuitMessage((int)msg.wParam); }
    }
    permission_window = NULL; free(wide); EnableWindow(window, TRUE);
    if (!closing) SetActiveWindow(window);
    if (!stopping && !closing && busy) {
        reply = cJSON_CreateObject(); cJSON_AddStringToObject(reply, "type", "permission");
        cJSON_AddStringToObject(reply, "id", string_value(request, "id")); cJSON_AddBoolToObject(reply, "allow", prompt.decision);
        permission_reply = cJSON_PrintUnformatted(reply); cJSON_Delete(reply); SetEvent(permission_event);
    }
    cJSON_Delete(request);
}

static WCHAR *edit_text(const WCHAR *title, const WCHAR *label, const WCHAR *initial, int multiline) {
    PROMPT prompt;
    HWND dialog;
    MSG msg;
    RECT owner;
    ZeroMemory(&prompt, sizeof(prompt)); prompt.label = label; prompt.initial = initial;
    prompt.multiline = multiline; prompt.limit = multiline ? MEMORY_BYTES : NAME_BYTES;
    GetWindowRect(window, &owner); EnableWindow(window, FALSE);
    dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"XPHarnessPrompt", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU, owner.left + 70, owner.top + 70,
        multiline ? 590 : 450, multiline ? 450 : 190, window, NULL, GetModuleHandleW(NULL), &prompt);
    if (dialog) {
        ShowWindow(dialog, SW_SHOW); SetFocus(prompt.edit); SendMessageW(prompt.edit, EM_SETSEL, 0, -1);
        while (!prompt.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (!IsDialogMessageW(dialog, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (!prompt.done) { DestroyWindow(dialog); PostQuitMessage((int)msg.wParam); }
    }
    EnableWindow(window, TRUE); SetActiveWindow(window); return prompt.result;
}

static void new_project(void) {
    BROWSEINFOW browse;
    LPITEMIDLIST folder;
    WCHAR path[MAX_PATH], *name, *base;
    char *utf8_name, *utf8_path;
    cJSON *message;
    ZeroMemory(&browse, sizeof(browse)); browse.hwndOwner = window;
    browse.lpszTitle = L"Choose the project's working folder"; browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    folder = SHBrowseForFolderW(&browse); if (!folder) return;
    if (!SHGetPathFromIDListW(folder, path)) { CoTaskMemFree(folder); return; }
    CoTaskMemFree(folder); base = wcsrchr(path, L'\\'); base = base && base[1] ? base + 1 : path;
    name = edit_text(L"New project", L"&Project name (up to 160 UTF-8 bytes):", base, 0); if (!name) return;
    utf8_name = to_utf8(name); utf8_path = to_utf8(path); free(name); message = request_type("new_project");
    if (utf8_name && utf8_path && message) {
        cJSON_AddStringToObject(message, "text", utf8_name); cJSON_AddStringToObject(message, "path", utf8_path); queue_request(message);
    } else cJSON_Delete(message);
    free(utf8_name); free(utf8_path);
}

static void edit_selection(int memory) {
    cJSON *item = selection(), *message;
    const char *value = memory ? string_value(session, "memory") :
        *string_value(item, "project") ? string_value(item, "title") : string_value(item, "name");
    WCHAR *initial = to_wide(value), *result;
    char *text;
    if (!initial) return;
    result = edit_text(memory ? L"Shared project memory" : L"Rename", memory ?
        L"&Notes shared by this project's chats (16 KiB UTF-8). Save explicitly to apply." :
        L"&Name (up to 160 UTF-8 bytes):", initial, memory); free(initial);
    if (!result) return;
    text = to_utf8(result); free(result); if (!text) return;
    message = request_type(memory ? "memory" : "rename"); cJSON_AddStringToObject(message, "text", text);
    if (!memory) cJSON_AddStringToObject(message, "id", string_value(item, "id"));
    free(text); queue_request(message);
}

static void send_chat(void) {
    WCHAR wide[INPUT_LIMIT + 1];
    char *text;
    cJSON *message;
    if (busy || closing || !*string_value(session, "id")) return;
    GetWindowTextW(input, wide, INPUT_LIMIT + 1); if (!*wide) { SetFocus(input); return; }
    text = to_utf8(wide); if (!text) return;
    message = request_type("chat"); cJSON_AddStringToObject(message, "id", string_value(session, "id"));
    cJSON_AddStringToObject(message, "text", text); cJSON_AddBoolToObject(message, "stream", 1); free(text);
    if (queue_request(message)) {
        section[0] = 0; begin_section("You"); append_text(wide); section[0] = 0;
        SetWindowTextW(input, L""); SetFocus(input);
    }
}

static void open_settings(void) {
    WCHAR ini[MAX_PATH], sample[MAX_PATH], notepad[MAX_PATH], params[MAX_PATH + 4];
    UINT size;
    wsprintfW(ini, L"%s\\vc6ai.ini", directory);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES) {
        wsprintfW(sample, L"%s\\vc6ai.sample.ini", directory);
        if (!CopyFileW(sample, ini, TRUE)) {
            MessageBoxW(window, L"Cannot create vc6ai.ini from vc6ai.sample.ini. Check folder permissions.", L"Settings", MB_ICONERROR); return;
        }
    }
    size = GetSystemDirectoryW(notepad, MAX_PATH); if (!size || size > MAX_PATH - 14) return;
    lstrcatW(notepad, L"\\notepad.exe"); wsprintfW(params, L"\"%s\"", ini);
    if ((int)ShellExecuteW(window, L"open", notepad, params, directory, SW_SHOWNORMAL) <= 32)
        MessageBoxW(window, L"Notepad could not open vc6ai.ini.", L"Settings", MB_ICONERROR);
    else { reload_settings = 1;
        SetWindowTextW(status_label, L"Save settings before the next action. Sessions and project memory are retained."); }
}

static void chat_action(const char *id, int action) {
    OPENFILENAMEW dialog;
    WCHAR path[MAX_PATH] = L"conversation.json";
    char *utf8;
    cJSON *message;
    if (busy || closing) return;
    if (action == ID_DELETE) {
        if (MessageBoxW(window, L"Delete this session and its saved history?\nProject files and shared notes are kept.",
                        L"Delete session", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        message = request_type("delete_chat");
    } else {
        ZeroMemory(&dialog, sizeof(dialog)); dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
        dialog.lpstrFilter = L"JSON session (*.json)\0*.json\0\0"; dialog.lpstrDefExt = L"json";
        dialog.lpstrFile = path; dialog.nMaxFile = MAX_PATH; dialog.lpstrTitle = L"Export session";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        if (!GetSaveFileNameW(&dialog)) return;
        utf8 = to_utf8(path); if (!utf8) return;
        message = request_type("export_chat"); cJSON_AddStringToObject(message, "path", utf8); free(utf8);
    }
    cJSON_AddStringToObject(message, "id", id); queue_request(message);
}

static void chat_menu(LPARAM position) {
    TVHITTESTINFO hit;
    TVITEMW item;
    RECT rect;
    POINT point;
    HMENU menu;
    int action;
    const char *id;
    if (busy || closing) return;
    point.x = (short)LOWORD(position); point.y = (short)HIWORD(position);
    ZeroMemory(&item, sizeof(item)); item.mask = TVIF_PARAM;
    if (position == (LPARAM)-1) {
        item.hItem = TreeView_GetSelection(tree);
        if (!item.hItem || !TreeView_GetItemRect(tree, item.hItem, &rect, TRUE)) return;
        point.x = rect.left; point.y = rect.bottom; ClientToScreen(tree, &point);
    } else {
        ZeroMemory(&hit, sizeof(hit)); hit.pt = point; ScreenToClient(tree, &hit.pt);
        item.hItem = TreeView_HitTest(tree, &hit);
    }
    if (!item.hItem || !SendMessageW(tree, TVM_GETITEMW, 0, (LPARAM)&item) ||
        !*string_value((cJSON *)item.lParam, "project")) return;
    id = string_value((cJSON *)item.lParam, "id");
    menu = CreatePopupMenu(); if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_EXPORT, L"&Export session...");
    AppendMenuW(menu, MF_STRING, ID_DELETE, L"&Delete session...");
    action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, NULL);
    DestroyMenu(menu);
    if (action) chat_action(id, action);
}

/* Draft paths only: no file read, upload or model request on drop. */
static void drop_files(HDROP drop) {
    WCHAR draft[INPUT_LIMIT + 1], path[MAX_PATH];
    UINT i, count = DragQueryFileW(drop, (UINT)-1, NULL, 0), length;
    int used = GetWindowTextW(input, draft, INPUT_LIMIT + 1), ok = !closing;
    for (i = 0; i < count && ok; ++i) {
        length = DragQueryFileW(drop, i, NULL, 0);
        if (!length || length >= MAX_PATH || used + length + 4 > INPUT_LIMIT ||
            !DragQueryFileW(drop, i, path, MAX_PATH)) { ok = 0; break; }
        if (used) { draft[used++] = L'\r'; draft[used++] = L'\n'; }
        draft[used++] = L'"'; memcpy(draft + used, path, length * sizeof(WCHAR)); used += length;
        draft[used++] = L'"'; draft[used] = 0;
    }
    DragFinish(drop);
    if (ok && count) {
        SetWindowTextW(input, draft); SetFocus(input); SendMessageW(input, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        SetWindowTextW(status_label, L"Paths added to draft. Add your instruction, then Send; file reads still require tool approval.");
    } else if (!closing) SetWindowTextW(status_label, L"Drop rejected: paths exceed MAX_PATH or the message limit. Draft kept; drop fewer files.");
}

static void system_colors(void) {
    TreeView_SetBkColor(tree, GetSysColor(COLOR_WINDOW)); TreeView_SetTextColor(tree, GetSysColor(COLOR_WINDOWTEXT));
    RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UI_EVENT *event;
    RECT rect;
    int width, height;
    MINMAXINFO *limits;
    MSG queued;
    cJSON *item, *message;
    const char *id;
    HWND child;
    switch (msg) {
    case WM_CREATE:
        window = hwnd;
        tree_label = control(hwnd, L"STATIC", L"Projects / sessions", 0, 114);
        project_button = control(hwnd, L"BUTTON", L"New &project...", WS_TABSTOP, ID_PROJECT);
        new_button = control(hwnd, L"BUTTON", L"&New chat", WS_TABSTOP, ID_NEW);
        rename_button = control(hwnd, L"BUTTON", L"&Rename...", WS_TABSTOP, ID_RENAME);
        memory_button = control(hwnd, L"BUTTON", L"&Memory...", WS_TABSTOP, ID_MEMORY);
        tree = control(hwnd, WC_TREEVIEWW, L"Projects and sessions", WS_TABSTOP | TVS_HASBUTTONS |
            TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS, ID_TREE);
        title_label = control(hwnd, L"STATIC", L"XP-Harness-Desktop", SS_LEFTNOWORDWRAP, 115);
        settings_button = control(hwnd, L"BUTTON", L"&Settings...", WS_TABSTOP, ID_SETTINGS);
        path_label = control(hwnd, L"STATIC", L"Loading working folder...", SS_PATHELLIPSIS, 116);
        context_label = control(hwnd, L"STATIC", L"Loading saved session...", SS_LEFTNOWORDWRAP, 117);
        transcript = control(hwnd, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | WS_TABSTOP, 110);
        input_label = control(hwnd, L"STATIC", L"&Message (Ctrl+Enter to send; drop files to add paths):", 0, 111);
        input = control(hwnd, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 112);
        send_button = control(hwnd, L"BUTTON", L"&Send", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_SEND);
        stop_button = control(hwnd, L"BUTTON", L"S&top", WS_TABSTOP, ID_STOP);
        status_label = control(hwnd, L"STATIC", L"Opening your saved projects...", SS_LEFTNOWORDWRAP, 113);
        if (!tree || !transcript || !input || !send_button || !status_label) return -1;
        SendMessageW(tree, CCM_SETUNICODEFORMAT, TRUE, 0);
        SendMessageW(input, EM_LIMITTEXT, INPUT_LIMIT, 0); SendMessageW(transcript, EM_LIMITTEXT, 1048576, 0);
        DragAcceptFiles(hwnd, TRUE); system_colors(); return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        GetClientRect(hwnd, &rect); width = rect.right; height = rect.bottom;
        MoveWindow(tree_label, 12, 14, 218, 20, TRUE);
        MoveWindow(project_button, 12, 40, 110, 26, TRUE); MoveWindow(new_button, 130, 40, 100, 26, TRUE);
        MoveWindow(rename_button, 12, 74, 110, 26, TRUE); MoveWindow(memory_button, 130, 74, 100, 26, TRUE);
        MoveWindow(tree, 12, 110, 218, height - 152, TRUE);
        MoveWindow(title_label, 250, 14, width - 370, 22, TRUE);
        MoveWindow(settings_button, width - 110, 10, 98, 26, TRUE);
        MoveWindow(path_label, 250, 42, width - 262, 26, TRUE);
        MoveWindow(context_label, 250, 76, width - 262, 20, TRUE);
        MoveWindow(transcript, 250, 106, width - 262, height - 258, TRUE);
        MoveWindow(input_label, 250, height - 140, width - 262, 20, TRUE);
        MoveWindow(input, 250, height - 114, width - 364, 72, TRUE);
        MoveWindow(send_button, width - 102, height - 114, 90, 32, TRUE);
        MoveWindow(stop_button, width - 102, height - 74, 90, 32, TRUE);
        MoveWindow(status_label, 12, height - 28, width - 24, 20, TRUE); return 0;
    case WM_GETMINMAXINFO:
        limits = (MINMAXINFO *)lp; limits->ptMinTrackSize.x = 780; limits->ptMinTrackSize.y = 540; return 0;
    case WM_SETFOCUS: SetFocus(input); return 0;
    case WM_CONTEXTMENU:
        if ((HWND)wp == tree) { chat_menu(lp); return 0; }
        break;
    case WM_DROPFILES: drop_files((HDROP)wp); return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == ID_TREE && ((NMHDR *)lp)->code == TVN_SELCHANGEDW && !rebuilding && !busy) {
            item = selection(); id = string_value(item, "id"); replace_string(&selected_id, id); enable_controls();
            if (*string_value(item, "project") && strcmp(id, string_value(session, "id"))) open_session(id);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEND) send_chat();
        else if (LOWORD(wp) == ID_STOP && busy && request_is_chat && !stopping && !closing) {
            stopping = 1; bootstrap = 0; enable_controls();
            SetWindowTextW(status_label, L"Stopping and saving tool outcomes..."); SetEvent(core_cancel); SetEvent(cancel_event);
            SetTimer(hwnd, 2, 5000, NULL);
        } else if (!busy && !closing) {
            if (LOWORD(wp) == ID_NEW && *selected_project()) {
                message = request_type("new_chat"); cJSON_AddStringToObject(message, "project", selected_project()); queue_request(message);
            } else if (LOWORD(wp) == ID_PROJECT) new_project();
            else if (LOWORD(wp) == ID_RENAME && selection()) edit_selection(0);
            else if (LOWORD(wp) == ID_MEMORY && IsWindowEnabled(memory_button)) edit_selection(1);
            else if (LOWORD(wp) == ID_SETTINGS) open_settings();
        }
        return 0;
    case WM_EVENT:
        event = (UI_EVENT *)lp;
        if (!closing && event->serial == serial) {
            if (event->kind == EV_PROTOCOL) protocol_event(event->text);
            else if (event->kind == EV_FAILED) { request_failed = 1; begin_section("Error"); append_utf8(event->text); }
            else {
                KillTimer(hwnd, 2);
                if (permission_window) DestroyWindow(permission_window);
                busy = stopping = 0;
                if (event->kind == EV_CANCELLED) {
                    begin_section("Interrupted"); append_text(L"Task cancelled. Saved conversation and project memory are retained.");
                    SetWindowTextW(status_label, L"Cancelled. Send resumes this saved session.");
                } else SetWindowTextW(status_label, request_failed ? L"Request failed. Existing saved history is retained." :
                    request_is_export ? L"Session exported as UTF-8 JSON. Ready." : L"Ready. Ctrl+Enter sends.");
                enable_controls();
                if (bootstrap) { bootstrap = 0; if (!request_failed && active_id && *active_id) open_session(active_id); }
            }
        }
        free(event); return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        child = (HWND)lp;
        SetTextColor((HDC)wp, GetSysColor(child == transcript || child == input ? COLOR_WINDOWTEXT : COLOR_BTNTEXT));
        SetBkColor((HDC)wp, GetSysColor(child == transcript || child == input ? COLOR_WINDOW : COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(child == transcript || child == input ? COLOR_WINDOW : COLOR_BTNFACE);
    case WM_SYSCOLORCHANGE: system_colors(); break;
    case WM_CLOSE:
        if (closing) return 0;
        closing = 1; enable_controls(); SetWindowTextW(status_label, L"Closing AgentCore...");
        if (permission_window) DestroyWindow(permission_window);
        SetEvent(core_cancel); SetEvent(quit_event); SetEvent(cancel_event);
        SetTimer(hwnd, 2, 5000, NULL); SetTimer(hwnd, 1, 30, NULL); return 0;
    case WM_TIMER:
        if (wp == 2) { KillTimer(hwnd, 2); SetEvent(force_event); }
        if (closing && WaitForSingleObject(worker, 0) == WAIT_OBJECT_0) {
            KillTimer(hwnd, 1);
            while (PeekMessageW(&queued, hwnd, WM_EVENT, WM_EVENT, PM_REMOVE)) free((void *)queued.lParam);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    WNDCLASSW cls;
    WCHAR *slash;
    MSG msg;
    DWORD length;
    (void)previous; (void)command;
    length = GetModuleFileNameW(NULL, directory, MAX_PATH);
    if (!length || length >= MAX_PATH - 28) return 1;
    slash = wcsrchr(directory, L'\\'); if (!slash) return 1; *slash = 0;
    quit_event = CreateEventW(NULL, TRUE, FALSE, NULL); cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    work_event = CreateEventW(NULL, FALSE, FALSE, NULL); if (!quit_event || !cancel_event || !work_event) return 1;
    core_cancel = CreateEventW(NULL, TRUE, FALSE, NULL); force_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    permission_event = CreateEventW(NULL, TRUE, FALSE, NULL); if (!core_cancel || !force_event || !permission_event) return 1;
    InitCommonControls(); CoInitialize(NULL);
    ZeroMemory(&cls, sizeof(cls)); cls.lpfnWndProc = window_proc; cls.hInstance = instance;
    cls.hCursor = LoadCursor(NULL, IDC_ARROW); cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); cls.lpszClassName = L"XPHarnessDesktop";
    if (!RegisterClassW(&cls)) return 1;
    cls.lpfnWndProc = prompt_proc; cls.lpszClassName = L"XPHarnessPrompt"; if (!RegisterClassW(&cls)) return 1;
    window = CreateWindowExW(WS_EX_CONTROLPARENT, L"XPHarnessDesktop", L"XP-Harness-Desktop",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1020, 700, NULL, NULL, instance, NULL);
    if (!window) return 1;
    worker = (HANDLE)_beginthreadex(NULL, 0, worker_main, NULL, 0, NULL);
    if (!worker) { DestroyWindow(window); return 1; }
    bootstrap = 1; queue_request(request_type("library"));
    ShowWindow(window, show); UpdateWindow(window); SetFocus(input);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            GetFocus() == input && (GetKeyState(VK_CONTROL) & 0x8000)) { send_chat(); continue; }
        if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    CloseHandle(worker); CloseHandle(work_event); CloseHandle(cancel_event); CloseHandle(quit_event);
    CloseHandle(core_cancel); CloseHandle(force_event); CloseHandle(permission_event);
    cJSON_Delete(session); cJSON_Delete(catalog); free(active_id); free(selected_id); CoUninitialize();
    return (int)msg.wParam;
}
