// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTabBar>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>

#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/game_check/game_check.h"
#include "core/game_check/game_patches.h"
#include "core/language/language.h"
#include "core/prompt_packs/prompt_packs.h"
#include "core/user_settings.h"
#include "input/input_handler.h"
#include "input/selected_pad.h"
#include "launcher/qt/art.h"
#include "launcher/qt/avatar.h"
#include "launcher/qt/displays.h"
#include "launcher/qt/input_config.h"
#include "launcher/qt/key_bindings.h"
#include "launcher/qt/setup_dialog.h"
#include "launcher/qt/text.h"
#include "launcher/qt/video_card.h"
#include "video_core/gr2port/settings.h"

namespace Gr2Port::Launcher {

namespace {

// A rectangle by its edges; the right and bottom edges lie outside it.
struct Area {
    int left;
    int top;
    int right;
    int bottom;
};

// Pixels at 96 DPI in the dialog's client area, as in the Windows launcher, which measured them
// from Rise of the Tomb Raider's Setup dialog.
constexpr int ClientWidth = 602;
constexpr int ClientHeight = 461;
constexpr int BannerHeight = 50;
constexpr Area TabArea{9, 59, 595, 434};
constexpr int CheckLeft = 198;
constexpr int CheckWidth = 220;
constexpr int CheckHeight = 17;
constexpr int FullscreenTop = 88;
constexpr int ExclusiveTop = 111;
constexpr int LabelLeft = 18;
constexpr int LabelWidth = 170;
constexpr int ComboLeft = 198;
constexpr int ComboWidth = 225;
constexpr int ComboHeight = 22;
// The rows a combo box's list shows, as the Windows launcher's 240 pixel lists do.
constexpr int ComboListRows = 14;
constexpr Area DefaultsButton{10, 437, 143, 459};
constexpr Area OkButton{433, 437, 506, 459};
constexpr Area CancelButton{514, 437, 587, 459};

enum Page : size_t {
    DisplayPage,
    GraphicsPage,
    ControllerPage,
    MousePage,
    DlcPage,
    OnlinePage,
    TrophiesPage,
    ExtrasPage,
    PageCount,
};

// A tab bar draws a single ampersand as an underline.
constexpr std::array<std::string_view, PageCount> PageNames{
    "Display", "Graphics", "Controller", "Mouse && Keyboard",
    "DLC",     "Online",   "Trophies",   "Cheats && Extras"};

constexpr Area ExtrasList{18, 88, 577, 424};
constexpr int OnlineIdTop = 121;
constexpr int PasswordTop = 150;
constexpr int AvatarTop = 192;
constexpr Area AvatarChooseButton{338, 252, 448, 274};

// The Controller tab: GR2Launcher's controller settings in two columns of group boxes.
constexpr Area ControllerGroup{18, 88, 290, 280};
constexpr Area ActivePadGroup{300, 88, 577, 154};
constexpr Area DefaultPadGroup{300, 160, 577, 280};
constexpr Area SetDefaultPadButton{312, 220, 565, 243};
constexpr Area RemoveDefaultPadButton{312, 249, 565, 272};
constexpr int GroupInset = 12;
constexpr int ToggleTop = 108;
constexpr int ToggleStep = 21;
constexpr int PadTop = 108;
constexpr int PadIdTop = 134;
constexpr int DefaultPadNameTop = 180;
constexpr int DefaultPadIdTop = 198;
constexpr int InfoHeight = 15;
constexpr int SliderTop = 16;
constexpr int SliderHeight = 26;
constexpr int SliderOverhang = 6;
constexpr int SliderValueWidth = 36;

struct Toggle {
    std::string_view text;
    bool (EmulatorSettingsImpl::*get)() const;
    void (EmulatorSettingsImpl::*set)(bool, bool);
    bool fallback;
};

// GR2Launcher's controller check boxes, the settings they show and those settings' defaults.
constexpr std::array Toggles{
    Toggle{"Enable Motion Controls", &EmulatorSettingsImpl::IsMotionControlsEnabled,
           &EmulatorSettingsImpl::SetMotionControlsEnabled, true},
    Toggle{"Swap Gyro Yaw/Roll (Handheld)", &EmulatorSettingsImpl::IsGyroSwapYawRoll,
           &EmulatorSettingsImpl::SetGyroSwapYawRoll, false},
    Toggle{"Invert Gyro Yaw", &EmulatorSettingsImpl::IsGyroInvertYaw,
           &EmulatorSettingsImpl::SetGyroInvertYaw, false},
    Toggle{"Invert Gyro Pitch", &EmulatorSettingsImpl::IsGyroInvertX,
           &EmulatorSettingsImpl::SetGyroInvertX, false},
    Toggle{"Invert Gyro Roll", &EmulatorSettingsImpl::IsGyroInvertRoll,
           &EmulatorSettingsImpl::SetGyroInvertRoll, false},
    Toggle{"Enable Vibration", &EmulatorSettingsImpl::IsVibrationEnabled,
           &EmulatorSettingsImpl::SetVibrationEnabled, true},
    Toggle{"Enable Background Controller Input", &EmulatorSettingsImpl::IsBackgroundControllerInput,
           &EmulatorSettingsImpl::SetBackgroundControllerInput, false},
    Toggle{"Use Keyboard as Keyboard", &EmulatorSettingsImpl::IsKeyboardUsedAsKeyboard,
           &EmulatorSettingsImpl::SetKeyboardUsedAsKeyboard, false},
};
// The toggles a handheld preset checks.
constexpr size_t GyroSwapToggle = 1;
constexpr size_t GyroYawToggle = 2;

struct Slider {
    std::string_view text;
    int fallback;
    int top;
};

struct Stick {
    std::string_view text;
    Area area;
    std::array<Slider, 2> sliders;
};

// The sticks of the input config's deadzone lines, in the order of StickNames, with GR2Launcher's
// labels and defaults.
constexpr std::array Sticks{
    Stick{"Left Stick Deadzone",
          {18, 288, 290, 396},
          {Slider{"Min Deadzone (def:2 max:127)", 2, 306},
           Slider{"Max Deadzone (def:127 max:127)", 127, 348}}},
    Stick{"Right Stick Deadzone",
          {300, 288, 577, 396},
          {Slider{"Min Deadzone (def:2 max:127)", 2, 306},
           Slider{"Max Deadzone (def:127 max:127)", 127, 348}}},
};

// The Mouse && Keyboard tab: each camera's mouse look speeds, in percent of the base speed.
constexpr int SpeedLow = 10;
constexpr int SpeedHigh = 300;
constexpr int PromptsTop = 226;
// The Controller tab shows the same setting below the sticks.
constexpr int PadPromptsTop = 402;
constexpr int MouseNoteTop = 280;
constexpr int TouchSwipeTop = 336;
constexpr int TouchSwipeNoteTop = 356;
constexpr Area KeyBindingsButton{18, 396, 178, 419};
constexpr Area PadBindingsButton{433, 402, 577, 424};

// The button prompts setting's values, in the order of the combo box.
constexpr std::array<std::pair<std::string_view, std::string_view>, 7> PromptChoices{{
    {"Automatic", "auto"},
    {"PlayStation", "playstation"},
    {"Xbox", "xbox"},
    {"Nintendo Switch", "switch"},
    {"Steam Deck", "steamdeck"},
    {"Steam Controller", "steamcontroller"},
    {"Keyboard and Mouse", "keyboard"},
}};

struct Speed {
    std::string_view text;
    int top;
    u32 Settings::Mouse::* setting;
};

struct Camera {
    std::string_view text;
    Area area;
    std::array<Speed, 2> speeds;
};

constexpr std::array Cameras{
    Camera{"On Foot Camera",
           {18, 88, 290, 212},
           {Speed{"Horizontal Sensitivity", 106, &Settings::Mouse::foot_horizontal},
            Speed{"Vertical Sensitivity", 154, &Settings::Mouse::foot_vertical}}},
    Camera{"Gravity Shift Camera",
           {300, 88, 577, 212},
           {Speed{"Horizontal Sensitivity", 106, &Settings::Mouse::shift_horizontal},
            Speed{"Vertical Sensitivity", 154, &Settings::Mouse::shift_vertical}}},
};

struct Pad {
    std::string name;
    std::string guid;
};

// The Display tab's rows, 28 pixels apart, which fits ten below the check boxes.
constexpr int MonitorTop = 140;
constexpr int ResolutionTop = 168;
constexpr int RefreshTop = 196;
constexpr int VSyncTop = 224;
constexpr int FrameRateTop = 252;
constexpr int AspectTop = 280;
constexpr int RenderSizeTop = 308;
constexpr int UpscalerTop = 336;
constexpr int UpscaleQualityTop = 364;
constexpr int LanguageTop = 392;

// In the order of Settings::VSync.
constexpr std::array<std::string_view, 3> VSyncNames{"Off", "Double Buffered", "Triple Buffered"};
// Indexed by Settings::Upscaler and Settings::UpscaleQuality.
constexpr std::array<std::string_view, 4> UpscalerChoices{"Off", "FSR 3.1", "FSR 4", "DLSS"};
constexpr std::array<std::string_view, 5> UpscaleQualityChoices{"Native AA", "Quality", "Balanced",
                                                                "Performance", "Ultra Performance"};

// The Graphics tab: the preset across the top, each pass's choice in two columns of rows 28
// pixels apart, then the note. Quality choices are in the order of Settings::Quality; switches
// put On first.
constexpr int PresetTop = 88;
constexpr int PassTop = 118;
constexpr int PassStep = 28;
constexpr int GraphicsNoteTop = 322;
// The Texture Streaming choices after Auto, in MB. One at least the GPU's memory keeps within the
// driver's budget, as Auto does.
constexpr std::array<u32, 13> TextureStreamingSizes{1024,  1536,  2048,  3072,  4096,  6144, 8192,
                                                    10240, 11264, 12288, 16384, 24576, 32768};
constexpr std::array<std::string_view, 4> QualityChoices{"PS4 Original", "Medium", "Low", "Off"};
constexpr std::array<std::string_view, 3> FogChoices{"PS4 Original", "Medium", "Low"};
constexpr std::array<std::string_view, 2> SwitchChoices{"On", "Off"};
// Ambient occlusion and motion blur offer three qualities each, in the order of their choices.
constexpr std::array<std::string_view, 3> OcclusionChoices{"PS4 Original", "Half Resolution",
                                                           "Off"};
constexpr std::array OcclusionQualities{Settings::Quality::Original, Settings::Quality::Medium,
                                        Settings::Quality::Off};
constexpr std::array<std::string_view, 3> MotionBlurChoices{"PS4 Original", "Low", "Off"};
constexpr std::array MotionBlurQualities{Settings::Quality::Original, Settings::Quality::Low,
                                         Settings::Quality::Off};
constexpr std::array<std::string_view, 2> ParticleChoices{"Full Resolution", "Half Resolution"};
// The most anisotropic filtering samples of each choice; 2 is the game's.
constexpr std::array<std::string_view, 5> AnisotropyChoices{"Off", "2x (PS4)", "4x", "8x", "16x"};
constexpr std::array<u32, 5> AnisotropyLevels{0, 2, 4, 8, 16};

struct GraphicsColumn {
    int label_left;
    int label_width;
    int combo_left;
    int combo_width;
};

// Each column's labels fit their longest translation, 130 and 170 pixels.
constexpr GraphicsColumn LeftColumn{LabelLeft, 130, 152, 122};
constexpr GraphicsColumn RightColumn{280, 170, 454, 123};

// English text and the values its {N} markers name, shown in the language in use.
struct Phrase {
    std::string english;
    std::vector<std::string> values;

