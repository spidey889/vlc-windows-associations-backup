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

#ifndef VLC_QT_WIN32_ASSOCIATION_TASK_HPP
#define VLC_QT_WIN32_ASSOCIATION_TASK_HPP

#include "asynctask.hpp"
#include "win32/associations/association_batch.hpp"

#include <string>
#include <utility>
#include <vector>

namespace vlc::win32
{
class AssociationTask final : public AsyncTask<AssociationBatchResult>
{
public:
    explicit AssociationTask(std::vector<std::wstring> extensions);
    AssociationBatchResult execute() override;

private:
    const std::vector<std::wstring> m_extensions;
};

// Keep registry/codec reads off the UI thread too. Results follow input order;
// abandoning this task suppresses delivery without changing any associations.
class AssociationQueryTask final : public AsyncTask<std::vector<bool>>
{
public:
    explicit AssociationQueryTask(std::vector<std::wstring> extensions);
    std::vector<bool> execute() override;

private:
    const std::vector<std::wstring> m_extensions;
};
} // namespace vlc::win32

#endif
