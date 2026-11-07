/*****************************************************************************
 * Early association command contract and context regressions (no registry I/O)
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *****************************************************************************/

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <shellapi.h>
#include "win32_association_command.h"
#include "win32/associations/association_batch.hpp"
#include "win32/associations/win32_associations.hpp"

#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace
{
struct Environment
{
    bool noShell = false, ownElevated = false, shellElevated = false;
    bool wrongUser = false, denyOwnToken = false, denyShell = false, denyShellToken = false;
    bool denyInformation = false, malformedUser = false, restartShell = false, noShellPid = false;
    DWORD ownIntegrity = SECURITY_MANDATORY_MEDIUM_RID;
    DWORD shellIntegrity = SECURITY_MANDATORY_MEDIUM_RID;
    DWORD ownSession = 4, shellSession = 4;
    bool failEnvironment = false, failAttributes = false, failParent = false, failExecutable = false;
    bool failCreate = false, failExitRead = false, changeBeforeLaunch = false, failSecurity = false;
    bool needDebug = false, denyPrivilege = false, failPrivilegeRestore = false;
    bool initiallyDebugEnabled = false;
    DWORD waitResult = WAIT_OBJECT_0, workerExit = 0;
} environment;
unsigned shellQueries = 0, handles = 0, checks = 0, onceCalls = 0;
bool onceCompleted = false, onceFailure = false;
bool debugEnabled = false;
unsigned privilegeEnables = 0, privilegeRestores = 0;
unsigned environments = 0, attributes = 0, launches = 0, waits = 0, exitReads = 0;
HANDLE parentValue = nullptr;
bool launchContract = false, waitedForCompletion = false;
DWORD requestedWait = INFINITE;
std::vector<std::wstring> launchedArguments;
PSID ownSid, otherSid, mediumSid, highSid, lowSid, systemSid;
std::vector<std::wstring> writes;
std::vector<vlc::win32::AssociationResult> results;

HANDLE ownToken() { return reinterpret_cast<HANDLE>(1); }
HANDLE shellToken() { return reinterpret_cast<HANDLE>(2); }
HANDLE shellProcess() { return reinterpret_cast<HANDLE>(3); }
HWND shellWindow() { return reinterpret_cast<HWND>(4); }

void require(bool condition, const char *message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

BOOL WINAPI commandCloseHandle(HANDLE)
{
    --handles;
    return TRUE;
}

HWND WINAPI commandGetShellWindow()
{
    ++shellQueries;
    return environment.noShell || (environment.restartShell && shellQueries > 1) ||
+           (environment.changeBeforeLaunch && shellQueries > 2)
        ? nullptr : shellWindow();
}

DWORD WINAPI commandGetWindowThreadProcessId(HWND, LPDWORD pid)
{
    *pid = environment.noShellPid ? 0 : 777;
    return *pid ? 888 : 0;
}

HANDLE WINAPI commandOpenProcess(DWORD access, BOOL inherit, DWORD pid)
{
    if (environment.denyShell || (environment.needDebug && !debugEnabled) || access != (PROCESS_QUERY_LIMITED_INFORMATION |
+        ((environment.ownElevated && environment.ownIntegrity == SECURITY_MANDATORY_HIGH_RID) ? PROCESS_CREATE_PROCESS : 0)) || inherit || pid != 777)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    ++handles;
    return shellProcess();
}

BOOL WINAPI commandOpenProcessToken(HANDLE process, DWORD access, PHANDLE token)
{
    const bool own = process == GetCurrentProcess();
    if ((access != (TOKEN_QUERY | ((!own && environment.ownElevated &&
        environment.ownIntegrity == SECURITY_MANDATORY_HIGH_RID) ? TOKEN_DUPLICATE : 0)) ||
        (own ? environment.denyOwnToken : environment.denyShellToken)) &&
        !(own && access == (TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES) && !environment.denyOwnToken))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    *token = own ? ownToken() : shellToken();
    ++handles;
    return TRUE;
}

BOOL WINAPI commandLookupPrivilegeValueW(LPCWSTR system, LPCWSTR name, PLUID luid)
{
    if (system || std::wstring(name) != L"SeDebugPrivilege") return FALSE;
    *luid = LUID{17, 0};
    return TRUE;
}

BOOL WINAPI commandAdjustTokenPrivileges(HANDLE token, BOOL all, PTOKEN_PRIVILEGES requested,
    DWORD size, PTOKEN_PRIVILEGES previous, PDWORD needed)
{
    require(token == ownToken() && !all && requested && requested->PrivilegeCount == 1 &&
            requested->Privileges[0].Luid.LowPart == 17, "Only the broker's debug privilege may be adjusted");
    if (previous)
    {
        ++privilegeEnables;
        require(size == sizeof(TOKEN_PRIVILEGES) && needed, "Save the broker's prior privilege state");
        *previous = *requested;
        previous->Privileges[0].Attributes = debugEnabled ? SE_PRIVILEGE_ENABLED : 0;
        previous->PrivilegeCount = debugEnabled ? 0 : 1;
        *needed = sizeof(TOKEN_PRIVILEGES);
        SetLastError(environment.denyPrivilege ? ERROR_NOT_ALL_ASSIGNED : ERROR_SUCCESS);
        if (!environment.denyPrivilege) debugEnabled = true;
        return TRUE;
    }
    ++privilegeRestores;
    SetLastError(environment.failPrivilegeRestore ? ERROR_ACCESS_DENIED : ERROR_SUCCESS);
    if (environment.failPrivilegeRestore) return FALSE;
    debugEnabled = (requested->Privileges[0].Attributes & SE_PRIVILEGE_ENABLED) != 0;
    return TRUE;
}

template <typename T>
BOOL copyTokenValue(const T &value, LPVOID data, DWORD size, PDWORD needed)
{
    *needed = sizeof(value);
    if (!data || size < sizeof(value))
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    std::memcpy(data, &value, sizeof(value));
    return TRUE;
}

BOOL WINAPI commandGetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS kind,
                                       LPVOID data, DWORD size, PDWORD needed)
{
    if (environment.denyInformation)
    {
        *needed = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const bool own = token == ownToken();
    switch (kind)
    {
        case TokenElevation:
        {
            const TOKEN_ELEVATION elevation{own ? environment.ownElevated : environment.shellElevated};
            return copyTokenValue(elevation, data, size, needed);
        }
        case TokenSessionId:
            return copyTokenValue(own ? environment.ownSession : environment.shellSession, data, size, needed);
        case TokenIntegrityLevel:
        {
            const DWORD level = own ? environment.ownIntegrity : environment.shellIntegrity;
            TOKEN_MANDATORY_LABEL label{};
            label.Label.Sid = level == SECURITY_MANDATORY_MEDIUM_RID ? mediumSid
                : level == SECURITY_MANDATORY_HIGH_RID ? highSid : lowSid;
            return copyTokenValue(label, data, size, needed);
        }
        case TokenUser:
        {
            if (environment.malformedUser)
                return copyTokenValue(DWORD{0}, data, size, needed);
            TOKEN_USER user{};
            user.User.Sid = own || !environment.wrongUser ? ownSid : otherSid;
            return copyTokenValue(user, data, size, needed);
        }
        default:
            return FALSE;
    }
}
BOOL WINAPI commandCreateEnvironmentBlock(LPVOID *output, HANDLE token, BOOL inherit)
{
    if (environment.failEnvironment || token != shellToken() || inherit) return FALSE;
    static wchar_t ordinary[] = L"VLC_HANDOFF_TEST=ordinary\0\0";
    *output = ordinary;
    ++environments;
    return TRUE;
}
BOOL WINAPI commandDestroyEnvironmentBlock(LPVOID) { --environments; return TRUE; }

BOOL WINAPI commandInitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST list,
                                                    DWORD count, DWORD flags, PSIZE_T size)
{
    if (environment.failAttributes || count != 1 || flags) { *size = 0; return FALSE; }
    if (!list) { *size = 96; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    ++attributes;
    return TRUE;
}
void WINAPI commandDeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST) { --attributes; }
BOOL WINAPI commandUpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD flags,
    DWORD_PTR key, PVOID value, SIZE_T size, PVOID, PSIZE_T)
{
    if (environment.failParent || flags || key != PROC_THREAD_ATTRIBUTE_PARENT_PROCESS || size != sizeof(HANDLE))
        return FALSE;
    parentValue = *static_cast<HANDLE *>(value);
    return parentValue == shellProcess();
}

