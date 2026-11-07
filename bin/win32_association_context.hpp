/*****************************************************************************
 * Private Windows association command context and handoff
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *****************************************************************************/
#ifndef VLC_WIN32_ASSOCIATION_CONTEXT_HPP
#define VLC_WIN32_ASSOCIATION_CONTEXT_HPP

#include <cstdint>
#include <string_view>

namespace vlc::win32
{
bool associationCommandUserContext() noexcept;
bool associationCommandWorkerContext(std::wstring_view sid, std::uint32_t session) noexcept;
// NOT_HANDLED means the verified ordinary caller should execute directly.
// Otherwise return the broker/refusal result; the broker never writes defaults.
int associationCommandHandoff(std::wstring_view extensions, bool once = false) noexcept;
}
#endif
