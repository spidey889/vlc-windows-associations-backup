/*****************************************************************************
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef VLC_WIN32_ASSOCIATION_BATCH_HPP
#define VLC_WIN32_ASSOCIATION_BATCH_HPP

#include <string>
#include <utility>
#include <vector>

namespace vlc::win32
{
enum class AssociationBatchStopReason
{
    none,
    writerBusy,
};

struct AssociationBatchResult
{
    unsigned changed = 0;
    unsigned unchanged = 0;
    std::vector<std::pair<std::wstring, std::string>> needsUserChoice;
    std::vector<std::pair<std::wstring, std::string>> failures;
    // Additive metadata; skipped entries remain in failures for existing UIs.
    AssociationBatchStopReason stopReason = AssociationBatchStopReason::none;
    unsigned skipped = 0;
    // Installer-only historical success; does not assert today's defaults.
    bool alreadyCompleted = false;
};

// Synchronous and current-user only, with the same preconditions as
// setDefaultAssociation(). Preserve input order and report every requested
// extension; after a writer-lock timeout, do not retry the remaining entries.
AssociationBatchResult setDefaultAssociations(const std::vector<std::wstring> &extensions);
// Called only after the CLI has verified the ordinary current-user context.
// Shared by both installers. Errors reading/persisting completion may throw.
// Preferences continues to use the unrestricted batch above.
AssociationBatchResult setInstallerDefaultAssociations(const std::vector<std::wstring> &extensions);
} // namespace vlc::win32

#endif