DWORD WINAPI commandGetModuleFileNameW(HMODULE, LPWSTR output, DWORD size)
{
    if (environment.failExecutable) return size;
    const std::wstring path = L"C:\\Program Files\\VideoLAN\\\u03a9 VLC\\vlc.exe";
    if (path.size() + 1 > size) return size;
    std::memcpy(output, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    return static_cast<DWORD>(path.size());
}

BOOL WINAPI commandConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR text,
    DWORD revision, PSECURITY_DESCRIPTOR *descriptor, PULONG size)
{
    if (environment.failSecurity) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(text, revision, descriptor, size);
}

bool workerAccess(LPSECURITY_ATTRIBUTES access)
{
    if (!access || access->nLength != sizeof(SECURITY_ATTRIBUTES) || access->bInheritHandle)
        return false;
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!IsValidSecurityDescriptor(access->lpSecurityDescriptor) ||
        !GetSecurityDescriptorDacl(access->lpSecurityDescriptor, &present, &acl, &defaulted) ||
        !present || !acl || acl->AceCount != 2)
        return false;
    // Inspect the real SDDL result, not a mocked descriptor: only SYSTEM and the
    // shell user receive access. A different administrator must not replace it.
    const PSID user = environment.wrongUser ? otherSid : ownSid;
    for (DWORD i = 0; i < 2; ++i)
    {
        void *raw = nullptr;
        if (!GetAce(acl, i, &raw)) return false;
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Mask != GENERIC_ALL ||
            !EqualSid(const_cast<DWORD *>(&ace->SidStart), i == 0 ? systemSid : user))
            return false;
    }
    return true;
}

