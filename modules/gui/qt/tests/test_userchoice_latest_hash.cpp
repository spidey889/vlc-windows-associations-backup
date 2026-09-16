/*****************************************************************************
 * test_userchoice_latest_hash.cpp: Windows UserChoiceLatest hash tests
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See ../util/LICENSE for the full license text.
 *****************************************************************************/

#include "../util/userchoice_latest_hash/HashCommon.h"

#include <clocale>
#include <iostream>

using namespace UserChoiceLatestHash;

static bool check_hash(const wchar_t *name, const std::wstring &input,
                       const wchar_t *expected)
{
    WorkingSeeds seeds{};
    LoadProvidedSeeds(&seeds);

    std::wstring actual;
    const bool computed = ComputeHash(input, seeds, false, &actual, nullptr);
    if (!computed || actual != expected)
    {
        std::wcerr << name << L": expected " << expected << L", got "
                   << (computed ? actual : L"<calculation failed>") << L'\n';
        return false;
    }
    return true;
}

static bool check_locale_folding(const std::wstring &canonical, const WorkingSeeds &seeds)
{
    const std::string original(std::setlocale(LC_CTYPE, nullptr));
    const std::wstring input{L'I', L'\u00c4', L'\u03a9', L'\u0410', L'\u4e2d',
                             L'\0', L'I', wchar_t(0xd83d), wchar_t(0xde00), wchar_t(0xd800)};
    const std::wstring expected{L'i', L'\u00c4', L'\u03a9', L'\u0410', L'\u4e2d',
                                L'\0', L'i', wchar_t(0xd83d), wchar_t(0xde00), wchar_t(0xd800)};
    bool ok = true;
    // Change only this test process's CRT locale, after its worker threads finish.
    for (const char *locale : {"C", "Turkish", "Azerbaijani (Latin)"})
    {
        if (std::setlocale(LC_CTYPE, locale) == nullptr)
        {
            std::cerr << "Locale unavailable, skipping: " << locale << '\n';
            continue;
        }
        std::wstring hash;
        const bool passed = ToLowerWide(input) == expected &&
            ComputeHash(canonical, seeds, false, &hash, nullptr) && hash == L"QLDNVKIO7uc=";
        if (!passed)
            std::cerr << "C-locale hash casing regression: " << locale << '\n';
        ok &= passed;
    }
    ok &= std::setlocale(LC_CTYPE, original.c_str()) != nullptr;
    return ok;
}

