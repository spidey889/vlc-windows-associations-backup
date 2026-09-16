/*****************************************************************************
 * test_userchoice_hash.cpp: legacy Windows UserChoice hash tests
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See ../../../src/win32/associations/LICENSE for the full license text.
 *****************************************************************************/

#include "win32/associations/userchoice_hash.hpp"

#include <iostream>

int main()
{
    const std::string actual = vlc::win32::calculateLegacyUserChoiceHash(
        ".3g2", "S-1-5-21-819709642-920330688-1657285119-500", "WMP11.AssocFile.3G2",
        "01d4d98267246000");
    if (actual != "PCCqEmkvW2Y=")
    {
        std::cerr << "legacy UserChoice hash mismatch: " << actual << '\n';
        return 1;
    }
    return 0;
}