BOOL WINAPI commandCreateProcessW(LPCWSTR app, LPWSTR command, LPSECURITY_ATTRIBUTES processAccess,
    LPSECURITY_ATTRIBUTES threadAccess, BOOL inherit, DWORD flags, LPVOID block, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
{
    ++launches;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(command, &argc);
    launchedArguments.clear();
    if (argv)
    {
        for (int i = 0; i < argc; ++i) launchedArguments.emplace_back(argv[i]);
        LocalFree(argv);
    }
    const std::wstring executable = L"C:\\Program Files\\VideoLAN\\\u03a9 VLC\\vlc.exe";
    const std::wstring target = environment.wrongUser ? L"S-1-5-21-1-2-3-1001:4" : L"S-1-5-21-1-2-3-1000:4";
    launchContract = (debugEnabled == environment.initiallyDebugEnabled) &&
        workerAccess(processAccess) && workerAccess(threadAccess) &&
        !inherit && flags == (CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT) &&
        block && environments == 1 && attributes == 1 && parentValue && parentValue == shellProcess() &&
        startup->cb == sizeof(STARTUPINFOEXW) &&
        reinterpret_cast<STARTUPINFOEXW *>(startup)->lpAttributeList &&
        std::wstring(app) == executable && std::wstring(directory) == L"C:\\Program Files\\VideoLAN\\\u03a9 VLC\\" &&
        launchedArguments.size() == 3 && launchedArguments[0] == executable &&
        (launchedArguments[1] == L"--win32-association-worker=" + target ||
         launchedArguments[1] == L"--win32-association-once-worker=" + target);
    if (environment.failCreate || !launchContract) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    process->hProcess = reinterpret_cast<HANDLE>(5);
    process->hThread = reinterpret_cast<HANDLE>(6);
    process->dwProcessId = 1234;
    process->dwThreadId = 5678;
    handles += 2;
    return TRUE;
}
DWORD WINAPI commandWaitForSingleObject(HANDLE process, DWORD timeout)
{
    ++waits;
    requestedWait = timeout;
    waitedForCompletion = process == reinterpret_cast<HANDLE>(5) && timeout == 5 * 60 * 1000;
    SetLastError(ERROR_ACCESS_DENIED);
    return environment.waitResult;
}
BOOL WINAPI commandGetExitCodeProcess(HANDLE, LPDWORD code)
{
    ++exitReads;
    if (environment.failExitRead) return FALSE;
    *code = environment.workerExit;
    return TRUE;
}

}

