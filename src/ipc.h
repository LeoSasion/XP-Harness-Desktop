#ifndef VC6AI_IPC_H
#define VC6AI_IPC_H
#include <windows.h>
#define IPC_MAX_FRAME (256UL * 1024UL)
/* All handles use FILE_FLAG_OVERLAPPED. Read allocates; caller frees. */
int ipc_read(HANDLE pipe, char **json, HANDLE cancel, DWORD timeout_ms);
int ipc_write(HANDLE pipe, const char *json, HANDLE cancel, DWORD timeout_ms);
HANDLE ipc_create_server(const char *name);
int ipc_connect(HANDLE pipe, HANDLE cancel, DWORD timeout_ms);
#endif