    Phrase(const char* text) : english{text} {}
    Phrase(std::string_view text) : english{text} {}
    Phrase(std::string text) : english{std::move(text)} {}
    Phrase(std::string text, std::vector<std::string> named)
        : english{std::move(text)}, values{std::move(named)} {}

    QString Shown() const {
        return Tr(english, values);
    }
};

// A label that reports a click, as the profile picture does.
class ClickLabel : public QLabel {
public:
    using QLabel::QLabel;

    std::function<void()> clicked;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && clicked &&
            rect().contains(event->position().toPoint())) {
            clicked();
        }
    }
};

struct Dialog {
    const Art* art = nullptr;
    QDialog* window = nullptr;
    Settings::Values values;
    std::vector<Monitor> monitors;
    Modes modes;
    QTabBar* tab = nullptr;
    std::array<QWidget*, PageCount> pages{};
    std::vector<GamePatches::Extra> extras;
    // What each control and combo box item says in English, so a change of language can show it
    // again.
    std::unordered_map<QWidget*, Phrase> texts;
    std::unordered_map<QComboBox*, std::vector<Phrase>> items;
    // The user on controller port 1, who signs in to shadNet; null when there is none.
    User* player = nullptr;
    // A picture chosen and not saved yet; null for none.
    QImage avatar;
    // SDL's gamepads, started when the Controller tab is first shown, and those connected then.
    bool gamepads_started = false;
    std::vector<Pad> pads;
    // The user picked the gamepad for this run.
    bool pad_picked = false;
    // The default gamepad's GUID as Ok saves it; empty for none.
    std::string default_pad;
    // The input config that holds the stick deadzones and the touch pad swipe switch, and their
    // values in it.
    std::filesystem::path input_config;
    Deadzones deadzones{};
    bool touch_swipe = false;
    // The keys of the game's functions as the Key Bindings dialog left them, to save on Ok.
    std::optional<KeyBindings> key_bindings;
    // The pad buttons as the Button Bindings dialog left them, to save on Ok.
    std::optional<PadBindings> pad_bindings;
    // The MB of each Texture Streaming choice; 0 for Auto.
    std::vector<u32> texture_streaming;

    QCheckBox* fullscreen = nullptr;
    QCheckBox* exclusive = nullptr;
    QComboBox* monitor = nullptr;
    QLabel* resolution_label = nullptr;
    QComboBox* resolution = nullptr;
    QLabel* refresh_label = nullptr;
    QComboBox* refresh = nullptr;
    QComboBox* vsync = nullptr;
    QComboBox* frame_rate = nullptr;
    QComboBox* aspect = nullptr;
    QComboBox* render_size = nullptr;
    QComboBox* upscaler = nullptr;
    QComboBox* upscale_quality = nullptr;
    QComboBox* language = nullptr;
    QComboBox* preset = nullptr;
    QComboBox* shadows = nullptr;
    QComboBox* fog = nullptr;
    QComboBox* fog_shadows = nullptr;
    QComboBox* occlusion = nullptr;
    QComboBox* motion_blur = nullptr;
    QComboBox* antialias = nullptr;
    QComboBox* bloom = nullptr;
    QComboBox* outlines = nullptr;
    QComboBox* decals = nullptr;
    QComboBox* glass_refraction = nullptr;
    QComboBox* particles = nullptr;
    QComboBox* anisotropy = nullptr;
    QComboBox* texture_streaming_list = nullptr;
    std::array<QCheckBox*, Toggles.size()> toggles{};
    QComboBox* active_pad = nullptr;
    QLabel* active_pad_id = nullptr;
    QLabel* default_pad_name = nullptr;
    QLabel* default_pad_id = nullptr;
    QPushButton* set_default_pad = nullptr;
    std::array<std::array<QSlider*, 2>, Sticks.size()> deadzone_bars{};
    std::array<std::array<QSlider*, 2>, Cameras.size()> speed_bars{};
    QComboBox* prompts = nullptr;
    QComboBox* pad_prompts = nullptr;
    QCheckBox* touch_swipe_box = nullptr;
    QCheckBox* dlc = nullptr;
    QCheckBox* halloween = nullptr;
    QLineEdit* online_id = nullptr;
    QLineEdit* password = nullptr;
    ClickLabel* avatar_picture = nullptr;
    // The Online tab's controls for an account, off when there is no user.
    std::vector<QWidget*> account_controls;
    QListWidget* extras_list = nullptr;
};

void Apply(QWidget* control, const QString& text) {
    if (auto* const label = qobject_cast<QLabel*>(control)) {
        label->setText(text);
    } else if (auto* const button = qobject_cast<QAbstractButton*>(control)) {
        button->setText(text);
    } else if (auto* const group = qobject_cast<QGroupBox*>(control)) {
        group->setTitle(text);
    } else {
        control->setWindowTitle(text);
    }
}

void SetText(Dialog& dialog, QWidget* control, Phrase text) {
    Apply(control, text.Shown());
    dialog.texts.insert_or_assign(control, std::move(text));
}

void AddText(Dialog& dialog, QComboBox* combo, Phrase text) {
    combo->addItem(text.Shown());
    dialog.items[combo].push_back(std::move(text));
}

void ClearItems(Dialog& dialog, QComboBox* combo) {
    combo->clear();
    dialog.items.erase(combo);
}

void Tell(const Dialog& dialog, std::string_view text, std::string_view title) {
    QMessageBox::warning(dialog.window, Tr(title), Tr(text));
}

void ShowAvatar(const Dialog& dialog, const QImage& picture) {
    dialog.avatar_picture->setPixmap(
        Avatar::Preview(picture, Avatar::Side, dialog.window->devicePixelRatioF()));
}

// The Online tab shows player 1's shadNet account and profile picture.
void ShowOnline(Dialog& dialog) {
    UserManager& users = UserManagement;
    dialog.player = users.GetUserByPlayerIndex(1);
    if (dialog.player == nullptr && !users.GetUsers().user.empty()) {
        dialog.player = &users.GetUsers().user.front();
    }
    for (QWidget* const control : dialog.account_controls) {
        control->setEnabled(dialog.player != nullptr);
    }
    if (dialog.player == nullptr) {
        ShowAvatar(dialog, {});
        return;
    }
    dialog.online_id->setText(QString::fromStdString(dialog.player->shadnet_npid));
    dialog.password->setText(QString::fromStdString(dialog.player->shadnet_password));
    ShowAvatar(dialog, Avatar::Make(Avatar::Path(dialog.player->user_id)));
}

void ChooseAvatar(Dialog& dialog) {
    const QString file =
        QFileDialog::getOpenFileName(dialog.window, Tr("Select Profile Picture"), {},
                                     Tr("Images") + QStringLiteral(" (*.png *.jpg *.jpeg *.bmp)"));
    if (file.isEmpty()) {
        return;
    }
    QImage picture = Avatar::Make(std::filesystem::path{file.toStdString()});
    if (picture.isNull()) {
        Tell(dialog, "The image could not be loaded.", "Profile Picture");
        return;
    }
    dialog.avatar = std::move(picture);
    ShowAvatar(dialog, dialog.avatar);
}

