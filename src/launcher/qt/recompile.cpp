// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <string>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include "common/path_util.h"
#include "core/recompiled/build.h"
#include "launcher/qt/recompile.h"
#include "launcher/qt/text.h"

namespace Gr2Port::Launcher {

namespace {

using Core::Recompiled::Build::Job;

// How often the window looks at the build, and at the Command Line Tools while they install.
constexpr std::chrono::milliseconds Poll{100};
constexpr std::chrono::milliseconds ToolsPoll{2000};
// How long a stopped build has to end.
constexpr std::chrono::milliseconds StopWait{5000};
// The bar's steps: the build's steps before the compile fill the first tenth, the compile, by
// files, most of the rest.
constexpr int Steps = 1000;
constexpr int BeforeCompile = 100;
constexpr int Compiled = 950;
constexpr int StepBeforeCompile = 15;
constexpr int Width = 480;
// The lines of the build's output that a failure shows.
constexpr qsizetype TailLines = 8;

QString Title() {
    return Tr("Gravity Rush 2");
}

QString Shown(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

// Whether a program exits with 0, within ten seconds.
bool Runs(const QString& program, const QStringList& arguments, QString* output = nullptr) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(10000) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        return false;
    }
    if (output != nullptr) {
        *output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }
    return true;
}

// Whether Apple's Command Line Tools (or Xcode) are there: xcode-select names their folder, and
// xcrun finds the compiler and Python in it. xcrun runs only once the folder is there, since
// without one it asks the player to install them.
bool HaveTools() {
    QString folder;
    return Runs(QStringLiteral("/usr/bin/xcode-select"), {QStringLiteral("-p")}, &folder) &&
           QDir{folder}.exists() &&
           Runs(QStringLiteral("/usr/bin/xcrun"),
                {QStringLiteral("--find"), QStringLiteral("clang")}) &&
           Runs(QStringLiteral("/usr/bin/xcrun"),
                {QStringLiteral("--find"), QStringLiteral("python3")});
}

// Asks to install the Command Line Tools, starts Apple's installer and waits for it. Returns
// whether they are there.
bool InstallTools() {
    QMessageBox ask{QMessageBox::Information, Title(),
                    Tr("On this Mac, Gravity Rush 2 runs from code that GR2port recompiles from "
                       "your copy of the game, once. That needs Apple's Command Line Tools, which "
                       "are not installed. Install them now?"),
                    QMessageBox::NoButton};
    ask.setFont(DialogFont());
    const QPushButton* const install = ask.addButton(Tr("Install"), QMessageBox::AcceptRole);
    ask.addButton(Tr("Quit"), QMessageBox::RejectRole);
    ask.exec();
    if (ask.clickedButton() != install) {
        return false;
    }
    QProcess::startDetached(QStringLiteral("/usr/bin/xcode-select"), {QStringLiteral("--install")});
    QProgressDialog wait{Tr("Waiting for the Command Line Tools to install. Apple's installer "
                            "shows their download."),
                         Tr("Cancel"), 0, 0};
    wait.setWindowTitle(Title());
    wait.setFont(DialogFont());
    wait.setMinimumWidth(Width);
    wait.setMinimumDuration(0);
    wait.setAutoClose(false);
    wait.setAutoReset(false);
    wait.show();
    bool installed = false;
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&installed, &loop] {
        if (HaveTools()) {
            installed = true;
            loop.quit();
        }
    });
    QObject::connect(&wait, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
    poll.start(ToolsPoll);
    loop.exec();
    return installed;
}

void Fail(const QString& text) {
    QMessageBox box{QMessageBox::Critical, Title(), text, QMessageBox::Ok};
    box.setFont(DialogFont());
    box.exec();
}

// A step the build prints ("== reading the executable"), as the window shows it.
QString StepText(const QString& line) {
    QString text = line.mid(3).trimmed();
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
    }
    return text;
}

} // namespace

