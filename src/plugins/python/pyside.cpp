// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "pyside.h"

#include "pipsupport.h"
#include "pythonplugin.h"
#include "pythontr.h"
#include "pythonutils.h"

#include <coreplugin/icore.h>

#include <projectexplorer/runconfigurationaspects.h>
#include <projectexplorer/target.h>

#include <qtsupport/qtoptionspage.h>

#include <texteditor/textdocument.h>

#include <utils/algorithm.h>
#include <utils/async.h>
#include <utils/infobar.h>
#include <utils/process.h>
#include <utils/qtcassert.h>

#include <QBoxLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QTextCursor>

using namespace Utils;
using namespace ProjectExplorer;

namespace Python::Internal {

const char installPySideInfoBarId[] = "Python::InstallPySide";

PySideInstaller *PySideInstaller::instance()
{
    static PySideInstaller *instance = new PySideInstaller; // FIXME: Leaks.
    return instance;
}

void PySideInstaller::checkPySideInstallation(const FilePath &python,
                                              TextEditor::TextDocument *document)
{
    document->infoBar()->removeInfo(installPySideInfoBarId);
    const QString pySide = importedPySide(document->plainText());
    if (pySide == "PySide2" || pySide == "PySide6")
        instance()->runPySideChecker(python, pySide, document);
}

bool PySideInstaller::missingPySideInstallation(const FilePath &pythonPath,
                                                const QString &pySide)
{
    QTC_ASSERT(!pySide.isEmpty(), return false);
    static QMap<FilePath, QSet<QString>> pythonWithPyside;
    if (pythonWithPyside[pythonPath].contains(pySide))
        return false;

    Process pythonProcess;
    pythonProcess.setCommand({pythonPath, {"-c", "import " + pySide}});
    pythonProcess.runBlocking();
    const bool missing = pythonProcess.result() != ProcessResult::FinishedWithSuccess;
    if (!missing)
        pythonWithPyside[pythonPath].insert(pySide);
    return missing;
}

QString PySideInstaller::importedPySide(const QString &text)
{
    static QRegularExpression importScanner("^\\s*(import|from)\\s+(PySide\\d)",
                                            QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = importScanner.match(text);
    return match.captured(2);
}

PySideInstaller::PySideInstaller()
    : QObject(PythonPlugin::instance())
{}

void PySideInstaller::installPyside(const FilePath &python,
                                    const QString &pySide,
                                    TextEditor::TextDocument *document)
{
    QMap<QVersionNumber, Utils::FilePath> availablePySides;

    const std::optional<FilePath> qtInstallDir
        = QtSupport::LinkWithQtSupport::linkedQt().tailRemoved("Tools/sdktool/share/qtcreator");
    if (qtInstallDir) {
        const FilePath qtForPythonDir = qtInstallDir->pathAppended("QtForPython");
        for (const FilePath &versionDir : qtForPythonDir.dirEntries(QDir::Dirs | QDir::NoDotAndDotDot)) {
            FilePath requirements = versionDir.pathAppended("requirements.txt");
            if (requirements.exists())
                availablePySides[QVersionNumber::fromString(versionDir.fileName())] = requirements;
        }
    }


    auto install = new PipInstallTask(python);
    connect(install, &PipInstallTask::finished, install, &QObject::deleteLater);
    connect(install, &PipInstallTask::finished, this, [=](bool success){
        if (success)
            emit pySideInstalled(python, pySide);
    });
    if (availablePySides.isEmpty()) {
        install->setPackages({PipPackage(pySide)});
    } else {
        QDialog dialog;
        dialog.setWindowTitle(Tr::tr("Select PySide version"));
        dialog.setLayout(new QVBoxLayout());
        dialog.layout()->addWidget(new QLabel(Tr::tr("Select which PySide version to install:")));
        QComboBox *pySideSelector = new QComboBox();
        pySideSelector->addItem(Tr::tr("Latest PySide from the Python Package Index"));
        for (const Utils::FilePath &version : std::as_const(availablePySides)) {
            const FilePath dir = version.parentDir();
            const QString text
                = Tr::tr("PySide %1 wheel (%2)").arg(dir.fileName(), dir.toUserOutput());
            pySideSelector->addItem(text, version.toVariant());
        }
        dialog.layout()->addWidget(pySideSelector);
        QDialogButtonBox box;
        box.setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        dialog.layout()->addWidget(&box);
        connect(&box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() == QDialog::Rejected)
            return;

        const FilePath requirementsFile = FilePath::fromVariant(pySideSelector->currentData());
        if (requirementsFile.isEmpty()) {
            install->setPackages({PipPackage(pySide)});
        } else {
            install->setWorkingDirectory(requirementsFile.parentDir());
            install->setRequirements(requirementsFile);
        }
    }
    document->infoBar()->removeInfo(installPySideInfoBarId);
    install->run();
}

void PySideInstaller::handlePySideMissing(const FilePath &python,
                                          const QString &pySide,
                                          TextEditor::TextDocument *document)
{
    if (!document || !document->infoBar()->canInfoBeAdded(installPySideInfoBarId))
        return;
    const QString message = Tr::tr("%1 installation missing for %2 (%3)")
                                .arg(pySide, pythonName(python), python.toUserOutput());
    InfoBarEntry info(installPySideInfoBarId, message, InfoBarEntry::GlobalSuppression::Enabled);
    auto installCallback = [=]() { installPyside(python, pySide, document); };
    const QString installTooltip = Tr::tr("Install %1 for %2 using pip package installer.")
                                       .arg(pySide, python.toUserOutput());
    info.addCustomButton(Tr::tr("Install"), installCallback, installTooltip);
    document->infoBar()->addInfo(info);
}

void PySideInstaller::runPySideChecker(const FilePath &python,
                                       const QString &pySide,
                                       TextEditor::TextDocument *document)
{
    using CheckPySideWatcher = QFutureWatcher<bool>;

    QPointer<CheckPySideWatcher> watcher = new CheckPySideWatcher();

    // cancel and delete watcher after a 10 second timeout
    QTimer::singleShot(10000, this, [watcher]() {
        if (watcher) {
            watcher->cancel();
            watcher->deleteLater();
        }
    });
    connect(watcher,
            &CheckPySideWatcher::resultReadyAt,
            this,
            [=, document = QPointer<TextEditor::TextDocument>(document)]() {
                if (watcher->result())
                    handlePySideMissing(python, pySide, document);
                watcher->deleteLater();
            });
    watcher->setFuture(Utils::asyncRun(&missingPySideInstallation, python, pySide));
}

} // Python::Internal
