// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <clocale>
#include <string>
#include <string_view>
#include <utility>
#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QScreen>
#include <QStringList>
#include <QWidget>
#include <QWindow>

#include "common/scm_rev.h"
#include "launcher/launcher.h"
#include "launcher/qt/art.h"
#include "launcher/qt/setup_dialog.h"
#include "launcher/qt/style.h"
#include "launcher/qt/text.h"

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
constexpr int LogoLeft = 16;
constexpr int LogoBottom = 12;
constexpr int LogoWidth = 290;
// Menu text is white with a black outline, and all black under the pointer.
constexpr qreal MenuOutline = 2.5;

enum class Item {
    Play,
    Options,
    Quit,
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
    }

    /// The player picked Play.
    bool Play() const {
        return play;
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
            play = true;
            close();
            break;
        case Item::Options:
            ShowSetup(this, art);
            // The menu takes the language Setup left.
            Layout();
            update();
            break;
        case Item::Quit:
            play = false;
            close();
            break;
        }
    }

    const Art& art;
    bool play = false;
    QString version;
    QFont menu_font;
    QFont version_font;
    QPixmap background;
    std::array<QRectF, Entries.size()> item_rects{};
    int hot = -1;
    int pressed = -1;
};

} // namespace

bool Run() {
    // Qt takes the desktop's locale. The game keeps the one it started in.
    const std::string locale = std::setlocale(LC_ALL, nullptr);
    bool play = false;
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
        const Art art;
        Window window{art};
        // Centred on the primary monitor, where the desktop lets a window place itself.
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        window.move(area.center() - window.rect().center());
        window.show();
        window.activateWindow();
        QApplication::exec();
        play = window.Play();
    }
    std::setlocale(LC_ALL, locale.c_str());
    return play;
}

} // namespace Gr2Port::Launcher
