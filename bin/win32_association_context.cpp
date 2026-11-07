/*****************************************************************************
 * Interactive desktop verification and association worker handoff
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *****************************************************************************/
#include "win32_association_context.hpp"
#include "win32_association_command.h"

#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace vlc::win32
{
namespace
{
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
using Local = std::unique_ptr<void, decltype(&LocalFree)>;

template <typename T>
bool readToken(HANDLE token, TOKEN_INFORMATION_CLASS kind, T &value)
{
    DWORD size = 0;
    return GetTokenInformation(token, kind, &value, sizeof(value), &size) && size == sizeof(value);
}

bool readTokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS kind, std::vector<BYTE> &data)
{
    DWORD size = 0;
    if (GetTokenInformation(token, kind, nullptr, 0, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return false;
    data.resize(size);
    DWORD written = 0;
    return GetTokenInformation(token, kind, data.data(), size, &written) && written == size;
}

struct Identity
{
    std::vector<BYTE> user;
    DWORD session = 0, integrity = 0;
    bool elevated = false;
    PSID sid() const { return reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid; }
    bool ordinary() const { return !elevated && integrity == SECURITY_MANDATORY_MEDIUM_RID; }
};

bool readIdentity(HANDLE token, Identity &identity)
{
    TOKEN_ELEVATION elevation{};
    std::vector<BYTE> integrity;
    if (!readToken(token, TokenElevation, elevation) ||
        !readToken(token, TokenSessionId, identity.session) || identity.session == 0 ||
        !readTokenBuffer(token, TokenIntegrityLevel, integrity) || integrity.size() < sizeof(TOKEN_MANDATORY_LABEL) ||
        !readTokenBuffer(token, TokenUser, identity.user) || identity.user.size() < sizeof(TOKEN_USER))
        return false;
    const PSID label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(integrity.data())->Label.Sid;
    if (!IsValidSid(label) || !IsValidSid(identity.sid())) return false;
    const UCHAR count = *GetSidSubAuthorityCount(label);
    if (!count) return false;
    identity.integrity = *GetSidSubAuthority(label, count - 1);
    identity.elevated = elevation.TokenIsElevated != 0;
    return true;
}

struct DesktopContext
{
    Handle ownToken{nullptr, CloseHandle}, shell{nullptr, CloseHandle}, shellToken{nullptr, CloseHandle};
    Identity own, user;
    HWND desktop = nullptr;
    DWORD shellPid = 0;
    bool broker = false;
    bool stable() const
    {
        DWORD pid = 0;
        return GetShellWindow() == desktop && GetWindowThreadProcessId(desktop, &pid) && pid == shellPid;
    }
};

struct ShellAccessPrivilege
{
    Handle token{nullptr, CloseHandle};
    TOKEN_PRIVILEGES previous{};
    bool changed = false;
    ~ShellAccessPrivilege() { restore(); }
    bool enable()
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &raw)) return false;
        token.reset(raw);
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &requested.Privileges[0].Luid)) return false;
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD size = sizeof(previous);
        // TRUE can still report ERROR_NOT_ALL_ASSIGNED through GetLastError.
        SetLastError(ERROR_SUCCESS);
        if (!AdjustTokenPrivileges(raw, FALSE, &requested, size, &previous, &size) ||
            GetLastError() != ERROR_SUCCESS)
            return false;
        // An already-enabled privilege has no previous-state entry to restore.
        changed = previous.PrivilegeCount != 0;
        return true;
    }
    bool restore()
    {
        if (!changed) return true;
        changed = false;
        SetLastError(ERROR_SUCCESS);
        return AdjustTokenPrivileges(token.get(), FALSE, &previous, 0, nullptr, nullptr) &&
               GetLastError() == ERROR_SUCCESS;
    }
};

