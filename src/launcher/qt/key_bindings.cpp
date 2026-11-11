// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>
#include <QDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QWheelEvent>

#include "input/game_functions.h"
#include "input/input_handler.h"
#include "launcher/qt/key_bindings.h"
#include "launcher/qt/text.h"

namespace Gr2Port::Launcher {

namespace {

namespace Game = Input::GameFunctions;

// Pixels at 96 DPI in the dialog's client area, as in the Windows launcher.
constexpr int ClientWidth = 560;
constexpr int ClientHeight = 430;
constexpr QRect TabArea{QPoint{10, 10}, QPoint{549, 329}};
constexpr QRect TypeLine{QPoint{10, 338}, QPoint{549, 352}};
constexpr QRect StatusLine{QPoint{10, 358}, QPoint{549, 372}};
constexpr QRect ChangeButton{QPoint{10, 398}, QPoint{109, 419}};
constexpr QRect ClearButton{QPoint{116, 398}, QPoint{205, 419}};
constexpr QRect DefaultsButton{QPoint{212, 398}, QPoint{301, 419}};
constexpr QRect OkButton{QPoint{394, 398}, QPoint{466, 419}};
constexpr QRect CancelButton{QPoint{475, 398}, QPoint{547, 419}};
constexpr std::array<int, 3> ColumnWidths{180, 185, 140};
constexpr int RowHeight = 19;
constexpr int CaptureWidth = 320;
constexpr int CaptureHeight = 90;
constexpr int CaptureSeconds = 5;

constexpr std::array<std::string_view, Game::TypeCount> TypeNames{"Type A", "Type B", "Type C",
                                                                  "Type D", "Type E"};

// X11 key symbols, which Qt reports as a key's native virtual key under X11 and Wayland.
constexpr quint32 RightShiftSymbol = 0xffe2;
constexpr quint32 RightControlSymbol = 0xffe4;
constexpr quint32 RightAltSymbol = 0xffea;
constexpr quint32 RightSuperSymbol = 0xffec;

// --------------------------------------------------------------------------------- key names

// The input config's names of the keys and buttons whose text is not the name capitalised.
constexpr std::pair<std::string_view, std::string_view> KeyTexts[]{
    {"space", "Space"},
    {"escape", "Esc"},
    {"enter", "Enter"},
    {"backspace", "Backspace"},
    {"tab", "Tab"},
    {"capslock", "Caps Lock"},
    {"lshift", "Left Shift"},
    {"rshift", "Right Shift"},
    {"lctrl", "Left Ctrl"},
    {"rctrl", "Right Ctrl"},
    {"lalt", "Left Alt"},
    {"ralt", "Right Alt"},
    {"lwin", "Left Windows"},
    {"rwin", "Right Windows"},
    {"up", "Up Arrow"},
    {"down", "Down Arrow"},
    {"left", "Left Arrow"},
    {"right", "Right Arrow"},
    {"pgup", "Page Up"},
    {"pgdown", "Page Down"},
    {"printscreen", "Print Screen"},
    {"scrolllock", "Scroll Lock"},
    {"pausebreak", "Pause"},
    {"grave", "`"},
    {"minus", "-"},
    {"equals", "="},
    {"lbracket", "["},
    {"rbracket", "]"},
    {"backslash", "\\"},
    {"semicolon", ";"},
    {"apostrophe", "'"},
    {"comma", ","},
    {"period", "."},
    {"slash", "/"},
    {"kpperiod", "Num ."},
    {"kpslash", "Num /"},
    {"kpasterisk", "Num *"},
    {"kpminus", "Num -"},
    {"kpplus", "Num +"},
    {"kpenter", "Num Enter"},
    {"leftbutton", "Left Mouse Button"},
    {"rightbutton", "Right Mouse Button"},
    {"middlebutton", "Middle Mouse Button"},
    {"sidebuttonback", "Mouse Back"},
    {"sidebuttonforward", "Mouse Forward"},
    {"mousewheelup", "Mouse Wheel Up"},
    {"mousewheeldown", "Mouse Wheel Down"},
    {"mousewheelleft", "Mouse Wheel Left"},
    {"mousewheelright", "Mouse Wheel Right"},
};

QString KeyText(std::string_view name) {
    if (name.empty()) {
        return Tr("None");
    }
    for (const auto& [key, text] : KeyTexts) {
        if (key == name) {
            return Tr(text);
        }
    }
    if (name.size() == 3 && name.starts_with("kp") && std::isdigit(name[2])) {
        return Tr("Num {0}", {std::string(name.substr(2))});
    }
    std::string text{name};
    if (name.size() == 1 || (name[0] == 'f' && name.size() <= 3)) {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::toupper(c); });
    } else {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    }
    return QString::fromStdString(text);
}

