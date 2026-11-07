/*****************************************************************************
 * Early Windows association command, private to the VLC executable
 * Copyright (C) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: GPL-2.0-or-later
 *****************************************************************************/

#ifndef VLC_WIN32_ASSOCIATION_COMMAND_H
#define VLC_WIN32_ASSOCIATION_COMMAND_H

#include <stddef.h>

enum vlc_win32_association_exit
{
    VLC_WIN32_ASSOCIATION_NOT_HANDLED = -1,
    VLC_WIN32_ASSOCIATION_SUCCESS = 0,
    VLC_WIN32_ASSOCIATION_FAILED = 1,
    VLC_WIN32_ASSOCIATION_INVALID_REQUEST = 2,
    VLC_WIN32_ASSOCIATION_UNSAFE_CONTEXT = 3,
    VLC_WIN32_ASSOCIATION_BUSY = 4,
};

/* Return NOT_HANDLED for normal VLC startup. Recognized commands always exit
 * through this path, including invalid requests; never forward them to Qt.
 * No exception may cross the C launcher boundary. argv is not modified. */
#ifdef __cplusplus
extern "C" int vlc_win32_association_command(int argc, wchar_t **argv) noexcept;
#else
int vlc_win32_association_command(int argc, wchar_t **argv);
#endif

#endif