bool captureContext(DesktopContext &context, bool allowBroker)
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    context.ownToken.reset(raw);
    if (!readIdentity(raw, context.own)) return false;
    context.broker = context.own.elevated && context.own.integrity == SECURITY_MANDATORY_HIGH_RID;
    if (!context.own.ordinary() && !(allowBroker && context.broker)) return false;
    context.desktop = GetShellWindow();
    if (!context.desktop || !GetWindowThreadProcessId(context.desktop, &context.shellPid) || !context.shellPid)
        return false;
    const auto openShell = [&]() {
        context.shell.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
            (context.broker ? PROCESS_CREATE_PROCESS : 0), FALSE, context.shellPid));
        HANDLE shellToken = nullptr;
        if (!context.shell || !OpenProcessToken(context.shell.get(), TOKEN_QUERY |
            (context.broker ? TOKEN_DUPLICATE : 0), &shellToken)) return false;
        context.shellToken.reset(shellToken);
        return true;
    };
    if (!openShell())
    {
        if (!context.broker || GetLastError() != ERROR_ACCESS_DENIED) return false;
        // Cross-account token access may require SeDebugPrivilege. Scope it to
        // acquiring this desktop's shell handles, and restore before any launch.
        // Failure to acquire or restore it refuses the command without a worker.
        ShellAccessPrivilege privilege;
        if (!privilege.enable() || !openShell() || !privilege.restore()) return false;
    }
    // Only the elevated broker may differ from the interactive shell account.
    // Direct callers and private workers still have to be that exact user.
    if (!readIdentity(context.shellToken.get(), context.user) || !context.user.ordinary() ||
        context.own.session != context.user.session ||
        (!context.broker && !EqualSid(context.own.sid(), context.user.sid())))
        return false;
    return context.stable();
}

// Windows argv quoting, including embedded quotes and trailing backslashes.
std::wstring quoteAssociationArgument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : argument)
    {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct UserEnvironment
{
    LPVOID value = nullptr;
    ~UserEnvironment() { if (value) DestroyEnvironmentBlock(value); }
};

struct ParentAttributes
{
    std::vector<BYTE> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    HANDLE parent = nullptr;
    ~ParentAttributes() { if (value) DeleteProcThreadAttributeList(value); }
    bool initialize(HANDLE shell)
    {
        parent = shell; // Attribute values must remain alive through CreateProcess.
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size) return false;
        storage.resize(size);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) return false;
        value = list;
        return UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
            &parent, sizeof(parent), nullptr, nullptr) != FALSE;
    }
};

int launchFailure(const char *stage)
{
    std::fprintf(stderr, "default-app setup: %s failed (%lu); no retry\n", stage,
                 static_cast<unsigned long>(GetLastError()));
    return VLC_WIN32_ASSOCIATION_FAILED;
}
}

bool associationCommandUserContext() noexcept
{
    try { DesktopContext context; return captureContext(context, false); }
    catch (...) { return false; }
}

bool associationCommandWorkerContext(std::wstring_view sid, std::uint32_t session) noexcept
{
    try
    {
        DesktopContext context;
        if (!captureContext(context, false) || context.own.session != session) return false;
        PSID raw = nullptr;
        const std::wstring expected(sid);
        if (!ConvertStringSidToSidW(expected.c_str(), &raw)) return false;
        Local expectedSid(raw, LocalFree);
        return EqualSid(context.own.sid(), raw) != FALSE;
    }
    catch (...) { return false; }
}

