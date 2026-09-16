/*****************************************************************************
 * win32_association_smoke.cpp: opt-in Windows Shell acceptance check
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See ../../../../src/win32/associations/LICENSE for the full license text.
 *****************************************************************************/

// Exercise the production transaction writers without fault hooks. This runner
// is not VLC, so registration validation uses the explicitly supplied VLC path.
// Keep this opt-in: it writes disposable real default-app records, not mocks.
#include "win32/associations/win32_associations.cpp"

#include <iostream>

namespace
{
using namespace vlc::win32;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void probe(const std::wstring &extension, const std::wstring &expected)
{
    // A new process avoids the association cache of the process doing writes.
    std::wstring command = L"\"" + currentExecutable() + L"\" --probe \"" + extension +
                           L"\" \"" + expected + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &child), "Starting Shell probe failed");
    const Handle process(child.hProcess), thread(child.hThread);
    if (WaitForSingleObject(process.get(), 10000) != WAIT_OBJECT_0)
    {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        throw std::runtime_error("Shell probe timed out");
    }
    DWORD code = 1;
    require(GetExitCodeProcess(process.get(), &code) && code == 0,
            "Windows did not resolve the expected executable");
}

}

int wmain(int argc, wchar_t **argv)
{
    using namespace vlc::win32;
    if (argc == 4 && std::wstring_view(argv[1]) == L"--probe")
    {
        // This asks Windows, not the hash verifier, and does not compute a hash.
        const auto executable = associatedExecutable(argv[2]);
        if (executable)
            std::wcout << L"Windows resolver: " << *executable << L'\n';
        return executable && samePath(*executable, argv[3]) ? 0 : 1;
    }
    if (argc != 3 || std::wstring_view(argv[1]) != L"--run")
    {
        std::cerr << "Usage: win32_association_smoke.exe --run <installed vlc.exe>\n";
        return 1;
    }

    // Keep the accepted-write fixture short; Windows rejected some long repeated
    // synthetic inputs despite self-valid hashes. Rejection is fault-tested too.
    const std::wstring extension = L".v" + std::to_wstring(GetCurrentProcessId()) +
                                    std::to_wstring(GetTickCount64());
    const std::wstring target = L"VLC" + extension, other = target + L".Other";
    const std::array<std::wstring, 5> roots = {
        extensionRoot(extension), recoveryPath(extension), L"Software\\Classes\\" + extension,
        L"Software\\Classes\\" + target, L"Software\\Classes\\" + other};
    bool owned = false;
    const auto cleanup = [&]() {
        if (!owned)
            return;
        for (const auto &root : roots)
            deleteUserTree(root);
        notifyAssociationChange();
        for (const auto &root : roots)
            require(!keyExists(HKEY_CURRENT_USER, root), "Smoke registry cleanup failed");
    };
    int code = 1;
    try
    {
        const std::wstring vlc(argv[2]);
        require(programIdUsesCurrentVlc(L"VLC.mp4", vlc), "Supplied VLC is not the registered install");
        const auto scheme = hashScheme(false);
        require(scheme != detail::HashScheme::unsupported, "Windows format is unsupported");
        for (const auto &root : roots)
            require(!keyExists(HKEY_CURRENT_USER, root), "Refusing an existing fixture root");
        for (const auto &name : {extension, target, other})
            require(!keyExists(HKEY_CLASSES_ROOT, name), "Refusing an existing class registration");
        const AssociationLock lock;
        std::wcout << L"Disposable extension: " << extension << std::endl;
        owned = true;
        std::array<wchar_t, MAX_PATH> buffer{};
        const DWORD length = GetSystemDirectoryW(buffer.data(), DWORD(buffer.size()));
        require(length != 0 && length < buffer.size(), "Reading system directory failed");
        const std::wstring fallback = std::wstring(buffer.data(), length) + L"\\notepad.exe";
        writeString(createUserKey(roots[2]).get(), L"", other);
        writeString(createUserKey(roots[4] + L"\\shell\\open\\command").get(), L"",
                    L"\"" + fallback + L"\" \"%1\"");
        // Neither command is launched. The non-VLC fallback makes a false
        // positive from an inherited VLC association impossible.
        writeString(createUserKey(roots[3] + L"\\shell\\open\\command").get(), L"",
                    L"\"" + vlc + L"\" --started-from-file \"%1\"");
        notifyAssociationChange();
        probe(extension, fallback);
        require(!currentVlcIsEffective(extension, vlc), "Fallback was reported as VLC");
        require(programIdUsesCurrentVlc(target, vlc), "Test registration failed validation");
        const auto format = scheme == detail::HashScheme::latest ? ChoiceFormat::latest : ChoiceFormat::legacy;
        setChoice(extension, format, target, vlc);
        probe(extension, vlc);
        require(currentVlcIsEffective(extension, vlc), "In-process Windows query missed VLC");
        writeString(createUserKey(format == ChoiceFormat::latest ? latestChoicePath(extension)
                                                                : legacyChoicePath(extension)).get(),
                    L"Hash", L"invalid");
        notifyAssociationChange();
        std::wcout << L"Negative control: invalid hash must not resolve to VLC" << std::endl;
        bool refused = false;
        try { setChoice(extension, format, target, vlc); }
        catch (const NeedsUserChoice &) { refused = true; }
        require(refused && !keyExists(HKEY_CURRENT_USER, recoveryPath(extension)),
                "Invalid backup was not refused before mutation");
        // Querying the Shell can discard an invalid record. Check refusal first,
        // while the owned invalid fixture still exists, then ask Windows.
        probe(extension, fallback);
        require(!currentVlcIsEffective(extension, vlc), "In-process query trusted the invalid hash");
        // An ignored record cannot be regenerated during rollback. Explicitly
        // remove this owned invalid fixture before exercising a fresh transaction.
        removeChoice(extension, format);
        setChoice(extension, format, target, vlc);
        probe(extension, vlc);
        removeChoice(extension, format);
        notifyAssociationChange();
        probe(extension, fallback);
        cleanup();
        std::wcout << L"Restoration and all fixture cleanup confirmed" << std::endl;
        code = 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Acceptance check failed: " << error.what() << '\n';
        try { cleanup(); }
        catch (const std::exception &cleanupError) { std::cerr << cleanupError.what() << '\n'; }
    }
    return code;
}