// The config name of a pressed key, or empty for one the config cannot name.
std::string KeyName(const QKeyEvent& event) {
    const int key = event.key();
    const bool keypad = event.modifiers().testFlag(Qt::KeypadModifier);
    const quint32 symbol = event.nativeVirtualKey();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return std::string(1, static_cast<char>('a' + (key - Qt::Key_A)));
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return (keypad ? "kp" : "") + std::string(1, static_cast<char>('0' + (key - Qt::Key_0)));
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
        return "f" + std::to_string(key - Qt::Key_F1 + 1);
    }
    switch (key) {
    case Qt::Key_Shift:
        return symbol == RightShiftSymbol ? "rshift" : "lshift";
    case Qt::Key_Control:
        return symbol == RightControlSymbol ? "rctrl" : "lctrl";
    case Qt::Key_Alt:
        return symbol == RightAltSymbol ? "ralt" : "lalt";
    case Qt::Key_AltGr:
        return "ralt";
    case Qt::Key_Meta:
        return symbol == RightSuperSymbol ? "rwin" : "lwin";
    case Qt::Key_Super_L:
        return "lwin";
    case Qt::Key_Super_R:
        return "rwin";
    case Qt::Key_Return:
        return "enter";
    case Qt::Key_Enter:
        return "kpenter";
    case Qt::Key_Space:
        return "space";
    case Qt::Key_Escape:
        return "escape";
    case Qt::Key_Tab:
        return "tab";
    case Qt::Key_Backspace:
        return "backspace";
    case Qt::Key_CapsLock:
        return "capslock";
    case Qt::Key_Print:
        return "printscreen";
    case Qt::Key_ScrollLock:
        return "scrolllock";
    case Qt::Key_Pause:
        return "pausebreak";
    case Qt::Key_Insert:
        return "insert";
    case Qt::Key_Delete:
        return "delete";
    case Qt::Key_Home:
        return "home";
    case Qt::Key_End:
        return "end";
    case Qt::Key_PageUp:
        return "pgup";
    case Qt::Key_PageDown:
        return "pgdown";
    case Qt::Key_Up:
        return "up";
    case Qt::Key_Down:
        return "down";
    case Qt::Key_Left:
        return "left";
    case Qt::Key_Right:
        return "right";
    case Qt::Key_QuoteLeft:
        return "grave";
    case Qt::Key_Minus:
        return keypad ? "kpminus" : "minus";
    case Qt::Key_Equal:
        return "equals";
    case Qt::Key_BracketLeft:
        return "lbracket";
    case Qt::Key_BracketRight:
        return "rbracket";
    case Qt::Key_Backslash:
        return "backslash";
    case Qt::Key_Semicolon:
        return "semicolon";
    case Qt::Key_Apostrophe:
        return "apostrophe";
    case Qt::Key_Comma:
        return keypad ? "kpperiod" : "comma";
    case Qt::Key_Period:
        return keypad ? "kpperiod" : "period";
    case Qt::Key_Slash:
        return keypad ? "kpslash" : "slash";
    case Qt::Key_Asterisk:
        return keypad ? "kpasterisk" : std::string{};
    case Qt::Key_Plus:
        return keypad ? "kpplus" : std::string{};
    default:
        return {};
    }
}

// ------------------------------------------------------------------------------- key capture

// A popup that takes the next key or mouse button, or nothing when the countdown ends.
class Capture : public QDialog {
public:
    Capture(QWidget* owner, QString function_name)
        : QDialog{owner, Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint},
          function{std::move(function_name)} {
        setModal(true);
        setFixedSize(CaptureWidth, CaptureHeight);
        setFont(DialogFont());
        label = new QLabel{this};
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setGeometry(0, CaptureHeight / 10, CaptureWidth,
                           CaptureHeight - 2 * (CaptureHeight / 10));
        label->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        label->setWordWrap(true);
        ShowText();
        QObject::connect(&timer, &QTimer::timeout, this, [this] {
            if (--seconds_left <= 0) {
                Finish({});
            } else {
                ShowText();
            }
        });
        timer.start(1000);
        if (owner != nullptr) {
            move(owner->mapToGlobal(owner->rect().center()) - rect().center());
        }
    }

