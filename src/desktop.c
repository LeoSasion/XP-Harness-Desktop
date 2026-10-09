/* XP-Harness-Desktop: a deliberately small Unicode Win32 shell. C89 / VC6. */
#define _WIN32_WINNT 0x0501
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "cJSON.h"
#include "ipc.h"

#define ID_SEND 101
#define ID_NEW 102
#define ID_STOP 103
#define ID_SETTINGS 104
#define WM_EVENT (WM_APP + 1)
#define EV_LINE 1
#define EV_STATUS 2
#define EV_IDLE 3
#define EV_RESET 4
#define INPUT_LIMIT 16384

typedef struct { LONG serial; int kind; WCHAR text[1]; } UI_EVENT;
static HWND window, transcript, input, send_button, new_button, stop_button;
static HWND settings_button, status_label, input_label;
static HANDLE quit_event, cancel_event, work_event, worker;
static HANDLE core_process, pipe_handle = INVALID_HANDLE_VALUE;
static WCHAR directory[MAX_PATH];
static char *pending_request;
static LONG serial, pending_serial;
static int busy, stopping, closing, pending_reset;

static void post_event(int kind, LONG id, const char *text) {
    UI_EVENT *event;
    int count;
    if (WaitForSingleObject(quit_event, 0) == WAIT_OBJECT_0) return;
    if (!text) text = "";
    count = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (!count) return;
    event = (UI_EVENT *)malloc(sizeof(UI_EVENT) + count * sizeof(WCHAR));
    if (!event) return;
    event->serial = id; event->kind = kind;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, event->text, count);
    if (!PostMessageW(window, WM_EVENT, 0, (LPARAM)event)) free(event);
}

