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

#include "association_batch.hpp"
#include "win32_associations.hpp"

namespace vlc::win32
{
AssociationBatchResult setDefaultAssociations(const std::vector<std::wstring> &extensions)
{
    AssociationBatchResult batch;
    for (const std::wstring &extension : extensions)
    {
        if (batch.stopReason == AssociationBatchStopReason::writerBusy)
        {
            // Report every skipped item without waiting on the same lock again.
            ++batch.skipped;
            batch.failures.emplace_back(extension,
                "Not attempted because another association update is still running");
            continue;
        }
        const AssociationResult result = setDefaultAssociation(extension);
        if (result.lockBusy)
            batch.stopReason = AssociationBatchStopReason::writerBusy;
        switch (result.status)
        {
            case AssociationStatus::changed:
                ++batch.changed;
                break;
            case AssociationStatus::unchanged:
                ++batch.unchanged;
                break;
            case AssociationStatus::needsUserChoice:
                batch.needsUserChoice.emplace_back(extension, result.error);
                break;
            case AssociationStatus::failed:
                batch.failures.emplace_back(extension, result.error);
                break;
        }
    }
    return batch;
}
} // namespace vlc::win32