// Writes player 1's account to users.json and a newly chosen picture to the user's folder.
void SaveOnline(const Dialog& dialog) {
    if (dialog.player == nullptr) {
        return;
    }
    dialog.player->shadnet_npid = dialog.online_id->text().toStdString();
    dialog.player->shadnet_password = dialog.password->text().toStdString();
    dialog.player->shadnet_enabled =
        !dialog.player->shadnet_npid.empty() && !dialog.player->shadnet_password.empty();
    UserSettings.Save();
    if (!dialog.avatar.isNull() &&
        !Avatar::Save(dialog.avatar, Avatar::Path(dialog.player->user_id))) {
        Tell(dialog, "The profile picture could not be saved.", "Profile Picture");
    }
}

Deadzones DefaultDeadzones() {
    Deadzones deadzones{};
    for (size_t i = 0; i < Sticks.size(); ++i) {
        for (size_t j = 0; j < Sticks[i].sliders.size(); ++j) {
            deadzones[i][j] = Sticks[i].sliders[j].fallback;
        }
    }
    return deadzones;
}

void ShowDeadzones(const Dialog& dialog, const Deadzones& deadzones) {
    for (size_t i = 0; i < Sticks.size(); ++i) {
        for (size_t j = 0; j < Sticks[i].sliders.size(); ++j) {
            dialog.deadzone_bars[i][j]->setValue(deadzones[i][j]);
        }
    }
}

Deadzones ShownDeadzones(const Dialog& dialog) {
    Deadzones deadzones{};
    for (size_t i = 0; i < Sticks.size(); ++i) {
        for (size_t j = 0; j < Sticks[i].sliders.size(); ++j) {
            deadzones[i][j] = dialog.deadzone_bars[i][j]->value();
        }
    }
    return deadzones;
}

int PromptIndex(std::string_view setting) {
    const auto choice = std::ranges::find(PromptChoices, setting,
                                          &std::pair<std::string_view, std::string_view>::second);
    return choice != PromptChoices.end() ? static_cast<int>(choice - PromptChoices.begin()) : -1;
}

void ShowMouse(const Dialog& dialog, const Settings::Mouse& mouse) {
    const int prompts = std::max(PromptIndex(mouse.prompts), 0);
    dialog.prompts->setCurrentIndex(prompts);
    dialog.pad_prompts->setCurrentIndex(prompts);
    for (size_t i = 0; i < Cameras.size(); ++i) {
        for (size_t j = 0; j < Cameras[i].speeds.size(); ++j) {
            dialog.speed_bars[i][j]->setValue(std::clamp(
                static_cast<int>(mouse.*Cameras[i].speeds[j].setting), SpeedLow, SpeedHigh));
        }
    }
}

// The Automatic choice names the prompts it starts the game with, from the pads connected now.
void ShowAutomaticPrompts(Dialog& dialog) {
    const std::string layout = PromptPacks::AutomaticLayout();
    const int choice = PromptIndex(layout == "off" ? "playstation" : std::string_view{layout});
    const std::string_view name = choice >= 0 ? PromptChoices[choice].first : "PlayStation";
    const Phrase text{"Automatic ({0})", {Language::Text(name)}};
    for (QComboBox* const combo : {dialog.prompts, dialog.pad_prompts}) {
        combo->setItemText(0, text.Shown());
        if (auto& items = dialog.items[combo]; !items.empty()) {
            items.front() = text;
        }
    }
}

// The prompts whose names the Button Bindings dialog gives the pad's buttons: the Controller
// tab's Button Prompts choice, PlayStation for the keyboard's.
std::string PadNamesLayout(const Dialog& dialog) {
    const int chosen = dialog.pad_prompts->currentIndex();
    std::string layout = chosen >= 0 && chosen < static_cast<int>(PromptChoices.size())
                             ? std::string(PromptChoices[chosen].second)
                             : std::string("auto");
    if (layout == "auto") {
        layout = PromptPacks::AutomaticLayout();
    }
    return layout == "keyboard" ? std::string("playstation") : layout;
}

Settings::Mouse ReadMouse(const Dialog& dialog) {
    Settings::Mouse mouse = dialog.values.mouse;
    const int prompts = dialog.prompts->currentIndex();
    if (prompts >= 0 && prompts < static_cast<int>(PromptChoices.size())) {
        mouse.prompts = std::string(PromptChoices[prompts].second);
    }
    for (size_t i = 0; i < Cameras.size(); ++i) {
        for (size_t j = 0; j < Cameras[i].speeds.size(); ++j) {
            mouse.*Cameras[i].speeds[j].setting =
                static_cast<u32>(dialog.speed_bars[i][j]->value());
        }
    }
    return mouse;
}

// The gamepads connected now, in SDL's order.
std::vector<Pad> ListPads() {
    std::vector<Pad> pads;
    SDL_UpdateJoysticks();
    int count = 0;
    SDL_JoystickID* const ids = SDL_GetGamepads(&count);
    if (ids == nullptr) {
        return pads;
    }
    for (int i = 0; i < count; ++i) {
        const char* const name = SDL_GetGamepadNameForID(ids[i]);
        pads.push_back({name != nullptr ? name : "", Input::SelectedPad::GuidOf(ids[i])});
    }
    SDL_free(ids);
    return pads;
}

// GR2Launcher shows the last 16 digits of a GUID.
Phrase PadId(std::string_view guid) {
    return {"ID: {0}", {std::string(guid.substr(guid.size() > 16 ? guid.size() - 16 : 0))}};
}

void ShowActivePadId(Dialog& dialog) {
    const int pad = dialog.active_pad->currentIndex();
    const bool listed = pad >= 0 && pad < static_cast<int>(dialog.pads.size());
    SetText(dialog, dialog.active_pad_id, listed ? PadId(dialog.pads[pad].guid) : Phrase{"n/a"});
}

void ShowDefaultPad(Dialog& dialog) {
    const auto pad = std::ranges::find(dialog.pads, dialog.default_pad, &Pad::guid);
    const bool connected = !dialog.default_pad.empty() && pad != dialog.pads.end();
    const Phrase missing =
        dialog.default_pad.empty() ? "No default selected" : "Default controller not connected";
    // A gamepad's name is shown as SDL gives it.
    SetText(dialog, dialog.default_pad_name, connected ? Phrase{"{0}", {pad->name}} : missing);
    SetText(dialog, dialog.default_pad_id, connected ? PadId(pad->guid) : Phrase{"n/a"});
}

