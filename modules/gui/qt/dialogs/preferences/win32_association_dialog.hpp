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

#ifndef VLC_QT_WIN32_ASSOCIATION_DIALOG_HPP
#define VLC_QT_WIN32_ASSOCIATION_DIALOG_HPP

#include <QDialog>
#include <QList>

#include <string_view>

class QPushButton;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
struct qt_intf_t;

namespace vlc::win32
{
class AssociationTask;
class AssociationQueryTask;
}

class Win32AssociationDialog final : public QDialog
{
public:
    explicit Win32AssociationDialog(qt_intf_t *intf, QWidget *parent = nullptr);
    ~Win32AssociationDialog() override;
    void done(int result) override;

private:
    void addType(std::wstring_view extension, QTreeWidgetItem *parent);
    void loadDefaults();
    void updateApplyButton();
    void apply();

    QList<QTreeWidgetItem *> m_associations;
    vlc::win32::AssociationTask *m_task = nullptr;
    vlc::win32::AssociationQueryTask *m_queryTask = nullptr;
    QPushButton *m_applyButton = nullptr;
    QTreeWidget *m_filetypeList = nullptr;
    QLabel *m_description = nullptr;
    qt_intf_t *const m_intf;
};

#endif