static void stop_core(void) {
    if (pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_handle); pipe_handle = INVALID_HANDLE_VALUE;
    }
    if (core_process) {
        /* This handle belongs only to the child created below. Never kill by name. */
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
    if (!CryptAcquireContextW(&crypto, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return 0;
    error = CryptGenRandom(crypto, sizeof(random), (BYTE *)random);
    CryptReleaseContext(crypto, 0);
    if (!error) return 0;
    wsprintfW(name, L"\\\\.\\pipe\\vc6agent-%lu-%08lx%08lx", GetCurrentProcessId(), random[0], random[1]);
    wsprintfW(executable, L"%s\\AgentCore.exe", directory);
    wsprintfW(command, L"\"%s\" --pipe %s --parent %lu", executable, name, GetCurrentProcessId());
    ZeroMemory(&startup, sizeof(startup)); startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, directory, &startup, &process)) return 0;
    core_process = process.hProcess; CloseHandle(process.hThread);
    started = GetTickCount();
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
    HANDLE events[3];
    DWORD wait;
    LONG id;
    char *request, *reply;
    cJSON *message, *type, *text;
    int ok, finished, reset;
    (void)unused;
    events[0] = quit_event; events[1] = cancel_event; events[2] = work_event;
    for (;;) {
        wait = WaitForMultipleObjects(3, events, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        if (wait == WAIT_OBJECT_0 + 1) {
            stop_core(); free(pending_request); pending_request = NULL;
            ResetEvent(work_event); ResetEvent(cancel_event);
            post_event(EV_IDLE, InterlockedCompareExchange(&serial, 0, 0),
                       "Stopped. Model history cleared; the next Send starts a new session.");
            continue;
        }
        request = pending_request; pending_request = NULL; id = pending_serial;
        if (!request) continue;
        reset = pending_reset;
        post_event(EV_STATUS, id, "Working... Stop cancels and clears model history.");
        ok = start_core() && ipc_write(pipe_handle, request, cancel_event, 10000);
        free(request); finished = 0;
        while (ok && !finished) {
            reply = NULL;
            /* Timeout bounds a stalled provider; cancellation interrupts every pipe wait. */
            ok = ipc_read(pipe_handle, &reply, cancel_event, 600000);
            if (!ok) { free(reply); break; }
            message = cJSON_Parse(reply); free(reply);
            type = cJSON_GetObjectItemCaseSensitive(message, "type");
            text = cJSON_GetObjectItemCaseSensitive(message, "text");
            if (!cJSON_IsString(type)) ok = 0;
            else if (!strcmp(type->valuestring, "done")) finished = 1;
            else if (cJSON_IsString(text)) {
                if (!strcmp(type->valuestring, "status")) post_event(EV_STATUS, id, text->valuestring);
                else {
                    post_event(EV_LINE, id, !strcmp(type->valuestring, "tool") ? "[Tool]" :
                        !strcmp(type->valuestring, "error") ? "[Error]" : "[Agent]");
                    post_event(EV_LINE, id, text->valuestring);
                }
            }
            cJSON_Delete(message);
        }
        if (WaitForSingleObject(cancel_event, 0) == WAIT_OBJECT_0) {
            stop_core(); ResetEvent(cancel_event);
            post_event(EV_IDLE, InterlockedCompareExchange(&serial, 0, 0),
                       "Stopped. Model history cleared; the next Send starts a new session.");
        } else {
            if (!ok) {
                stop_core();
                post_event(EV_LINE, id, "[Connection error] Check AgentCore.exe, its DLLs and vc6ai.ini. Model history was cleared.");
            } else if (reset) post_event(EV_RESET, id, "New session. Model history cleared.");
            post_event(EV_IDLE, id, ok ? "Ready. Ctrl+Enter sends." : "Connection failed. Send starts a fresh core.");
        }
    }
    stop_core(); free(pending_request); pending_request = NULL;
    return 0;
}

static void append_text(const WCHAR *text) {
    WCHAR *normalized;
    int i, length = lstrlenW(text), used = 0;
    normalized = (WCHAR *)malloc((length * 2 + 3) * sizeof(WCHAR));
    if (!normalized) return;
    for (i = 0; i < length; ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) normalized[used++] = L'\r';
        normalized[used++] = text[i];
    }
    normalized[used++] = L'\r'; normalized[used++] = L'\n'; normalized[used] = 0;
    if (GetWindowTextLengthW(transcript) + used > 1000000)
        SetWindowTextW(transcript, L"[Earlier display text removed; model history is unchanged.]\r\n");
    SendMessageW(transcript, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(transcript, EM_REPLACESEL, FALSE, (LPARAM)normalized);
    SendMessageW(transcript, EM_SCROLLCARET, 0, 0); free(normalized);
}

static void enable_controls(void) {
    EnableWindow(send_button, !busy && !closing);
    EnableWindow(new_button, !busy && !closing);
    EnableWindow(settings_button, !busy && !closing);
    EnableWindow(stop_button, !stopping && !closing);
}

static void submit(int reset) {
    WCHAR wide[INPUT_LIMIT + 1];
    char *utf8;
    cJSON *message;
    int count;
    if (busy || closing) return;
    GetWindowTextW(input, wide, INPUT_LIMIT + 1);
    if (!reset && !wide[0]) { SetFocus(input); return; }
    count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    utf8 = (char *)malloc(count ? count : 1);
    if (!utf8) return;
    if (!count || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, count, NULL, NULL)) {
        free(utf8); return;
    }
    message = cJSON_CreateObject();
    if (!message) { free(utf8); return; }
    cJSON_AddStringToObject(message, "type", reset ? "reset" : "chat");
    if (!reset) cJSON_AddStringToObject(message, "text", utf8);
    free(utf8); pending_request = cJSON_PrintUnformatted(message); cJSON_Delete(message);
    if (!pending_request) return;
    pending_reset = reset; pending_serial = InterlockedIncrement(&serial); busy = 1; enable_controls();
    if (!reset) { append_text(L"[You]"); append_text(wide); SetWindowTextW(input, L""); }
    SetWindowTextW(status_label, L"Starting request..."); SetEvent(work_event); SetFocus(input);
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
    size = GetSystemDirectoryW(notepad, MAX_PATH);
    if (!size || size > MAX_PATH - 14) return;
    lstrcatW(notepad, L"\\notepad.exe"); wsprintfW(params, L"\"%s\"", ini);
    if ((int)ShellExecuteW(window, L"open", notepad, params, directory, SW_SHOWNORMAL) <= 32)
        MessageBoxW(window, L"Notepad could not open vc6ai.ini.", L"Settings", MB_ICONERROR);
    else SetWindowTextW(status_label, L"Save settings, then click Stop to apply. Stop clears model history.");
}