// Lists the gamepads again. The one shown stays chosen; at first it is the one player 1 gets.
void ShowPads(Dialog& dialog) {
    if (!dialog.gamepads_started) {
        dialog.gamepads_started = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    const int shown = dialog.active_pad->currentIndex();
    const std::string kept = shown >= 0 && shown < static_cast<int>(dialog.pads.size())
                                 ? dialog.pads[shown].guid
                                 : Input::SelectedPad::Guid();
    dialog.pads = dialog.gamepads_started ? ListPads() : std::vector<Pad>{};
    ClearItems(dialog, dialog.active_pad);
    if (dialog.pads.empty()) {
        AddText(dialog, dialog.active_pad, "No gamepads detected");
    }
    for (size_t i = 0; i < dialog.pads.size(); ++i) {
        AddText(dialog, dialog.active_pad,
                Phrase{"{0}: {1}", {std::to_string(i + 1), dialog.pads[i].name}});
    }
    const auto pad = std::ranges::find(dialog.pads, kept, &Pad::guid);
    dialog.active_pad->setCurrentIndex(
        pad != dialog.pads.end() ? static_cast<int>(pad - dialog.pads.begin()) : 0);
    dialog.set_default_pad->setEnabled(!dialog.pads.empty());
    ShowActivePadId(dialog);
    ShowDefaultPad(dialog);
}

// The Controller tab shows config.json's controller settings and the deadzones of the input
// config the game reads, and the Mouse && Keyboard tab that config's touch pad swipe switch. The
// gamepads are listed when the tab is shown.
void ShowController(Dialog& dialog) {
    for (size_t i = 0; i < Toggles.size(); ++i) {
        dialog.toggles[i]->setChecked((EmulatorSettings.*Toggles[i].get)());
    }
    dialog.input_config = Input::GetInputConfigFile(
        EmulatorSettings.IsUseUnifiedInputConfig() ? "default" : GameCheck::TitleId());
    dialog.deadzones = ReadDeadzones(dialog.input_config);
    ShowDeadzones(dialog, dialog.deadzones);
    dialog.touch_swipe = ReadTouchSwipe(dialog.input_config);
    dialog.touch_swipe_box->setChecked(dialog.touch_swipe);
    dialog.default_pad = EmulatorSettings.GetDefaultControllerId();
}

// Saves the check boxes and the default gamepad to config.json and the deadzones and the touch pad
// swipe switch to the input config. The gamepad picked plays as player 1 in this run.
void SaveController(const Dialog& dialog) {
    for (size_t i = 0; i < Toggles.size(); ++i) {
        (EmulatorSettings.*Toggles[i].set)(dialog.toggles[i]->isChecked(), false);
    }
    EmulatorSettings.SetDefaultControllerId(dialog.default_pad);
    EmulatorSettings.Save();
    const int pad = dialog.active_pad->currentIndex();
    if (dialog.pad_picked && pad >= 0 && pad < static_cast<int>(dialog.pads.size())) {
        Input::SelectedPad::Set(dialog.pads[pad].guid);
    }
    std::vector<ConfigLine> lines;
    const Deadzones deadzones = ShownDeadzones(dialog);
    if (deadzones != dialog.deadzones) {
        lines = DeadzoneLines(deadzones);
    }
    const bool touch_swipe = dialog.touch_swipe_box->isChecked();
    if (touch_swipe != dialog.touch_swipe) {
        lines.push_back(TouchSwipeLine(touch_swipe));
    }
    if (!lines.empty() && !SaveConfigLines(dialog.input_config, lines)) {
        Tell(dialog, "The input config could not be saved.", "Controller");
    }
}

void FillResolutions(Dialog& dialog, u32 width, u32 height) {
    ClearItems(dialog, dialog.resolution);
    int chosen = -1;
    int current = -1;
    for (size_t i = 0; i < dialog.modes.sizes.size(); ++i) {
        const Size& size = dialog.modes.sizes[i];
        AddText(dialog, dialog.resolution,
                std::to_string(size.width) + " x " + std::to_string(size.height));
        if (size.width == width && size.height == height) {
            chosen = static_cast<int>(i);
        }
        if (size.width == dialog.modes.current_width &&
            size.height == dialog.modes.current_height) {
            current = static_cast<int>(i);
        }
    }
    dialog.resolution->setCurrentIndex(chosen >= 0 ? chosen : current);
}

// A rate of 0 picks the monitor's current rate, or the highest when the size is not current.
void FillRates(Dialog& dialog, u32 rate) {
    ClearItems(dialog, dialog.refresh);
    const int size = dialog.resolution->currentIndex();
    if (size < 0) {
        return;
    }
    const std::vector<u32>& rates = dialog.modes.sizes[size].rates;
    int chosen = -1;
    int current = -1;
    for (size_t i = 0; i < rates.size(); ++i) {
        AddText(dialog, dialog.refresh, std::to_string(rates[i]) + " Hz");
        if (rates[i] == rate) {
            chosen = static_cast<int>(i);
        }
        if (rates[i] == dialog.modes.current_rate) {
            current = static_cast<int>(i);
        }
    }
    dialog.refresh->setCurrentIndex(chosen >= 0    ? chosen
                                    : current >= 0 ? current
                                                   : static_cast<int>(rates.size()) - 1);
}

// The rate the Refresh Rate list shows, read from its text; 0 for none.
u32 ShownRate(const Dialog& dialog) {
    return dialog.refresh->currentText().section(QLatin1Char(' '), 0, 0).toUInt();
}

void UpdateEnabled(const Dialog& dialog) {
    const bool fullscreen = dialog.fullscreen->isChecked();
    const bool exclusive = fullscreen && dialog.exclusive->isChecked();
    dialog.exclusive->setEnabled(fullscreen);
    dialog.resolution_label->setEnabled(!fullscreen || exclusive);
    dialog.resolution->setEnabled(!fullscreen || exclusive);
    dialog.refresh_label->setEnabled(exclusive);
    dialog.refresh->setEnabled(exclusive);
}

void CheckRow(const Dialog& dialog, int row, bool on) {
    dialog.extras_list->item(row)->setCheckState(on ? Qt::Checked : Qt::Unchecked);
}

void ShowGame(const Dialog& dialog, const Settings::Game& game) {
    const auto rate =
        std::ranges::find(GamePatches::FrameRates, game.frame_rate, &GamePatches::FrameRate::fps);
    dialog.frame_rate->setCurrentIndex(
        rate != GamePatches::FrameRates.end()
            ? static_cast<int>(rate - GamePatches::FrameRates.begin())
            : 0);
    const auto aspect = std::ranges::find(GamePatches::Aspects, std::string_view{game.aspect},
                                          &GamePatches::Aspect::name);
    dialog.aspect->setCurrentIndex(aspect != GamePatches::Aspects.end()
                                       ? static_cast<int>(aspect - GamePatches::Aspects.begin())
                                       : 0);
    const auto size =
        std::ranges::find(GamePatches::RenderSizes, std::string_view{game.render_size},
                          &GamePatches::RenderSize::name);
    const auto own_size = std::ranges::find(GamePatches::RenderSizes, std::string_view{"1080p"},
                                            &GamePatches::RenderSize::name);
    dialog.render_size->setCurrentIndex(
        static_cast<int>((size != GamePatches::RenderSizes.end() ? size : own_size) -
                         GamePatches::RenderSizes.begin()));
    dialog.dlc->setChecked(game.dlc_entitlements);
    dialog.halloween->setChecked(game.halloween_items);
    for (size_t i = 0; i < dialog.extras.size(); ++i) {
        const auto chosen = game.extras.find(dialog.extras[i].name);
        CheckRow(dialog, static_cast<int>(i),
                 chosen != game.extras.end() ? chosen->second : dialog.extras[i].listed_on);
    }
}

// Extras keep only the choices that differ from the patch file's flags.
Settings::Game ReadGame(const Dialog& dialog) {
    Settings::Game game = dialog.values.game;
    const int rate = dialog.frame_rate->currentIndex();
    if (rate >= 0 && rate < static_cast<int>(GamePatches::FrameRates.size())) {
        game.frame_rate = GamePatches::FrameRates[rate].fps;
    }
    const int aspect = dialog.aspect->currentIndex();
    if (aspect >= 0 && aspect < static_cast<int>(GamePatches::Aspects.size())) {
        game.aspect = std::string(GamePatches::Aspects[aspect].name);
    }
    const int size = dialog.render_size->currentIndex();
    if (size >= 0 && size < static_cast<int>(GamePatches::RenderSizes.size())) {
        game.render_size = std::string(GamePatches::RenderSizes[size].name);
    }
    game.dlc_entitlements = dialog.dlc->isChecked();
    game.halloween_items = dialog.halloween->isChecked();
    game.extras.clear();
    for (size_t i = 0; i < dialog.extras.size(); ++i) {
        const bool on = dialog.extras_list->item(static_cast<int>(i))->checkState() == Qt::Checked;
        if (on != dialog.extras[i].listed_on) {
            game.extras.emplace(dialog.extras[i].name, on);
        }
    }
    return game;
}

// The index of value in values, or fallback when it is none of them.
template <typename Value, size_t Count>
int IndexOf(const std::array<Value, Count>& values, Value value, int fallback) {
    const auto found = std::ranges::find(values, value);
    return found != values.end() ? static_cast<int>(found - values.begin()) : fallback;
}

// Sets value to the entry of values that the combo box shows, when it shows one.
template <typename Value, size_t Count>
void ReadChoice(const QComboBox* combo, const std::array<Value, Count>& values, Value& value) {
    const int index = combo->currentIndex();
    if (index >= 0 && index < static_cast<int>(Count)) {
        value = values[index];
    }
}

// A value the choices lack shows as the choice the renderer draws it as.
void ShowGraphics(const Dialog& dialog, const Settings::Graphics& graphics) {
    dialog.shadows->setCurrentIndex(static_cast<int>(graphics.shadows));
    dialog.fog->setCurrentIndex(
        std::min(static_cast<int>(graphics.fog), static_cast<int>(FogChoices.size()) - 1));
    dialog.fog_shadows->setCurrentIndex(graphics.fog_shadows ? 0 : 1);
    dialog.occlusion->setCurrentIndex(IndexOf(OcclusionQualities, graphics.ambient_occlusion, 1));
    dialog.motion_blur->setCurrentIndex(IndexOf(MotionBlurQualities, graphics.motion_blur, 0));
    dialog.antialias->setCurrentIndex(graphics.antialiasing ? 0 : 1);
    dialog.bloom->setCurrentIndex(graphics.bloom ? 0 : 1);
    dialog.outlines->setCurrentIndex(graphics.outlines ? 0 : 1);
    dialog.decals->setCurrentIndex(graphics.decals ? 0 : 1);
    dialog.glass_refraction->setCurrentIndex(graphics.glass_refraction ? 0 : 1);
    dialog.particles->setCurrentIndex(graphics.half_resolution_particles ? 1 : 0);
    dialog.anisotropy->setCurrentIndex(
        IndexOf(AnisotropyLevels, graphics.anisotropic_filtering, 1));
}

// The upscaler and its quality, which presets leave.
void ShowUpscaler(const Dialog& dialog, const Settings::Graphics& graphics) {
    dialog.upscaler->setCurrentIndex(static_cast<int>(graphics.upscaler));
    dialog.upscale_quality->setCurrentIndex(static_cast<int>(graphics.upscale_quality));
}

// The passes as shown, with the preset they and the Display tab's sizes match.
Settings::Graphics ReadGraphics(const Dialog& dialog) {
    Settings::Graphics graphics = dialog.values.graphics;
    const int shadows = dialog.shadows->currentIndex();
    if (shadows >= 0 && shadows < static_cast<int>(QualityChoices.size())) {
        graphics.shadows = static_cast<Settings::Quality>(shadows);
    }
    const int fog = dialog.fog->currentIndex();
    if (fog >= 0 && fog < static_cast<int>(FogChoices.size())) {
        graphics.fog = static_cast<Settings::Quality>(fog);
    }
    graphics.fog_shadows = dialog.fog_shadows->currentIndex() != 1;
    ReadChoice(dialog.occlusion, OcclusionQualities, graphics.ambient_occlusion);
    ReadChoice(dialog.motion_blur, MotionBlurQualities, graphics.motion_blur);
    graphics.antialiasing = dialog.antialias->currentIndex() != 1;
    graphics.bloom = dialog.bloom->currentIndex() != 1;
    graphics.outlines = dialog.outlines->currentIndex() != 1;
    graphics.decals = dialog.decals->currentIndex() != 1;
    graphics.glass_refraction = dialog.glass_refraction->currentIndex() != 1;
    graphics.half_resolution_particles = dialog.particles->currentIndex() == 1;
    ReadChoice(dialog.anisotropy, AnisotropyLevels, graphics.anisotropic_filtering);
    const int upscaler = dialog.upscaler->currentIndex();
    if (upscaler >= 0 && upscaler < static_cast<int>(UpscalerChoices.size())) {
        graphics.upscaler = static_cast<Settings::Upscaler>(upscaler);
    }
    const int upscale_quality = dialog.upscale_quality->currentIndex();
    if (upscale_quality >= 0 && upscale_quality < static_cast<int>(UpscaleQualityChoices.size())) {
        graphics.upscale_quality = static_cast<Settings::UpscaleQuality>(upscale_quality);
    }
    const int texture_streaming = dialog.texture_streaming_list->currentIndex();
    if (texture_streaming >= 0 &&
        texture_streaming < static_cast<int>(dialog.texture_streaming.size())) {
        graphics.texture_streaming_mb = dialog.texture_streaming[texture_streaming];
    }
    graphics.preset = std::string(Settings::MatchPreset(ReadGame(dialog), graphics));
    return graphics;
}

// The preset the choices match, or Custom, the entry after the presets.
void ShowPreset(const Dialog& dialog) {
    const std::string preset = ReadGraphics(dialog).preset;
    const auto found =
        std::ranges::find(Settings::Presets, std::string_view{preset}, &Settings::Preset::key);
    dialog.preset->setCurrentIndex(static_cast<int>(found - Settings::Presets.begin()));
}

void ShowTextureStreaming(const Dialog& dialog, u32 megabytes);

// Shows a preset's passes and, for the one that sets them, its aspect and render size and its
// handheld settings.
void ApplyPreset(const Dialog& dialog, int index) {
    if (index < 0 || index >= static_cast<int>(Settings::Presets.size())) {
        return;
    }
    const Settings::Preset& preset = Settings::Presets[index];
    ShowGraphics(dialog, Settings::FromPreset(preset));
    const auto aspect =
        std::ranges::find(GamePatches::Aspects, preset.aspect, &GamePatches::Aspect::name);
    if (aspect != GamePatches::Aspects.end()) {
        dialog.aspect->setCurrentIndex(static_cast<int>(aspect - GamePatches::Aspects.begin()));
    }
    const auto size = std::ranges::find(GamePatches::RenderSizes, preset.render_size,
                                        &GamePatches::RenderSize::name);
    if (size != GamePatches::RenderSizes.end()) {
        dialog.render_size->setCurrentIndex(
            static_cast<int>(size - GamePatches::RenderSizes.begin()));
    }
    if (preset.handheld) {
        dialog.vsync->setCurrentIndex(static_cast<int>(Settings::VSync::Off));
        dialog.toggles[GyroSwapToggle]->setChecked(true);
        dialog.toggles[GyroYawToggle]->setChecked(true);
    }
    if (preset.texture_streaming_mb != 0) {
        ShowTextureStreaming(dialog, preset.texture_streaming_mb);
    }
}

// A size in MB as GB with at most one decimal.
std::string Gigabytes(u64 megabytes) {
    const u64 tenths = (megabytes * 10 + 512) / 1024;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        text += "." + std::to_string(tenths % 10);
    }
    return text + " GB";
}

