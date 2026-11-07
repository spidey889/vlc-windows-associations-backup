/*****************************************************************************
 * win32_associations.cpp: Windows default application associations
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See LICENSE for the full license text.
 *****************************************************************************/

#include "win32_associations.hpp"
#include "association_batch.hpp"
#include "userchoice_hash.hpp"
#include "userchoice_latest_hash/HashCommon.h"

#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#define RRF_SUBKEY_WOW6432KEY 0x00020000
#endif

namespace vlc::win32
{
namespace
{
class NeedsUserChoice : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

class AssociationLockBusy : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

class ChoiceConflict : public NeedsUserChoice
{
  public:
    ChoiceConflict() : NeedsUserChoice("A concurrent default-app update was detected; try again") {}
};

class Handle
{
  public:
    explicit Handle(HANDLE value = nullptr) : m_value(value) {}
    ~Handle()
    {
        if (m_value != nullptr && m_value != INVALID_HANDLE_VALUE)
            CloseHandle(m_value);
    }

    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const { return m_value; }

  private:
    HANDLE m_value;
};

class RegistryKey
{
  public:
    explicit RegistryKey(HKEY value = nullptr) : m_value(value) {}
    ~RegistryKey()
    {
        if (m_value != nullptr)
            RegCloseKey(m_value);
    }

    RegistryKey(const RegistryKey &) = delete;
    RegistryKey &operator=(const RegistryKey &) = delete;
    HKEY get() const { return m_value; }

  private:
    HKEY m_value;
};

struct LocalFreeDeleter
{
    void operator()(void *value) const noexcept { LocalFree(value); }
};

using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

std::string utf8(std::wstring_view text)
{
    if (text.empty())
        return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return "Windows message is too large";

    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return "Unknown Windows error";

    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr) <= 0)
        return "Unknown Windows error";
    return result;
}

std::string windowsError(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    const LocalMemory allocation(buffer);
    std::string result = size != 0 && buffer != nullptr ? utf8(std::wstring_view(buffer, size))
                                                        : "Unknown Windows error";
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
        result.pop_back();
    return result;
}

[[noreturn]] void throwWindowsError(const char *operation, DWORD code)
{
    throw std::runtime_error(std::string(operation) + ": " + windowsError(code) + " (" +
                             std::to_string(code) + ")");
}

using detail::validExtension;

std::string ascii(std::wstring_view text, const char *description)
{
    std::string result;
    result.reserve(text.size());
    for (const wchar_t character : text)
    {
        if (character > 0x7f)
            throw std::runtime_error(std::string(description) + " is not ASCII");
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::wstring wide(std::string_view text)
{
    return std::wstring(text.begin(), text.end());
}

std::wstring extensionRoot(std::wstring_view extension)
{
    if (!validExtension(extension))
        throw std::invalid_argument("Invalid file extension");
    return L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" +
           std::wstring(extension);
}

std::wstring legacyChoicePath(std::wstring_view extension)
{
    return extensionRoot(extension) + L"\\UserChoice";
}

std::wstring latestChoicePath(std::wstring_view extension)
{
    return extensionRoot(extension) + L"\\UserChoiceLatest";
}

std::optional<std::wstring> readString(HKEY root, const std::wstring &path, const wchar_t *name,
                                       DWORD registryView = 0)
{
    DWORD bytes = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | registryView;
    LSTATUS status = RegGetValueW(root, path.c_str(), name, flags, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return std::nullopt;
    if (status != ERROR_SUCCESS)
        throwWindowsError("Reading a registry string", status);

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = RegGetValueW(root, path.c_str(), name, flags, nullptr, buffer.data(), &bytes);
    if (status != ERROR_SUCCESS)
        throwWindowsError("Reading a registry string", status);
    return std::wstring(buffer.data());
}

std::optional<DWORD> readDword(HKEY root, const std::wstring &path, const wchar_t *name,
                               DWORD registryView = 0)
{
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(root, path.c_str(), name, RRF_RT_REG_DWORD | registryView,
                                        nullptr, &value, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return std::nullopt;
    if (status != ERROR_SUCCESS)
        throwWindowsError("Reading a registry number", status);
    return value;
}

void writeString(HKEY key, const wchar_t *name, const std::wstring &value)
{
    const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    if (bytes > std::numeric_limits<DWORD>::max())
        throw std::runtime_error("Registry string is too large");
    const LSTATUS status =
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                       static_cast<DWORD>(bytes));
    if (status != ERROR_SUCCESS)
        throwWindowsError("Writing a registry string", status);
}

void writeDword(HKEY key, const wchar_t *name, DWORD value)
{
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_DWORD,
                                          reinterpret_cast<const BYTE *>(&value), sizeof(value));
    if (status != ERROR_SUCCESS)
        throwWindowsError("Writing a registry number", status);
}

RegistryKey createUserKey(const std::wstring &path)
{
    HKEY key = nullptr;
    const LSTATUS status =
        RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS)
        throwWindowsError("Creating a registry key", status);
    return RegistryKey(key);
}

bool keyExists(HKEY root, const std::wstring &path)
{
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return true;
    }
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return false;
    throwWindowsError("Checking a registry key", status);
}

void deleteUserTree(const std::wstring &path)
{
    const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
        throwWindowsError("Removing a registry key", status);
}

void flushUserKey(const std::wstring &path)
{
    HKEY rawKey = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE, &rawKey);
    if (status != ERROR_SUCCESS)
        throwWindowsError("Opening a registry key for persistence", status);
    const RegistryKey key(rawKey);
    status = RegFlushKey(key.get());
    if (status != ERROR_SUCCESS)
        throwWindowsError("Persisting an association update", status);
}

