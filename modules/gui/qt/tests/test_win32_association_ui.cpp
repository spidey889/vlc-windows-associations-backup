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

#include "win32/associations/win32_associations.hpp"
#include "../util/win32_association_task.hpp"
#include "../util/win32_association_types.hpp"

#include <windows.h>
#include <shellapi.h>
#include <QApplication>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QTreeWidget>
#include <QtPlugin>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace
{
std::mutex backendMutex;
std::vector<std::wstring> reads, writes;
std::set<std::wstring> defaults;
std::map<std::wstring, vlc::win32::AssociationResult> results;
std::atomic_bool offUiThread{true}, pauseRead{false}, pauseWrite{false};
QSemaphore entered, resume;
unsigned settingsLaunches = 0;

void pauseOnce(std::atomic_bool &pause)
{
    if (QThread::currentThread() == qApp->thread())
        offUiThread = false;
    if (pause.exchange(false))
    {
        entered.release();
        // Bounded even if an assertion fails before the test releases us.
        resume.tryAcquire(1, 5000);
    }
}

[[maybe_unused]] HINSTANCE WINAPI testLaunchSettings(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT)
{
    ++settingsLaunches;
    return reinterpret_cast<HINSTANCE>(33);
}
}

// Test the actual dialog/task without registry writes, VLC registrations, or
// Settings launches. Retain the OS-launch trap to detect automatic handoff
// regressions; the backend boundary is stubbed as a linked implementation.
#define ShellExecuteW testLaunchSettings
#include "../dialogs/preferences/win32_association_dialog.cpp"
#undef ShellExecuteW

extern "C" const char vlc_module_name[] = "association-ui-test";

namespace vlc::win32
{
bool isDefaultAssociation(std::wstring_view extension) noexcept
{
    pauseOnce(pauseRead);
    const std::lock_guard<std::mutex> lock(backendMutex);
    reads.emplace_back(extension);
    return defaults.count(std::wstring(extension)) != 0;
}

AssociationResult setDefaultAssociation(std::wstring_view extension) noexcept
{
    pauseOnce(pauseWrite);
    const std::lock_guard<std::mutex> lock(backendMutex);
    writes.emplace_back(extension);
    const auto found = results.find(std::wstring(extension));
    return found == results.end() ? AssociationResult{AssociationStatus::changed, {}}
                                 : found->second;
}
}

class TestWin32AssociationUi : public QObject
{
    Q_OBJECT

    static QTreeWidget *tree(Win32AssociationDialog &dialog)
    {
        return dialog.findChild<QTreeWidget *>();
    }

    static QPushButton *applyButton(Win32AssociationDialog &dialog)
    {
        const auto *box = dialog.findChild<QDialogButtonBox *>();
        for (QAbstractButton *button : box->buttons())
            if (box->buttonRole(button) == QDialogButtonBox::ApplyRole)
                return qobject_cast<QPushButton *>(button);
        return nullptr;
    }

    static std::vector<std::wstring> appliedExtensions()
    {
        const std::lock_guard<std::mutex> lock(backendMutex);
        return writes;
    }

    static void verifyFailureNotice(const QMessageBox *notice, bool partial, bool manual = false)
    {
        QCOMPARE(notice->icon(), QMessageBox::Warning);
        QCOMPARE(notice->textFormat(), Qt::PlainText);
        QString expected = partial
            ? QStringLiteral("VLC could not be made the default for some file types.")
            : QStringLiteral("VLC could not be made your default media player.");
        if (manual)
            expected += QStringLiteral("\nYou can change defaults manually in Windows Settings > Apps > Default apps.");
        QCOMPARE(notice->text(), expected);
        QCOMPARE(notice->standardButtons(), QMessageBox::Ok);
        QCOMPARE(notice->buttons().size(), 1);
        QVERIFY(notice->informativeText().isEmpty());
        QVERIFY(notice->detailedText().isEmpty());
        QCOMPARE(settingsLaunches, 0U);
    }

private slots:
    void init()
    {
        reads.clear();
        writes.clear();
        defaults.clear();
        results.clear();
        offUiThread = true;
        pauseRead = false;
        pauseWrite = false;
        settingsLaunches = 0;
        entered.tryAcquire(entered.available());
        resume.tryAcquire(resume.available());
    }