// Auto, naming the GPU and its memory, or for an integrated GPU its share of RAM, then the
// sizes, then saved when it is none of them.
void FillTextureStreaming(Dialog& dialog, u32 saved) {
    const VideoCard card = FindVideoCard();
    Phrase automatic = "Auto";
    if (card.integrated) {
        const u64 share = std::min(card.bytes + card.shared, Settings::RamShare(InstalledRam()));
        automatic = {"Auto ({0} of RAM, {1})", {Gigabytes(share >> 20), card.name}};
    } else if (card.bytes != 0) {
        automatic = {"Auto ({0}, {1})", {Gigabytes(card.bytes >> 20), card.name}};
    }
    dialog.texture_streaming = {0};
    AddText(dialog, dialog.texture_streaming_list, automatic);
    for (const u32 size : TextureStreamingSizes) {
        dialog.texture_streaming.push_back(size);
        AddText(dialog, dialog.texture_streaming_list, Gigabytes(size));
    }
    if (std::ranges::find(dialog.texture_streaming, saved) == dialog.texture_streaming.end()) {
        dialog.texture_streaming.push_back(saved);
        AddText(dialog, dialog.texture_streaming_list, Gigabytes(saved));
    }
}

void ShowTextureStreaming(const Dialog& dialog, u32 megabytes) {
    const auto found = std::ranges::find(dialog.texture_streaming, megabytes);
    dialog.texture_streaming_list->setCurrentIndex(
        found != dialog.texture_streaming.end()
            ? static_cast<int>(found - dialog.texture_streaming.begin())
            : 0);
}

void Show(Dialog& dialog, const Settings::Display& display) {
    dialog.fullscreen->setChecked(display.fullscreen);
    dialog.exclusive->setChecked(display.exclusive);
    int monitor = 0;
    for (size_t i = 0; i < dialog.monitors.size(); ++i) {
        bool chosen = dialog.monitors[i].primary;
        if (display.monitor) {
            const SDL_Point point{display.monitor->x, display.monitor->y};
            chosen = SDL_PointInRect(&point, &dialog.monitors[i].area);
        }
        if (chosen) {
            monitor = static_cast<int>(i);
        }
    }
    dialog.monitor->setCurrentIndex(monitor);
    dialog.modes = dialog.monitors.empty() ? Modes{} : ListModes(dialog.monitors[monitor].id);
    FillResolutions(dialog, display.width, display.height);
    FillRates(dialog, display.refresh_rate);
    dialog.vsync->setCurrentIndex(static_cast<int>(display.vsync));
    UpdateEnabled(dialog);
}

// The primary monitor is stored as none, so it follows the primary monitor when that changes.
Settings::Display Read(const Dialog& dialog) {
    Settings::Display display = dialog.values.display;
    display.fullscreen = dialog.fullscreen->isChecked();
    display.exclusive = dialog.exclusive->isChecked();
    const int monitor = dialog.monitor->currentIndex();
    if (monitor >= 0 && monitor < static_cast<int>(dialog.monitors.size())) {
        const Monitor& chosen = dialog.monitors[monitor];
        display.monitor.reset();
        if (!chosen.primary) {
            display.monitor = Settings::Point{chosen.area.x + chosen.area.w / 2,
                                              chosen.area.y + chosen.area.h / 2};
        }
    }
    const int size = dialog.resolution->currentIndex();
    if (size >= 0 && size < static_cast<int>(dialog.modes.sizes.size())) {
        display.width = dialog.modes.sizes[size].width;
        display.height = dialog.modes.sizes[size].height;
    }
    display.refresh_rate = ShownRate(dialog);
    const int vsync = dialog.vsync->currentIndex();
    if (vsync >= 0 && vsync < static_cast<int>(VSyncNames.size())) {
        display.vsync = static_cast<Settings::VSync>(vsync);
    }
    return display;
}

// Kat's face from the splash, and the logo over the dark scarf beside her.
QPixmap ComposeBanner(const Art& art, int width, int height, qreal ratio) {
    return Art::Render(width, height, ratio, [&](QPainter& painter) {
        constexpr qreal Left = 860.0;
        constexpr qreal Middle = 243.0;
        const qreal full_width = width;
        const qreal full_height = height;
        const qreal part_width = art.SplashWidth() - Left;
        const qreal part_height = part_width * full_height / full_width;
        art.DrawSplash(painter, {0.0, 0.0, full_width, full_height},
                       {Left, Middle - part_height / 2.0, part_width, part_height});
        Art::Fade(painter, {full_width * 0.62, 0.0, full_width * 0.38, full_height}, 150);
        const qreal logo_height = 42.0;
        const qreal logo_width = logo_height * art.LogoAspect();
        art.DrawLogo(painter, {full_width - 10.0 - logo_width,
                               (full_height - logo_height) / 2.0 + 1.0, logo_width, logo_height});
    });
}

// The place in page of a rectangle given in the dialog's client area.
QRect Place(const QWidget* page, int left, int top, int width, int height) {
    return {left - page->x(), top - page->y(), width, height};
}

QRect Place(const QWidget* page, const Area& area) {
    return Place(page, area.left, area.top, area.right - area.left, area.bottom - area.top);
}

QLabel* MakeLabel(Dialog& dialog, QWidget* page, const Phrase& text, Qt::Alignment alignment,
                  const QRect& place) {
    auto* const label = new QLabel{page};
    label->setGeometry(place);
    label->setAlignment(alignment);
    if (!text.english.empty()) {
        SetText(dialog, label, text);
    }
    return label;
}

// A note of several lines, which wraps at the page's edge.
QLabel* MakeNote(Dialog& dialog, QWidget* page, const Phrase& text, const QRect& place) {
    QLabel* const note = MakeLabel(dialog, page, text, Qt::AlignLeft | Qt::AlignTop, place);
    note->setWordWrap(true);
    return note;
}