FILETIME keyWriteTime(HKEY key)
{
    FILETIME value{};
    const LSTATUS status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr,
                                            nullptr, nullptr, nullptr, nullptr, nullptr, &value);
    if (status != ERROR_SUCCESS)
        throwWindowsError("Reading a registry timestamp", status);
    return value;
}

FILETIME keyWriteTime(HKEY root, const std::wstring &path)
{
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS)
        throwWindowsError("Opening a registry key", status);
    const RegistryKey holder(key);
    return keyWriteTime(holder.get());
}

std::wstring currentExecutable()
{
    std::vector<wchar_t> buffer(512);
    for (;;)
    {
        const DWORD size =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0)
            throwWindowsError("Finding the running VLC executable", GetLastError());
        if (size < buffer.size() - 1)
            return std::wstring(buffer.data(), size);
        if (buffer.size() >= 32768)
            throw std::runtime_error("The VLC executable path is too long");
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring fullPath(std::wstring_view path)
{
    const std::wstring input(path);
    const DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0)
        return input;
    std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1);
    const DWORD size =
        GetFullPathNameW(input.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return size != 0 && size < buffer.size() ? std::wstring(buffer.data(), size) : input;
}

bool samePath(std::wstring_view first, std::wstring_view second)
{
    const std::wstring firstFull = fullPath(first);
    const std::wstring secondFull = fullPath(second);
    return _wcsicmp(firstFull.c_str(), secondFull.c_str()) == 0;
}

std::optional<std::wstring> commandExecutable(std::wstring_view command)
{
    const std::wstring commandText(command);
    int count = 0;
    LPWSTR *arguments = CommandLineToArgvW(commandText.c_str(), &count);
    const LocalMemory allocation(arguments);
    if (arguments == nullptr || count < 1)
    {
        return std::nullopt;
    }
    return std::wstring(arguments[0]);
}

bool programIdUsesCurrentVlc(const std::wstring &programId, const std::wstring &vlcPath)
{
    const auto command =
        readString(HKEY_CLASSES_ROOT, programId + L"\\shell\\open\\command", nullptr);
    if (!command)
        return false;
    const auto executable = commandExecutable(*command);
    return executable && samePath(*executable, vlcPath);
}

std::optional<std::wstring> findVlcProgramId(std::wstring_view extension,
                                             const std::wstring &vlcPath)
{
    // Capabilities supply the identifier, but its open command must resolve to
    // this executable. A VLC-named ProgId alone could belong to another install.
    static const std::wstring capabilities =
        L"Software\\Clients\\Media\\VLC\\Capabilities\\FileAssociations";
    const std::wstring name(extension);
    const std::array<std::pair<HKEY, DWORD>, 4> locations = {{
        {HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6464KEY},
        {HKEY_LOCAL_MACHINE, RRF_SUBKEY_WOW6432KEY},
        {HKEY_CURRENT_USER, RRF_SUBKEY_WOW6464KEY},
        {HKEY_CURRENT_USER, RRF_SUBKEY_WOW6432KEY},
    }};

    for (const auto &[root, view] : locations)
    {
        const auto programId = readString(root, capabilities, name.c_str(), view);
        if (programId && !programId->empty() && programIdUsesCurrentVlc(*programId, vlcPath))
            return programId;
    }

    const std::wstring conventional = L"VLC" + name;
    return programIdUsesCurrentVlc(conventional, vlcPath)
               ? std::optional<std::wstring>(conventional)
               : std::nullopt;
}

std::optional<std::wstring> associatedExecutable(std::wstring_view extension)
{
    const std::wstring value(extension);
    DWORD size = 0;
    AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, value.c_str(), L"open", nullptr, &size);
    if (size == 0)
        return std::nullopt;
    std::vector<wchar_t> buffer(size, L'\0');
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, value.c_str(), L"open",
                                 buffer.data(), &size)))
        return std::nullopt;
    return std::wstring(buffer.data());
}

std::optional<std::wstring> effectiveProgramId(std::wstring_view extension)
{
    // QueryCurrentDefault remains supported on Windows 8 and later. Ask Windows
    // for AL_EFFECTIVE instead of treating our own hash as acceptance evidence.
    struct ComScope
    {
        HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
    } com;
    if (FAILED(com.status) && com.status != RPC_E_CHANGED_MODE)
        return std::nullopt;

    IApplicationAssociationRegistration *raw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ApplicationAssociationRegistration, nullptr,
                                CLSCTX_INPROC_SERVER, IID_IApplicationAssociationRegistration,
                                reinterpret_cast<void **>(&raw))))
        return std::nullopt;
    const auto release = [](IApplicationAssociationRegistration *value) { value->Release(); };
    const std::unique_ptr<IApplicationAssociationRegistration, decltype(release)> query(raw, release);
    wchar_t *value = nullptr;
    const std::wstring name(extension);
    const HRESULT status = query->QueryCurrentDefault(name.c_str(), AT_FILEEXTENSION,
                                                      AL_EFFECTIVE, &value);
    const auto free = [](wchar_t *text) { CoTaskMemFree(text); };
    const std::unique_ptr<wchar_t, decltype(free)> allocation(value, free);
    if (FAILED(status) || value == nullptr)
        return std::nullopt;
    return std::wstring(value);
}

