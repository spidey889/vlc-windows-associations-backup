/*****************************************************************************
 * test_win32_association_decisions.cpp: Windows association safety tests
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See ../../../../src/win32/associations/LICENSE for the full license text.
 *****************************************************************************/

#include "win32/associations/win32_associations.hpp"

#include <iostream>

int main()
{
    using vlc::win32::detail::HashScheme;
    using vlc::win32::detail::RecoveryAction;
    using vlc::win32::detail::chooseRecoveryAction;
    using vlc::win32::detail::classifyHashScheme;

    const bool routingWorks =
        classifyHashScheme(false, 0, false) == HashScheme::legacy &&
        classifyHashScheme(false, 0, true) == HashScheme::unsupported &&
        classifyHashScheme(true, 1, false) == HashScheme::latest &&
        classifyHashScheme(true, 1, true) == HashScheme::latest &&
        classifyHashScheme(true, 0, false) == HashScheme::unsupported &&
        classifyHashScheme(true, 0, true) == HashScheme::unsupported &&
        classifyHashScheme(true, 2, false) == HashScheme::unsupported &&
        classifyHashScheme(true, 2, true) == HashScheme::unsupported &&
        classifyHashScheme(true, 0xffffffffU, false) == HashScheme::unsupported &&
        classifyHashScheme(true, 0xffffffffU, true) == HashScheme::unsupported;

    // Exhaustive policy matrix, with no registry I/O. Bits: exists / belongs
    // to journal / valid / previous existed. Missing records need removal or
    // restoration; external and complete records are kept. The registry suite
    // separately checks that these actions persist data before clearing a journal.
    constexpr RecoveryAction expectedRecovery[] = {
        RecoveryAction::remove, RecoveryAction::restore,
        RecoveryAction::remove, RecoveryAction::restore,
        RecoveryAction::remove, RecoveryAction::restore,
        RecoveryAction::remove, RecoveryAction::restore,
        RecoveryAction::clear, RecoveryAction::clear,
        RecoveryAction::clear, RecoveryAction::clear,
        RecoveryAction::remove, RecoveryAction::restore,
        RecoveryAction::clear, RecoveryAction::clear,
    };
    bool recoveryWorks = true;
    for (unsigned flags = 0; flags < 16; ++flags)
    {
        recoveryWorks &= chooseRecoveryAction((flags & 8) != 0, (flags & 4) != 0,
                                              (flags & 2) != 0, (flags & 1) != 0) ==
                         expectedRecovery[flags];
    }

    if (!routingWorks || !recoveryWorks)
    {
        std::cerr << "Windows association safety decision mismatch\n";
        return 1;
    }
    return 0;
}