// A link opens in the browser.
QLabel* MakeLink(Dialog& dialog, QWidget* page, const Phrase& text, const QRect& place) {
    QLabel* const link = MakeLabel(dialog, page, text, Qt::AlignLeft | Qt::AlignTop, place);
    link->setTextFormat(Qt::RichText);
    link->setOpenExternalLinks(true);
    link->setFocusPolicy(Qt::TabFocus);
    return link;
}

QCheckBox* MakeCheck(Dialog& dialog, QWidget* page, const Phrase& text, const QRect& place) {
    auto* const check = new QCheckBox{page};
    check->setGeometry(place);
    SetText(dialog, check, text);
    return check;
}

QComboBox* MakeCombo(QWidget* page, const QRect& place) {
    auto* const combo = new QComboBox{page};
    combo->setGeometry(place);
    combo->setMaxVisibleItems(ComboListRows);
    return combo;
}

QGroupBox* MakeGroup(Dialog& dialog, QWidget* page, const Phrase& text, const Area& area) {
    auto* const group = new QGroupBox{page};
    group->setGeometry(Place(page, area));
    SetText(dialog, group, text);
    return group;
}

QPushButton* MakeButton(Dialog& dialog, QWidget* page, const Phrase& text, const Area& area) {
    auto* const button = new QPushButton{page};
    button->setGeometry(Place(page, area));
    button->setAutoDefault(false);
    SetText(dialog, button, text);
    return button;
}

// A label and its combo box in a row, at the Display tab's places.
QComboBox* MakeRow(Dialog& dialog, QWidget* page, const Phrase& text, int top,
                   QLabel** label = nullptr) {
    QLabel* const made = MakeLabel(dialog, page, text, Qt::AlignLeft | Qt::AlignVCenter,
                                   Place(page, LabelLeft, top, LabelWidth, ComboHeight));
    if (label != nullptr) {
        *label = made;
    }
    return MakeCombo(page, Place(page, ComboLeft, top, ComboWidth, ComboHeight));
}

// A slider between left and right with its label above it and its value at the label's right,
// which follows the slider with suffix after the number.
QSlider* MakeSlider(Dialog& dialog, QWidget* page, const Phrase& text, int left, int right, int top,
                    int low, int high, const QString& suffix) {
    MakeLabel(dialog, page, text, Qt::AlignLeft | Qt::AlignTop,
              Place(page, left, top, right - SliderValueWidth - left, InfoHeight));
    QLabel* const value =
        MakeLabel(dialog, page, "", Qt::AlignRight | Qt::AlignTop,
                  Place(page, right - SliderValueWidth, top, SliderValueWidth, InfoHeight));
    auto* const bar = new QSlider{Qt::Horizontal, page};
    bar->setGeometry(Place(page, left - SliderOverhang, top + SliderTop,
                           right - left + 2 * SliderOverhang, SliderHeight));
    bar->setRange(low, high);
    QObject::connect(bar, &QSlider::valueChanged, value, [value, suffix](int position) {
        value->setText(QString::number(position) + suffix);
    });
    value->setText(QString::number(bar->value()) + suffix);
    return bar;
}

// Shows every text again in the language in use.
void Retranslate(Dialog& dialog) {
    for (const auto& [control, text] : dialog.texts) {
        Apply(control, text.Shown());
    }
    for (const auto& [combo, items] : dialog.items) {
        const int selected = combo->currentIndex();
        combo->clear();
        for (const Phrase& item : items) {
            combo->addItem(item.Shown());
        }
        combo->setCurrentIndex(selected);
    }
    for (size_t i = 0; i < PageCount; ++i) {
        dialog.tab->setTabText(static_cast<int>(i), Tr(PageNames[i]));
    }
    ShowAutomaticPrompts(dialog);
}

// Picks a console language in the Language list and shows the texts in it.
void ShowLanguage(Dialog& dialog, int language) {
    const auto listed = Language::Listed();
    const auto found = std::ranges::find(listed, language);
    if (found == listed.end()) {
        return;
    }
    dialog.language->setCurrentIndex(static_cast<int>(found - listed.begin()));
    if (language != Language::Current()) {
        Language::Use(language);
        Retranslate(dialog);
    }
}

void BuildDisplay(Dialog& dialog, QWidget* page) {
    dialog.fullscreen = MakeCheck(dialog, page, "Fullscreen",
                                  Place(page, CheckLeft, FullscreenTop, CheckWidth, CheckHeight));
    dialog.exclusive = MakeCheck(dialog, page, "Exclusive Fullscreen",
                                 Place(page, CheckLeft, ExclusiveTop, CheckWidth, CheckHeight));
    dialog.monitor = MakeRow(dialog, page, "Monitor", MonitorTop);
    dialog.resolution =
        MakeRow(dialog, page, "Monitor Resolution", ResolutionTop, &dialog.resolution_label);
    dialog.refresh = MakeRow(dialog, page, "Refresh Rate", RefreshTop, &dialog.refresh_label);
    dialog.vsync = MakeRow(dialog, page, "VSync", VSyncTop);
    dialog.frame_rate = MakeRow(dialog, page, "FPS Cap", FrameRateTop);
    dialog.aspect = MakeRow(dialog, page, "Aspect Ratio", AspectTop);
    dialog.render_size = MakeRow(dialog, page, "Render Resolution", RenderSizeTop);
    dialog.upscaler = MakeRow(dialog, page, "Upscaler", UpscalerTop);
    dialog.upscale_quality = MakeRow(dialog, page, "Upscaler Quality", UpscaleQualityTop);
    dialog.language = MakeRow(dialog, page, "Language", LanguageTop);
}

// Tab order follows creation: the left column's rows, then the right's.
void BuildGraphics(Dialog& dialog, QWidget* page) {
    MakeLabel(dialog, page, "Preset", Qt::AlignLeft | Qt::AlignVCenter,
              Place(page, LeftColumn.label_left, PresetTop, LeftColumn.label_width, ComboHeight));
    dialog.preset =
        MakeCombo(page, Place(page, LeftColumn.combo_left, PresetTop, ComboWidth, ComboHeight));
    const auto row = [&](const Phrase& text, const GraphicsColumn& column, int index,
                         std::span<const std::string_view> choices) {
        const int top = PassTop + index * PassStep;
        MakeLabel(dialog, page, text, Qt::AlignLeft | Qt::AlignVCenter,
                  Place(page, column.label_left, top, column.label_width, ComboHeight));
        QComboBox* const combo =
            MakeCombo(page, Place(page, column.combo_left, top, column.combo_width, ComboHeight));
        for (const std::string_view choice : choices) {
            AddText(dialog, combo, choice);
        }
        return combo;
    };
    dialog.shadows = row("Shadows", LeftColumn, 0, QualityChoices);
    dialog.fog = row("Volumetric Fog", LeftColumn, 1, FogChoices);
    dialog.fog_shadows = row("Fog Shadows", LeftColumn, 2, SwitchChoices);
    dialog.occlusion = row("Ambient Occlusion", LeftColumn, 3, OcclusionChoices);
    dialog.motion_blur = row("Motion Blur", LeftColumn, 4, MotionBlurChoices);
    dialog.antialias = row("Anti-Aliasing", LeftColumn, 5, SwitchChoices);
    dialog.bloom = row("Bloom", LeftColumn, 6, SwitchChoices);
    dialog.outlines = row("Outlines", RightColumn, 0, SwitchChoices);
    dialog.decals = row("Decals", RightColumn, 1, SwitchChoices);
    dialog.glass_refraction = row("Glass Refraction", RightColumn, 2, SwitchChoices);
    dialog.particles = row("Particles", RightColumn, 3, ParticleChoices);
    dialog.anisotropy = row("Anisotropic Filtering", RightColumn, 4, AnisotropyChoices);
    // Its choices come from the GPU.
    dialog.texture_streaming_list = row("Texture Streaming", RightColumn, 5, {});
    FillTextureStreaming(dialog, dialog.values.graphics.texture_streaming_mb);
    MakeNote(dialog, page,
             "Steam Deck also sets Aspect Ratio to 16:10, Render Resolution to 720p (1280 x 800) "
             "and VSync to Off on the Display tab, and Swap Gyro Yaw/Roll and Invert Gyro Yaw on "
             "the Controller tab. Changing a choice makes the preset Custom. With Texture "
             "Streaming below the GPU's memory, textures keep fewer mip levels: first those not "
             "drawn for a while, then the largest.",
             Place(page, LabelLeft, GraphicsNoteTop, ExtrasList.right - LabelLeft, 5 * InfoHeight));
    for (const Settings::Preset& preset : Settings::Presets) {
        AddText(dialog, dialog.preset, preset.name);
    }
    AddText(dialog, dialog.preset, "Custom");
}