bool currentVlcIsEffective(std::wstring_view extension, const std::wstring &vlcPath)
{
    const auto programId = effectiveProgramId(extension);
    // Resolve the returned ProgId directly, avoiding the extension query cache.
    const auto executable = programId ? associatedExecutable(*programId) : std::nullopt;
    return executable && samePath(*executable, vlcPath);
}

std::wstring currentUserSid()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        throwWindowsError("Opening the current user token", GetLastError());
    const Handle token(rawToken);

    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        throwWindowsError("Measuring the current user SID", GetLastError());
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), bytes, &bytes))
        throwWindowsError("Reading the current user SID", GetLastError());

    wchar_t *value = nullptr;
    const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
    if (!ConvertSidToStringSidW(user->User.Sid, &value))
        throwWindowsError("Formatting the current user SID", GetLastError());
    const LocalMemory allocation(value);
    return std::wstring(value);
}

std::string minuteTimestamp(const FILETIME &time)
{
    constexpr uint64_t ticksPerMinute = 600000000ULL;
    uint64_t value = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    value -= value % ticksPerMinute;
    std::ostringstream result;
    result << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
    return result.str();
}

std::string currentMinuteTimestamp()
{
    FILETIME value{};
    GetSystemTimeAsFileTime(&value);
    return minuteTimestamp(value);
}

std::wstring latestTimestamp(const FILETIME &time)
{
    // The format uses the FILETIME -> SYSTEMTIME -> FILETIME round-trip's
    // millisecond precision, not the raw registry time's 100-nanosecond ticks.
    SYSTEMTIME systemTime{};
    FILETIME normalized{};
    if (!FileTimeToSystemTime(&time, &systemTime) ||
        !SystemTimeToFileTime(&systemTime, &normalized))
        throwWindowsError("Normalizing a registry timestamp", GetLastError());

    // Avoid CRT printf differences between the MinGW build and Windows runtime.
    static constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result(16, L'0');
    const auto writeWord = [&result](size_t offset, DWORD value) {
        for (size_t index = 0; index < 8; ++index)
        {
            const unsigned shift = static_cast<unsigned>((7 - index) * 4);
            result[offset + index] = hex[(value >> shift) & 0xf];
        }
    };
    writeWord(0, normalized.dwHighDateTime);
    writeWord(8, normalized.dwLowDateTime);
    return result;
}

std::wstring machineId()
{
    // SQMClient is redirected under WOW64; use the same MachineID as Windows.
    const auto value =
        readString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\SQMClient", L"MachineID",
                   RRF_SUBKEY_WOW6464KEY);
    if (!value || value->empty())
        throw std::runtime_error("Windows MachineID is unavailable");
    if (value->size() >= 2 && value->front() == L'{' && value->back() == L'}')
    {
        if (value->size() == 2)
            throw std::runtime_error("Windows MachineID is empty");
        return value->substr(1, value->size() - 2);
    }
    return *value;
}

std::wstring calculateLatestHash(std::wstring_view extension, std::wstring_view programId,
                                 const FILETIME &writeTime)
{
    static constexpr std::array<std::wstring_view, 3> salts = {
        L"Copyright (C) Microsoft. All rights reserved "
        L"{3822B7CA-C2F4-4889-B8CC-4CE39A8FB81C}",
        L"Copyright (C) Microsoft. All rights reserved "
        L"{D185E0A1-E265-4724-AA21-3A17B038D72E}",
        L"Copyright (C) Microsoft. All rights reserved "
        L"{97B6BCF4-C367-4577-95BE-73BD3053A5E0}",
    };

    const std::wstring machine = machineId();
    const std::wstring sid = currentUserSid();
    // The class is the final MachineID character modulo three, not the GUID's
    // numeric value. Each class selects its own salt AND concatenation order;
    // all three feed the same imported codec (provenance in its LICENSE).
    const int machineClass = machine.back() % 3;
    const std::wstring timestamp = latestTimestamp(writeTime);
    std::wstring canonical;
    if (machineClass == 0)
        canonical = std::wstring(salts[0]) + std::wstring(extension) + timestamp + machine +
                    std::wstring(programId) + sid;
    else if (machineClass == 1)
        canonical = std::wstring(salts[1]) + timestamp + std::wstring(extension) + sid + machine +
                    std::wstring(programId);
    else
        canonical = sid + timestamp + std::wstring(salts[2]) + std::wstring(extension) + machine +
                    std::wstring(programId);

    UserChoiceLatestHash::WorkingSeeds seeds{};
    UserChoiceLatestHash::LoadProvidedSeeds(&seeds);
    std::wstring result;
    if (!UserChoiceLatestHash::ComputeHash(canonical, seeds, false, &result, nullptr))
        throw std::runtime_error("UserChoiceLatest hash calculation failed");
    return result;
}

bool choiceMatches(const std::wstring &path, const std::wstring &programId,
                   const std::wstring &hash)
{
    const auto storedProgramId = readString(HKEY_CURRENT_USER, path, L"ProgId");
    const auto storedHash = readString(HKEY_CURRENT_USER, path, L"Hash");
    return storedProgramId && storedHash &&
           _wcsicmp(storedProgramId->c_str(), programId.c_str()) == 0 && *storedHash == hash;
}

bool legacyChoiceValid(std::wstring_view extension, const std::wstring &programId)
{
    const std::wstring path = legacyChoicePath(extension);
    if (!keyExists(HKEY_CURRENT_USER, path))
        return false;
    const std::wstring hash = wide(calculateLegacyUserChoiceHash(
        ascii(extension, "File extension"), ascii(currentUserSid(), "User SID"),
        ascii(programId, "Program ID"), minuteTimestamp(keyWriteTime(HKEY_CURRENT_USER, path))));
    return choiceMatches(path, programId, hash);
}