bool RecompileWindow(const Job& job) {
    if (!HaveTools() && !InstallTools()) {
        return false;
    }
    const QString heading = Tr(job.title) + QStringLiteral("\n\n");
    QProgressDialog progress{heading + Tr("Reading the game's files..."), Tr("Cancel"), 0, 0};
    progress.setWindowTitle(Title());
    progress.setFont(DialogFont());
    progress.setMinimumWidth(Width);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.show();

    // The game's files, read off the window's thread.
    std::future<bool> prepared = std::async(std::launch::async, job.prepare);
    while (prepared.wait_for(Poll) != std::future_status::ready) {
        QApplication::processEvents();
    }
    if (!prepared.get()) {
        Fail(Tr("The game's files could not be read for the recompilation. The log in the "
                "userdata folder tells more."));
        return false;
    }
    if (progress.wasCanceled()) {
        return false;
    }

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const auto& [name, value] : job.environment) {
        environment.insert(QString::fromStdString(name), QString::fromStdString(value));
    }
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    QStringList arguments;
    for (const std::string& argument : job.arguments) {
        arguments.push_back(QString::fromStdString(argument));
    }
    process.start(Shown(job.program), arguments);
    progress.setRange(0, Steps);
    progress.setValue(0);

    std::ofstream log{job.log, std::ios::binary};
    const QRegularExpression compiled{QStringLiteral(R"(^\s+(\d+)/(\d+) \()")};
    QStringList tail;
    QByteArray pending;
    int value = 0;
    const auto take = [&](const QString& line) {
        log << line.toStdString() << '\n';
        tail.push_back(line);
        if (tail.size() > TailLines) {
            tail.pop_front();
        }
        if (const QRegularExpressionMatch match = compiled.match(line); match.hasMatch()) {
            const int done = match.captured(1).toInt();
            const int files = std::max(match.captured(2).toInt(), 1);
            value = BeforeCompile + (Compiled - BeforeCompile) * done / files;
            progress.setLabelText(heading + Tr("Compiling: {0} of {1} files",
                                               {std::to_string(done), std::to_string(files)}));
        } else if (line.startsWith(QStringLiteral("== compiling"))) {
            value = BeforeCompile;
            progress.setLabelText(heading + StepText(line));
        } else if (line.startsWith(QStringLiteral("== linking"))) {
            value = Compiled;
            progress.setLabelText(heading + Tr("Linking the libraries..."));
        } else if (line.startsWith(QStringLiteral("== "))) {
            value = std::min(value + StepBeforeCompile, std::max(value, BeforeCompile));
            progress.setLabelText(heading + StepText(line));
        }
        progress.setValue(value);
    };
    for (;;) {
        const bool finished = process.state() == QProcess::NotRunning;
        pending += process.readAll();
        for (qsizetype end; (end = pending.indexOf('\n')) >= 0; pending.remove(0, end + 1)) {
            take(QString::fromUtf8(pending.left(end)));
        }
        if (finished) {
            break;
        }
        if (progress.wasCanceled()) {
            // The build ends its compiles with it on a SIGTERM.
            process.terminate();
            if (!process.waitForFinished(static_cast<int>(StopWait.count()))) {
                process.kill();
                process.waitForFinished();
            }
            return false;
        }
        process.waitForReadyRead(static_cast<int>(Poll.count()));
        QApplication::processEvents();
    }
    if (!pending.isEmpty()) {
        take(QString::fromUtf8(pending));
    }
    log.close();
    if (process.error() == QProcess::FailedToStart) {
        Fail(Tr("{0} could not start for the recompilation.",
                {Common::FS::PathToUTF8String(job.program)}));
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        Fail(Tr("The recompilation failed. Its output is in\n{0}\n\nIts last lines:\n{1}",
                {Common::FS::PathToUTF8String(job.log), tail.join(QChar{'\n'}).toStdString()}));
        return false;
    }
    if (!job.install()) {
        Fail(Tr("The recompiled libraries could not be put in place. The log in the userdata "
                "folder tells more."));
        return false;
    }
    progress.setValue(Steps);
    return true;
}

} // namespace Gr2Port::Launcher
