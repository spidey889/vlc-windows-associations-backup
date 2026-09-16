/*****************************************************************************
 * userchoice_hash.hpp: legacy Windows UserChoice hash
 *****************************************************************************
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See LICENSE for the full license text.
 *****************************************************************************/

#ifndef VLC_QT_USERCHOICE_HASH_HPP
#define VLC_QT_USERCHOICE_HASH_HPP

#include <string>
#include <string_view>

namespace vlc::win32
{
std::string calculateLegacyUserChoiceHash(std::string_view extension, std::string_view sid,
                                          std::string_view programId, std::string_view timestamp);
}

#endif
