/*****************************************************************************
 * win32_associations.hpp: Windows default application associations
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See LICENSE for the full license text.
 *****************************************************************************/

#ifndef VLC_WIN32_ASSOCIATIONS_HPP
#define VLC_WIN32_ASSOCIATIONS_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace vlc::win32
{
enum class AssociationStatus
{
    // changed/unchanged report a validated Windows effective default, not a
    // launched media file.
    changed,
    unchanged,
    needsUserChoice,
    failed,
};

struct AssociationResult
{
    AssociationStatus status;
    std::string error;
    // A batch must stop trying the shared writer lock after a timeout.
    bool lockBusy = false;
};

// Synchronous, current-user only: the caller must provide explicit user intent
// and run updates off the UI thread. No registration/elevation/Settings launch.
// Each call is an independent extension transaction, not an atomic batch.
// A failed result may retain a journal; the next update attempts recovery first.
// Snapshot checks detect external interference, including same-ProgId changes.
// Settings does not share our mutex: its writes can still race a final check and
// mutation. This API cannot guarantee atomicity against other applications.
AssociationResult setDefaultAssociation(std::wstring_view extension) noexcept;
// Read-only: does not repair journals. False also covers unreadable/invalid state.
bool isDefaultAssociation(std::wstring_view extension) noexcept;

namespace detail
{
// Shared request validation: validate a complete batch before its first write.
inline bool validExtension(std::wstring_view extension) noexcept
{
    if (extension.size() < 2 || extension.size() > 64 || extension.front() != L'.')
        return false;
    for (const wchar_t character : extension.substr(1))
    {
        if (!((character >= L'a' && character <= L'z') ||
              (character >= L'A' && character <= L'Z') ||
              (character >= L'0' && character <= L'9') || character == L'+' || character == L'-' ||
              character == L'_'))
            return false;
    }
    return true;
}

// Keep the safety-critical state decisions free of registry access so they can
// be exhaustively tested without changing a user's default applications.
enum class HashScheme
{
    legacy,
    latest,
    unsupported,
};

constexpr HashScheme classifyHashScheme(bool versionPresent, std::uint32_t version,
                                        bool latestEntryExists) noexcept
{
    // Absence of the marker selects legacy only when no latest record exists.
    // Version 1 selects latest even for an extension with no choice yet.
    if (versionPresent)
        return version == 1 ? HashScheme::latest : HashScheme::unsupported;
    return latestEntryExists ? HashScheme::unsupported : HashScheme::legacy;
}

enum class RecoveryAction
{
    clear,
    restore,
    remove,
};

// Target and previous ProgIds both belong to a journal: a partial rollback
// needs repair too. A different ProgId is an external choice and is preserved.
constexpr RecoveryAction chooseRecoveryAction(bool currentChoiceExists,
                                              bool currentChoiceBelongsToJournal,
                                              bool currentChoiceIsValid,
                                              bool previousChoiceExisted) noexcept
{
    if (currentChoiceExists && (!currentChoiceBelongsToJournal || currentChoiceIsValid))
        return RecoveryAction::clear;
    return previousChoiceExisted ? RecoveryAction::restore : RecoveryAction::remove;
}
} // namespace detail
} // namespace vlc::win32

#endif