static HWND control(LPCWSTR cls, LPCWSTR label, DWORD style, int id) {
    HWND child = CreateWindowExW(!lstrcmpW(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0,
        cls, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window, (HMENU)id,
        GetModuleHandleW(NULL), NULL);
    SendMessageW(child, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return child;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UI_EVENT *event;
    RECT rect;
    int width, height;
    MINMAXINFO *limits;
    MSG queued;
    switch (msg) {
    case WM_CREATE:
        window = hwnd;
        new_button = control(L"BUTTON", L"&New chat", WS_TABSTOP, ID_NEW);
        stop_button = control(L"BUTTON", L"S&top", WS_TABSTOP, ID_STOP);
        settings_button = control(L"BUTTON", L"&Settings...", WS_TABSTOP, ID_SETTINGS);
        transcript = control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | WS_TABSTOP, 110);
        input_label = control(L"STATIC", L"&Message (Ctrl+Enter to send):", 0, 111);
        input = control(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 112);
        send_button = control(L"BUTTON", L"&Send", WS_TABSTOP | BS_DEFPUSHBUTTON, ID_SEND);
        status_label = control(L"STATIC", L"Ready. AgentCore starts on the first request.", SS_LEFTNOWORDWRAP, 113);
        if (!new_button || !stop_button || !settings_button || !transcript || !input_label ||
            !input || !send_button || !status_label) return -1;
        SendMessageW(input, EM_LIMITTEXT, INPUT_LIMIT, 0);
        SendMessageW(transcript, EM_LIMITTEXT, 1048576, 0);
        append_text(L"XP-Harness-Desktop\r\nNative desktop client. Responses appear as plain text.\r\nStop cancels the owned core process and clears its conversation history.\r\n");
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        GetClientRect(hwnd, &rect); width = rect.right; height = rect.bottom;
        MoveWindow(new_button, 12, 10, 92, 26, TRUE);
        MoveWindow(stop_button, 112, 10, 76, 26, TRUE);
        MoveWindow(settings_button, 196, 10, 100, 26, TRUE);
        MoveWindow(transcript, 12, 46, width - 24, height - 202, TRUE);
        MoveWindow(input_label, 12, height - 146, width - 24, 20, TRUE);
        MoveWindow(input, 12, height - 122, width - 116, 80, TRUE);
        MoveWindow(send_button, width - 94, height - 122, 82, 32, TRUE);
        MoveWindow(status_label, 12, height - 30, width - 24, 20, TRUE); return 0;
    case WM_GETMINMAXINFO:
        limits = (MINMAXINFO *)lp; limits->ptMinTrackSize.x = 540; limits->ptMinTrackSize.y = 400; return 0;
    case WM_SETFOCUS: SetFocus(input); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEND) submit(0);
        else if (LOWORD(wp) == ID_NEW) submit(1);
        else if (LOWORD(wp) == ID_SETTINGS && !busy && !closing) open_settings();
        else if (LOWORD(wp) == ID_STOP && !stopping && !closing) {
            InterlockedIncrement(&serial); busy = stopping = 1; enable_controls();
            SetWindowTextW(status_label, L"Stopping AgentCore and clearing model history..."); SetEvent(cancel_event);
        }
        return 0;
    case WM_EVENT:
        event = (UI_EVENT *)lp;
        if (!closing && event->serial == serial) {
            if (event->kind == EV_LINE) append_text(event->text);
            else if (event->kind == EV_RESET) { SetWindowTextW(transcript, L""); append_text(event->text); }
            else {
                SetWindowTextW(status_label, event->text);
                if (event->kind == EV_IDLE) { busy = stopping = 0; enable_controls(); }
            }
        }
        free(event); return 0;
    case WM_CLOSE:
        if (closing) return 0;
        closing = 1; enable_controls(); SetWindowTextW(status_label, L"Closing AgentCore...");
        SetEvent(quit_event); SetEvent(cancel_event); SetTimer(hwnd, 1, 30, NULL); return 0;
    case WM_TIMER:
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
    quit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    work_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!quit_event || !cancel_event || !work_event) return 1;
    ZeroMemory(&cls, sizeof(cls)); cls.lpfnWndProc = window_proc; cls.hInstance = instance;
    cls.hCursor = LoadCursor(NULL, IDC_ARROW); cls.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); cls.lpszClassName = L"XPHarnessDesktop";
    if (!RegisterClassW(&cls)) return 1;
    window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, L"XP-Harness-Desktop",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 780, 580, NULL, NULL, instance, NULL);
    if (!window) return 1;
    worker = (HANDLE)_beginthreadex(NULL, 0, worker_main, NULL, 0, NULL);
    if (!worker) { DestroyWindow(window); return 1; }
    ShowWindow(window, show); UpdateWindow(window); SetFocus(input);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            GetFocus() == input && (GetKeyState(VK_CONTROL) & 0x8000)) { submit(0); continue; }
        if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    CloseHandle(worker); CloseHandle(work_event); CloseHandle(cancel_event); CloseHandle(quit_event);
    return (int)msg.wParam;
}
