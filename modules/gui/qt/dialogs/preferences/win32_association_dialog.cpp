/*****************************************************************************
 * Copyright (C) 2006-2010 the VideoLAN team
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

#include "win32_association_dialog.hpp"

#include "qt.hpp"
#include "util/win32_association_task.hpp"
#include "util/win32_association_types.hpp"

#include <QDialogButtonBox>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <vector>

Win32AssociationDialog::Win32AssociationDialog(qt_intf_t *intf, QWidget *parent)
    : QDialog(parent), m_intf(intf)
{
    setWindowTitle(qtr("Default apps"));
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *title = new QLabel(
        qtr("Choose which media types VLC should open by default"), this);
    title->setWordWrap(true);
    QFont titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    m_description = new QLabel(qtr("Checking current defaults..."), this);
    m_description->setWordWrap(true);
    layout->addWidget(m_description);

    QTreeWidget *filetypeList = new QTreeWidget(this);
    m_filetypeList = filetypeList;
    filetypeList->setEnabled(false);
    filetypeList->setAccessibleName(qtr("Media types"));
    filetypeList->setUniformRowHeights(true);
    filetypeList->header()->hide();
    layout->addWidget(filetypeList, 1);

    QTreeWidgetItem *audioType =
        new QTreeWidgetItem(QStringList(qtr("Audio files")));
    QTreeWidgetItem *videoType =
        new QTreeWidgetItem(QStringList(qtr("Video files")));
    QTreeWidgetItem *playlistType =
        new QTreeWidgetItem(QStringList(qtr("Playlist files")));

    filetypeList->addTopLevelItem(audioType);
    filetypeList->addTopLevelItem(videoType);
    filetypeList->addTopLevelItem(playlistType);

    const auto populateCategory = [this](QTreeWidgetItem *category,
                                         const auto &extensions) {
        category->setFlags(category->flags() | Qt::ItemIsAutoTristate);
        for (const std::wstring_view extension : extensions)
            addType(extension, category);
        category->setExpanded(false);
    };
    populateCategory(audioType, vlc::win32::audioExtensions);
    populateCategory(videoType, vlc::win32::videoExtensions);
    populateCategory(playlistType, vlc::win32::playlistExtensions);

    QLabel *selectionNote = new QLabel(
        qtr("Only checked file types will be changed. Unchecked types keep their current app."),
        this);
    selectionNote->setWordWrap(true);
    layout->addWidget(selectionNote);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(this);
    m_applyButton = new QPushButton(qtr("&Apply"), this);
    QPushButton *cancelButton = new QPushButton(qtr("&Cancel"), this);
    m_applyButton->setDefault(true);
    buttonBox->addButton(m_applyButton, QDialogButtonBox::ApplyRole);
    buttonBox->addButton(cancelButton, QDialogButtonBox::RejectRole);
    layout->addWidget(buttonBox);

    connect(filetypeList, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *, int) { updateApplyButton(); });
    connect(m_applyButton, &QPushButton::clicked, this, &Win32AssociationDialog::apply);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    updateApplyButton();
    resize(460, 430);
    loadDefaults();
}

Win32AssociationDialog::~Win32AssociationDialog()
{
    // Abandon suppresses delivery; an already-running batch can finish
    // after this dialog has been destroyed.
    if (m_task != nullptr)
        m_task->abandon();
    if (m_queryTask != nullptr)
        m_queryTask->abandon();
}

void Win32AssociationDialog::done(int result)
{
    // Disabling widgets does not prevent Escape or the native close button.
    if (m_task == nullptr)
        QDialog::done(result);
}

void Win32AssociationDialog::addType(std::wstring_view extension,
                                     QTreeWidgetItem *parent)
{
    const QString extensionText = QString::fromWCharArray(
        extension.data(), static_cast<int>(extension.size()));
    QTreeWidgetItem *item = new QTreeWidgetItem(parent, QStringList(extensionText));
    // The display label may change; the backend identifier must not.
    item->setData(0, Qt::UserRole, extensionText);
    item->setCheckState(0, Qt::Unchecked);
    m_associations.append(item);
}

void Win32AssociationDialog::loadDefaults()
{
    // No widget or intf pointer crosses into the worker. Keep results in the
    // same order as the immutable identifiers, independent of displayed labels.
    std::vector<std::wstring> extensions;
    extensions.reserve(m_associations.size());
    for (const QTreeWidgetItem *item : m_associations)
        extensions.push_back(item->data(0, Qt::UserRole).toString().toStdWString());
    auto *task = new vlc::win32::AssociationQueryTask(std::move(extensions));
    m_queryTask = task;
    connect(task, &BaseAsyncTask::result, this, [this, task]() {
        const auto defaults = task->takeResult();
        task->abandon();
        m_queryTask = nullptr;
        {
            const QSignalBlocker blocker(m_filetypeList);
            for (qsizetype index = 0; index < m_associations.size(); ++index)
                m_associations[index]->setCheckState(0, defaults[index] ? Qt::Checked
                                                                      : Qt::Unchecked);
        }
        m_description->setText(
            qtr("Select a category, or expand it to choose individual file types."));
        m_filetypeList->setEnabled(true);
        updateApplyButton();
    });
    task->start(*QThreadPool::globalInstance());
}

void Win32AssociationDialog::updateApplyButton()
{
    if (!m_filetypeList->isEnabled())
    {
        m_applyButton->setEnabled(false);
        return;
    }
    for (const QTreeWidgetItem *item : m_associations)
    {
        if (item->checkState(0) == Qt::Checked)
        {
            m_applyButton->setEnabled(true);
            return;
        }
    }
    m_applyButton->setEnabled(false);
}

void Win32AssociationDialog::apply()
{
    if (m_task != nullptr || m_queryTask != nullptr)
        return;

    std::vector<std::wstring> extensions;
    extensions.reserve(m_associations.size());
    for (const QTreeWidgetItem *item : m_associations)
    {
        if (item->checkState(0) == Qt::Checked)
            extensions.push_back(item->data(0, Qt::UserRole).toString().toStdWString());
    }
    if (extensions.empty())
        return;

    setEnabled(false);
    setCursor(Qt::WaitCursor);

    // Changes are immediate; outer Preferences Save/Cancel cannot undo them.
    vlc::win32::AssociationTask *task =
        new vlc::win32::AssociationTask(std::move(extensions));
    m_task = task;
    connect(task, &BaseAsyncTask::result, this, [this, task]() {
        vlc::win32::AssociationBatchResult batch = task->takeResult();
        task->abandon();
        m_task = nullptr;

        unsetCursor();
        setEnabled(true);

        msg_Dbg(m_intf, "default-app setup: %u changed, %u unchanged, %zu need confirmation, %zu failed",
                batch.changed, batch.unchanged, batch.needsUserChoice.size(), batch.failures.size());
        for (const auto &[extension, error] : batch.needsUserChoice)
            msg_Warn(m_intf, "default-app setup for %ls needs confirmation: %s",
                     extension.c_str(), error.c_str());
        for (const auto &[extension, error] : batch.failures)
            msg_Warn(m_intf, "default-app setup for %ls failed: %s",
                     extension.c_str(), error.c_str());

        const bool succeeded = batch.needsUserChoice.empty() && batch.failures.empty();
        QStringList messages;
        if (succeeded)
        {
            messages << qtr("Selected file types now open with VLC.");
            if (batch.changed > 0)
                messages << qtr("Changed: %1").arg(batch.changed);
            if (batch.unchanged > 0)
                messages << qtr("Already set: %1").arg(batch.unchanged);
        }
        else
            messages << (batch.changed == 0 && batch.unchanged == 0
                ? qtr("VLC could not be made your default media player.")
                : qtr("VLC could not be made the default for some file types."));

        if (!batch.needsUserChoice.empty())
            messages << qtr("You can change defaults manually in Windows Settings > Apps > Default apps.");

        auto *notice = new QMessageBox(succeeded ? QMessageBox::Information : QMessageBox::Warning,
                                      qtr("Default apps"), messages.join(QStringLiteral("\n")),
                                      QMessageBox::Ok, this);
        notice->setTextFormat(Qt::PlainText);
        notice->setAttribute(Qt::WA_DeleteOnClose);
        // Do not nest an event loop inside the worker's completion callback.
        // QObject owns the notice and disconnects delivery if Preferences closes.
        // Leave incomplete selections available for retry; diagnostics stay in the log.
        if (succeeded)
            connect(notice, &QDialog::finished, this, &QDialog::accept);
        notice->open();
    });
    task->start(*QThreadPool::globalInstance());
}