    /// The key or button taken; empty when the countdown ended first.
    std::string key;

protected:
    // Keys reach the popup before Qt uses Tab or Escape itself.
    bool event(QEvent* event) override {
        switch (event->type()) {
        case QEvent::ShortcutOverride:
            event->accept();
            return true;
        case QEvent::KeyPress: {
            const auto* pressed = static_cast<const QKeyEvent*>(event);
            if (!pressed->isAutoRepeat()) {
                Finish(KeyName(*pressed));
            }
            return true;
        }
        case QEvent::KeyRelease:
            return true;
        default:
            return QDialog::event(event);
        }
    }

    // White with a black edge.
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.fillRect(rect(), Qt::white);
        painter.setPen(Qt::black);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void showEvent(QShowEvent* event) override {
        QDialog::showEvent(event);
        activateWindow();
        setFocus();
        grabKeyboard();
        grabMouse();
    }

    void hideEvent(QHideEvent* event) override {
        releaseMouse();
        releaseKeyboard();
        QDialog::hideEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        switch (event->button()) {
        case Qt::LeftButton:
            Finish("leftbutton");
            break;
        case Qt::RightButton:
            Finish("rightbutton");
            break;
        case Qt::MiddleButton:
            Finish("middlebutton");
            break;
        case Qt::BackButton:
            Finish("sidebuttonback");
            break;
        case Qt::ForwardButton:
            Finish("sidebuttonforward");
            break;
        default:
            break;
        }
    }

    // Qt counts a turn to the left as positive.
    void wheelEvent(QWheelEvent* event) override {
        const QPoint turn = event->angleDelta();
        if (turn.y() != 0) {
            Finish(turn.y() > 0 ? "mousewheelup" : "mousewheeldown");
        } else if (turn.x() != 0) {
            Finish(turn.x() > 0 ? "mousewheelleft" : "mousewheelright");
        }
    }

private:
    void ShowText() {
        label->setText(Tr("Press a key or mouse button for\n{0}\n\nNothing changes in {1} s.",
                          {function.toStdString(), std::to_string(seconds_left)}));
    }

    // A key the config cannot name ends the capture with no key; a name the input handler does
    // not know is passed over.
    void Finish(std::string taken) {
        if (done || !(taken.empty() || Input::string_to_keyboard_key_map.contains(taken))) {
            return;
        }
        key = std::move(taken);
        done = true;
        timer.stop();
        accept();
    }

    QString function;
    QLabel* label = nullptr;
    QTimer timer;
    int seconds_left = CaptureSeconds;
    bool done = false;
};

// ------------------------------------------------------------------------------------ dialog

struct Editor {
    KeyBindings keys;
    u32 type = 0;
    u32 game_type = 0;
    QDialog* dialog = nullptr;
    QTableWidget* list = nullptr;
    QLabel* status = nullptr;
};

// The pad column shows the type's pad inputs; the key column is the same in every type.
void ShowRows(const Editor& editor) {
    const auto functions = Game::Functions();
    for (size_t i = 0; i < functions.size(); ++i) {
        const int row = static_cast<int>(i);
        editor.list->item(row, 1)->setText(Tr(functions[i].pad[editor.type]));
        editor.list->item(row, 2)->setText(KeyText(editor.keys[i]));
    }
    editor.list->horizontalHeaderItem(1)->setText(
        Tr("Controller ({0})", {Tr(TypeNames[editor.type]).toStdString()}));
}

int SelectedRow(const Editor& editor) {
    const auto selected = editor.list->selectionModel()->selectedRows();
    return selected.isEmpty() ? -1 : selected.front().row();
}

// Takes a key for the selected function. A key that another function had moves to this one.
void ChangeKey(Editor& editor) {
    const int row = SelectedRow(editor);
    const auto functions = Game::Functions();
    if (row < 0 || row >= static_cast<int>(functions.size())) {
        editor.status->setText(Tr("Pick a game function first."));
        return;
    }
    const QString label = Tr(functions[row].label);
    Capture capture{editor.dialog, label};
    capture.exec();
    const std::string key = capture.key;
    if (key.empty()) {
        editor.status->setText(Tr("No key was pressed; {0} keeps its key.", {label.toStdString()}));
        return;
    }
    QString status = Tr("{0} now does {1}.", {KeyText(key).toStdString(), label.toStdString()});
    for (size_t i = 0; i < editor.keys.size(); ++i) {
        if (static_cast<int>(i) != row && editor.keys[i] == key) {
            editor.keys[i].clear();
            status += QLatin1Char(' ') +
                      Tr("{0} has no key now.", {Tr(functions[i].label).toStdString()});
        }
    }
    editor.keys[row] = key;
    ShowRows(editor);
    editor.status->setText(status);
}

QPushButton* MakeButton(QDialog* dialog, std::string_view text, const QRect& area) {
    auto* const button = new QPushButton{Tr(text), dialog};
    button->setGeometry(area);
    button->setAutoDefault(false);
    return button;
}

} // namespace

