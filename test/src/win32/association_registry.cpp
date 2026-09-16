/*****************************************************************************
 * test_win32_association_registry.cpp: Windows association transaction tests
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See ../../../src/win32/associations/LICENSE for the full license text.
 *****************************************************************************/

#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include "win32/associations/userchoice_latest_hash/HashCommon.h"

#include <cwchar>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
bool failNextHash = false;
unsigned hashFailuresRemaining = 0;
unsigned choiceWrites = 0;
bool failNextProgramId = false;
bool failNextDelete = false;
bool throwNextRead = false;
unsigned minuteOffset = 0;
unsigned choiceMinuteOffset = 0;
unsigned flushesUntilFailure = 0;
unsigned flushFailuresRemaining = 0;
bool timeoutNextAssociationWait = false;
DWORD associationWaitMilliseconds = 0;
std::function<void()> onHashFailure;
std::function<void()> onProgramIdWritten;
std::function<void()> onHashCalculation;
unsigned calculationsUntilInterleave = 0;
std::function<std::optional<std::wstring>(std::wstring_view)> onDefaultQuery;

// Control only the Windows acceptance query. The opt-in smoke test below this
// suite's scope exercises the actual COM query and Shell on a registered fixture.
class DefaultQuery final : public IApplicationAssociationRegistration
{
    ULONG refs = 1;
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **output) override
    {
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IApplicationAssociationRegistration)
            return E_NOINTERFACE;
        *output = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --refs;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryCurrentDefault(LPCWSTR name, ASSOCIATIONTYPE type,
                                                  ASSOCIATIONLEVEL level, LPWSTR *output) override
    {
        *output = nullptr;
        if (type != AT_FILEEXTENSION || level != AL_EFFECTIVE || !onDefaultQuery)
            return E_FAIL;
        const auto value = onDefaultQuery(name);
        if (!value) return E_FAIL;
        const size_t bytes = (value->size() + 1) * sizeof(wchar_t);
        *output = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (*output == nullptr) return E_OUTOFMEMORY;
        std::memcpy(*output, value->c_str(), bytes);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryAppIsDefault(LPCWSTR, ASSOCIATIONTYPE, ASSOCIATIONLEVEL,
                                                LPCWSTR, BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryAppIsDefaultAll(ASSOCIATIONLEVEL, LPCWSTR, BOOL *) override
    { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetAppAsDefault(LPCWSTR, LPCWSTR, ASSOCIATIONTYPE) override
    { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetAppAsDefaultAll(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ClearUserAssociations() override { return E_NOTIMPL; }
};

HRESULT WINAPI testCoCreateInstance(REFCLSID clsid, IUnknown *outer, DWORD context,
                                   REFIID iid, void **output)
{
    *output = nullptr;
    if (clsid != CLSID_ApplicationAssociationRegistration || outer != nullptr ||
        context != CLSCTX_INPROC_SERVER || iid != IID_IApplicationAssociationRegistration)
        return E_NOINTERFACE;
    *output = new DefaultQuery;
    return S_OK;
}

HRESULT WINAPI testAssocQueryString(ASSOCF, ASSOCSTR type, LPCWSTR association, LPCWSTR,
                                   LPWSTR output, DWORD *size)
{
    if (type != ASSOCSTR_EXECUTABLE) return E_FAIL;
    const wchar_t *path = std::wcsncmp(association, L"VLC.", 4) == 0 ? L"unused" : L"other";
    const DWORD needed = DWORD(std::wcslen(path) + 1);
    if (output == nullptr) { *size = needed; return S_FALSE; }
    if (*size < needed) { *size = needed; return E_POINTER; }
    std::wcscpy(output, path);
    *size = needed;
    return S_OK;
}

DWORD WINAPI testWaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
    associationWaitMilliseconds = milliseconds;
    if (timeoutNextAssociationWait)
    {
        timeoutNextAssociationWait = false;
        return WAIT_TIMEOUT;
    }
    return WaitForSingleObject(handle, milliseconds);
}

LSTATUS WINAPI testSetValue(HKEY key, LPCWSTR name, DWORD reserved,
                           DWORD type, const BYTE *data, DWORD bytes)
{
    if (name != nullptr && (std::wcscmp(name, L"ProgId") == 0 || std::wcscmp(name, L"Hash") == 0))
        ++choiceWrites;
    if (failNextProgramId && name != nullptr && std::wcscmp(name, L"ProgId") == 0)
    {
        failNextProgramId = false;
        return ERROR_ACCESS_DENIED;
    }
    if ((failNextHash || hashFailuresRemaining != 0) &&
        name != nullptr && std::wcscmp(name, L"Hash") == 0)
    {
        failNextHash = false;
        if (hashFailuresRemaining != 0)
            --hashFailuresRemaining;
        if (onHashFailure)
            onHashFailure();
        return ERROR_ACCESS_DENIED;
    }
    const LSTATUS status = RegSetValueExW(key, name, reserved, type, data, bytes);
    if (status == ERROR_SUCCESS && name != nullptr &&
        (std::wcscmp(name, L"ProgId") == 0 || std::wcscmp(name, L"Hash") == 0))
        choiceMinuteOffset = minuteOffset;
    if (status == ERROR_SUCCESS && name != nullptr &&
        std::wcscmp(name, L"ProgId") == 0 && onProgramIdWritten)
    {
        // Moving std::function need not empty it; consume the hook before reentry.
        auto callback = std::exchange(onProgramIdWritten, nullptr);
        callback();
    }
    return status;
}

LSTATUS WINAPI testFlushKey(HKEY key)
{
    if (flushesUntilFailure != 0)
    {
        if (--flushesUntilFailure != 0)
            return RegFlushKey(key);
        if (flushFailuresRemaining != 0)
            --flushFailuresRemaining;
        return ERROR_WRITE_FAULT;
    }
    if (flushFailuresRemaining != 0)
    {
        --flushFailuresRemaining;
        return ERROR_WRITE_FAULT;
    }
    return RegFlushKey(key);
}

LSTATUS WINAPI testDeleteTree(HKEY key, LPCWSTR path)
{
    if (failNextDelete)
    {
        failNextDelete = false;
        return ERROR_ACCESS_DENIED;
    }
    return RegDeleteTreeW(key, path);
}

LSTATUS WINAPI testGetValue(HKEY key, LPCWSTR path, LPCWSTR name, DWORD flags,
                           LPDWORD type, PVOID data, LPDWORD bytes)
{
    if (throwNextRead)
    {
        throwNextRead = false;
        throw 42; // Exercise the public API's catch-all, not just std::exception.
    }
    return RegGetValueW(key, path, name, flags, type, data, bytes);
}

void offsetTime(FILETIME *time, unsigned offset)
{
    const auto ticks = (static_cast<std::uint64_t>(time->dwHighDateTime) << 32) |
                       time->dwLowDateTime;
    const auto shifted = ticks + static_cast<std::uint64_t>(offset) * 600000000ULL;
    time->dwLowDateTime = static_cast<DWORD>(shifted);
    time->dwHighDateTime = static_cast<DWORD>(shifted >> 32);
}

void WINAPI testSystemTime(FILETIME *time)
{
    GetSystemTimeAsFileTime(time);
    offsetTime(time, minuteOffset);
}

LSTATUS WINAPI testQueryInfoKey(HKEY key, LPWSTR name, LPDWORD nameSize, LPDWORD reserved,
                               LPDWORD subkeys, LPDWORD maxSubkey, LPDWORD maxClass,
                               LPDWORD values, LPDWORD maxValueName, LPDWORD maxValue,
                               LPDWORD security, PFILETIME time)
{
    const LSTATUS status = RegQueryInfoKeyW(key, name, nameSize, reserved, subkeys, maxSubkey,
                                           maxClass, values, maxValueName, maxValue, security, time);
    if (status == ERROR_SUCCESS && time != nullptr)
        offsetTime(time, choiceMinuteOffset);
    return status;
}

void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
}

namespace UserChoiceLatestHash
{
bool failNextCalculation = false;
unsigned calculationsUntilFailure = 0;
bool testComputeHash(const std::wstring &input, const WorkingSeeds &seeds,
                     bool lowercase, std::wstring *output, DebugData *debug)
{
    if (failNextCalculation ||
        (calculationsUntilFailure != 0 && --calculationsUntilFailure == 0))
    {
        failNextCalculation = false;
        return false;
    }
    const bool calculated = ComputeHash(input, seeds, lowercase, output, debug);
    if (calculated && calculationsUntilInterleave != 0 && --calculationsUntilInterleave == 0)
    {
        auto callback = std::move(onHashCalculation);
        callback();
    }
    return calculated;
}
}

// Compile the real private transaction code into this test translation unit.
// Fault hooks never enter the plugin, and no test-only public API is needed.
// Registry operations remain real except at selected failure/time boundaries;
// effective-default/command queries are controlled separately from hash validity.
// Crash cases construct interrupted records and invoke production recovery;
// they test persistence ordering/repair, not a physical power-loss experiment.
#define RegSetValueExW testSetValue
#define RegFlushKey testFlushKey
#define RegDeleteTreeW testDeleteTree
#define ComputeHash testComputeHash
#define RegGetValueW testGetValue
#define RegQueryInfoKeyW testQueryInfoKey
#define GetSystemTimeAsFileTime testSystemTime
#define CoCreateInstance testCoCreateInstance
#define AssocQueryStringW testAssocQueryString
#define WaitForSingleObject testWaitForSingleObject
#include "win32/associations/win32_associations.cpp"
#undef WaitForSingleObject
#undef AssocQueryStringW
#undef CoCreateInstance
#undef GetSystemTimeAsFileTime
#undef RegQueryInfoKeyW
#undef RegGetValueW
#undef ComputeHash
#undef RegFlushKey
#undef RegDeleteTreeW
#undef RegSetValueExW

int main()
{
    using namespace vlc::win32;
    // No real media extensions or registrations are modified. Refuse collisions
    // before touching anything, and remove only these two unique fixture roots.
    const std::wstring extension = L".vlc-choice-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::wstring base = extensionRoot(extension);
    const std::wstring journal = recoveryPath(extension);
    if (keyExists(HKEY_CURRENT_USER, base) || keyExists(HKEY_CURRENT_USER, journal))
        return 1;

    bool rejectTarget = false;
    bool queryUnavailable = false;
    std::optional<std::wstring> effectiveOverride;
    onDefaultQuery = [&](std::wstring_view name) -> std::optional<std::wstring> {
        if (queryUnavailable) return std::nullopt;
        if (effectiveOverride) return effectiveOverride;
        auto current = readString(HKEY_CURRENT_USER, latestChoicePath(name) + L"\\ProgId", L"ProgId");
        if (!current) current = readString(HKEY_CURRENT_USER, legacyChoicePath(name), L"ProgId");
        if (rejectTarget && current && *current == L"VLC.mp4") return L"Other.Player";
        return current;
    };
    const auto cleanup = [&]() {
        rejectTarget = false;
        queryUnavailable = false;
        effectiveOverride.reset();
        failNextHash = false;
        hashFailuresRemaining = 0;
        UserChoiceLatestHash::failNextCalculation = false;
        UserChoiceLatestHash::calculationsUntilFailure = 0;
        failNextProgramId = false;
        failNextDelete = false;
        throwNextRead = false;
        minuteOffset = 0;
        choiceMinuteOffset = 0;
        flushesUntilFailure = 0;
        flushFailuresRemaining = 0;
        timeoutNextAssociationWait = false;
        associationWaitMilliseconds = 0;
        onHashFailure = {};
        onProgramIdWritten = {};
        onHashCalculation = {};
        calculationsUntilInterleave = 0;
        deleteUserTree(base);
        deleteUserTree(journal);
        notifyAssociationChange();
    };
    const auto expectFailure = [&](const auto &operation) {
        bool failed = false;
        try { operation(); }
        catch (const std::exception &) { failed = true; }
        require(failed, "Injected failure was ignored");
    };
    const auto expectStaleSnapshotRejected = [&](ChoiceFormat format) {
        // A stale observation must not authorize deletion/restoration, including
        // an external reselection of the same ProgId with a new invalid hash.
        writeChoice(extension, format, L"VLC.mp3", false, L"unused");
        const auto stale = snapshotChoice(extension, format);
        const std::wstring root = format == ChoiceFormat::latest ? latestChoicePath(extension)
                                                                  : legacyChoicePath(extension);
        writeString(createUserKey(root).get(), L"Hash", L"external");
        choiceWrites = 0;
        expectFailure([&]() { removeChoice(extension, format, &stale); });
        expectFailure([&]() { writeChoice(extension, format, L"VLC.mp4", false, L"unused", &stale); });
        require(choiceWrites == 0 && readString(HKEY_CURRENT_USER, root, L"Hash") == L"external",
                "Stale snapshot authorized mutation of an external record");
    };
    const auto denyChoiceWrites = [&](const std::wstring &path) {
        // Deny only value writes on this disposable key; retain read/delete
        // access so cleanup and the legacy delete/recreate path remain possible.
        const std::wstring sid = currentUserSid();
        const std::wstring sddl = L"D:P(D;;0x2;;;" + sid + L")(A;;KA;;;" + sid + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr), "Test DACL creation failed");
        const LocalMemory allocation(descriptor);
        HKEY rawKey = nullptr;
        require(RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, WRITE_DAC, &rawKey) ==
                ERROR_SUCCESS, "Test DACL key open failed");
        const RegistryKey key(rawKey);
        require(RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION, descriptor) ==
                ERROR_SUCCESS, "Test DACL update failed");
        rawKey = nullptr;
        const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
                                            KEY_SET_VALUE, &rawKey);
        const RegistryKey denied(rawKey);
        require(status == ERROR_ACCESS_DENIED, "Test DACL did not deny value writes");
    };

    try
    {
        const AssociationLock lock;

        // Legacy rollback/recovery does not need latest-format metadata. Run it
        // before checking that metadata so older Windows keeps this coverage.
        writeLegacyChoice(extension, L"VLC.mp3", false, L"unused");
        // Simulate a later minute for system and registry timestamps together;
        // no machine clock or real default is changed. Reusing the old hash
        // would now fail, including after an interrupted rollback.
        minuteOffset = 2;
        hashFailuresRemaining = 2;
        expectFailure([&]() { setLegacy(extension, L"VLC.mp4", L"unused"); });
        require(keyExists(HKEY_CURRENT_USER, journal) && !legacyChoiceValid(extension, L"VLC.mp3"),
                "Legacy rollback fault was not exercised");
        recoverPending(extension, L"unused", detail::HashScheme::legacy);
        require(legacyChoiceValid(extension, L"VLC.mp3") && !keyExists(HKEY_CURRENT_USER, journal),
                "Legacy rollback recovery did not regenerate its timestamp-dependent hash");
        minuteOffset = 0;
        writeLegacyChoice(extension, L"VLC.mp3", false, L"unused");
        failNextHash = true;
        expectFailure([&]() { setLegacy(extension, L"VLC.mp4", L"unused"); });
        require(legacyChoiceValid(extension, L"VLC.mp3"), "Legacy rollback lost the previous choice");
        failNextHash = true;
        onHashFailure = [&]() { writeLegacyChoice(extension, L"Other.Player", false, L"unused"); };
        expectFailure([&]() { setLegacy(extension, L"VLC.mp4", L"unused"); });
        onHashFailure = {};
        require(legacyChoiceValid(extension, L"Other.Player"), "Legacy rollback overwrote another app");
        writeLegacyChoice(extension, L"VLC.mp3", false, L"unused");
        onProgramIdWritten = [&]() { writeLegacyChoice(extension, L"Other.Player", false, L"unused"); };
        expectFailure([&]() { setLegacy(extension, L"VLC.mp4", L"unused"); });
        require(legacyChoiceValid(extension, L"Other.Player") && !keyExists(HKEY_CURRENT_USER, journal),
                "Legacy Hash write corrupted a concurrent choice");
        {
            const RegistryKey legacy = createUserKey(legacyChoicePath(extension));
            writeString(legacy.get(), L"ProgId", L"Player.\u0100");
            writeString(legacy.get(), L"Hash", L"untouched");
        }
        expectFailure([&]() { setLegacy(extension, L"VLC.mp4", L"unused"); });
        require(choiceMatches(legacyChoicePath(extension), L"Player.\u0100", L"untouched") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Unsupported legacy backup was modified");

        // Invalid/missing hashes and OS-ignored records are not safe backups.
        for (const wchar_t *hash : {L"invalid", static_cast<const wchar_t *>(nullptr)})
        {
            writeLegacyChoice(extension, L"Other.Player", false, L"unused");
            const RegistryKey key = createUserKey(legacyChoicePath(extension));
            if (hash) writeString(key.get(), L"Hash", hash);
            else require(RegDeleteValueW(key.get(), L"Hash") == ERROR_SUCCESS, "Removing fixture hash failed");
            choiceWrites = 0;
            expectFailure([&]() { setLegacy(extension, L"VLC.mp4", L"unused"); });
            require(choiceWrites == 0 && !legacyChoiceValid(extension, L"Other.Player") &&
                    !keyExists(HKEY_CURRENT_USER, journal), "Invalid legacy backup was activated");
        }
        writeLegacyChoice(extension, L"Other.Player", false, L"unused");
        effectiveOverride = L"Fallback.Player";
        choiceWrites = 0;
        expectFailure([&]() { setLegacy(extension, L"VLC.mp4", L"unused"); });
        effectiveOverride.reset();
        require(choiceWrites == 0 && legacyChoiceValid(extension, L"Other.Player") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Ignored legacy backup was modified");

        expectStaleSnapshotRejected(ChoiceFormat::legacy);

        // Real permissions, not an injected failure: legacy can recreate a
        // value-protected key without losing its timestamp/hash consistency.
        writeLegacyChoice(extension, L"VLC.mp3", false, L"unused");
        denyChoiceWrites(legacyChoicePath(extension));
        writeLegacyChoice(extension, L"VLC.mp4", false, L"unused");
        require(legacyChoiceValid(extension, L"VLC.mp4"), "Protected legacy key was not recreated");

        cleanup();
        require(!keyExists(HKEY_CURRENT_USER, base) && !keyExists(HKEY_CURRENT_USER, journal),
                "Legacy registry cleanup failed");
        std::cout << "Legacy association transaction tests passed\n";

        // The compatibility writer requires the published MachineID and format.
        // Only latest-format cases are unavailable on other Windows builds;
        // the legacy cases above have run, so do not skip the entire executable.
        if (hashScheme(false) != detail::HashScheme::latest)
        {
            std::cout << "UserChoiceLatest cases skipped: format unavailable\n";
            return 0;
        }

        // A self-consistent latest record must not count as Windows acceptance.
        rejectTarget = true;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(!keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)) &&
                !keyExists(HKEY_CURRENT_USER, journal), "Rejected fresh latest write reported success");
        queryUnavailable = true;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        queryUnavailable = false;
        rejectTarget = false;
        require(!keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)) &&
                !keyExists(HKEY_CURRENT_USER, journal), "Unavailable Windows query reported success");

        setLatest(extension, L"VLC.mp4", L"unused");
        require(latestChoiceValid(extension, L"VLC.mp4"), "Fresh choice/hash invalid");
        require(!keyExists(HKEY_CURRENT_USER, journal), "Success retained its journal");

        require(choiceIsDefault(extension, detail::HashScheme::latest, L"VLC.mp4", L"unused"),
                "Accepted latest choice was not reported as default");
        rejectTarget = true;
        require(!choiceIsDefault(extension, detail::HashScheme::latest, L"VLC.mp4", L"unused"),
                "Self-valid latest record was reported as an effective default");
        choiceWrites = 0;
        expectFailure([&]() { setLatest(extension, L"VLC.mp3", L"unused"); });
        require(choiceWrites == 0 && latestChoiceValid(extension, L"VLC.mp4") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Ignored latest backup was modified");
        rejectTarget = false;
        for (const wchar_t *hash : {L"invalid", static_cast<const wchar_t *>(nullptr)})
        {
            writeLatestChoice(extension, L"Other.Player");
            const RegistryKey key = createUserKey(latestChoicePath(extension));
            if (hash) writeString(key.get(), L"Hash", hash);
            else require(RegDeleteValueW(key.get(), L"Hash") == ERROR_SUCCESS, "Removing fixture hash failed");
            choiceWrites = 0;
            expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
            require(choiceWrites == 0 && !latestChoiceValid(extension, L"Other.Player") &&
                    !keyExists(HKEY_CURRENT_USER, journal), "Invalid latest backup was activated");
        }
        writeLatestChoice(extension, L"Other.Player");
        rejectTarget = true;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        rejectTarget = false;
        require(latestChoiceValid(extension, L"Other.Player") && !keyExists(HKEY_CURRENT_USER, journal),
                "Rejected latest write did not restore the valid backup");

        setLatest(extension, L"VLC.mp3", L"unused");
        // Both the forward and restore writes fail after changing ProgId. The
        // journal must survive, and the next recovery must repair the old hash.
        Sleep(20);
        hashFailuresRemaining = 2;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(keyExists(HKEY_CURRENT_USER, journal), "Failed rollback lost its journal");
        require(!latestChoiceValid(extension, L"VLC.mp3"), "Rollback fault was not exercised");
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(latestChoiceValid(extension, L"VLC.mp3") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Interrupted rollback was not repaired");

        choiceWrites = 0;
        UserChoiceLatestHash::failNextCalculation = true;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(choiceWrites == 0 && latestChoiceValid(extension, L"VLC.mp3") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Hash failure mutated the previous choice");

        choiceWrites = 0;
        UserChoiceLatestHash::calculationsUntilFailure = 3; // Backup validation, target, then previous preflight.
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(choiceWrites == 0 && latestChoiceValid(extension, L"VLC.mp3") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Unrestorable backup was modified");

        UserChoiceLatestHash::calculationsUntilFailure = 4; // Validation and both preflights precede the actual hash.
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(latestChoiceValid(extension, L"VLC.mp3") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Post-write calculation failure lost backup");

        failNextProgramId = true;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(latestChoiceValid(extension, L"VLC.mp3") &&
                !keyExists(HKEY_CURRENT_USER, journal), "ProgId write failure changed the choice");

        failNextHash = true;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(latestChoiceValid(extension, L"VLC.mp3"), "Previous choice not restored");
        require(!keyExists(HKEY_CURRENT_USER, journal), "Rollback retained its journal");

        // Two journal flushes precede the choice flush. A failed third flush
        // must roll back, not report success and discard the durable backup.
        flushesUntilFailure = 3;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(flushesUntilFailure == 0, "Choice was not flushed before journal deletion");
        require(latestChoiceValid(extension, L"VLC.mp3"), "Flush failure lost the previous choice");

        // Valid data is not necessarily durable. Failed persistence during both
        // writes retains the journal, including another failed recovery flush.
        flushesUntilFailure = 3;
        flushFailuresRemaining = 2;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(latestChoiceValid(extension, L"VLC.mp3") && keyExists(HKEY_CURRENT_USER, journal),
                "Unflushed rollback lost its journal");
        flushesUntilFailure = 1;
        expectFailure([&]() { recoverPending(extension, L"unused", detail::HashScheme::latest); });
        require(keyExists(HKEY_CURRENT_USER, journal), "Failed recovery flush lost its journal");
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(latestChoiceValid(extension, L"VLC.mp3") && !keyExists(HKEY_CURRENT_USER, journal),
                "Durable rollback recovery failed");

        failNextHash = true;
        // Deterministic interleaving: another writer publishes a different choice
        // before rollback reads it. This checks preservation, not lock cooperation.
        onHashFailure = [&]() { writeLatestChoice(extension, L"Other.Player"); };
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        onHashFailure = {};
        require(latestChoiceValid(extension, L"Other.Player"), "Rollback overwrote another app");
        require(!keyExists(HKEY_CURRENT_USER, journal), "Concurrent choice retained journal");

        // Complete an external pair while the actual post-ProgId hash is being
        // calculated, or during preflight before our first mutation.
        for (unsigned interleave : {4U, 2U})
        {
            writeLatestChoice(extension, L"VLC.mp3");
            calculationsUntilInterleave = interleave; // Validation, target, backup, actual hash.
            onHashCalculation = [&]() { writeLatestChoice(extension, L"Other.Player"); };
            expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
            require(latestChoiceValid(extension, L"Other.Player") &&
                    !keyExists(HKEY_CURRENT_USER, journal),
                    "Concurrent Hash write corrupted another application's choice");
        }

        // ProgId alone is not ownership: a newer same-ProgId choice has its own
        // timestamp/hash and must not be corrupted or rolled back to the backup.
        writeLatestChoice(extension, L"VLC.mp3");
        calculationsUntilInterleave = 4;
        onHashCalculation = [&]() { Sleep(20); writeLatestChoice(extension, L"VLC.mp4"); };
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(latestChoiceValid(extension, L"VLC.mp4") && !keyExists(HKEY_CURRENT_USER, journal),
                "Newer same-ProgId choice was corrupted or rolled back");

        expectStaleSnapshotRejected(ChoiceFormat::latest);

        // Interference during recovery's preflight retains the journal and the
        // newer valid choice. The next recovery recognizes it as external.
        writeLatestChoice(extension, L"VLC.mp4");
        writeString(createUserKey(latestChoicePath(extension)).get(), L"Hash", L"invalid");
        saveRecovery(extension, ChoiceFormat::latest, true, L"VLC.mp3", L"VLC.mp4");
        calculationsUntilInterleave = 2; // Current validation, then restore preflight.
        onHashCalculation = [&]() { writeLatestChoice(extension, L"Other.Player"); };
        expectFailure([&]() { recoverPending(extension, L"unused", detail::HashScheme::latest); });
        require(latestChoiceValid(extension, L"Other.Player") && keyExists(HKEY_CURRENT_USER, journal),
                "Recovery overwrote a choice changed after its ownership check");
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(latestChoiceValid(extension, L"Other.Player") && !keyExists(HKEY_CURRENT_USER, journal),
                "External recovery choice was not preserved on retry");

        removeChoice(extension, ChoiceFormat::latest);
        {
            const RegistryKey legacy = createUserKey(legacyChoicePath(extension));
            writeString(legacy.get(), L"ProgId", L"Legacy.Player");
            writeString(legacy.get(), L"Hash", L"untouched");
        }
        failNextHash = true;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(!keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)),
                "First-time rollback left its new entry behind");
        require(choiceMatches(legacyChoicePath(extension), L"Legacy.Player", L"untouched"),
                "First-time rollback modified the legacy default");

        failNextHash = true;
        onHashFailure = [&]() { failNextDelete = true; };
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        onHashFailure = {};
        require(keyExists(HKEY_CURRENT_USER, journal), "Failed removal lost its journal");
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(!keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)) &&
                !keyExists(HKEY_CURRENT_USER, journal) &&
                choiceMatches(legacyChoicePath(extension), L"Legacy.Player", L"untouched"),
                "First-time removal recovery changed the legacy default");

        flushesUntilFailure = 3;
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(!keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)) &&
                !keyExists(HKEY_CURRENT_USER, journal), "First-time flush rollback incomplete");
        require(choiceMatches(legacyChoicePath(extension), L"Legacy.Player", L"untouched"),
                "First-time flush failure modified the legacy default");

        failNextHash = true;
        onHashFailure = [&]() { writeLatestChoice(extension, L"Other.Player"); };
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        onHashFailure = {};
        require(latestChoiceValid(extension, L"Other.Player") &&
                !keyExists(HKEY_CURRENT_USER, journal), "First-time rollback overwrote another app");
        removeChoice(extension, ChoiceFormat::latest);

        saveRecovery(extension, ChoiceFormat::latest, false, std::nullopt, L"VLC.mp4");
        {
            const RegistryKey program = createUserKey(latestChoicePath(extension) + L"\\ProgId");
            writeString(program.get(), L"ProgId", L"VLC.mp4");
        }
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(!keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)) &&
                !keyExists(HKEY_CURRENT_USER, journal), "Interrupted first-time write not removed");
        require(choiceMatches(legacyChoicePath(extension), L"Legacy.Player", L"untouched"),
                "Crash recovery modified the legacy default");

        saveRecovery(extension, ChoiceFormat::latest, true, L"Other.Player", L"VLC.mp4");
        {
            const RegistryKey program = createUserKey(latestChoicePath(extension) + L"\\ProgId");
            writeString(program.get(), L"ProgId", L"VLC.mp4");
        }
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(latestChoiceValid(extension, L"Other.Player"), "Crash recovery did not restore backup");

        saveRecovery(extension, ChoiceFormat::latest, true, L"VLC.mp3", L"VLC.mp4");
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(latestChoiceValid(extension, L"Other.Player"), "Crash recovery overwrote another app");
        require(!keyExists(HKEY_CURRENT_USER, journal), "Crash recovery retained journal");

        saveRecovery(extension, ChoiceFormat::latest, true, L"VLC.mp3", L"VLC.mp4");
        writeLatestChoice(extension, L"VLC.mp4");
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(latestChoiceValid(extension, L"VLC.mp4"), "Completed choice was rolled back");
        require(!keyExists(HKEY_CURRENT_USER, journal), "Completed choice retained journal");

        saveRecovery(extension, ChoiceFormat::latest, true, L"VLC.mp4", L"VLC.mp3");
        choiceWrites = 0;
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(choiceWrites == 0 && latestChoiceValid(extension, L"VLC.mp4") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Unstarted write modified its valid backup");

        // Recovery must not commit a self-valid target that Windows rejected.
        saveRecovery(extension, ChoiceFormat::latest, true, L"Other.Player", L"VLC.mp4");
        writeLatestChoice(extension, L"VLC.mp4");
        rejectTarget = true;
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        rejectTarget = false;
        require(latestChoiceValid(extension, L"Other.Player") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Rejected target was committed during recovery");
        writeLatestChoice(extension, L"VLC.mp4");

        for (const wchar_t *field : {L"Ready", L"Format", L"HadPrevious"})
        {
            saveRecovery(extension, ChoiceFormat::latest, true, L"VLC.mp3", L"VLC.mp4");
            { const RegistryKey key = createUserKey(journal); writeDword(key.get(), field, 9); }
            choiceWrites = 0;
            expectFailure([&]() { recoverPending(extension, L"unused", detail::HashScheme::latest); });
            require(choiceWrites == 0 && latestChoiceValid(extension, L"VLC.mp4") &&
                    !keyExists(HKEY_CURRENT_USER, journal), "Malformed journal changed the choice");
        }

        // Read/type errors are not authority to discard a potentially useful
        // backup. Leave the journal and the current choice untouched.
        saveRecovery(extension, ChoiceFormat::latest, true, L"VLC.mp3", L"VLC.mp4");
        { const RegistryKey key = createUserKey(journal); writeString(key.get(), L"Ready", L"bad"); }
        choiceWrites = 0;
        expectFailure([&]() { recoverPending(extension, L"unused", detail::HashScheme::latest); });
        require(choiceWrites == 0 && keyExists(HKEY_CURRENT_USER, journal) &&
                latestChoiceValid(extension, L"VLC.mp4"), "Unreadable journal was discarded");
        clearRecovery(extension);

        for (unsigned failedFlush : {1U, 2U})
        {
            flushesUntilFailure = failedFlush;
            expectFailure([&]() { setLatest(extension, L"VLC.mp3", L"unused"); });
            require(latestChoiceValid(extension, L"VLC.mp4"), "Journal failure changed the default");
            recoverPending(extension, L"unused", detail::HashScheme::latest);
            require(!keyExists(HKEY_CURRENT_USER, journal), "Unstarted journal was not cleared");
        }

        // An OS migration makes the inactive format's journal stale, not an
        // instruction to overwrite the newly active format.
        saveRecovery(extension, ChoiceFormat::latest, true, L"VLC.mp3", L"VLC.mp4");
        recoverPending(extension, L"unused", detail::HashScheme::legacy);
        require(latestChoiceValid(extension, L"VLC.mp4") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Format migration changed the choice");

        // Real key permissions, not an injected return code: latest must leave
        // a protected backup intact.
        denyChoiceWrites(latestChoicePath(extension) + L"\\ProgId");
        // DACL setup can change the key timestamp. Establish a valid protected
        // fixture before checking whether the backend leaves it unchanged.
        {
            const RegistryKey key = createUserKey(latestChoicePath(extension));
            writeString(key.get(), L"Hash", calculateLatestHash(extension, L"VLC.mp4",
                keyWriteTime(HKEY_CURRENT_USER, latestChoicePath(extension) + L"\\ProgId")));
        }
        require(latestChoiceValid(extension, L"VLC.mp4"), "Protected latest fixture is invalid");
        expectFailure([&]() { setLatest(extension, L"VLC.mp3", L"unused"); });
        require(latestChoiceValid(extension, L"VLC.mp4") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Protected latest backup was changed");
        // A crash before any choice key is created must also recover cleanly.
        deleteUserTree(base);
        saveRecovery(extension, ChoiceFormat::latest, false, std::nullopt, L"VLC.mp4");
        recoverPending(extension, L"unused", detail::HashScheme::latest);
        require(!keyExists(HKEY_CURRENT_USER, base) && !keyExists(HKEY_CURRENT_USER, journal),
                "Recovery of an unstarted write failed");

        { const RegistryKey malformed = createUserKey(latestChoicePath(extension)); }
        expectFailure([&]() { setLatest(extension, L"VLC.mp4", L"unused"); });
        require(keyExists(HKEY_CURRENT_USER, latestChoicePath(extension)) &&
                !keyExists(HKEY_CURRENT_USER, latestChoicePath(extension) + L"\\ProgId") &&
                !keyExists(HKEY_CURRENT_USER, journal), "Malformed choice was changed");

        require(setDefaultAssociation(L"..\\bad").status == AssociationStatus::failed &&
                !isDefaultAssociation(L"..\\bad"), "Invalid extension was accepted");

        choiceWrites = 0;
        timeoutNextAssociationWait = true;
        associationWaitMilliseconds = 0;
        const auto lockBusy = setDefaultAssociation(extension);
        require(lockBusy.status == AssociationStatus::failed && lockBusy.lockBusy &&
                !timeoutNextAssociationWait && associationWaitMilliseconds == 30000 &&
                choiceWrites == 0, "Lock timeout was not reported as contention");

        choiceWrites = 0;
        throwNextRead = true;
        require(setDefaultAssociation(extension).status == AssociationStatus::failed &&
                !throwNextRead && choiceWrites == 0, "Public no-throw contract failed");
        throwNextRead = true;
        require(!isDefaultAssociation(extension) && !throwNextRead && choiceWrites == 0,
                "Read-only query no-throw contract failed");

        cleanup();
        require(!keyExists(HKEY_CURRENT_USER, base) && !keyExists(HKEY_CURRENT_USER, journal),
                "Disposable registry cleanup failed");
        std::cout << "Windows association transaction tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Windows association transaction test failed: " << error.what() << '\n';
        try { cleanup(); }
        catch (const std::exception &cleanupError) { std::cerr << cleanupError.what() << '\n'; }
        return 1;
    }
}