bool latestChoiceValid(std::wstring_view extension, const std::wstring &programId)
{
    const std::wstring programPath = latestChoicePath(extension) + L"\\ProgId";
    const auto storedProgramId = readString(HKEY_CURRENT_USER, programPath, L"ProgId");
    const auto storedHash = readString(HKEY_CURRENT_USER, latestChoicePath(extension), L"Hash");
    if (!storedProgramId || !storedHash ||
        _wcsicmp(storedProgramId->c_str(), programId.c_str()) != 0)
        return false;
    return *storedHash ==
           calculateLatestHash(extension, programId, keyWriteTime(HKEY_CURRENT_USER, programPath));
}

bool choiceIsDefault(std::wstring_view extension, detail::HashScheme scheme,
                     const std::wstring &programId, const std::wstring &vlcPath)
{
    return (scheme == detail::HashScheme::latest ? latestChoiceValid(extension, programId)
                                               : legacyChoiceValid(extension, programId)) &&
           currentVlcIsEffective(extension, vlcPath);
}

void notifyAssociationChange()
{
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

enum class ChoiceFormat : DWORD
{
    legacy = 0,
    latest = 1
};

struct ChoiceSnapshot
{
    bool exists;
    std::optional<std::wstring> programId;
    std::optional<std::wstring> hash;
    FILETIME rootTime{};
    FILETIME programTime{};
};

ChoiceSnapshot snapshotChoice(std::wstring_view extension, ChoiceFormat format)
{
    const std::wstring root = format == ChoiceFormat::latest
                                  ? latestChoicePath(extension) : legacyChoicePath(extension);
    ChoiceSnapshot snapshot{keyExists(HKEY_CURRENT_USER, root), {}, {}};
    if (!snapshot.exists)
        return snapshot;
    const std::wstring programPath = format == ChoiceFormat::latest ? root + L"\\ProgId" : root;
    snapshot.programId = readString(HKEY_CURRENT_USER, programPath, L"ProgId");
    snapshot.hash = readString(HKEY_CURRENT_USER, root, L"Hash");
    snapshot.rootTime = keyWriteTime(HKEY_CURRENT_USER, root);
    if (format == ChoiceFormat::latest && keyExists(HKEY_CURRENT_USER, programPath))
        snapshot.programTime = keyWriteTime(HKEY_CURRENT_USER, programPath);
    return snapshot;
}

bool sameTime(const FILETIME &first, const FILETIME &second)
{
    return first.dwLowDateTime == second.dwLowDateTime &&
           first.dwHighDateTime == second.dwHighDateTime;
}

void requireChoiceSnapshot(std::wstring_view extension, ChoiceFormat format,
                           const ChoiceSnapshot &expected)
{
    // Matching values can hide a same-ProgId rewrite; compare key times too.
    const auto current = snapshotChoice(extension, format);
    if (current.exists != expected.exists || current.programId != expected.programId ||
        current.hash != expected.hash || !sameTime(current.rootTime, expected.rootTime) ||
        !sameTime(current.programTime, expected.programTime))
        throw ChoiceConflict();
}

// These checks detect interference; RegSetValueEx/RegDeleteTree offer no atomic
// compare-and-write. Settings does not share VLC's mutex and can still race the
// final check and mutation. Never regenerate a foreign record to hide that gap.
void requireWrittenProgram(const ChoiceSnapshot &snapshot, const std::wstring &programId,
                           const std::optional<std::wstring> &previousHash)
{
    if (!snapshot.programId || *snapshot.programId != programId || snapshot.hash != previousHash)
        throw ChoiceConflict();
}

void writeLegacyChoice(std::wstring_view extension, const std::wstring &programId,
                       bool requireEffective, const std::wstring &vlcPath,
                       const ChoiceSnapshot *expected = nullptr)
{
    const std::wstring path = legacyChoicePath(extension);
    const std::string extensionText = ascii(extension, "File extension");
    const std::string sid = ascii(currentUserSid(), "User SID");
    const std::string program = ascii(programId, "Program ID");
    ChoiceSnapshot observed = expected ? *expected : snapshotChoice(extension, ChoiceFormat::legacy);

    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        // A legacy hash binds to the key's write minute. If the two value writes
        // cross that boundary, retry the complete pair before verifying it.
        const std::string timestamp = currentMinuteTimestamp();
        const std::wstring hash =
            wide(calculateLegacyUserChoiceHash(extensionText, sid, program, timestamp));

        requireChoiceSnapshot(extension, ChoiceFormat::legacy, observed);
        HKEY rawKey = nullptr;
        LSTATUS status =
            RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &rawKey, nullptr);
        if (status == ERROR_ACCESS_DENIED)
        {
            // Some UserChoice keys deny value writes but permit replacement.
            // Transaction callers journal the logical choice before taking this
            // delete/recreate path; key permissions are not part of that backup.
            requireChoiceSnapshot(extension, ChoiceFormat::legacy, observed);
            deleteUserTree(path);
            observed = ChoiceSnapshot{false, {}, {}};
            requireChoiceSnapshot(extension, ChoiceFormat::legacy, observed);
            status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE,
                                     nullptr, &rawKey, nullptr);
        }
        if (status != ERROR_SUCCESS)
            throwWindowsError("Opening UserChoice", status);
        {
            const RegistryKey key(rawKey);
            writeString(key.get(), L"ProgId", programId);
            const auto written = snapshotChoice(extension, ChoiceFormat::legacy);
            requireWrittenProgram(written, programId, observed.hash);
            requireChoiceSnapshot(extension, ChoiceFormat::legacy, written);
            writeString(key.get(), L"Hash", hash);
            observed = snapshotChoice(extension, ChoiceFormat::legacy);
            if (!observed.programId || *observed.programId != programId || observed.hash != hash)
                throw ChoiceConflict();
            if (minuteTimestamp(keyWriteTime(key.get())) != timestamp)
                continue;
        }

        notifyAssociationChange();
        for (unsigned check = 0; check < 20; ++check)
        {
            if (choiceMatches(path, programId, hash) &&
                (!requireEffective || currentVlcIsEffective(extension, vlcPath)))
            {
                // Persist the choice before its durable recovery journal is removed.
                flushUserKey(path);
                return;
            }
            Sleep(100);
        }
        throw NeedsUserChoice("Windows rejected the default-app update");
    }
    throw NeedsUserChoice("The default-app update crossed a minute boundary repeatedly");
}