std::optional<KeyBindings> EditKeyBindings(QWidget* owner, const KeyBindings& keys,
                                           u32 control_type) {
    QDialog dialog{owner};
    dialog.setWindowTitle(Tr("Key Bindings"));
    dialog.setFixedSize(ClientWidth, ClientHeight);
    dialog.setFont(DialogFont());

    Editor editor;
    editor.dialog = &dialog;
    editor.keys = keys;
    editor.keys.resize(Game::Functions().size());
    editor.game_type = std::min(control_type, Game::TypeCount - 1);
    editor.type = editor.game_type;

    auto* const tab = new QTabBar{&dialog};
    tab->setExpanding(false);
    for (size_t i = 0; i < TypeNames.size(); ++i) {
        QString name = Tr(TypeNames[i]);
        if (i == editor.game_type) {
            name = Tr("{0} (in use)", {name.toStdString()});
        }
        tab->addTab(name);
    }
    tab->setCurrentIndex(static_cast<int>(editor.type));
    const int tab_height = tab->sizeHint().height();
    tab->setGeometry(TabArea.left(), TabArea.top(), TabArea.width(), tab_height);

    const auto functions = Game::Functions();
    editor.list = new QTableWidget{static_cast<int>(functions.size()), 3, &dialog};
    editor.list->setGeometry(TabArea.left(), TabArea.top() + tab_height, TabArea.width(),
                             TabArea.height() - tab_height);
    editor.list->setSelectionBehavior(QAbstractItemView::SelectRows);
    editor.list->setSelectionMode(QAbstractItemView::SingleSelection);
    editor.list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    editor.list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor.list->verticalHeader()->hide();
    editor.list->verticalHeader()->setDefaultSectionSize(RowHeight);
    editor.list->horizontalHeader()->setSectionsClickable(false);
    editor.list->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    editor.list->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    constexpr std::array<std::string_view, 3> Headers{"Game Function", "Controller", "Key"};
    for (size_t i = 0; i < Headers.size(); ++i) {
        const int column = static_cast<int>(i);
        editor.list->setHorizontalHeaderItem(column, new QTableWidgetItem{Tr(Headers[i])});
        editor.list->setColumnWidth(column, ColumnWidths[i]);
    }
    for (size_t i = 0; i < functions.size(); ++i) {
        const int row = static_cast<int>(i);
        editor.list->setItem(row, 0, new QTableWidgetItem{Tr(functions[i].label)});
        editor.list->setItem(row, 1, new QTableWidgetItem{});
        editor.list->setItem(row, 2, new QTableWidgetItem{});
    }
    ShowRows(editor);

    auto* const in_use =
        new QLabel{Tr("The game used {0} last (Options > Controls). Each key does its function "
                      "in every type.",
                      {Tr(TypeNames[editor.game_type]).toStdString()}),
                   &dialog};
    in_use->setGeometry(TypeLine);
    editor.status = new QLabel{
        Tr("Double-click a function, then press its key. Mouse buttons and the wheel count."),
        &dialog};
    editor.status->setGeometry(StatusLine);

    QPushButton* const change = MakeButton(&dialog, "Change Key...", ChangeButton);
    QPushButton* const clear = MakeButton(&dialog, "Clear Key", ClearButton);
    QPushButton* const defaults = MakeButton(&dialog, "Defaults", DefaultsButton);
    QPushButton* const ok = MakeButton(&dialog, "OK", OkButton);
    QPushButton* const cancel = MakeButton(&dialog, "Cancel", CancelButton);
    ok->setDefault(true);

    QObject::connect(tab, &QTabBar::currentChanged, &dialog, [&editor](int index) {
        editor.type = static_cast<u32>(index);
        ShowRows(editor);
    });
    QObject::connect(editor.list, &QTableWidget::cellDoubleClicked, &dialog,
                     [&editor] { ChangeKey(editor); });
    QObject::connect(change, &QPushButton::clicked, &dialog, [&editor] { ChangeKey(editor); });
    QObject::connect(clear, &QPushButton::clicked, &dialog, [&editor] {
        if (const int row = SelectedRow(editor); row >= 0) {
            editor.keys[row].clear();
            ShowRows(editor);
        }
    });
    QObject::connect(defaults, &QPushButton::clicked, &dialog, [&editor] {
        editor.keys = DefaultKeyBindings();
        ShowRows(editor);
        editor.status->setText(Tr("Every function has its default key again."));
    });
    QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);

    editor.list->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return editor.keys;
}

} // namespace Gr2Port::Launcher