void BuildController(Dialog& dialog, QWidget* page) {
    MakeGroup(dialog, page, "Controller", ControllerGroup);
    for (size_t i = 0; i < Toggles.size(); ++i) {
        const int top = ToggleTop + static_cast<int>(i) * ToggleStep;
        dialog.toggles[i] = MakeCheck(
            dialog, page, Toggles[i].text,
            Place(page, ControllerGroup.left + GroupInset, top,
                  ControllerGroup.right - ControllerGroup.left - 2 * GroupInset, CheckHeight));
    }
    const int pad_left = ActivePadGroup.left + GroupInset;
    const int pad_width = ActivePadGroup.right - GroupInset - pad_left;
    MakeGroup(dialog, page, "Active Gamepad", ActivePadGroup);
    dialog.active_pad = MakeCombo(page, Place(page, pad_left, PadTop, pad_width, ComboHeight));
    dialog.active_pad_id = MakeLabel(dialog, page, "", Qt::AlignLeft | Qt::AlignTop,
                                     Place(page, pad_left, PadIdTop, pad_width, InfoHeight));
    MakeGroup(dialog, page, "Default Gamepad", DefaultPadGroup);
    dialog.default_pad_name =
        MakeLabel(dialog, page, "", Qt::AlignLeft | Qt::AlignTop,
                  Place(page, pad_left, DefaultPadNameTop, pad_width, InfoHeight));
    dialog.default_pad_id =
        MakeLabel(dialog, page, "", Qt::AlignLeft | Qt::AlignTop,
                  Place(page, pad_left, DefaultPadIdTop, pad_width, InfoHeight));
    dialog.set_default_pad =
        MakeButton(dialog, page, "Set Active Gamepad as Default", SetDefaultPadButton);
    QPushButton* const remove_default_pad =
        MakeButton(dialog, page, "Remove Default Gamepad", RemoveDefaultPadButton);
    for (size_t i = 0; i < Sticks.size(); ++i) {
        const Stick& stick = Sticks[i];
        MakeGroup(dialog, page, stick.text, stick.area);
        for (size_t j = 0; j < stick.sliders.size(); ++j) {
            dialog.deadzone_bars[i][j] = MakeSlider(
                dialog, page, stick.sliders[j].text, stick.area.left + GroupInset,
                stick.area.right - GroupInset, stick.sliders[j].top, DeadzoneLow, DeadzoneHigh, {});
        }
    }
    dialog.pad_prompts = MakeRow(dialog, page, "Button Prompts", PadPromptsTop);
    QPushButton* const pad_bindings =
        MakeButton(dialog, page, "Button Bindings...", PadBindingsButton);

    QObject::connect(dialog.active_pad, &QComboBox::activated, dialog.window, [&dialog] {
        dialog.pad_picked = true;
        ShowActivePadId(dialog);
    });
    QObject::connect(pad_bindings, &QPushButton::clicked, dialog.window, [&dialog] {
        const PadBindings buttons =
            dialog.pad_bindings ? *dialog.pad_bindings : ReadPadBindings(dialog.input_config);
        if (auto edited = EditPadBindings(dialog.window, buttons, dialog.values.mouse.control_type,
                                          PadNamesLayout(dialog))) {
            dialog.pad_bindings = std::move(edited);
        }
    });
    QObject::connect(dialog.set_default_pad, &QPushButton::clicked, dialog.window, [&dialog] {
        const int pad = dialog.active_pad->currentIndex();
        if (pad >= 0 && pad < static_cast<int>(dialog.pads.size())) {
            dialog.default_pad = dialog.pads[pad].guid;
            ShowDefaultPad(dialog);
        }
    });
    QObject::connect(remove_default_pad, &QPushButton::clicked, dialog.window, [&dialog] {
        dialog.default_pad.clear();
        ShowDefaultPad(dialog);
    });
}

void BuildMouse(Dialog& dialog, QWidget* page) {
    for (size_t i = 0; i < Cameras.size(); ++i) {
        const Camera& camera = Cameras[i];
        MakeGroup(dialog, page, camera.text, camera.area);
        for (size_t j = 0; j < camera.speeds.size(); ++j) {
            QSlider* const bar =
                MakeSlider(dialog, page, camera.speeds[j].text, camera.area.left + GroupInset,
                           camera.area.right - GroupInset, camera.speeds[j].top, SpeedLow,
                           SpeedHigh, QStringLiteral("%"));
            bar->setPageStep(10);
            dialog.speed_bars[i][j] = bar;
        }
    }
    dialog.prompts = MakeRow(dialog, page, "Button Prompts", PromptsTop);
    for (QComboBox* const combo : {dialog.prompts, dialog.pad_prompts}) {
        for (const auto& [name, value] : PromptChoices) {
            AddText(dialog, combo, name);
        }
    }
    const int note_width = ExtrasList.right - LabelLeft;
    MakeNote(dialog, page,
             "The mouse turns the camera. The game's Camera Sensitivity option scales both. "
             "Automatic follows the controller of player 1, or the keyboard when none is "
             "connected.",
             Place(page, LabelLeft, MouseNoteTop, note_width, 3 * InfoHeight));
    dialog.touch_swipe_box =
        MakeCheck(dialog, page, "Touchscreen to Touch Pad Swipes",
                  Place(page, LabelLeft, TouchSwipeTop, note_width, CheckHeight));
    MakeNote(dialog, page,
             "Turns off mouse camera control. Touches and left-button drags become touch pad "
             "swipes, and the cursor stays free.",
             Place(page, LabelLeft, TouchSwipeNoteTop, note_width, 2 * InfoHeight));
    QPushButton* const key_bindings =
        MakeButton(dialog, page, "Key Bindings...", KeyBindingsButton);

    // Both tabs show the one button prompts setting.
    QObject::connect(dialog.prompts, &QComboBox::activated, dialog.window,
                     [&dialog](int index) { dialog.pad_prompts->setCurrentIndex(index); });
    QObject::connect(dialog.pad_prompts, &QComboBox::activated, dialog.window,
                     [&dialog](int index) { dialog.prompts->setCurrentIndex(index); });
    QObject::connect(key_bindings, &QPushButton::clicked, dialog.window, [&dialog] {
        const KeyBindings keys =
            dialog.key_bindings ? *dialog.key_bindings : ReadKeyBindings(dialog.input_config);
        if (auto edited = EditKeyBindings(dialog.window, keys, dialog.values.mouse.control_type)) {
            dialog.key_bindings = std::move(edited);
        }
    });
}

void BuildDlc(Dialog& dialog, QWidget* page) {
    dialog.dlc = MakeCheck(dialog, page, "Enable DLC Entitlements",
                           Place(page, CheckLeft, FullscreenTop, CheckWidth, CheckHeight));
    dialog.halloween = MakeCheck(dialog, page, "Halloween Photo Items",
                                 Place(page, CheckLeft, ExclusiveTop, CheckWidth, CheckHeight));
}

void BuildOnline(Dialog& dialog, QWidget* page) {
    MakeLink(dialog, page,
             Phrase{"Sign up for shadNet: <a href=\"{0}\">{0}</a>",
                    {std::string(GamePatches::ShadNetSignUp)}},
             Place(page, LabelLeft, FullscreenTop, ExtrasList.right - LabelLeft, CheckHeight));
    QLabel* const id_label =
        MakeLabel(dialog, page, "Online ID", Qt::AlignLeft | Qt::AlignVCenter,
                  Place(page, LabelLeft, OnlineIdTop, LabelWidth, ComboHeight));
    dialog.online_id = new QLineEdit{page};
    dialog.online_id->setGeometry(Place(page, ComboLeft, OnlineIdTop, ComboWidth, ComboHeight));
    QLabel* const password_label =
        MakeLabel(dialog, page, "Password", Qt::AlignLeft | Qt::AlignVCenter,
                  Place(page, LabelLeft, PasswordTop, LabelWidth, ComboHeight));
    dialog.password = new QLineEdit{page};
    dialog.password->setGeometry(Place(page, ComboLeft, PasswordTop, ComboWidth, ComboHeight));
    dialog.password->setEchoMode(QLineEdit::Password);
    QLabel* const avatar_label =
        MakeLabel(dialog, page, "Profile Picture", Qt::AlignLeft | Qt::AlignTop,
                  Place(page, LabelLeft, AvatarTop, LabelWidth, CheckHeight));
    dialog.avatar_picture = new ClickLabel{page};
    dialog.avatar_picture->setGeometry(
        Place(page, ComboLeft, AvatarTop, Avatar::Side, Avatar::Side));
    dialog.avatar_picture->clicked = [&dialog] { ChooseAvatar(dialog); };
    QPushButton* const choose = MakeButton(dialog, page, "Choose Picture...", AvatarChooseButton);
    QLabel* const note = MakeLabel(
        dialog, page, "Gravity Rush 2 sends it to its server the next time you sign in to shadNet.",
        Qt::AlignLeft | Qt::AlignTop,
        Place(page, ComboLeft, AvatarTop + Avatar::Side + 10, ExtrasList.right - ComboLeft,
              CheckHeight));
    dialog.account_controls = {id_label,     dialog.online_id,      password_label, dialog.password,
                               avatar_label, dialog.avatar_picture, choose,         note};
    QObject::connect(choose, &QPushButton::clicked, dialog.window,
                     [&dialog] { ChooseAvatar(dialog); });
}

void BuildTrophies(Dialog& dialog, QWidget* page) {
    MakeLink(dialog, page,
             Phrase{"Sign in to <a href=\"{0}\">shadNet Trophies</a> to view your trophies.",
                    {std::string(GamePatches::ShadNetTrophies)}},
             Place(page, LabelLeft, FullscreenTop, ExtrasList.right - LabelLeft, CheckHeight));
}

