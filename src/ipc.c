#define _WIN32_WINNT 0x0501
#include "ipc.h"
#include <stdlib.h>
#include <string.h>
#ifndef FILE_FLAG_FIRST_PIPE_INSTANCE
#define FILE_FLAG_FIRST_PIPE_INSTANCE 0x00080000
#endif

static DWORD remaining(DWORD start, DWORD timeout) {
    DWORD elapsed = GetTickCount() - start;
    return timeout == INFINITE ? INFINITE : (elapsed >= timeout ? 0 : timeout - elapsed);
}

static int pending(HANDLE pipe, OVERLAPPED *ov, HANDLE cancel, DWORD timeout, DWORD *count) {
    HANDLE waits[2];
    DWORD result;
    waits[0] = ov->hEvent; waits[1] = cancel;
    result = WaitForMultipleObjects(cancel ? 2 : 1, waits, FALSE, timeout);
    if (result != WAIT_OBJECT_0) {
        /* XP CancelIo cancels this thread's I/O; drain before releasing OVERLAPPED. */
        CancelIo(pipe);
        GetOverlappedResult(pipe, ov, count, TRUE);
        return 0;
    }
    return GetOverlappedResult(pipe, ov, count, FALSE) != 0;
}

static int transfer(HANDLE pipe, void *data, DWORD size, int writing, HANDLE cancel,
                    DWORD start, DWORD timeout) {
    OVERLAPPED ov;
    DWORD count, error;
    BOOL ok;
    int result = 1;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    while (size) {
        if ((cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) || !remaining(start, timeout)) {
            result = 0; break;
        }
        ResetEvent(ov.hEvent);
        count = 0;
        ok = writing ? WriteFile(pipe, data, size, &count, &ov) : ReadFile(pipe, data, size, &count, &ov);
        error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && (error != ERROR_IO_PENDING || !pending(pipe, &ov, cancel, remaining(start, timeout), &count))) {
            result = 0; break;
        }
        if (!count) { result = 0; break; }
        data = (char *)data + count; size -= count;
    }
    CloseHandle(ov.hEvent);
    return result;
}

int ipc_read(HANDLE pipe, char **json, HANDLE cancel, DWORD timeout_ms) {
    unsigned char header[4];
    DWORD size, start = GetTickCount();
    *json = NULL;
    if (!transfer(pipe, header, 4, 0, cancel, start, timeout_ms)) return 0;
    size = (DWORD)header[0] | ((DWORD)header[1] << 8) | ((DWORD)header[2] << 16) | ((DWORD)header[3] << 24);
    if (!size || size > IPC_MAX_FRAME) return 0;
    *json = (char *)malloc(size + 1);
    if (!*json) return 0;
    if (!transfer(pipe, *json, size, 0, cancel, start, timeout_ms) || memchr(*json, 0, size)) {
        free(*json); *json = NULL; return 0;
    }
    (*json)[size] = 0;
    return 1;
}

int ipc_write(HANDLE pipe, const char *json, HANDLE cancel, DWORD timeout_ms) {
    unsigned char header[4];
    size_t size = strlen(json);
    DWORD start = GetTickCount();
    if (!size || size > IPC_MAX_FRAME) return 0;
    header[0] = (unsigned char)size; header[1] = (unsigned char)(size >> 8);
    header[2] = (unsigned char)(size >> 16); header[3] = (unsigned char)(size >> 24);
    return transfer(pipe, header, 4, 1, cancel, start, timeout_ms) &&
           transfer(pipe, (void *)json, (DWORD)size, 1, cancel, start, timeout_ms);
}

HANDLE ipc_create_server(const char *name) {
    HANDLE token = NULL, pipe = INVALID_HANDLE_VALUE;
    TOKEN_GROUPS *groups = NULL;
    PSID logon = NULL, network = NULL;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PACL acl = NULL;
    SECURITY_DESCRIPTOR sd;
    SECURITY_ATTRIBUTES sa;
    DWORD size = 0, i;
    if (strncmp(name, "\\\\.\\pipe\\vc6agent-", 18) || strlen(name) > 240) return pipe;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto cleanup;
    GetTokenInformation(token, TokenGroups, NULL, 0, &size);
    groups = (TOKEN_GROUPS *)malloc(size);
    if (!groups || !GetTokenInformation(token, TokenGroups, groups, size, &size)) goto cleanup;
    for (i = 0; i < groups->GroupCount; ++i)
        if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) logon = groups->Groups[i].Sid;
    if (!logon || !AllocateAndInitializeSid(&authority, 1, SECURITY_NETWORK_RID, 0, 0, 0, 0, 0, 0, 0, &network)) goto cleanup;
    size = sizeof(ACL) + sizeof(ACCESS_DENIED_ACE) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(logon) + GetLengthSid(network);
    acl = (PACL)malloc(size);
    if (!acl || !InitializeAcl(acl, size, ACL_REVISION) ||
        !AddAccessDeniedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, network) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, logon) ||
        !InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE)) goto cleanup;
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = &sd; sa.bInheritHandle = FALSE;
    pipe = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 8192, 8192, 0, &sa);
cleanup:
    free(acl); free(groups);
    if (network) FreeSid(network);
    if (token) CloseHandle(token);
    return pipe;
}

int ipc_connect(HANDLE pipe, HANDLE cancel, DWORD timeout_ms) {
    OVERLAPPED ov;
    BOOL ok;
    DWORD error, count = 0;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    ok = ConnectNamedPipe(pipe, &ov);
    error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok) ok = error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING && pending(pipe, &ov, cancel, timeout_ms, &count));
    CloseHandle(ov.hEvent);
    return ok != 0;
}
