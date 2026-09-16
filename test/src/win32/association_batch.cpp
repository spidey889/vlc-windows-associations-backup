/*****************************************************************************
 * Windows association batch regression tests (no registry access)
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *****************************************************************************/

#include "win32/associations/association_batch.hpp"
#include "win32/associations/win32_associations.hpp"

#include <iostream>
#include <stdexcept>

namespace
{
std::vector<std::wstring> calls;
std::vector<vlc::win32::AssociationResult> results;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
}

namespace vlc::win32
{
// Link the production batch implementation against a deterministic fake writer.
AssociationResult setDefaultAssociation(std::wstring_view extension) noexcept
{
    const auto index = calls.size();
    calls.emplace_back(extension);
    if (index >= results.size())
        return {AssociationStatus::failed, "Unexpected backend call"};
    return results[index];
}
}

int main()
{
    using namespace vlc::win32;
    try
    {
        const auto empty = setDefaultAssociations({});
        require(calls.empty() && empty.changed == 0 && empty.unchanged == 0 &&
                empty.needsUserChoice.empty() && empty.failures.empty(),
                "Empty input must not call the writer");

        results = {{AssociationStatus::changed, {}},
                   {AssociationStatus::needsUserChoice, "Needs Settings"},
                   {AssociationStatus::failed, "Write rejected"},
                   {AssociationStatus::unchanged, {}},
                   {AssociationStatus::changed, {}}};
        const std::vector<std::wstring> requested = {L".mp3", L".mp4", L".flac", L".wav", L".mp3"};
        const auto mixed = setDefaultAssociations(requested);
        require(calls == requested, "Preserve input order and caller selection, including duplicates");
        require(mixed.stopReason == AssociationBatchStopReason::none && mixed.skipped == 0,
                "Ordinary failures must not masquerade as busy/skipped work");
        require(mixed.changed == 2 && mixed.unchanged == 1,
                "Non-lock failures must not stop later transactions");
        require(mixed.needsUserChoice == decltype(mixed.needsUserChoice){{L".mp4", "Needs Settings"}} &&
                mixed.failures == decltype(mixed.failures){{L".flac", "Write rejected"}},
                "Preserve extension-specific status and diagnostic text");

        calls.clear();
        results = {{AssociationStatus::changed, {}},
                   {AssociationStatus::failed, "Lock timeout", true}};
        const auto busy = setDefaultAssociations({L".mp3", L".mp4", L".wav", L".flac"});
        require(calls == std::vector<std::wstring>{L".mp3", L".mp4"},
                "Do not call the writer again after lock timeout");
        require(busy.stopReason == AssociationBatchStopReason::writerBusy && busy.skipped == 2,
                "Expose typed lock-timeout and skipped count without parsing messages");
        require(busy.changed == 1 && busy.failures.size() == 3 &&
                busy.failures[0] == std::make_pair(std::wstring(L".mp4"), std::string("Lock timeout")),
                "Keep completed work and the original lock diagnostic");
        require(busy.failures[1].first == L".wav" && busy.failures[2].first == L".flac" &&
                busy.failures[1].second == "Not attempted because another association update is still running" &&
                busy.failures[2].second == busy.failures[1].second,
                "Report all skipped extensions in order with the existing UI diagnostic");

        calls.clear();
        results = {{AssociationStatus::unchanged, {}}};
        const auto retry = setDefaultAssociations({L".wav"});
        require(retry.stopReason == AssociationBatchStopReason::none && retry.skipped == 0,
                "Busy metadata must reset for each explicit batch");
        require(calls == std::vector<std::wstring>{L".wav"} && retry.unchanged == 1,
                "A later explicit batch must not inherit a previous timeout");
        calls.clear();
        results = {{AssociationStatus::failed, "Busy", true}};
        const auto lastBusy = setDefaultAssociations({L".mp3"});
        require(lastBusy.stopReason == AssociationBatchStopReason::writerBusy && lastBusy.skipped == 0,
                "A busy final entry is still busy even with no skipped entries");
        std::cout << "Association batch regression tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