int associationCommandHandoff(std::wstring_view extensions, bool once) noexcept
{
    try
    {
        DesktopContext context;
        if (!captureContext(context, true))
        {
            std::fprintf(stderr, "default-app setup: unsafe or unverifiable desktop context; no changes attempted\n");
            return VLC_WIN32_ASSOCIATION_UNSAFE_CONTEXT;
        }
        if (!context.broker) return VLC_WIN32_ASSOCIATION_NOT_HANDLED;

        std::vector<wchar_t> path(32768);
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return launchFailure("resolve worker executable");
        const std::wstring executable(path.data(), length);
        const size_t slash = executable.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return VLC_WIN32_ASSOCIATION_FAILED;
        const std::wstring directory = executable.substr(0, slash + 1);
        LPWSTR rawSid = nullptr;
        if (!ConvertSidToStringSidW(context.user.sid(), &rawSid)) return launchFailure("format worker identity");
        Local sid(rawSid, LocalFree);
        // Over-the-shoulder UAC changes the broker's account, not the intended
        // desktop user. Inherit the held shell's token below, never the broker's
        // linked token. Default process/thread ACLs otherwise come from the
        // creator and can prevent that different user from querying its own token.
        const std::wstring acl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(rawSid) + L")";
        PSECURITY_DESCRIPTOR rawSecurity = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1,
                &rawSecurity, nullptr))
            return launchFailure("build desktop-user worker access control");
        Local security(rawSecurity, LocalFree);
        SECURITY_ATTRIBUTES access{sizeof(SECURITY_ATTRIBUTES), rawSecurity, FALSE};
        const std::wstring marker = std::wstring(once ? L"--win32-association-once-worker="
                                                    : L"--win32-association-worker=") + std::wstring(rawSid) + L":" +
                                    std::to_wstring(context.user.session);
        std::wstring command = quoteAssociationArgument(executable) + L" " +
            quoteAssociationArgument(marker) + L" " + quoteAssociationArgument(extensions);
        if (command.size() >= 32767)
        {
            std::fprintf(stderr, "default-app setup: worker command exceeds Windows length limit\n");
            return VLC_WIN32_ASSOCIATION_INVALID_REQUEST;
        }
        UserEnvironment environment;
        if (!CreateEnvironmentBlock(&environment.value, context.shellToken.get(), FALSE))
            return launchFailure("build ordinary-user environment");
        ParentAttributes attributes;
        if (!attributes.initialize(context.shell.get())) return launchFailure("select desktop parent");
        if (!context.stable()) return VLC_WIN32_ASSOCIATION_UNSAFE_CONTEXT;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes.value;
        PROCESS_INFORMATION process{};
        // No inherited broker handles: the parent/token source is the shell.
        // Result transport is the real child process handle, not log text.
        if (!CreateProcessW(executable.c_str(), command.data(), &access, &access, FALSE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment.value, directory.c_str(), &startup.StartupInfo, &process))
            return launchFailure("launch ordinary-user worker");
        Handle child(process.hProcess, CloseHandle), thread(process.hThread, CloseHandle);
        // Bound only the broker's wait, never the transaction's lifetime. The
        // worker may still be writing after we return: no kill, cancel or retry.
        constexpr DWORD completionTimeoutMs = 5 * 60 * 1000;
        const DWORD wait = WaitForSingleObject(child.get(), completionTimeoutMs);
        if (wait != WAIT_OBJECT_0)
        {
            const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            std::fprintf(stderr,
                "default-app setup: worker PID %lu completion unconfirmed (%s; wait %lu; error %lu); "
                "worker not terminated; do not retry\n",
                static_cast<unsigned long>(process.dwProcessId),
                wait == WAIT_TIMEOUT ? "five-minute timeout" : "wait failed",
                static_cast<unsigned long>(wait), static_cast<unsigned long>(error));
            // Do not query an unsignaled process and mistake its still-active
            // status for a completed operation, even if it exits just after timeout.
            return VLC_WIN32_ASSOCIATION_FAILED;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(child.get(), &code))
        {
            const DWORD error = GetLastError();
            std::fprintf(stderr,
                "default-app setup: worker PID %lu exited, but result is unavailable (%lu); "
                "completion unconfirmed; do not retry\n",
                static_cast<unsigned long>(process.dwProcessId), static_cast<unsigned long>(error));
            return VLC_WIN32_ASSOCIATION_FAILED;
        }
        std::fprintf(stderr, "default-app setup: ordinary-user worker exited with status %lu\n",
                     static_cast<unsigned long>(code));
        return code <= VLC_WIN32_ASSOCIATION_BUSY ? static_cast<int>(code) : VLC_WIN32_ASSOCIATION_FAILED;
    }
    catch (...)
    {
        std::fprintf(stderr, "default-app setup: handoff failed; completion may be unconfirmed; do not retry\n");
        return VLC_WIN32_ASSOCIATION_FAILED;
    }
}
}
