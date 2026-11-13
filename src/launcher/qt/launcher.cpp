// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <clocale>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QFontMetricsF>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QStringList>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include "common/path_util.h"
#include "common/scm_rev.h"
#include "launcher/launcher.h"
#include "launcher/qt/art.h"
#include "launcher/qt/setup_dialog.h"
#include "launcher/qt/style.h"
#include "launcher/qt/text.h"
#include "launcher/updater.h"

namespace Gr2Port::Launcher {

namespace {

// Pixels at 96 DPI, as in the Windows launcher, which measured them from Rise of the Tomb
// Raider's launcher.
constexpr int Width = 790;
constexpr int Height = 445;
constexpr int MenuFontSize = 16;
constexpr int MenuRight = 39;
constexpr int VersionFontSize = 11;
constexpr int VersionRight = 27;
constexpr int VersionBaseline = 14;
// The side of the icon beside the message that no game is found.
constexpr int IconSize = 32;
constexpr int LogoLeft = 16;
constexpr int LogoBottom = 12;
constexpr int LogoWidth = 290;
// Menu text is white with a black outline, and all black under the pointer.
constexpr qreal MenuOutline = 2.5;

// The update window's size, and how often the launcher looks at the updater's threads.
constexpr int UpdateWidth = 480;
constexpr int UpdateHeight = 360;
constexpr std::chrono::milliseconds UpdatePoll{100};

enum class Item {
    Play,
    Options,
    Quit,
};

// How the launcher ends. Restart starts the executable again, for a change of the Paths tab or
// an update to take effect.
enum class Choice {
    Quit,
    Play,
    Restart,
};

struct Entry {
    Item item;
    std::string_view text;
    int top;
};

// Entries 23 pixels apart and Quit 13 further down, as in the original.
constexpr std::array Entries{
    Entry{Item::Play, "Play", 65},
    Entry{Item::Options, "Options", 88},
    Entry{Item::Quit, "Quit", 124},
};

QString Shown(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

// Tells what became of the request for the other user folder mode that this run started with.
void ShowFolderNotice(QWidget* owner) {
    const bool global = Common::FS::GetUserFolderMode() == Common::FS::UserFolderMode::Global;
    const QString global_folder = Shown(Common::FS::GetGlobalUserFolder());
    const QString portable_folder =
        Shown(Common::FS::GetExecutableDirectory() / Common::FS::PORTABLE_DIR);
    const QString& used = global ? global_folder : portable_folder;
    const QString& other = global ? portable_folder : global_folder;
    QString text;
    switch (Common::FS::GetUserFolderSwitch()) {
    case Common::FS::UserFolderSwitch::None:
        return;
    case Common::FS::UserFolderSwitch::Kept:
        text = Tr("A folder was already at\n{0}\nThe game uses it now. The folder at\n{1}\nstays "
                  "where it is.",
                  {used.toStdString(), other.toStdString()});
        break;
    case Common::FS::UserFolderSwitch::GlobalMissing:
        text = Tr("The global folder was not found at\n{0}\nA new portable userdata folder was "
                  "created instead.",
                  {global_folder.toStdString()});
        break;
    case Common::FS::UserFolderSwitch::Failed:
        text = Tr("The folder could not be moved to\n{0}\nThe game still uses\n{1}",
                  {other.toStdString(), used.toStdString()});
        break;
    }
    QMessageBox::warning(owner, Tr("User Data Folder"), text);
}

// Shows a release's name and notes, and returns whether the player wants the update.
bool AskUpdate(QWidget* owner, const Updater::Release& release) {
    QDialog dialog{owner};
    dialog.setWindowTitle(Tr("Update Available"));
    dialog.setFont(DialogFont());
    dialog.resize(UpdateWidth, UpdateHeight);
    auto* const name = new QLabel{QString::fromStdString(release.name), &dialog};
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    QFont bold = name->font();
    bold.setBold(true);
    name->setFont(bold);
    auto* const notes = new QTextBrowser{&dialog};
    notes->setMarkdown(QString::fromStdString(release.notes));
    notes->setOpenExternalLinks(true);
    auto* const question = new QLabel{Tr("Do you want to update now?"), &dialog};
    auto* const update = new QPushButton{Tr("Update"), &dialog};
    auto* const later = new QPushButton{Tr("Not Now"), &dialog};
    update->setDefault(true);
    auto* const buttons = new QHBoxLayout;
    buttons->addWidget(question, 1);
    buttons->addWidget(update);
    buttons->addWidget(later);
    auto* const places = new QVBoxLayout{&dialog};
    places->addWidget(name);
    places->addWidget(notes, 1);
    places->addLayout(buttons);
    QObject::connect(update, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(later, &QPushButton::clicked, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted;
}

// Downloads and installs a release behind a progress window. Returns whether its files are in
// place.
bool InstallUpdate(QWidget* owner, const Updater::Release& release) {
    QProgressDialog progress{Tr("Downloading the update..."), Tr("Cancel"), 0, 0, owner};
    progress.setWindowTitle(Tr("Update Available"));
    progress.setFont(DialogFont());
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.show();
    const auto download = std::make_shared<Updater::Download>();
    std::future<bool> installed = Updater::Install(release, download);
    while (installed.wait_for(UpdatePoll) != std::future_status::ready) {
        // The bar counts in KB, which fit its numbers.
        progress.setMaximum(static_cast<int>(download->size / 1024));
        progress.setValue(static_cast<int>(download->received / 1024));
        QApplication::processEvents();
        if (progress.wasCanceled()) {
            download->stopped = true;
            return false;
        }
    }
    progress.close();
    if (!installed.get()) {
        QMessageBox::warning(owner, Tr("Update Available"),
                             Tr("The update could not be installed."));
        return false;
    }
    return true;
}

QString VersionText() {
    const std::string_view revision{Common::g_scm_rev};
    QString text = QStringLiteral("GR2port");
    if (!revision.empty()) {
        text += QStringLiteral(" build ") +
                QString::fromUtf8(revision.data(), static_cast<qsizetype>(revision.size())).left(8);
    }
    return text;
}

// The splash fills the window from its middle and darkens under the menu; the logo sits at the
// bottom left, and a grey line edges the window.
QPixmap ComposeBackground(const Art& art, qreal ratio) {
    return Art::Render(Width, Height, ratio, [&](QPainter& painter) {
        constexpr qreal Edge = 1.0;
        const QRectF inner{Edge, Edge, Width - 2.0 * Edge, Height - 2.0 * Edge};
        const qreal part_width = art.SplashHeight() * inner.width() / inner.height();
        art.DrawSplash(
            painter, inner,
            {(art.SplashWidth() - part_width) / 2.0, 0.0, part_width, art.SplashHeight()});
        Art::Fade(
            painter,
            {inner.x() + inner.width() * 0.58, inner.y(), inner.width() * 0.42, inner.height()},
            170);
        const qreal logo_width = LogoWidth;
        const qreal logo_height = logo_width / art.LogoAspect();
        art.DrawLogo(painter,
                     {LogoLeft, Height - LogoBottom - logo_height, logo_width, logo_height});
        const QColor grey{100, 100, 100};
        painter.fillRect(QRectF{0.0, 0.0, Width, Edge}, grey);
        painter.fillRect(QRectF{0.0, Height - Edge, Width, Edge}, grey);
        painter.fillRect(QRectF{0.0, 0.0, Edge, Height}, grey);
        painter.fillRect(QRectF{Width - Edge, 0.0, Edge, Height}, grey);
    });
}

// Verdana, as in the original, or the typeface closest to it. The desktop's fonts supply the
// letters these lack.
QFont MenuFont() {
    QFont font;
    font.setFamilies(
        {QStringLiteral("Verdana"), QStringLiteral("DejaVu Sans"), QStringLiteral("Noto Sans")});
    font.setPixelSize(MenuFontSize);
    return font;
}

QFont VersionFont() {
    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Noto Sans")});
    font.setPixelSize(VersionFontSize);
    return font;
}

// The dialogs are white with dark text, as the Windows launcher's, whatever the desktop's theme.
QPalette DialogPalette() {
    QPalette palette{QColor{240, 240, 240}};
    palette.setColor(QPalette::Window, Qt::white);
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Highlight, QColor{0, 120, 215});
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor{0, 102, 204});
    return palette;
}

class Window : public QWidget {
public:
    explicit Window(const Art& pictures)
        : QWidget{nullptr, Qt::Window | Qt::FramelessWindowHint}, art{pictures},
          version{VersionText()}, menu_font{MenuFont()}, version_font{VersionFont()} {
        setWindowTitle(QStringLiteral("Gravity Rush 2"));
        setWindowIcon(art.Icon());
        setFixedSize(Width, Height);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        Layout();
        // The updater's answer comes on a thread of its own; the window looks for it.
        auto* const poll = new QTimer{this};
        QObject::connect(poll, &QTimer::timeout, this, [this, poll] {
            // The offer waits for a dialog over the launcher to close.
            if (QApplication::activeModalWidget() != nullptr ||
                found.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
                return;
            }
            poll->stop();
            const std::optional<Updater::Release> release = found.get();
            if (release && AskUpdate(this, *release) && InstallUpdate(this, *release)) {
                choice = Choice::Restart;
                close();
            }
        });
        poll->start(UpdatePoll);
    }

    /// How the player left the launcher.
    Choice Chosen() const {
        return choice;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const qreal ratio = devicePixelRatioF();
        if (background.isNull() || background.devicePixelRatio() != ratio) {
            background = ComposeBackground(art, ratio);
        }
        QPainter painter{this};
        painter.drawPixmap(0, 0, background);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen outline{Qt::black, MenuOutline};
        outline.setJoinStyle(Qt::RoundJoin);
        const QFontMetricsF metrics{menu_font};
        for (size_t i = 0; i < Entries.size(); ++i) {
            QPainterPath path;
            path.addText(QPointF{item_rects[i].left(), item_rects[i].top() + metrics.ascent()},
                         menu_font, Tr(Entries[i].text));
            painter.strokePath(path, outline);
            painter.fillPath(path, static_cast<int>(i) == hot ? Qt::black : Qt::white);
        }
        painter.setFont(version_font);
        painter.setPen(Qt::white);
        const qreal version_width = QFontMetricsF{version_font}.horizontalAdvance(version);
        painter.drawText(
            QPointF{Width - VersionRight - version_width, qreal{Height - VersionBaseline}},
            version);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const int item = ItemAt(event->position());
        setCursor(item >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        SetHot(item);
    }

    void leaveEvent(QEvent*) override {
        SetHot(-1);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        pressed = ItemAt(event->position());
        // The art drags the window, as a title bar would.
        if (pressed < 0 && windowHandle() != nullptr) {
            windowHandle()->startSystemMove();
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        const int was_pressed = std::exchange(pressed, -1);
        const int item = ItemAt(event->position());
        if (item >= 0 && item == was_pressed) {
            Activate(Entries[item].item);
        }
    }

    void keyPressEvent(QKeyEvent* event) override {
        const int count = static_cast<int>(Entries.size());
        switch (event->key()) {
        case Qt::Key_Up:
            SetHot(hot <= 0 ? count - 1 : hot - 1);
            break;
        case Qt::Key_Down:
            SetHot(hot < 0 ? 0 : (hot + 1) % count);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            Activate(hot >= 0 ? Entries[hot].item : Item::Play);
            break;
        case Qt::Key_Escape:
            Activate(Item::Quit);
            break;
        default:
            QWidget::keyPressEvent(event);
            break;
        }
    }

private:
    // Places the entries, right aligned, in the language in use.
    void Layout() {
        const QFontMetricsF metrics{menu_font};
        const qreal right = Width - MenuRight;
        for (size_t i = 0; i < Entries.size(); ++i) {
            const qreal width = metrics.horizontalAdvance(Tr(Entries[i].text));
            item_rects[i] = QRectF{right - width, qreal(Entries[i].top), width, metrics.height()};
        }
    }

    int ItemAt(const QPointF& point) const {
        for (size_t i = 0; i < item_rects.size(); ++i) {
            if (item_rects[i].adjusted(-4.0, -1.0, 4.0, 1.0).contains(point)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void SetHot(int item) {
        if (hot != item) {
            hot = item;
            update();
        }
    }

    void Activate(Item item) {
        switch (item) {
        case Item::Play:
            choice = Choice::Play;
            close();
            break;
        case Item::Options:
            if (ShowSetup(this, art)) {
                choice = Choice::Restart;
                close();
                break;
            }
            // The menu takes the language Setup left.
            Layout();
            update();
            break;
        case Item::Quit:
            choice = Choice::Quit;
            close();
            break;
        }
    }

    const Art& art;
    Choice choice = Choice::Quit;
    // The updater's answer, asked for as the launcher opens.
    std::future<std::optional<Updater::Release>> found = Updater::Check();
    QString version;
    QFont menu_font;
    QFont version_font;
    QPixmap background;
    std::array<QRectF, Entries.size()> item_rects{};
    int hot = -1;
    int pressed = -1;
};

// What show returns, which runs with a Qt application in the launcher's style.
template <typename Show>
auto InApplication(Show&& show) {
    // Qt takes the desktop's locale. The game keeps the one it started in.
    const std::string locale = std::setlocale(LC_ALL, nullptr);
    decltype(show()) shown{};
    {
        int argc = 1;
        char name[] = "Gravity Rush 2";
        char* argv[] = {name, nullptr};
        QApplication application{argc, argv};
        // The settings files are read and written with the C locale's numbers.
        std::setlocale(LC_NUMERIC, "C");
        // The application owns the style.
        QApplication::setStyle(MakeStyle());
        QApplication::setPalette(DialogPalette());
        // The dialogs' places are fixed, left to right, in every language.
        QApplication::setLayoutDirection(Qt::LeftToRight);
        shown = show();
    }
    std::setlocale(LC_ALL, locale.c_str());
    return shown;
}

} // namespace

std::optional<std::filesystem::path> AskGameFolder(const std::string& text) {
    const QString folder = InApplication([&text] {
        QDialog dialog;
        dialog.setWindowTitle(QStringLiteral("Gravity Rush 2"));
        dialog.setFont(DialogFont());
        auto* const icon = new QLabel{&dialog};
        icon->setPixmap(
            dialog.style()->standardIcon(QStyle::SP_MessageBoxCritical).pixmap(IconSize, IconSize));
        auto* const message = new QLabel{QString::fromStdString(text), &dialog};
        message->setTextFormat(Qt::PlainText);
        message->setWordWrap(true);
        auto* const ok = new QPushButton{Tr("OK"), &dialog};
        ok->setDefault(true);
        auto* const link =
            new QLabel{QStringLiteral("<a href=\"#\">%1</a>")
                           .arg(Tr("Choose custom game directory instead").toHtmlEscaped()),
                       &dialog};
        auto* const places = new QGridLayout{&dialog};
        places->addWidget(icon, 0, 0, Qt::AlignTop);
        places->addWidget(message, 0, 1);
        places->addWidget(ok, 1, 0, 1, 2, Qt::AlignHCenter);
        places->addWidget(link, 2, 0, 1, 2, Qt::AlignHCenter);
        bool choose = false;
        QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
        QObject::connect(link, &QLabel::linkActivated, &dialog, [&dialog, &choose] {
            choose = true;
            dialog.accept();
        });
        dialog.exec();
        return choose ? QFileDialog::getExistingDirectory(
                            nullptr, Tr("Choose the folder that holds your game and its update"))
                      : QString{};
    });
    return folder.isEmpty() ? std::nullopt
                            : std::optional{std::filesystem::path{folder.toStdString()}};
}

bool Run() {
    const Choice choice = InApplication([] {
        const Art art;
        Window window{art};
        // Centred on the primary monitor, where the desktop lets a window place itself.
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        window.move(area.center() - window.rect().center());
        window.show();
        window.activateWindow();
        ShowFolderNotice(&window);
        QApplication::exec();
        return window.Chosen();
    });
    // The executable starts again once Qt has let go of the desktop.
    if (choice == Choice::Restart) {
        Common::FS::RestartExecutable();
        return false;
    }
    return choice == Choice::Play;
}

} // namespace Gr2Port::Launcher