void writeLatestChoice(std::wstring_view extension, const std::wstring &programId,
                       const ChoiceSnapshot *expected = nullptr)
{
    const std::wstring root = latestChoicePath(extension);
    const std::wstring programPath = root + L"\\ProgId";
    const auto before = expected ? *expected : snapshotChoice(extension, ChoiceFormat::latest);
    requireChoiceSnapshot(extension, ChoiceFormat::latest, before);
    // This format binds the hash to the ProgId subkey's post-write timestamp.
    // The caller preflights and journals first; do not hash the old timestamp.
    // Hash is stored on the parent, so its write does not retimestamp ProgId.
    {
        const RegistryKey key = createUserKey(programPath);
        writeString(key.get(), L"ProgId", programId);
    }
    const auto written = snapshotChoice(extension, ChoiceFormat::latest);
    requireWrittenProgram(written, programId, before.hash);
    const std::wstring hash = calculateLatestHash(extension, programId, written.programTime);
    // Calculation can be slow. A detected Settings change must not receive the
    // hash calculated for our stale ProgId/timestamp.
    requireChoiceSnapshot(extension, ChoiceFormat::latest, written);
    {
        const RegistryKey key = createUserKey(root);
        writeString(key.get(), L"Hash", hash);
    }
    const auto published = snapshotChoice(extension, ChoiceFormat::latest);
    if (!published.programId || *published.programId != programId || published.hash != hash ||
        !sameTime(published.programTime, written.programTime))
        throw ChoiceConflict();
    // RegSetValueEx alone is not a power-loss durability guarantee.
    flushUserKey(root);
    notifyAssociationChange();
}

void preflightChoice(std::wstring_view extension, ChoiceFormat format,
                     const std::wstring &programId, const FILETIME &time)
{
    // Check identifiers, metadata and the hash engine before touching UserChoice.
    // The final hash still needs the actual post-write registry timestamp.
    if (format == ChoiceFormat::latest)
        (void)calculateLatestHash(extension, programId, time);
    else
        (void)calculateLegacyUserChoiceHash(ascii(extension, "File extension"),
                                           ascii(currentUserSid(), "User SID"),
                                           ascii(programId, "Program ID"), minuteTimestamp(time));
}

void writeChoice(std::wstring_view extension, ChoiceFormat format,
                 const std::wstring &programId, bool requireEffective,
                 const std::wstring &vlcPath, const ChoiceSnapshot *expected = nullptr)
{
    // Rollback/recovery use the same timestamp-aware writers as a forward update.
    // Restoring another player's legacy choice checks its record, not whether it
    // opens this VLC. Forward writes must also be accepted by Windows.
    if (format == ChoiceFormat::legacy)
    {
        writeLegacyChoice(extension, programId, requireEffective, vlcPath, expected);
        return;
    }

    writeLatestChoice(extension, programId, expected);
    for (unsigned check = 0; check < 30; ++check)
    {
        if (latestChoiceValid(extension, programId) &&
            (!requireEffective || currentVlcIsEffective(extension, vlcPath)))
            return;
        Sleep(100);
    }
    throw NeedsUserChoice("Windows rejected the default-app update");
}

// One journal per extension: each batch member commits or recovers independently.
// Registry value writes are not atomic as a group. A Ready journal identifies
// both ProgIds we may need to repair: the target and a partially restored backup.
// Complete records are flushed before clearing it. Unrelated choices are kept.
// Store identifiers, not old hashes: restoring either format changes its timestamp.
// The mutex serializes VLC instances only; Windows Settings can still race us.
struct PendingChange
{
    ChoiceFormat format;
    bool hadPrevious;
    std::optional<std::wstring> previousProgramId;
    std::wstring targetProgramId;
};

std::wstring recoveryPath(std::wstring_view extension)
{
    if (!validExtension(extension))
        throw std::invalid_argument("Invalid recovery extension");
    return L"Software\\VideoLAN\\VLC\\AssociationRecovery\\" + std::wstring(extension);
}

void clearRecovery(std::wstring_view extension)
{
    deleteUserTree(recoveryPath(extension));
}