    void cleanup()
    {
        resume.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(6000));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void loadingAndCancel()
    {
        pauseRead = true;
        auto dialog = std::make_unique<Win32AssociationDialog>(nullptr);
        QVERIFY(entered.tryAcquire(1, 1000));
        QVERIFY(offUiThread.load());
        QVERIFY(!tree(*dialog)->isEnabled());
        QVERIFY(!applyButton(*dialog)->isEnabled());
        QSignalSpy rejected(dialog.get(), &QDialog::rejected);
        dialog->reject(); // Read-only loading must not trap Escape/Cancel.
        QCOMPARE(rejected.count(), 1);
        dialog.reset();
        resume.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(6000));
        QVERIFY(appliedExtensions().empty());
        QCOMPARE(settingsLaunches, 0U);
    }

    void categorySelection()
    {
        defaults = {std::wstring(vlc::win32::audioExtensions[0])};
        Win32AssociationDialog dialog(nullptr);
        QTRY_VERIFY(tree(dialog)->isEnabled());
        QVERIFY(offUiThread.load());
        QCOMPARE(tree(dialog)->topLevelItemCount(), 3);
        auto *audio = tree(dialog)->topLevelItem(0);
        QCOMPARE(audio->checkState(0), Qt::PartiallyChecked);
        QVERIFY(!audio->isExpanded());
        QVERIFY(applyButton(dialog)->isEnabled());
        tree(dialog)->setCurrentItem(audio);
        QTest::keyClick(tree(dialog), Qt::Key_Space);
        for (int index = 0; index < audio->childCount(); ++index)
            QCOMPARE(audio->child(index)->checkState(0), Qt::Checked);
        audio->setExpanded(true);
        tree(dialog)->setCurrentItem(audio->child(0));
        QTest::keyClick(tree(dialog), Qt::Key_Space);
        QCOMPARE(audio->checkState(0), Qt::PartiallyChecked);
        audio->setCheckState(0, Qt::Unchecked);
        for (int index = 0; index < audio->childCount(); ++index)
            QCOMPARE(audio->child(index)->checkState(0), Qt::Unchecked);
        QVERIFY(!applyButton(dialog)->isEnabled());
        applyButton(dialog)->click();
        dialog.reject();
        QVERIFY(appliedExtensions().empty());
        const std::lock_guard<std::mutex> lock(backendMutex);
        QVERIFY(reads == vlc::win32::allMediaExtensions());
    }

    void parentOwnedLifetime()
    {
        auto parent = std::make_unique<QDialog>();
        const QPointer<Win32AssociationDialog> dialog =
            new Win32AssociationDialog(nullptr, parent.get());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
        QTRY_VERIFY(tree(*dialog)->isEnabled());
        QCOMPARE(dialog->windowModality(), Qt::WindowModal);
        dialog->reject();
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(appliedExtensions().empty());

        pauseRead = true;
        const QPointer<Win32AssociationDialog> loading =
            new Win32AssociationDialog(nullptr, parent.get());
        loading->setAttribute(Qt::WA_DeleteOnClose);
        loading->open();
        QVERIFY(entered.tryAcquire(1, 1000));
        parent.reset();
        QVERIFY(loading.isNull());
        resume.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(6000));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QVERIFY(appliedExtensions().empty());
        QCOMPARE(settingsLaunches, 0U);
    }

    void workerAggregation()
    {
        using namespace vlc::win32;
        const std::vector<std::wstring> extensions = {L".mp4", L".mp3", L".mkv", L".wav"};
        results[L".mp3"] = {AssociationStatus::unchanged, {}};
        results[L".mkv"] = {AssociationStatus::needsUserChoice, "Confirmation reason"};
        results[L".wav"] = {AssociationStatus::failed, "Failure reason"};
        auto *task = new AssociationTask(extensions);
        QSignalSpy completed(task, &BaseAsyncTask::result);
        task->start(*QThreadPool::globalInstance());
        QTRY_COMPARE(completed.count(), 1);
        const auto batch = task->takeResult();
        task->abandon();
        QCOMPARE(batch.changed, 1U);
        QCOMPARE(batch.unchanged, 1U);
        QVERIFY((batch.needsUserChoice == decltype(batch.needsUserChoice){{L".mkv", "Confirmation reason"}}));
        QVERIFY((batch.failures == decltype(batch.failures){{L".wav", "Failure reason"}}));
        QVERIFY(appliedExtensions() == extensions);
        QVERIFY(offUiThread.load());
        QCOMPARE(settingsLaunches, 0U); // The worker never launches UI.
    }

    void lockContentionStopsBatch()
    {
        using namespace vlc::win32;
        const std::vector<std::wstring> extensions = {L".mp4", L".mp3", L".mkv", L".wav"};
        results[L".mp3"] = {AssociationStatus::failed, "Another association update is still running", true};
        auto *task = new AssociationTask(extensions);
        QSignalSpy completed(task, &BaseAsyncTask::result);
        task->start(*QThreadPool::globalInstance());
        QTRY_COMPARE(completed.count(), 1);
        const auto batch = task->takeResult();
        task->abandon();
        QCOMPARE(batch.changed, 1U);
        QCOMPARE(batch.failures.size(), std::size_t{3});
        QCOMPARE(batch.failures[0].first, extensions[1]);
        QCOMPARE(batch.failures[1].first, extensions[2]);
        QCOMPARE(batch.failures[2].first, extensions[3]);
        QVERIFY(batch.failures[1].second.find("Not attempted") == 0);
        QVERIFY(batch.failures[2].second.find("Not attempted") == 0);
        QVERIFY(appliedExtensions() == std::vector<std::wstring>({L".mp4", L".mp3"}));
        QCOMPARE(settingsLaunches, 0U);
    }

    void emptyBatch()
    {
        auto *task = new vlc::win32::AssociationTask({});
        QSignalSpy completed(task, &BaseAsyncTask::result);
        task->start(*QThreadPool::globalInstance());
        QTRY_COMPARE(completed.count(), 1);
        const auto batch = task->takeResult();
        task->abandon();
        QCOMPARE(batch.changed, 0U);
        QCOMPARE(batch.unchanged, 0U);
        QVERIFY(batch.failures.empty() && batch.needsUserChoice.empty());
        QVERIFY(appliedExtensions().empty());
    }

    void selectedOnlyAndCloseProtection()
    {
        Win32AssociationDialog dialog(nullptr);
        QTRY_VERIFY(tree(dialog)->isEnabled());
        auto *selected = tree(dialog)->topLevelItem(1)->child(0);
        const auto identifier = selected->data(0, Qt::UserRole).toString().toStdWString();
        selected->setText(0, QStringLiteral("Display label, not a ProgId"));
        selected->setCheckState(0, Qt::Checked);
        QSignalSpy rejected(&dialog, &QDialog::rejected);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        pauseWrite = true;
        applyButton(dialog)->click();
        QVERIFY(entered.tryAcquire(1, 1000));
        QVERIFY(!dialog.isEnabled());
        dialog.reject();
        QCOMPARE(rejected.count(), 0);
        applyButton(dialog)->click(); // Double clicks cannot submit another batch.
        resume.release();
        QTRY_VERIFY(dialog.findChild<QMessageBox *>() != nullptr);
        QVERIFY(dialog.isEnabled());
        QVERIFY(offUiThread.load());
        QVERIFY(appliedExtensions() == std::vector<std::wstring>{identifier});
        QCOMPARE(settingsLaunches, 0U);
        auto *notice = dialog.findChild<QMessageBox *>();
        QCOMPARE(notice->icon(), QMessageBox::Information);
        notice->accept();
        QCOMPARE(accepted.count(), 1);
    }

    void partialFailureAndRetry()
    {
        Win32AssociationDialog dialog(nullptr);
        QTRY_VERIFY(tree(dialog)->isEnabled());
        auto *audio = tree(dialog)->topLevelItem(0);
        audio->setCheckState(0, Qt::Checked);
        results[std::wstring(vlc::win32::audioExtensions[0])] =
            {vlc::win32::AssociationStatus::unchanged, {}};
        for (unsigned index = 1; index <= 6; ++index)
            results[std::wstring(vlc::win32::audioExtensions[index])] =
                {vlc::win32::AssociationStatus::failed, "Error <not markup>"};
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        applyButton(dialog)->click();
        QTRY_VERIFY(dialog.findChild<QMessageBox *>() != nullptr);
        auto *notice = dialog.findChild<QMessageBox *>();
        verifyFailureNotice(notice, true);
        notice->accept();
        QCOMPARE(accepted.count(), 0);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        results.clear();
        audio->setCheckState(0, Qt::Unchecked);
        audio->child(1)->setCheckState(0, Qt::Checked);
        applyButton(dialog)->click();
        QTRY_VERIFY(dialog.findChild<QMessageBox *>() != nullptr);
        dialog.findChild<QMessageBox *>()->accept();
        QCOMPARE(accepted.count(), 1);
        QCOMPARE(appliedExtensions().size(), std::size(vlc::win32::audioExtensions) + 1);
    }

    void confirmationAndRetry()
    {
        Win32AssociationDialog dialog(nullptr);
        QTRY_VERIFY(tree(dialog)->isEnabled());
        const auto extension = std::wstring(vlc::win32::audioExtensions[0]);
        results[extension] = {vlc::win32::AssociationStatus::needsUserChoice, "Unknown hash format"};
        tree(dialog)->topLevelItem(0)->child(0)->setCheckState(0, Qt::Checked);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        applyButton(dialog)->click();
        QTRY_VERIFY(dialog.findChild<QMessageBox *>() != nullptr);
        auto *notice = dialog.findChild<QMessageBox *>();
        verifyFailureNotice(notice, false, true);
        QVERIFY(appliedExtensions() == std::vector<std::wstring>{extension});
        notice->accept();
        QCOMPARE(accepted.count(), 0); // Confirmation is incomplete, not success.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        results.clear();
        applyButton(dialog)->click();
        QTRY_VERIFY(dialog.findChild<QMessageBox *>() != nullptr);
        dialog.findChild<QMessageBox *>()->accept();
        QCOMPARE(accepted.count(), 1);
        QVERIFY(appliedExtensions() == std::vector<std::wstring>({extension, extension}));
    }

    void allFailed()
    {
        Win32AssociationDialog dialog(nullptr);
        QTRY_VERIFY(tree(dialog)->isEnabled());
        for (const auto &extension : vlc::win32::allMediaExtensions())
            results[extension] = {vlc::win32::AssociationStatus::failed, "Failure reason"};
        for (int index = 0; index < tree(dialog)->topLevelItemCount(); ++index)
            tree(dialog)->topLevelItem(index)->setCheckState(0, Qt::Checked);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        applyButton(dialog)->click();
        QTRY_VERIFY(dialog.findChild<QMessageBox *>() != nullptr);
        auto *notice = dialog.findChild<QMessageBox *>();
        verifyFailureNotice(notice, false);
        QVERIFY(appliedExtensions() == vlc::win32::allMediaExtensions());
        notice->accept();
        QCOMPARE(accepted.count(), 0);
    }

    void mixedFailureAndConfirmation()
    {
        Win32AssociationDialog dialog(nullptr);
        QTRY_VERIFY(tree(dialog)->isEnabled());
        const auto confirmation = std::wstring(vlc::win32::audioExtensions[0]);
        const auto failure = std::wstring(vlc::win32::audioExtensions[1]);
        results[confirmation] = {vlc::win32::AssociationStatus::needsUserChoice, "Confirmation reason"};
        results[failure] = {vlc::win32::AssociationStatus::failed, "Failure reason"};
        auto *audio = tree(dialog)->topLevelItem(0);
        audio->child(0)->setCheckState(0, Qt::Checked);
        audio->child(1)->setCheckState(0, Qt::Checked);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        applyButton(dialog)->click();
        QTRY_VERIFY(dialog.findChild<QMessageBox *>() != nullptr);
        auto *notice = dialog.findChild<QMessageBox *>();
        verifyFailureNotice(notice, false, true);
        notice->accept();
        QCOMPARE(accepted.count(), 0); // Failed types remain available for retry.
    }

    void destructionAfterCompletion()
    {
        auto dialog = std::make_unique<Win32AssociationDialog>(nullptr);
        QTRY_VERIFY(tree(*dialog)->isEnabled());
        tree(*dialog)->topLevelItem(0)->child(0)->setCheckState(0, Qt::Checked);
        applyButton(*dialog)->click();
        QTRY_VERIFY(dialog->findChild<QMessageBox *>() != nullptr);
        const QPointer<QMessageBox> notice = dialog->findChild<QMessageBox *>();
        dialog.reset();
        QVERIFY(notice.isNull());
        QCoreApplication::processEvents();
    }

    void destructionDuringApply()
    {
        auto dialog = std::make_unique<Win32AssociationDialog>(nullptr);
        QTRY_VERIFY(tree(*dialog)->isEnabled());
        const auto extension = std::wstring(vlc::win32::audioExtensions[0]);
        results[extension] = {vlc::win32::AssociationStatus::needsUserChoice, "No receiver"};
        tree(*dialog)->topLevelItem(0)->child(0)->setCheckState(0, Qt::Checked);
        pauseWrite = true;
        applyButton(*dialog)->click();
        QVERIFY(entered.tryAcquire(1, 1000));
        dialog.reset();
        resume.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(6000));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCOMPARE(settingsLaunches, 0U);
        QVERIFY(appliedExtensions() == std::vector<std::wstring>{extension});
    }
};

#ifdef QT_STATIC
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#endif

QTEST_MAIN(TestWin32AssociationUi)
#include "test_win32_association_ui.moc"