// Exercise the production guard with deterministic Windows token/shell responses.
// There is no real token manipulation, process launch, elevation or registry call.
#define CloseHandle commandCloseHandle
#define GetShellWindow commandGetShellWindow
#define GetWindowThreadProcessId commandGetWindowThreadProcessId
#define OpenProcess commandOpenProcess
#define OpenProcessToken commandOpenProcessToken
#define GetTokenInformation commandGetTokenInformation
#define LookupPrivilegeValueW commandLookupPrivilegeValueW
#define AdjustTokenPrivileges commandAdjustTokenPrivileges
#define CreateEnvironmentBlock commandCreateEnvironmentBlock
#define DestroyEnvironmentBlock commandDestroyEnvironmentBlock
#define InitializeProcThreadAttributeList commandInitializeProcThreadAttributeList
#define DeleteProcThreadAttributeList commandDeleteProcThreadAttributeList
#define UpdateProcThreadAttribute commandUpdateProcThreadAttribute
#define GetModuleFileNameW commandGetModuleFileNameW
#define CreateProcessW commandCreateProcessW
#define ConvertStringSecurityDescriptorToSecurityDescriptorW commandConvertStringSecurityDescriptorToSecurityDescriptorW
#define WaitForSingleObject commandWaitForSingleObject
#define GetExitCodeProcess commandGetExitCodeProcess
#include "../../../bin/win32_association_context.cpp"
#undef CreateEnvironmentBlock
#undef DestroyEnvironmentBlock
#undef InitializeProcThreadAttributeList
#undef DeleteProcThreadAttributeList
#undef UpdateProcThreadAttribute
#undef GetModuleFileNameW
#undef CreateProcessW
#undef ConvertStringSecurityDescriptorToSecurityDescriptorW
#undef WaitForSingleObject
#undef GetExitCodeProcess

#undef GetTokenInformation
#undef LookupPrivilegeValueW
#undef AdjustTokenPrivileges
#undef OpenProcessToken
#undef OpenProcess
#undef GetWindowThreadProcessId
#undef GetShellWindow
#undef CloseHandle

namespace vlc::win32
{
AssociationBatchResult setInstallerDefaultAssociations(const std::vector<std::wstring> &extensions)
{
    ++onceCalls;
    if (onceFailure) throw std::runtime_error("Completion storage unavailable");
    if (!onceCompleted) return setDefaultAssociations(extensions);
    AssociationBatchResult batch;
    batch.alreadyCompleted = true;
    return batch;
}

AssociationResult setDefaultAssociation(std::wstring_view extension) noexcept
{
    const size_t index = writes.size();
    writes.emplace_back(extension);
    return index < results.size() ? results[index]
        : AssociationResult{AssociationStatus::failed, "Unexpected writer call"};
}
}

namespace
{
int invoke(std::vector<std::wstring> arguments)
{
    require(handles == 0, "A prior invocation leaked a token/process handle");
    writes.clear();
    onceCalls = 0;
    shellQueries = 0;
    launches = waits = exitReads = 0;
    privilegeEnables = privilegeRestores = 0;
    launchedArguments.clear();
    parentValue = nullptr;
    launchContract = waitedForCompletion = false;
    std::vector<wchar_t *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    const int code = vlc_win32_association_command(static_cast<int>(argv.size()), argv.data());
    require(handles == 0, "Context rejection/success leaked a token/process handle");
    require(environments == 0 && attributes == 0, "Handoff setup leaked environment/attribute state");
    return code;
}

PSID sid(const wchar_t *text)
{
    PSID result = nullptr;
    require(ConvertStringSidToSidW(text, &result), "Cannot initialize an in-memory fixture SID");
    return result;
}
}