void saveRecovery(std::wstring_view extension, ChoiceFormat format, bool hadPrevious,
                  const std::optional<std::wstring> &previousProgramId,
                  const std::wstring &targetProgramId)
{
    if (targetProgramId.empty() ||
        (hadPrevious && (!previousProgramId || previousProgramId->empty())))
        throw std::runtime_error("Invalid association recovery state");

    const RegistryKey key = createUserKey(recoveryPath(extension));
    // Publish metadata first, then Ready, flushing each stage. No choice writer
    // runs until both flushes succeed. A non-ready journal therefore cannot
    // authorize restoration of a partially recorded backup.
    writeDword(key.get(), L"Ready", 0);
    writeDword(key.get(), L"Format", static_cast<DWORD>(format));
    writeDword(key.get(), L"HadPrevious", hadPrevious ? 1 : 0);
    writeString(key.get(), L"TargetProgId", targetProgramId);
    if (hadPrevious)
        writeString(key.get(), L"PreviousProgId", *previousProgramId);
    LSTATUS status = RegFlushKey(key.get());
    if (status != ERROR_SUCCESS)
        throwWindowsError("Saving association recovery state", status);
    writeDword(key.get(), L"Ready", 1);
    status = RegFlushKey(key.get());
    if (status != ERROR_SUCCESS)
        throwWindowsError("Finalizing association recovery state", status);
}

std::optional<PendingChange> loadRecovery(std::wstring_view extension)
{
    const std::wstring path = recoveryPath(extension);
    if (!keyExists(HKEY_CURRENT_USER, path))
        return std::nullopt;
    // Missing/non-ready and well-typed malformed journals are handled below.
    // A registry read/type error instead propagates, retaining the journal:
    // inability to read a backup is not evidence that it is safe to discard.
    const auto ready = readDword(HKEY_CURRENT_USER, path, L"Ready");
    if (!ready || *ready == 0)
    {
        clearRecovery(extension);
        return std::nullopt;
    }
    const auto format = readDword(HKEY_CURRENT_USER, path, L"Format");
    const auto hadPrevious = readDword(HKEY_CURRENT_USER, path, L"HadPrevious");
    const auto target = readString(HKEY_CURRENT_USER, path, L"TargetProgId");
    if (*ready != 1 || !format || *format > 1 || !hadPrevious || *hadPrevious > 1 || !target ||
        target->empty())
    {
        clearRecovery(extension);
        throw std::runtime_error("Invalid saved recovery state was discarded; try again");
    }
    const auto previous = readString(HKEY_CURRENT_USER, path, L"PreviousProgId");
    if (*hadPrevious != 0 && (!previous || previous->empty()))
    {
        clearRecovery(extension);
        throw std::runtime_error("Incomplete saved recovery state was discarded; try again");
    }
    return PendingChange{static_cast<ChoiceFormat>(*format), *hadPrevious != 0, previous, *target};
}

// Outside configuration files and installer-owned HKLM components: retain this
// user history across reset, uninstall/reinstall, bitness and installer changes.
const std::wstring installerDefaultsPath = L"Software\\VideoLAN\\VLC\\InstallerDefaults";

class AssociationLock
{
  public:
    AssociationLock()
        // HKCU is shared by the same user across console and remote sessions.
        : m_mutex(CreateMutexW(nullptr, FALSE,
                              (L"Global\\VLCFileAssociationsWriteLock-" + currentUserSid()).c_str()))
    {
        if (m_mutex.get() == nullptr)
            throwWindowsError("Creating the association lock", GetLastError());
        const DWORD result = WaitForSingleObject(m_mutex.get(), 30000);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        {
            if (result == WAIT_FAILED)
                throwWindowsError("Waiting for the association lock", GetLastError());
            throw AssociationLockBusy("Another association update is still running");
        }
        // WAIT_ABANDONED still grants ownership. The journal, not the mutex's
        // state, determines which interrupted registry writes need recovery.
        m_owned = true;
    }

    ~AssociationLock()
    {
        if (m_owned)
            ReleaseMutex(m_mutex.get());
    }

  private:
    Handle m_mutex;
    bool m_owned = false;
};

void removeChoice(std::wstring_view extension, ChoiceFormat format,
                  const ChoiceSnapshot *expected = nullptr)
{
    if (expected)
        requireChoiceSnapshot(extension, format, *expected);
    deleteUserTree(format == ChoiceFormat::latest ? latestChoicePath(extension)
                                                  : legacyChoicePath(extension));
    const std::wstring parent = extensionRoot(extension);
    if (keyExists(HKEY_CURRENT_USER, parent))
        flushUserKey(parent);
    notifyAssociationChange();
}

void recoverPending(std::wstring_view extension, const std::wstring &vlcPath,
                    detail::HashScheme scheme)
{
    const auto pending = loadRecovery(extension);
    if (!pending)
        return;

    const ChoiceFormat activeFormat = scheme == detail::HashScheme::latest
                                          ? ChoiceFormat::latest
                                          : ChoiceFormat::legacy;
    if (pending->format != activeFormat)
    {
        // Windows migrated formats while the operation was interrupted. Its
        // newly active choice is authoritative, so the old journal is stale.
        clearRecovery(extension);
        return;
    }

    const auto observed = snapshotChoice(extension, pending->format);
    const auto &current = observed.programId;
    const bool isTarget =
        current && _wcsicmp(current->c_str(), pending->targetProgramId.c_str()) == 0;
    // A rollback can itself be interrupted between ProgId and Hash. The old
    // ProgId is therefore also ours to repair, not automatically an external win.
    const bool isPrevious = pending->hadPrevious && current &&
        _wcsicmp(current->c_str(), pending->previousProgramId->c_str()) == 0;
    const bool currentValid = (isTarget || isPrevious) &&
                             (pending->format == ChoiceFormat::latest
                                  ? latestChoiceValid(extension, *current)
                                  : legacyChoiceValid(extension, *current)) &&
                             (!isTarget || currentVlcIsEffective(extension, vlcPath));

    const detail::RecoveryAction action = detail::chooseRecoveryAction(
        current.has_value(), isTarget || isPrevious, currentValid, pending->hadPrevious);

    // A valid owned record can be a completed forward write or rollback. Persist
    // it before forgetting the journal. An unrelated ProgId needs no repair and
    // is left untouched; these branches must not rewrite its hash.
    if (action == detail::RecoveryAction::clear)
    {
        if (currentValid)
            flushUserKey(pending->format == ChoiceFormat::latest
                             ? latestChoicePath(extension) : legacyChoicePath(extension));
        clearRecovery(extension);
        return;
    }
    if (action == detail::RecoveryAction::remove)
    {
        removeChoice(extension, pending->format, &observed);
        clearRecovery(extension);
        return;
    }

    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    preflightChoice(extension, pending->format, *pending->previousProgramId, time);
    writeChoice(extension, pending->format, *pending->previousProgramId, false, vlcPath, &observed);
    clearRecovery(extension);
}

