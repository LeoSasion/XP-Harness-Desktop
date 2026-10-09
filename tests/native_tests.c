/* Offline regression tests for the upstream code used by AgentCore.
   This translation unit uses the real implementation, not a duplicate parser.
   No test issues an HTTP request or runs a shell command. */
#define main upstream_cli_main
#include "../src/main.c"
#undef main
#include "../src/ipc.h"

static int checks;
static int failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    } \
} while (0)

static void fixture(struct conversation_t *convo)
{
    memset(convo, 0, sizeof(*convo));
    convo->root = cJSON_CreateObject();
    convo->messages = cJSON_CreateArray();
    cJSON_AddItemToObject(convo->root, "messages", convo->messages);
    convo_add_text_message(convo, "system", "Offline regression test.");
}

static void test_text_and_optional_usage(void)
{
    static const char *responses[] = {
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"hello\"}}]}",
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"hello\"}}],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3}}",
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"hello\"}}],\"usage\":{}}",
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"hello\"}}],\"usage\":{\"prompt_tokens_details\":{}}}"
    };
    struct conversation_t convo;
    unsigned int i;
    int status;
    for (i = 0; i < sizeof(responses) / sizeof(responses[0]); ++i) {
        fixture(&convo);
        status = convo_add_response(&convo, responses[i]);
        CHECK(status == 0);
        CHECK(cJSON_GetArraySize(convo.messages) == 2);
        CHECK(convo.content != NULL);
        if (convo.content) CHECK(strcmp(convo.content, "hello") == 0);
        CHECK(convo.cost == 0.0);
        if (i == 1) {
            CHECK(convo.tokens_in == 12);
            CHECK(convo.tokens_out == 3);
            CHECK(convo.tokens_ctx == 15);
        }
        convo_cleanup(&convo);
    }
}

static void test_usage_accumulates(void)
{
    const char *response =
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":\"ok\"}}],"
        "\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3,\"cost\":0.25,\"prompt_tokens_details\":{\"cached_tokens\":8}}}";
    struct conversation_t convo;
    fixture(&convo);
    CHECK(convo_add_response(&convo, response) == 0);
    CHECK(convo_add_response(&convo, response) == 0);
    CHECK(convo.tokens_in == 24);
    CHECK(convo.tokens_out == 6);
    CHECK(convo.tokens_ctx == 15);
    CHECK(convo.tokens_cached == 16);
    CHECK(convo.cost == 0.5);
    CHECK(convo.requests == 2);
    convo_cleanup(&convo);
}

static void test_malformed_responses(void)
{
    static const char *responses[] = {
        "", "not-json", "null", "{}", "{\"choices\":[]}",
        "{\"error\":{\"message\":\"test error\"}}",
        "{\"error\":{\"message\":42}}",
        "{\"choices\":[{}]}",
        "{\"choices\":[{\"finish_reason\":\"stop\"}]}",
        "{\"choices\":[{\"finish_reason\":17,\"message\":{\"role\":\"assistant\",\"content\":\"text\"}}]}",
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":\"text\"}}]}",
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\"}}]}",
        "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\",\"content\":42}}]}"
    };
    struct conversation_t convo;
    unsigned int i;
    for (i = 0; i < sizeof(responses) / sizeof(responses[0]); ++i) {
        fixture(&convo);
        CHECK(convo_add_response(&convo, responses[i]) == -1);
        CHECK(cJSON_GetArraySize(convo.messages) == 1);
        CHECK(convo.pending_tool_msg == NULL);
        convo_cleanup(&convo);
    }
}

static void test_tool_response_is_preserved_without_execution(void)
{
    const char *response =
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\",\"content\":null,"
        "\"tool_calls\":[{\"id\":\"call_test\",\"type\":\"function\",\"function\":{\"name\":\"cmd\",\"arguments\":\"{}\"}}]}}]}";
    struct conversation_t convo;
    fixture(&convo);
    CHECK(convo_add_response(&convo, response) == 1);
    CHECK(cJSON_GetArraySize(convo.messages) == 2);
    CHECK(convo.pending_tool_msg != NULL);
    /* Deliberately do not call convo_handle_tool_calls here. */
    convo_cleanup(&convo);
}

static void test_invalid_tool_dispatch(void)
{
    static const char *arguments[] = {"not-json", "null", "[]", "{}", "{\"command\":42}"};
    struct config_t config;
    unsigned int i;
    char *result;
    memset(&config, 0, sizeof(config));
    result = tool_dispatch(&config, "not_a_registered_tool", "{}");
    CHECK(result != NULL);
    if (result) {
        CHECK(result[0] != '\0');
        free(result);
    }
    for (i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        result = tool_dispatch(&config, "cmd", arguments[i]);
        CHECK(result != NULL);
        if (result) {
            CHECK(result[0] != '\0');
            free(result);
        }
    }
}

