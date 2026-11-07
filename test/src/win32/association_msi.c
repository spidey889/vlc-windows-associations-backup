/*****************************************************************************
 * Harmless worker for the opt-in MSI association fixture; no VLC/backend calls.
 * Copyright (C) 2026 VideoLAN and its authors
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *****************************************************************************/
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    wchar_t path[MAX_PATH], name[128];
    const wchar_t *prefix = L"--win32-set-default-associations-once=";
    if (argc != 2 || wcsncmp(argv[1], prefix, wcslen(prefix)))
        return 2;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !wcsrchr(path, L'\\'))
        return 2;
    wcscpy(wcsrchr(path, L'\\') + 1, L"invocations.txt");
    FILE *log = _wfopen(path, L"a");
    if (!log) return 2;
    fwprintf(log, L"%ls\n", argv[1]);
    fclose(log);
    if (!GetEnvironmentVariableW(L"VLC_MSI_TEST_STARTED", name, 128)) return 2;
    HANDLE started = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!GetEnvironmentVariableW(L"VLC_MSI_TEST_GATE", name, 128)) return 2;
    HANDLE gate = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!started || !gate) return 2;
    SetEvent(started);
    DWORD result = WaitForSingleObject(gate, 10000);
    CloseHandle(started);
    CloseHandle(gate);
    wcscpy(wcsrchr(path, L'\\') + 1, L"completed.txt");
    log = _wfopen(path, L"w");
    if (!log) return 2;
    fprintf(log, "%lu\n", (unsigned long)result);
    fclose(log);
    return 4; /* Association failure must remain nonfatal and must not retry. */
}