std::optional<DWORD> latestHashVersion()
{
    const std::wstring path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Sys"
                              L"temProtectedUserData\\" +
                              currentUserSid() + L"\\AnyoneRead\\AppDefaults";
    return readDword(HKEY_LOCAL_MACHINE, path, L"HashVersion", RRF_SUBKEY_WOW6464KEY);
}

detail::HashScheme hashScheme(bool latestEntryExists)
{
    const auto version = latestHashVersion();
    return detail::classifyHashScheme(version.has_value(), version.value_or(0),
                                      latestEntryExists);
}

// Transaction order: snapshot -> preflight target AND backup -> durable journal
// -> timestamp-aware write -> verified, persisted choice -> journal removal.
// Preflight uses a sample time to check calculability, not to prepare the final
// hash. Rollback regenerates the backup hash or removes a first-time choice;
// an incomplete rollback retains the ready journal for the next update.
void setChoice(std::wstring_view extension, ChoiceFormat format,
               const std::wstring &programId, const std::wstring &vlcPath)
{
    const auto before = snapshotChoice(extension, format);
    const bool hadPrevious = before.exists;
    const auto &previous = before.programId;
    if (hadPrevious && (!previous || previous->empty()))
        throw NeedsUserChoice("The current Windows default cannot be backed up safely");

    if (format == ChoiceFormat::legacy && previous)
    {
        // A non-ASCII legacy backup cannot be regenerated by this mixer.
        for (const wchar_t character : *previous)
        {
            if (character > 0x7f)
                throw NeedsUserChoice("The current Windows default cannot be backed up safely");
        }
    }

    if (hadPrevious)
    {
        // Regenerating a hash would activate a previously ignored record. Only
        // back up a valid choice Windows actually uses; otherwise leave it alone.
        const bool valid = format == ChoiceFormat::latest
                               ? latestChoiceValid(extension, *previous)
                               : legacyChoiceValid(extension, *previous);
        const auto effective = valid ? effectiveProgramId(extension) : std::nullopt;
        if (!effective || _wcsicmp(effective->c_str(), previous->c_str()) != 0)
            throw NeedsUserChoice("The current Windows default cannot be backed up safely");
    }

    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    preflightChoice(extension, format, programId, time);
    if (hadPrevious)
        preflightChoice(extension, format, *previous, time);

    // First-time rollback removes only the new entry. Existing-choice rollback
    // regenerates the previous application's hash; an old timestamp/hash is not reusable.
    saveRecovery(extension, format, hadPrevious, previous, programId);
    try
    {
        writeChoice(extension, format, programId, true, vlcPath, &before);
    }
    catch (const ChoiceConflict &)
    {
        // Detected interference must not trigger rollback, including same-ProgId
        // changes. Never invent a valid hash for an external record.
        clearRecovery(extension);
        throw;
    }
    catch (const std::exception &original)
    {
        const std::string message = original.what();
        bool newerDefault = false;
        try
        {
            const auto observed = snapshotChoice(extension, format);
            newerDefault = observed.programId &&
                _wcsicmp(observed.programId->c_str(), programId.c_str()) != 0;
            if (!newerDefault)
            {
                if (hadPrevious)
                    writeChoice(extension, format, *previous, false, vlcPath, &observed);
                else
                    removeChoice(extension, format, &observed);
            }
            clearRecovery(extension);
        }
        catch (const std::exception &rollback)
        {
            // Keep the ready journal until a later recovery can finish.
            throw std::runtime_error(message + ". Rollback failed: " + rollback.what());
        }
        throw NeedsUserChoice(message + (newerDefault ? ". A newer default was left unchanged"
                                                     : ". The previous default was restored"));
    }
    clearRecovery(extension);
}

void setLegacy(std::wstring_view extension, const std::wstring &programId,
               const std::wstring &vlcPath)
{
    setChoice(extension, ChoiceFormat::legacy, programId, vlcPath);
}

void setLatest(std::wstring_view extension, const std::wstring &programId,
               const std::wstring &vlcPath)
{
    setChoice(extension, ChoiceFormat::latest, programId, vlcPath);
}

AssociationResult failureResult(AssociationStatus status, const char *message) noexcept
{
    try
    {
        return {status, message};
    }
    catch (...)
    {
        // Even allocating error text can fail. Honour the public no-throw contract.
        return {AssociationStatus::failed, {}};
    }
}
} // namespace