int main()
{
    // Exercise first initialization before any other hash call in this process.
    // Concurrent callers must observe the same complete tables and fixed hash,
    // while keeping their encoder state independent.
    struct ThreadResult
    {
        const Tables *table = nullptr;
        bool hash_ok = false;
    } results[8];
    HANDLE threads[8]{};
    const auto loadTables = [](LPVOID output) WINAPI -> DWORD {
        auto &result = *static_cast<ThreadResult *>(output);
        try
        {
            result.table = &GetTables();
            WorkingSeeds seeds{};
            LoadProvidedSeeds(&seeds);
            std::wstring hash;
            result.hash_ok = ComputeHash(std::wstring(8, L'a'), seeds, false, &hash, nullptr) &&
                             hash == L"fogov5S/coE=";
        }
        catch (...)
        {
            return 1;
        }
        return 0;
    };
    unsigned started = 0;
    for (; started < 8; ++started)
    {
        threads[started] = CreateThread(nullptr, 0, loadTables, &results[started], 0, nullptr);
        if (threads[started] == nullptr)
            break;
    }
    const bool completed = started != 0 &&
        WaitForMultipleObjects(started, threads, TRUE, INFINITE) == WAIT_OBJECT_0;
    for (unsigned index = 0; index < started; ++index)
        CloseHandle(threads[index]);
    if (!completed || started != 8)
        return 1;
    for (const ThreadResult &result : results)
    {
        if (!result.hash_ok || result.table != results[0].table ||
            result.table->byte_class_table.size() != 256 ||
            result.table->token_region.empty() || result.table->token_bit_width_table.empty())
            return 1;
    }

    // Fixed canonical strings cover the salt/field order of all three classes.
    // They test codec bytes, not the backend's MachineID-to-class selection.
    static constexpr wchar_t class0_input[] =
        L"copyright (c) microsoft. all rights reserved "
        L"{3822b7ca-c2f4-4889-b8cc-4ce39a8fb81c}.pdd"
        L"01dcb06ea49f32a0b4deb148-0249-44c4-a8d3-5409e822c599"
        L"msedgepdfs-1-5-21-673349297-2269585490-1023937497-500";

    static constexpr wchar_t class1_input[] =
        L"copyright (c) microsoft. all rights reserved "
        L"{d185e0a1-e265-4724-aa21-3a17b038d72e}"
        L"01dcdf62daf28c30.mp4"
        L"s-1-5-21-1000-2000-3000-1001"
        L"00000000-0000-0000-0000-000000000001vlc.mp4";

    static constexpr wchar_t class2_input[] =
        L"s-1-5-21-1000-2000-3000-1001"
        L"01dcdf62daf28c30"
        L"copyright (c) microsoft. all rights reserved "
        L"{97b6bcf4-c367-4577-95be-73bd3053a5e0}.mp4"
        L"00000000-0000-0000-0000-000000000002vlc.mp4";

    const bool class0_ok = check_hash(L"class 0", class0_input,
                                      L"JOBZ2dl4dKM=");
    const bool class1_ok = check_hash(L"class 1", class1_input,
                                      L"QLDNVKIO7uc=");
    const bool class2_ok = check_hash(L"class 2", class2_input,
                                      L"n2D4J2XcNHg=");
    WorkingSeeds seeds{};
    LoadProvidedSeeds(&seeds);
    // Length checks exercise successful encoding at these sizes; the fixed
    // vectors below separately check the exact output bytes.
    bool boundaries_ok = true;
    for (size_t size : {0U, 1U, 2U, 8U, 256U, 512U, 1024U, 2048U})
    {
        std::wstring result;
        boundaries_ok &= ComputeHash(std::wstring(size, L'a'), seeds, false, &result, nullptr) &&
                         result.size() == 12;
    }
    std::wstring uppercase_input(class1_input);
    for (wchar_t &character : uppercase_input)
    {
        if (character >= L'a' && character <= L'z')
            character -= L'a' - L'A';
    }
    std::wstring uppercase_hash;
    const bool case_ok = ComputeHash(uppercase_input, seeds, false, &uppercase_hash, nullptr) &&
                         uppercase_hash == L"QLDNVKIO7uc=";
    // Snapshot vectors from the pinned codec, not additional real-Windows
    // association claims. The long repeat exercises the first-pass boundary.
    const bool snapshots_ok = check_hash(L"empty", L"", L"AAAAAAAAAAA=") &&
        check_hash(L"long repeat", std::wstring(592, L'a'), L"ksW1efpxS4o=");
    std::wstring repeating;
    for (size_t i = 0; i < 629; ++i)
        repeating.push_back(L"abc-0123456789"[i % 14]);
    std::wstring unchanged = L"unchanged";
    // This pattern previously over-counted the first-pass slice and hung.
    // Rejection must preserve the caller's output, not return a guessed hash.
    if (ComputeHash(repeating, seeds, false, &unchanged, nullptr) || unchanged != L"unchanged")
    {
        std::cerr << "Repeating-input regression: must fail without changing the output\n";
        return 1;
    }
    // A non-repeat history reference used lookahead to rewind 52 units after
    // only 40 had been consumed. Reject that pointer underflow, preserving output.
    constexpr wchar_t rewind_pattern[] = L"b--0bb-0bb---c-bc-acb0--a-0acac-";
    std::wstring rewind_input;
    for (size_t i = 0; i < 1047; ++i)
        rewind_input.push_back(rewind_pattern[i % (sizeof(rewind_pattern) / sizeof(wchar_t) - 1)]);
    if (ComputeHash(rewind_input, seeds, false, &unchanged, nullptr) || unchanged != L"unchanged")
    {
        std::cerr << "First-pass rewind regression: must fail without changing the output\n";
        return 1;
    }
    // Already-lowercase UTF-16 vectors match the pre-fix codec. Include a
    // surrogate pair, an isolated surrogate and embedded NUL, not just ASCII.
    const bool unicode_ok = check_hash(L"non-ASCII", L"vlc.\u00e4\u03c9\u0430\u4e2d", L"iVYkqesw5V0=") &&
        check_hash(L"surrogate pair", L"\xd83d\xde00" L".mp4", L"BID6Bymsr6Q=") &&
        check_hash(L"isolated surrogate", L"\xd800" L"vlc.mp4", L"MCuGjAqoSgA=") &&
        check_hash(L"embedded NUL", std::wstring{L'v', L'\0', L'l', L'c'}, L"Shyumt78C6o=");
    const bool locale_ok = check_locale_folding(uppercase_input, seeds);
    DebugData debug;
    std::wstring debug_hash, lowercase_hash;
    const bool optional_ok = ComputeHash(class1_input, seeds, false, &debug_hash, &debug) &&
        debug_hash == L"QLDNVKIO7uc=" && !debug.packed_words.empty() &&
        ComputeHash(class1_input, seeds, true, &lowercase_hash, nullptr) &&
        lowercase_hash == L"qldnvkio7uc=";
    return class0_ok && class1_ok && class2_ok && boundaries_ok && case_ok &&
           snapshots_ok && optional_ok && unicode_ok && locale_ok ? 0 : 1;
}
