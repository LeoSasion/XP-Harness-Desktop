/* Exercise real native drop messages and EDIT controls without a model or files. */
#define WinMain desktop_entry
#include "../src/desktop.c"
#undef WinMain

static int checks, failures;
static int context_events;
static HWND context_target;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static LRESULT CALLBACK test_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CONTEXTMENU) { ++context_events; context_target = (HWND)wp; return 0; }
    return window_proc(hwnd, msg, wp, lp);
}

static void drop(const WCHAR *paths, size_t size)
{
    HGLOBAL handle = GlobalAlloc(GHND, sizeof(DROPFILES) + size);
    DROPFILES *files = (DROPFILES *)GlobalLock(handle);
    CHECK(files != NULL);
    if (!files) return;
    files->pFiles = sizeof(DROPFILES); files->fWide = TRUE;
    memcpy((char *)files + sizeof(DROPFILES), paths, size);
    GlobalUnlock(handle);
    SendMessageW(window, WM_DROPFILES, (WPARAM)handle, 0); /* App owns and frees HDROP. */
}

int main(void)
{
    WNDCLASSW cls;
    WCHAR result[INPUT_LIMIT + 1], large[INPUT_LIMIT + 1], bad[MAX_PATH + 20];
    const WCHAR paths[] = L"C:\\docs\\\x4e2d\x6587 \x8d44\x6599.txt\0C:\\two.md\0";
    const WCHAR batch[] = L"C:\\valid.txt\0";
    WCHAR *mixed;
    cJSON *chat;
    HTREEITEM node;
    RECT rect;
    LPARAM point;
    size_t first = sizeof(batch) / sizeof(WCHAR) - 1;
    InitCommonControls();
    ZeroMemory(&cls, sizeof(cls)); cls.lpfnWndProc = test_window_proc;
    cls.hInstance = GetModuleHandleW(NULL); cls.lpszClassName = L"XPHarnessDropTests";
    CHECK(RegisterClassW(&cls));
    window = CreateWindowW(cls.lpszClassName, L"Drop tests", WS_OVERLAPPEDWINDOW,
                           0, 0, 800, 600, NULL, NULL, cls.hInstance, NULL);
    CHECK(window != NULL);
    CHECK((GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0);
    SetWindowTextW(input, L"Summarize these:");
    drop(paths, sizeof(paths));
    GetWindowTextW(input, result, INPUT_LIMIT + 1);
    CHECK(!wcscmp(result, L"Summarize these:\r\n\"C:\\docs\\\x4e2d\x6587 \x8d44\x6599.txt\"\r\n\"C:\\two.md\""));
    CHECK(pending_request == NULL && !busy); /* Draft only, no request or auto-read. */
    GetWindowTextW(status_label, result, INPUT_LIMIT + 1);
    CHECK(wcsstr(result, L"Paths added to draft") != NULL);
    wmemset(large, L'x', INPUT_LIMIT); large[INPUT_LIMIT] = 0;
    SetWindowTextW(input, large);
    drop(paths, sizeof(paths));
    GetWindowTextW(input, result, INPUT_LIMIT + 1);
    CHECK(!wcscmp(result, large));
    GetWindowTextW(status_label, result, INPUT_LIMIT + 1);
    CHECK(wcsstr(result, L"Drop rejected") != NULL);
    wmemset(bad, L'y', MAX_PATH + 18); bad[MAX_PATH + 18] = bad[MAX_PATH + 19] = 0;
    mixed = (WCHAR *)malloc((first + MAX_PATH + 20) * sizeof(WCHAR));
    CHECK(mixed != NULL);
    if (mixed) {
        memcpy(mixed, batch, first * sizeof(WCHAR)); memcpy(mixed + first, bad, sizeof(bad));
        SetWindowTextW(input, L"Unchanged draft");
        drop(mixed, (first + MAX_PATH + 20) * sizeof(WCHAR));
        GetWindowTextW(input, result, INPUT_LIMIT + 1);
        CHECK(!wcscmp(result, L"Unchanged draft")); /* Reject the whole mixed batch. */
        free(mixed);
    }
    busy = 1; SetWindowTextW(input, L"Next question");
    drop(paths, sizeof(paths));
    GetWindowTextW(input, result, INPUT_LIMIT + 1);
    CHECK(wcsstr(result, L"Next question\r\n") == result && pending_request == NULL);
    /* Verify that the native tree really routes right-clicks to WM_CONTEXTMENU. */
    busy = 0; rebuilding = 1;
    chat = cJSON_Parse("{\"id\":\"test-chat\",\"project\":\"test-project\"}");
    node = insert_item(TVI_ROOT, "Context menu target", chat);
    CHECK(node && TreeView_GetItemRect(tree, node, &rect, TRUE));
    point = MAKELPARAM(rect.left + 4, (rect.top + rect.bottom) / 2);
    SendMessageW(tree, WM_RBUTTONUP, 0, point);
    CHECK(context_events == 1 && context_target == tree);
    rebuilding = 0;
    DestroyWindow(window); cJSON_Delete(catalog); cJSON_Delete(session);
    cJSON_Delete(chat);
    printf("desktop_tests: %d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