AssociationResult setDefaultAssociation(std::wstring_view extension) noexcept
{
    try
    {
        if (!validExtension(extension))
            throw std::invalid_argument("Expected a file extension such as .mp4");

        const AssociationLock lock;
        const std::wstring vlcPath = currentExecutable();
        const bool hasLatestEntry = keyExists(HKEY_CURRENT_USER, latestChoicePath(extension));
        const detail::HashScheme scheme = hashScheme(hasLatestEntry);
        if (scheme == detail::HashScheme::unsupported)
            throw NeedsUserChoice("Windows uses an unsupported default-app format");
        // Recovery precedes registration and idempotence checks: a valid current
        // choice can still have a journal awaiting its final persistence/cleanup.
        recoverPending(extension, vlcPath, scheme);

        const auto programId = findVlcProgramId(extension, vlcPath);
        if (!programId)
            throw std::runtime_error("This VLC installation is not registered for " +
                                     ascii(extension, "File extension"));

        // A self-consistent record alone does not establish Windows acceptance.
        if (choiceIsDefault(extension, scheme, *programId, vlcPath))
            return {AssociationStatus::unchanged, {}};

        if (scheme == detail::HashScheme::latest)
        {
            setLatest(extension, *programId, vlcPath);
        }
        else
        {
            setLegacy(extension, *programId, vlcPath);
        }
        return {AssociationStatus::changed, {}};
    }
    catch (const AssociationLockBusy &error)
    {
        auto result = failureResult(AssociationStatus::failed, error.what());
        result.lockBusy = true;
        return result;
    }
    catch (const NeedsUserChoice &error)
    {
        return failureResult(AssociationStatus::needsUserChoice, error.what());
    }
    catch (const std::exception &error)
    {
        return failureResult(AssociationStatus::failed, error.what());
    }
    catch (...)
    {
        return {AssociationStatus::failed, {}};
    }
}

bool isDefaultAssociation(std::wstring_view extension) noexcept
{
    try
    {
        if (!validExtension(extension))
            return false;

        const std::wstring vlcPath = currentExecutable();
        const bool hasLatestEntry = keyExists(HKEY_CURRENT_USER, latestChoicePath(extension));
        const detail::HashScheme scheme = hashScheme(hasLatestEntry);
        if (scheme == detail::HashScheme::unsupported ||
            (scheme == detail::HashScheme::latest && !hasLatestEntry))
            return false;

        const auto programId = findVlcProgramId(extension, vlcPath);
        if (!programId)
            return false;
        return choiceIsDefault(extension, scheme, *programId, vlcPath);
    }
    catch (...)
    {
        return false;
    }
}
AssociationBatchResult setInstallerDefaultAssociations(const std::vector<std::wstring> &extensions)
{
    if (extensions.empty())
        throw std::invalid_argument("Installer association selection is empty");
    for (const auto &extension : extensions)
        if (!validExtension(extension))
            throw std::invalid_argument("Invalid installer association selection");

    try
    {
        // Windows mutex ownership is recursive on this thread. Hold the existing
        // per-user lock across the whole operation, including each transaction's
        // nested lock, so another VLC installer cannot pass the completion check.
        const AssociationLock lock;
        // Historical success suppresses new default requests, not cleanup of an
        // earlier interrupted transaction (possibly from a different selection).
        // Recover only existing journals for this request, using the same
        // ownership/snapshot checks that preserve later user choices.
        for (const auto &extension : extensions)
        {
            if (!keyExists(HKEY_CURRENT_USER, recoveryPath(extension)))
                continue;
            const auto scheme = hashScheme(keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)));
            if (scheme == detail::HashScheme::unsupported)
                throw NeedsUserChoice("Windows uses an unsupported default-app format");
            recoverPending(extension, currentExecutable(), scheme);
        }
        const auto completed = readDword(HKEY_CURRENT_USER, installerDefaultsPath, L"Completed");
        if (completed)
        {
            if (*completed != 1)
                throw std::runtime_error("Unrecognized installer association completion record");
            AssociationBatchResult batch;
            batch.alreadyCompleted = true;
            return batch; // Recovery above is cleanup only; never request new defaults.
        }

        auto batch = setDefaultAssociations(extensions);
        if (!batch.failures.empty() || !batch.needsUserChoice.empty() ||
            batch.stopReason != AssociationBatchStopReason::none)
            return batch;
        // Per-extension success is not an atomic batch. Recheck the entire
        // selection before recording success; Settings does not share our lock.
        for (const auto &extension : extensions)
            if (keyExists(HKEY_CURRENT_USER, recoveryPath(extension)) || !isDefaultAssociation(extension))
                throw std::runtime_error("Installer association completion could not be verified");

        const RegistryKey key = createUserKey(installerDefaultsPath);
        writeDword(key.get(), L"Completed", 1);
        const LSTATUS status = RegFlushKey(key.get());
        if (status != ERROR_SUCCESS)
        {
            // Do not leave a visible success value after failed persistence.
            RegDeleteValueW(key.get(), L"Completed");
            RegFlushKey(key.get());
            throwWindowsError("Persisting installer association completion", status);
        }
        return batch;
    }
    catch (const AssociationLockBusy &error)
    {
        AssociationBatchResult batch;
        batch.stopReason = AssociationBatchStopReason::writerBusy;
        batch.skipped = static_cast<unsigned>(extensions.size());
        for (const auto &extension : extensions)
            batch.failures.emplace_back(extension, error.what());
        return batch;
    }
}
} // namespace vlc::win32