static int pipe_pair(HANDLE *server, HANDLE *client)
{
    static unsigned int serial;
    char name[120];
    *client = INVALID_HANDLE_VALUE;
    sprintf(name, "\\\\.\\pipe\\vc6agent-tests-%lu-%u", GetCurrentProcessId(), ++serial);
    *server = ipc_create_server(name);
    if (*server == INVALID_HANDLE_VALUE) return 0;
    *client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (*client == INVALID_HANDLE_VALUE || !ipc_connect(*server, NULL, 1000)) {
        if (*client != INVALID_HANDLE_VALUE) CloseHandle(*client);
        CloseHandle(*server);
        *server = *client = INVALID_HANDLE_VALUE;
        return 0;
    }
    return 1;
}

static int raw_write(HANDLE pipe, const unsigned char *bytes, DWORD size)
{
    OVERLAPPED ov;
    DWORD count = 0;
    BOOL ok;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    ok = WriteFile(pipe, bytes, size, &count, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(pipe, &ov, &count, FALSE);
        else {
            CancelIo(pipe);
            GetOverlappedResult(pipe, &ov, &count, TRUE);
        }
    }
    CloseHandle(ov.hEvent);
    return ok && count == size;
}

static void test_ipc_round_trip_and_cancel(void)
{
    HANDLE server, client, cancel;
    char *json = NULL;
    int connected = pipe_pair(&server, &client);
    CHECK(connected);
    if (!connected) return;
    CHECK(ipc_write(client, "{\"text\":\"line1\\nline2\"}", NULL, 1000));
    CHECK(ipc_write(client, "{\"type\":\"new\"}", NULL, 1000));
    CHECK(ipc_read(server, &json, NULL, 1000));
    CHECK(json != NULL);
    if (json) CHECK(strcmp(json, "{\"text\":\"line1\\nline2\"}") == 0);
    free(json); json = NULL;
    CHECK(ipc_read(server, &json, NULL, 1000));
    if (json) CHECK(strcmp(json, "{\"type\":\"new\"}") == 0);
    free(json); json = NULL;
    CHECK(!ipc_read(server, &json, NULL, 20));
    CHECK(json == NULL);
    cancel = CreateEventA(NULL, TRUE, TRUE, NULL);
    CHECK(cancel != NULL);
    if (cancel) {
        CHECK(!ipc_read(server, &json, cancel, 1000));
        CHECK(json == NULL);
        CloseHandle(cancel);
    }
    /* A timed-out read must release its OVERLAPPED before the next request. */
    CHECK(ipc_write(client, "{}", NULL, 1000));
    CHECK(ipc_read(server, &json, NULL, 1000));
    if (json) CHECK(strcmp(json, "{}") == 0);
    free(json);
    CloseHandle(client);
    CloseHandle(server);
}

static void test_ipc_rejects_bad_frames(void)
{
    static const unsigned char packets[][7] = {
        {0, 0, 0, 0, 0, 0, 0},       /* zero-length frame */
        {1, 0, 4, 0, 0, 0, 0},       /* 256 KiB + 1 */
        {3, 0, 0, 0, '{', 0, '}'},   /* embedded NUL */
        {3, 0, 0, 0, '{', '}', 0}    /* one byte missing */
    };
    static const DWORD sizes[] = {4, 4, 7, 6};
    HANDLE server, client;
    char *json;
    unsigned int i;
    int connected;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        connected = pipe_pair(&server, &client);
        CHECK(connected);
        if (!connected) continue;
        CHECK(raw_write(client, packets[i], sizes[i]));
        json = NULL;
        CHECK(!ipc_read(server, &json, NULL, 30));
        CHECK(json == NULL);
        free(json);
        CloseHandle(client);
        CloseHandle(server);
    }
    CHECK(!ipc_write(INVALID_HANDLE_VALUE, "", NULL, 1000));
}

int main(void)
{
    term_init();
    test_text_and_optional_usage();
    test_usage_accumulates();
    test_malformed_responses();
    test_tool_response_is_preserved_without_execution();
    test_invalid_tool_dispatch();
    test_ipc_round_trip_and_cancel();
    test_ipc_rejects_bad_frames();
    term_reset();
    printf("native_tests: %d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