void BuildExtras(Dialog& dialog, QWidget* page) {
    dialog.extras_list = new QListWidget{page};
    dialog.extras_list->setGeometry(Place(page, ExtrasList));
    dialog.extras_list->setSelectionMode(QAbstractItemView::SingleSelection);
    for (const GamePatches::Extra& extra : dialog.extras) {
        auto* const item =
            new QListWidgetItem{QString::fromStdString(extra.name), dialog.extras_list};
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
}

// Ok saves everything the dialog shows.
void Save(Dialog& dialog) {
    dialog.values.display = Read(dialog);
    dialog.values.game = ReadGame(dialog);
    dialog.values.graphics = ReadGraphics(dialog);
    dialog.values.mouse = ReadMouse(dialog);
    Settings::Set(dialog.values);
    // The game starts in the language the launcher shows; SaveController saves it.
    EmulatorSettings.SetConsoleLanguage(Language::Current());
    SaveOnline(dialog);
    SaveController(dialog);
    if (dialog.key_bindings && !SaveKeyBindings(dialog.input_config, *dialog.key_bindings)) {
        Tell(dialog, "The key bindings could not be saved.", "Key Bindings");
    }
    if (dialog.pad_bindings && !SavePadBindings(dialog.input_config, *dialog.pad_bindings)) {
        Tell(dialog, "The button bindings could not be saved.", "Button Bindings");
    }
}

void SetDefaults(Dialog& dialog) {
    Show(dialog, Settings::Display{});
    ShowGame(dialog, Settings::Game{});
    ShowGraphics(dialog, Settings::Graphics{});
    ShowUpscaler(dialog, Settings::Graphics{});
    ShowTextureStreaming(dialog, Settings::Graphics{}.texture_streaming_mb);
    ShowPreset(dialog);
    for (size_t i = 0; i < Toggles.size(); ++i) {
        dialog.toggles[i]->setChecked(Toggles[i].fallback);
    }
    ShowDeadzones(dialog, DefaultDeadzones());
    ShowMouse(dialog, Settings::Mouse{});
    dialog.touch_swipe_box->setChecked(false);
    dialog.key_bindings = DefaultKeyBindings();
    dialog.pad_bindings = DefaultPadBindings();
    ShowLanguage(dialog, GeneralSettings{}.console_language.value);
}

void Build(Dialog& dialog) {
    QDialog* const window = dialog.window;
    window->setFixedSize(ClientWidth, ClientHeight);
    window->setFont(DialogFont());
    SetText(dialog, window, "Gravity Rush 2 - Setup");

    auto* const banner = new QLabel{window};
    banner->setGeometry(0, 0, ClientWidth, BannerHeight);
    banner->setPixmap(
        ComposeBanner(*dialog.art, ClientWidth, BannerHeight, window->devicePixelRatioF()));

    // The tabs sit on the frame that holds the pages, as a tab control's do.
    dialog.tab = new QTabBar{window};
    dialog.tab->setExpanding(false);
    dialog.tab->setUsesScrollButtons(true);
    for (const std::string_view name : PageNames) {
        dialog.tab->addTab(Tr(name));
    }
    const int tab_width = TabArea.right - TabArea.left;
    const int tab_height = dialog.tab->sizeHint().height();
    dialog.tab->setGeometry(TabArea.left, TabArea.top, tab_width, tab_height);
    auto* const frame = new QFrame{window};
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setGeometry(TabArea.left, TabArea.top + tab_height - 1, tab_width,
                       TabArea.bottom - TabArea.top - tab_height + 1);
    dialog.tab->raise();
    for (size_t i = 0; i < PageCount; ++i) {
        dialog.pages[i] = new QWidget{window};
        dialog.pages[i]->setGeometry(frame->geometry().adjusted(1, 1, -1, -1));
        dialog.pages[i]->setVisible(i == DisplayPage);
    }

    BuildDisplay(dialog, dialog.pages[DisplayPage]);
    BuildGraphics(dialog, dialog.pages[GraphicsPage]);
    BuildController(dialog, dialog.pages[ControllerPage]);
    BuildMouse(dialog, dialog.pages[MousePage]);
    BuildDlc(dialog, dialog.pages[DlcPage]);
    BuildOnline(dialog, dialog.pages[OnlinePage]);
    BuildTrophies(dialog, dialog.pages[TrophiesPage]);
    BuildExtras(dialog, dialog.pages[ExtrasPage]);

    QPushButton* const defaults = MakeButton(dialog, window, "Set To Default", DefaultsButton);
    QPushButton* const ok = MakeButton(dialog, window, "Ok", OkButton);
    QPushButton* const cancel = MakeButton(dialog, window, "Cancel", CancelButton);

    dialog.monitors = ListMonitors();
    for (const Monitor& monitor : dialog.monitors) {
        AddText(dialog, dialog.monitor, monitor.label);
    }
    for (const std::string_view name : VSyncNames) {
        AddText(dialog, dialog.vsync, name);
    }
    for (const GamePatches::FrameRate& rate : GamePatches::FrameRates) {
        AddText(dialog, dialog.frame_rate, std::to_string(rate.fps) + " FPS");
    }
    for (const GamePatches::Aspect& aspect : GamePatches::Aspects) {
        AddText(dialog, dialog.aspect, aspect.name);
    }
    for (const GamePatches::RenderSize& size : GamePatches::RenderSizes) {
        AddText(dialog, dialog.render_size, size.name);
    }
    for (const std::string_view name : UpscalerChoices) {
        AddText(dialog, dialog.upscaler, name);
    }
    for (const std::string_view name : UpscaleQualityChoices) {
        AddText(dialog, dialog.upscale_quality, name);
    }
    // Each language is named in itself, whatever the language in use.
    for (const int language : Language::Listed()) {
        AddText(dialog, dialog.language, Phrase{"{0}", {Language::Name(language)}});
    }
    ShowLanguage(dialog, Language::Current());
    Show(dialog, dialog.values.display);
    ShowGame(dialog, dialog.values.game);
    ShowGraphics(dialog, dialog.values.graphics);
    ShowUpscaler(dialog, dialog.values.graphics);
    ShowTextureStreaming(dialog, dialog.values.graphics.texture_streaming_mb);
    ShowPreset(dialog);
    ShowController(dialog);
    ShowMouse(dialog, dialog.values.mouse);
    ShowOnline(dialog);

    QObject::connect(dialog.tab, &QTabBar::currentChanged, window, [&dialog](int shown) {
        if (shown == ControllerPage) {
            ShowPads(dialog);
        }
        if (shown == ControllerPage || shown == MousePage) {
            ShowAutomaticPrompts(dialog);
        }
        for (size_t i = 0; i < PageCount; ++i) {
            dialog.pages[i]->setVisible(static_cast<int>(i) == shown);
        }
    });
    for (QCheckBox* const check : {dialog.fullscreen, dialog.exclusive}) {
        QObject::connect(check, &QCheckBox::clicked, window, [&dialog] { UpdateEnabled(dialog); });
    }
    QObject::connect(dialog.monitor, &QComboBox::activated, window, [&dialog](int monitor) {
        const Settings::Display shown = Read(dialog);
        if (monitor >= 0 && monitor < static_cast<int>(dialog.monitors.size())) {
            dialog.modes = ListModes(dialog.monitors[monitor].id);
        }
        FillResolutions(dialog, shown.width, shown.height);
        FillRates(dialog, shown.refresh_rate);
    });
    QObject::connect(dialog.resolution, &QComboBox::activated, window,
                     [&dialog] { FillRates(dialog, ShownRate(dialog)); });
    QObject::connect(dialog.language, &QComboBox::activated, window, [&dialog](int chosen) {
        const auto listed = Language::Listed();
        if (chosen >= 0 && chosen < static_cast<int>(listed.size())) {
            ShowLanguage(dialog, listed[chosen]);
        }
    });
    QObject::connect(dialog.preset, &QComboBox::activated, window, [&dialog](int index) {
        ApplyPreset(dialog, index);
        ShowPreset(dialog);
    });
    for (QComboBox* const combo :
         {dialog.shadows, dialog.fog, dialog.fog_shadows, dialog.occlusion, dialog.motion_blur,
          dialog.antialias, dialog.bloom, dialog.outlines, dialog.decals, dialog.glass_refraction,
          dialog.particles, dialog.anisotropy, dialog.aspect, dialog.render_size,
          dialog.texture_streaming_list}) {
        QObject::connect(combo, &QComboBox::activated, window, [&dialog] { ShowPreset(dialog); });
    }
    QObject::connect(defaults, &QPushButton::clicked, window, [&dialog] { SetDefaults(dialog); });
    QObject::connect(ok, &QPushButton::clicked, window, [&dialog] {
        Save(dialog);
        dialog.window->accept();
    });
    QObject::connect(cancel, &QPushButton::clicked, window, &QDialog::reject);
    // Cancel, and closing the window, go back to the saved language.
    QObject::connect(window, &QDialog::rejected, window,
                     [] { Language::Use(EmulatorSettings.GetConsoleLanguage()); });
    ok->setDefault(true);
    ok->setFocus();
}

} // namespace

void ShowSetup(QWidget* owner, const Art& art) {
    // The monitors, their modes and the GPU come from SDL, as the game's window finds them.
    const bool video_started = SDL_InitSubSystem(SDL_INIT_VIDEO);
    {
        QDialog window{owner};
        Dialog dialog;
        dialog.art = &art;
        dialog.window = &window;
        dialog.values = Settings::Get();
        dialog.extras = GamePatches::Extras();
        Build(dialog);
        window.exec();
        // The game starts SDL's gamepads again, with its own hints.
        if (dialog.gamepads_started) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
    }
    if (video_started) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
}

} // namespace Gr2Port::Launcher