int main()
{
    using namespace vlc::win32;
    try
    {
        ownSid = sid(L"S-1-5-21-1-2-3-1000");
        otherSid = sid(L"S-1-5-21-1-2-3-1001");
        mediumSid = sid(L"S-1-16-8192");
        highSid = sid(L"S-1-16-12288");
        lowSid = sid(L"S-1-16-4096");
        systemSid = sid(L"S-1-5-18");
        const std::wstring option = L"--win32-set-default-associations";
        require(invoke({L"vlc.exe"}) == -1 && shellQueries == 0 && writes.empty(),
                "Normal startup must bypass association context/backend work");
        require(invoke({L"vlc.exe", L"song.mp3"}) == -1 && shellQueries == 0,
                "Media arguments must retain normal startup");

        const std::vector<std::vector<std::wstring>> invalid = {
            {L"vlc.exe", option}, {L"vlc.exe", option + L"="},
            {L"vlc.exe", option + L"=,"}, {L"vlc.exe", option + L"=.mp3,"},
            {L"vlc.exe", option + L"=,.mp3"}, {L"vlc.exe", option + L"=.mp3,,.wav"},
            {L"vlc.exe", option + L"=.mp3,bad"}, {L"vlc.exe", option + L"=.mp3,."},
            {L"vlc.exe", option + L"=.mp3,.wav/evil"},
            {L"vlc.exe", option + L"=.mp3,.wav\\evil"},
            {L"vlc.exe", option + L"=.mp3,.wav evil"},
            {L"vlc.exe", option + L"=.mp3,.\u03a9"},
            {L"vlc.exe", option + L"=." + std::wstring(64, L'a')},
            {L"vlc.exe", option + L"=.mp3", L"movie.mkv"},
            {L"vlc.exe", L"--intf=dummy", option + L"=.mp3"},
            {L"vlc.exe", option + L"=.mp3", option + L"=.wav"},
            {L"vlc.exe", option, L".mp3"},
        };
        for (const auto &arguments : invalid)
            require(invoke(arguments) == 2 && writes.empty() && shellQueries == 0,
                    "Validate the entire exclusive request before context/writer calls");

        results = {{AssociationStatus::changed, {}}, {AssociationStatus::unchanged, {}}};
        require(invoke({L"vlc.exe", option + L"=.MP3,.x+y_z-2"}) == 0 &&
                writes == std::vector<std::wstring>{L".MP3", L".x+y_z-2"},
                "Use the shared extension rules and preserve exact requested order");
        require(invoke({L"vlc.exe", option + L"=.mp3,.mp3"}) == 0 && writes.size() == 2,
                "The caller's duplicates retain existing batch semantics");

        const std::vector<std::function<void()>> unsafe = {
            [] { environment.noShell = true; }, [] { environment.noShellPid = true; },
            [] { environment.ownElevated = true; }, [] { environment.shellElevated = true; },
            [] { environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID; },
            [] { environment.shellIntegrity = SECURITY_MANDATORY_HIGH_RID; },
            [] { environment.ownIntegrity = SECURITY_MANDATORY_LOW_RID; },
            [] { environment.shellIntegrity = SECURITY_MANDATORY_LOW_RID; },
            [] { environment.wrongUser = true; }, [] { environment.ownSession = 5; },
            [] { environment.ownSession = 0; }, [] { environment.denyOwnToken = true; },
            [] { environment.denyShell = true; }, [] { environment.denyShellToken = true; },
            [] { environment.denyInformation = true; }, [] { environment.malformedUser = true; },
            [] { environment.restartShell = true; },
        };
        for (const auto &configure : unsafe)
        {
            environment = {};
            configure();
            require(invoke({L"vlc.exe", option + L"=.mp3"}) == 3 && writes.empty(),
                    "An unsafe/unknown user context must never reach the writer");
        }
        environment = {};
        results = {{AssociationStatus::needsUserChoice, "Manual confirmation"}};
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 1 && writes.size() == 1,
                "Confirmation is an incomplete result, not successful setup");
        results = {{AssociationStatus::failed, "Busy is just text, not a typed timeout"}};
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 1,
                "Never derive a busy result by parsing diagnostic text");
        results = {{AssociationStatus::changed, {}}, {AssociationStatus::failed, "Failed"}};
        require(invoke({L"vlc.exe", option + L"=.mp3,.wav"}) == 1 && writes.size() == 2,
                "Partial work must not be reported as complete success");
        results = {{AssociationStatus::failed, "Earlier failure"}, {AssociationStatus::failed, "Timeout", true}};
        require(invoke({L"vlc.exe", option + L"=.mp3,.wav,.flac"}) == 4 && writes.size() == 2,
                "Typed busy takes precedence and suppresses subsequent writes");
        results = {{AssociationStatus::failed, "Timeout", true}};
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 4,
                "A busy final entry is still busy with zero skipped operations");
        const std::wstring worker = L"--win32-association-worker=";
        const std::wstring target = worker + L"S-1-5-21-1-2-3-1000:4";
        const std::vector<std::vector<std::wstring>> invalidWorkers = {
            {L"vlc.exe", worker}, {L"vlc.exe", target},
            {L"vlc.exe", worker + L"bad:4", L".mp3"},
            {L"vlc.exe", worker + L"BA:4", L".mp3"},
            {L"vlc.exe", worker + L"S-1-5-21-1-2-3-1000:0", L".mp3"},
            {L"vlc.exe", worker + L"S-1-5-21-1-2-3-1000:4294967296", L".mp3"},
            {L"vlc.exe", worker + L"S-1-5-21-1-2-3-1000:4x", L".mp3"},
            {L"vlc.exe", target, L".mp3,"},
            {L"vlc.exe", target, option + L"=.mp3"},
            {L"vlc.exe", target, L".mp3", target},
        };
        for (const auto &arguments : invalidWorkers)
            require(invoke(arguments) == 2 && writes.empty() && launches == 0,
                    "Malformed private invocations must not fall through or launch workers");

        results = {{AssociationStatus::changed, {}}};
        require(invoke({L"vlc.exe", target, L".mp3"}) == 0 && writes.size() == 1 && launches == 0,
                "A verified ordinary worker executes directly exactly once");
        require(invoke({L"vlc.exe", worker + L"S-1-5-21-1-2-3-1001:4", L".mp3"}) == 3 && writes.empty(),
                "Worker must independently reject the wrong expected SID");
        require(invoke({L"vlc.exe", worker + L"S-1-5-21-1-2-3-1000:5", L".mp3"}) == 3 && writes.empty(),
                "Worker must independently reject the wrong expected session");
        environment.ownElevated = true;
        environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
        require(invoke({L"vlc.exe", target, L".mp3"}) == 3 && writes.empty() && launches == 0,
                "An elevated worker must refuse, never recurse into the broker path");
        environment.wrongUser = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 0 && writes.empty() && launches == 1 && launchContract &&
                launchedArguments[1] == worker + L"S-1-5-21-1-2-3-1001:4",
                "Different-account broker must launch only the verified shell user's worker");
        const auto standardWorker = launchedArguments;
        require(invoke(standardWorker) == 3 && writes.empty() && launches == 0,
                "Administrator cannot execute the standard user's worker request");
        environment = {};
        std::swap(ownSid, otherSid); // Replay the launched request as the real target user.
        require(invoke(standardWorker) == 0 && writes == std::vector<std::wstring>{L".mp3"} && launches == 0,
                "Only the ordinary original user can execute the selected batch");
        std::swap(ownSid, otherSid);
        environment = {};
        environment.ownElevated = true;
        environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
        environment.wrongUser = environment.needDebug = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 0 && launchContract && launches == 1 &&
                writes.empty() && privilegeEnables == 1 && privilegeRestores == 1 && !debugEnabled,
                "Cross-account access must restore the broker privilege before launching the original-user worker");
        environment.denyPrivilege = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 3 && launches == 0 && writes.empty() && !debugEnabled,
                "Unavailable debug privilege must refuse without changing accounts or writing");
        environment.denyPrivilege = false;
        environment.denyShellToken = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 3 && launches == 0 && writes.empty() &&
                privilegeRestores == 1 && !debugEnabled,
                "Failed shell-token acquisition must restore privilege before refusing");
        environment.denyShellToken = false;
        environment.failPrivilegeRestore = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 3 && launches == 0 && writes.empty() &&
                privilegeRestores == 1,
                "Failed privilege restoration must exit without a worker or fallback");
        debugEnabled = false; // That failed broker exits; start a fresh fixture process state.
        environment.failPrivilegeRestore = false;
        environment.initiallyDebugEnabled = debugEnabled = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 0 && launchContract &&
                privilegeEnables == 0 && privilegeRestores == 0 && debugEnabled,
                "An already-enabled broker privilege must remain unchanged");
        debugEnabled = false;
        for (const auto &configure : std::vector<std::function<void()>>{
            [] { environment.noShell = true; }, [] { environment.shellElevated = true; },
            [] { environment.ownSession = 5; }, [] { environment.shellSession = 0; },
            [] { environment.denyShellToken = true; }, [] { environment.restartShell = true; },
            [] { environment.changeBeforeLaunch = true; }})
        {
            environment = {};
            environment.ownElevated = true;
            environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
            environment.wrongUser = true;
            configure();
            require(invoke({L"vlc.exe", option + L"=.mp3"}) == 3 && writes.empty() && launches == 0,
                    "Unverifiable different-account desktop must refuse without fallback");
        }
        environment = {};
        environment.ownElevated = true;
        environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
        for (DWORD code = 0; code <= 4; ++code)
        {
            environment.workerExit = code;
            require(invoke({L"vlc.exe", option + L"=.MP3,.mp3,.x+y_z-2"}) == static_cast<int>(code) &&
                    writes.empty() && launches == 1 && waits == 1 && exitReads == 1 &&
                    waitedForCompletion && launchContract && launchedArguments[2] == L".MP3,.mp3,.x+y_z-2",
                    "Broker must launch once with exact context/payload and forward documented statuses");
        }
        const std::vector<std::function<void()>> launchFailures = {
            [] { environment.failExecutable = true; }, [] { environment.failEnvironment = true; },
            [] { environment.failAttributes = true; }, [] { environment.failParent = true; },
            [] { environment.failCreate = true; }, [] { environment.waitResult = WAIT_FAILED; },
            [] { environment.failSecurity = true; },
            [] { environment.waitResult = WAIT_TIMEOUT; }, [] { environment.failExitRead = true; },
            [] { environment.workerExit = 0xC0000005; },
        };
        for (const auto &configure : launchFailures)
        {
            environment = {};
            environment.ownElevated = true;
            environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
            configure();
            require(invoke({L"vlc.exe", option + L"=.mp3"}) == 1 && writes.empty() && launches <= 1,
                    "Launch/wait/crash failures must fail without fallback writes or retry");
        }
        // A timeout is not cancellation or proof of success. In particular,
        // an exit-code value supplied by a racing worker must not be sampled.
        for (DWORD status : {DWORD(WAIT_TIMEOUT), DWORD(WAIT_FAILED), DWORD(WAIT_ABANDONED)})
        {
            for (DWORD code : {DWORD(0), DWORD(1), DWORD(4), DWORD(STILL_ACTIVE)})
            {
                environment = {};
                environment.ownElevated = true;
                environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
                environment.waitResult = status;
                environment.workerExit = code;
                require(invoke({L"vlc.exe", option + L"=.mp3"}) == 1 && writes.empty() &&
                        launches == 1 && waits == 1 && exitReads == 0 &&
                        requestedWait == 5 * 60 * 1000,
                        "Unconfirmed completion must return promptly without status sampling or retry");
            }
        }
        environment = {};
        environment.ownElevated = true;
        environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
        environment.failExitRead = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 1 && launches == 1 &&
                waits == 1 && exitReads == 1 && writes.empty(),
                "A signaled worker with an unreadable result must remain unconfirmed");
        environment = {};
        environment.ownElevated = true;
        environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
        environment.changeBeforeLaunch = true;
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 3 && writes.empty() && launches == 0,
                "Changing shell before process creation must refuse without a worker");
        environment.changeBeforeLaunch = false;
        std::wstring excessive;
        for (unsigned i = 0; i < 12000; ++i) excessive += L".a,";
        excessive.pop_back();
        require(invoke({L"vlc.exe", option + L"=" + excessive}) == 2 && writes.empty() && launches == 0,
                "Worker command overflow must not silently truncate or start a partial batch");
        const std::wstring canary = L"space \u03a9 \"quote\" trailing\\";
        const auto quoted = vlc::win32::quoteAssociationArgument(canary);
        int count = 0;
        LPWSTR *parsed = CommandLineToArgvW((L"program " + quoted).c_str(), &count);
        require(parsed && count == 2 && parsed[1] == canary, "Worker quoting must preserve Windows argv");
        LocalFree(parsed);

        // Both public modes and private workers share validation/context guards.
        const std::wstring once = L"--win32-set-default-associations-once";
        const std::wstring onceWorker = L"--win32-association-once-worker=";
        environment = {};
        results = {{AssociationStatus::changed, {}}, {AssociationStatus::unchanged, {}}};
        require(invoke({L"vlc.exe", once + L"=.mp3,.mp4"}) == 0 && onceCalls == 1 && writes.size() == 2,
                "Installer mode routes one complete request to the persistent wrapper");
        onceCompleted = true;
        require(invoke({L"vlc.exe", once + L"=.mp3"}) == 0 && onceCalls == 1 && writes.empty(),
                "Historical success skips without claiming today's defaults");
        require(invoke({L"vlc.exe", option + L"=.mp3"}) == 0 && onceCalls == 0 && writes.size() == 1,
                "Manual mode remains unrestricted after installer success");
        onceCompleted = false;
        onceFailure = true;
        require(invoke({L"vlc.exe", once + L"=.mp3"}) == 1 && onceCalls == 1 && writes.empty(),
                "Completion storage failure is nonfatal and never retries");
        onceFailure = false;
        for (const auto &args : std::vector<std::vector<std::wstring>>{
            {L"vlc.exe", once}, {L"vlc.exe", once + L"="},
            {L"vlc.exe", once + L"=.mp3,bad"},
            {L"vlc.exe", once + L"=.mp3", option + L"=.mp4"},
            {L"vlc.exe", once + L"=.mp3", once + L"=.mp4"},
            {L"vlc.exe", onceWorker + L"bad:4", L".mp3"},
            {L"vlc.exe", onceWorker + L"S-1-5-21-1-2-3-1000:4", L".mp3", L"extra"}})
            require(invoke(args) == 2 && onceCalls == 0 && writes.empty() && launches == 0,
                    "Invalid/mixed once-only requests never reach completion storage");
        environment.wrongUser = true;
        require(invoke({L"vlc.exe", once + L"=.mp3"}) == 3 && onceCalls == 0,
                "Wrong account cannot read or write installer success");
        environment = {};
        require(invoke({L"vlc.exe", onceWorker + L"S-1-5-21-1-2-3-1000:4", L".mp3"}) == 0 && onceCalls == 1,
                "Verified once-only worker preserves its installer purpose");
        environment.ownElevated = true;
        environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
        require(invoke({L"vlc.exe", once + L"=.mp3"}) == 0 && onceCalls == 0 && launches == 1 &&
                launchedArguments[1] == onceWorker + L"S-1-5-21-1-2-3-1000:4",
                "Elevated broker passes once-only purpose without touching user state");

        environment.wrongUser = true;
        onceCompleted = true; // An administrator's history cannot suppress the original user's request.
        require(invoke({L"vlc.exe", once + L"=.mp3,.mp4"}) == 0 && onceCalls == 0 && writes.empty() &&
                launches == 1 && launchContract &&
                launchedArguments[1] == onceWorker + L"S-1-5-21-1-2-3-1001:4",
                "Cross-account installer broker must not read administrator completion history");
        const auto standardOnceWorker = launchedArguments;
        require(invoke(standardOnceWorker) == 3 && onceCalls == 0 && writes.empty() && launches == 0,
                "Elevated administrator worker cannot touch either user's installer state");
        environment = {};
        std::swap(ownSid, otherSid);
        onceCompleted = false;
        require(invoke(standardOnceWorker) == 0 && onceCalls == 1 &&
                writes == std::vector<std::wstring>{L".mp3", L".mp4"} && launches == 0,
                "Original user's once-only worker must apply exactly the selected batch");
        onceCompleted = true;
        require(invoke(standardOnceWorker) == 0 && onceCalls == 1 && writes.empty(),
                "Original user's own completion history still suppresses later defaults");
        std::swap(ownSid, otherSid);
        onceCompleted = false;
        environment.ownElevated = true;
        environment.ownIntegrity = SECURITY_MANDATORY_HIGH_RID;
        environment.waitResult = WAIT_TIMEOUT;
        require(invoke({L"vlc.exe", once + L"=.mp3"}) == 1 && onceCalls == 0 && launches == 1 && waits == 1,
                "Unconfirmed installer worker is neither recorded by broker nor retried");

        LocalFree(lowSid); LocalFree(highSid); LocalFree(mediumSid);
        LocalFree(otherSid); LocalFree(ownSid); LocalFree(systemSid);
        std::cout << "Early association command: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
