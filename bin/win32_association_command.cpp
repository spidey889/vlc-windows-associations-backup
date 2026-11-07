/*****************************************************************************
 * Early Windows association command (before libVLC/Qt initialization)
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *****************************************************************************/

#include "win32_association_command.h"
#include "win32_association_context.hpp"
#include "win32/associations/association_batch.hpp"
#include "win32/associations/win32_associations.hpp"

#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <limits>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::wstring_view option = L"--win32-set-default-associations";
constexpr std::wstring_view prefix = L"--win32-set-default-associations=";
constexpr std::wstring_view onceOption = L"--win32-set-default-associations-once";
constexpr std::wstring_view oncePrefix = L"--win32-set-default-associations-once=";
constexpr std::wstring_view onceWorkerOption = L"--win32-association-once-worker";
constexpr std::wstring_view onceWorkerPrefix = L"--win32-association-once-worker=";
constexpr std::wstring_view workerOption = L"--win32-association-worker";
constexpr std::wstring_view workerPrefix = L"--win32-association-worker=";

bool startsWith(std::wstring_view text, std::wstring_view start)
{
    return text.size() >= start.size() && text.substr(0, start.size()) == start;
}

bool parseWorkerTarget(std::wstring_view target, std::wstring_view &sid, std::uint32_t &session)
{
    const size_t colon = target.find(L':');
    if (colon == std::wstring_view::npos) return false;
    sid = target.substr(0, colon);
    const auto number = target.substr(colon + 1);
    if (!startsWith(sid, L"S-1-") || number.empty()) return false;
    session = 0;
    for (const wchar_t ch : number)
    {
        if (ch < L'0' || ch > L'9') return false;
        const unsigned digit = ch - L'0';
        if (session > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return false;
        session = session * 10 + digit;
    }
    if (!session) return false;
    PSID parsed = nullptr;
    const std::wstring value(sid);
    if (!ConvertStringSidToSidW(value.c_str(), &parsed)) return false;
    LPWSTR canonical = nullptr;
    const bool converted = ConvertSidToStringSidW(parsed, &canonical) != FALSE;
    const bool valid = converted && std::wstring_view(canonical) == sid;
    LocalFree(canonical);
    LocalFree(parsed);
    return valid;
}

int invalidRequest()
{
    std::fprintf(stderr, "default-app setup: expected only --win32-set-default-associations=.mp3,.mp4\n");
    return VLC_WIN32_ASSOCIATION_INVALID_REQUEST;
}
}

extern "C" int vlc_win32_association_command(int argc, wchar_t **argv) noexcept
{
    unsigned requests = 0, workers = 0;
    for (int i = 1; argv && i < argc; ++i)
    {
        if (argv[i] && (std::wstring_view(argv[i]) == option || startsWith(argv[i], prefix) ||
                        std::wstring_view(argv[i]) == onceOption || startsWith(argv[i], oncePrefix)))
            ++requests;
        if (argv[i] && (std::wstring_view(argv[i]) == workerOption || startsWith(argv[i], workerPrefix) ||
                        std::wstring_view(argv[i]) == onceWorkerOption || startsWith(argv[i], onceWorkerPrefix)))
            ++workers;
    }
    if (!requests && !workers)
        return VLC_WIN32_ASSOCIATION_NOT_HANDLED;

    try
    {
        const bool worker = workers == 1 && requests == 0;
        const bool once = argv && argc > 1 && argv[1] &&
            (startsWith(argv[1], oncePrefix) || startsWith(argv[1], onceWorkerPrefix));
        const auto requestPrefix = once ? oncePrefix : prefix;
        const auto privatePrefix = once ? onceWorkerPrefix : workerPrefix;
        std::wstring_view payload, expectedSid;
        std::uint32_t expectedSession = 0;
        if (worker)
        {
            if (argc != 3 || !argv[1] || !argv[2] || !startsWith(argv[1], privatePrefix) ||
                !parseWorkerTarget(std::wstring_view(argv[1]).substr(privatePrefix.size()), expectedSid, expectedSession))
                return invalidRequest();
            payload = argv[2];
        }
        else
        {
            if (requests != 1 || workers || argc != 2 || !argv[1] || !startsWith(argv[1], requestPrefix))
                return invalidRequest();
            payload = std::wstring_view(argv[1]).substr(requestPrefix.size());
        }
        std::wstring_view remaining = payload;
        std::vector<std::wstring> extensions;
        // Validate the complete request before calling even the first writer.
        // Keep caller order/duplicates, as the existing synchronous batch does.
        for (;;)
        {
            const size_t comma = remaining.find(L',');
            const auto extension = remaining.substr(0, comma);
            if (!vlc::win32::detail::validExtension(extension))
                return invalidRequest();
            extensions.emplace_back(extension);
            if (comma == std::wstring_view::npos)
                break;
            remaining.remove_prefix(comma + 1);
        }
        if (worker)
        {
            // The marker carries expectations, not authority. Never hand off again.
            if (!vlc::win32::associationCommandWorkerContext(expectedSid, expectedSession))
            {
                std::fprintf(stderr, "default-app setup: worker identity/context mismatch; no changes attempted\n");
                return VLC_WIN32_ASSOCIATION_UNSAFE_CONTEXT;
            }
        }
        else
        {
            const int result = vlc::win32::associationCommandHandoff(payload, once);
            if (result != VLC_WIN32_ASSOCIATION_NOT_HANDLED) return result;
        }

        const auto batch = once ? vlc::win32::setInstallerDefaultAssociations(extensions)
                                : vlc::win32::setDefaultAssociations(extensions);
        if (batch.alreadyCompleted)
        {
            std::fprintf(stderr, "default-app setup: installer operation completed previously; no new defaults requested\n");
            return VLC_WIN32_ASSOCIATION_SUCCESS;
        }
        std::fprintf(stderr, "default-app setup: %u changed, %u unchanged, %zu need confirmation, %zu failed (including %u skipped)\n",
                     batch.changed, batch.unchanged, batch.needsUserChoice.size(), batch.failures.size(), batch.skipped);
        for (const auto &[extension, reason] : batch.needsUserChoice)
            std::fprintf(stderr, "default-app setup for %ls needs confirmation: %s\n", extension.c_str(), reason.c_str());
        for (const auto &[extension, reason] : batch.failures)
            std::fprintf(stderr, "default-app setup for %ls failed: %s\n", extension.c_str(), reason.c_str());
        if (batch.stopReason == vlc::win32::AssociationBatchStopReason::writerBusy)
            return VLC_WIN32_ASSOCIATION_BUSY;
        return batch.needsUserChoice.empty() && batch.failures.empty()
            ? VLC_WIN32_ASSOCIATION_SUCCESS : VLC_WIN32_ASSOCIATION_FAILED;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "default-app setup: %s\n", error.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "default-app setup: unexpected failure\n");
    }
    return VLC_WIN32_ASSOCIATION_FAILED;
}
