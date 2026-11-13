// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/logging/formatter.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "core/user_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input/input_mouse.h"
#include "sdl_window.h"

#include "graphics/gr2port/window.h"
#include "graphics/renderdoc.h"

#include <core/emulator_settings.h>
#include "core/libraries/keyboard/keyboard.h"
#include "core/libraries/mouse/sdl_mouse.h"

CMRC_DECLARE(res);

namespace Frontend {

using namespace Libraries::Pad;

static OrbisPadButtonDataOffset SDLGamepadToOrbisButton(u8 button) {
    using OPBDO = OrbisPadButtonDataOffset;

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return OPBDO::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return OPBDO::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return OPBDO::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return OPBDO::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return OPBDO::Cross;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return OPBDO::Triangle;
    case SDL_GAMEPAD_BUTTON_WEST:
        return OPBDO::Square;
    case SDL_GAMEPAD_BUTTON_EAST:
        return OPBDO::Circle;
    case SDL_GAMEPAD_BUTTON_START:
        return OPBDO::Options;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_BACK:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return OPBDO::L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return OPBDO::R1;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return OPBDO::L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return OPBDO::R3;
    default:
        return OPBDO::None;
    }
}

static Uint32 SDLCALL PollController(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->PollState();
    return interval;
}

static Uint32 SDLCALL PollControllerLightColour(void* userdata, SDL_TimerID timer_id,
                                                Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->PollLightColour();
    return interval;
}

WindowSDL::WindowSDL(Input::GameControllers* controllers_) : controllers{*controllers_} {
    if (!SDL_SetHint(SDL_HINT_APP_NAME, "GR2port")) {
        UNREACHABLE_MSG("Failed to set SDL window hint: {}", SDL_GetError());
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        UNREACHABLE_MSG("Failed to initialize SDL video subsystem: {}", SDL_GetError());
    }
    // On macOS, the future Intel compatibility environment does not include camera frameworks.
    // Just skip initializing it entirely, no point in splitting old vs new OS versions here.
#ifndef __APPLE__
    if (!SDL_Init(SDL_INIT_CAMERA)) {
        LOG_ERROR(Input, "Failed to initialize SDL camera subsystem: {}", SDL_GetError());
    }
#endif
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    // GR2port: the game is played in one window, which the renderer shows when it starts.
    window = Gr2Port::Window::Open();
    if (window == nullptr) {
        UNREACHABLE_MSG("Failed to create window handle: {}", SDL_GetError());
    }

    SDL_SetWindowMinimumSize(window, 640, 360);
    SDL_GetWindowSizeInPixels(window, &width, &height);

    SDL_InitSubSystem(SDL_INIT_GAMEPAD);

    // input handler init-s
    Input::ControllerOutput::LinkJoystickAxes();
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));

    // Apply the mouse mode requested by mouse_default_mode in the input config.
    const auto default_mouse_mode = Input::GetMouseMode();
    if (default_mouse_mode == Input::MouseMode::Joystick ||
        default_mouse_mode == Input::MouseMode::Gyro) {
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(), true);
    }

    if (EmulatorSettings.IsBackgroundControllerInput()) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::SetIcon(std::span<const u8> png_data) {
    if (png_data.empty()) {
        LOG_WARNING(Core, "No window icon data available, using default icon.");
        SetDefaultWindowIcon(window);
        return;
    }
    SetWindowIcon(window, std::vector<u8>(png_data.begin(), png_data.end()));
}

void WindowSDL::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    if (!SDL_WaitEvent(&event)) {
        return;
    }

    if (Libraries::Mouse::PushSDLEvent(event) || Libraries::Keyboard::PushSDLEvent(event)) {
        return;
    }

    if (ImGui::Core::ProcessEvent(&event)) {
        return;
    }

    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        OnResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = event.type == SDL_EVENT_WINDOW_EXPOSED;
        OnResize();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        Input::OnMouseMotion(event.motion.xrel, event.motion.yrel);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_WHEEL_OFF:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        OnKeyboardMouseInput(&event);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        controllers.TryOpenSDLControllers();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        OnGamepadEvent(&event);
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    case SDL_EVENT_QUIT_DIALOG:
        Overlay::ToggleQuitWindow();
        break;
    case SDL_EVENT_TOGGLE_FULLSCREEN: {
        // GR2port: the display settings choose the fullscreen mode.
        Gr2Port::Window::ToggleFullscreen();
        break;
    }
    case SDL_EVENT_TOGGLE_PAUSE:
        if (DebugState.IsGuestThreadsPaused()) {
            LOG_INFO(Frontend, "Game Resumed");
            DebugState.ResumeGuestThreads();
        } else {
            LOG_INFO(Frontend, "Game Paused");
            DebugState.PauseGuestThreads();
        }
        break;
    case SDL_EVENT_CHANGE_CONTROLLER:
        UNREACHABLE_MSG("todo");
        break;
    case SDL_EVENT_TOGGLE_SIMPLE_FPS:
        Overlay::ToggleSimpleFps();
        break;
    case SDL_EVENT_TOGGLE_FRIENDS:
        // GR2port has no friends window: the key is taken and dropped.
        break;
    case SDL_EVENT_RELOAD_INPUTS:
        Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
        break;
    case SDL_EVENT_MOUSE_TO_JOYSTICK:
    case SDL_EVENT_MOUSE_TO_GYRO:
    case SDL_EVENT_MOUSE_TO_TOUCHPAD:
        // GR2port: the mouse always turns the camera, so its mode does not change.
        break;
    case SDL_EVENT_MOUSE_TO_TOUCHPAD_SWIPE: {
        // GR2port: swipes take the left button and a free cursor, so the mouse stops turning the
        // camera while they are on.
        const bool swipe = !Input::IsTouchpadSwipeEnabled();
        Input::EnableTouchpadSwipe(swipe);
        Input::SetMouseMode(swipe ? Input::MouseMode::Off : Input::MouseMode::Joystick);
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(), !swipe);
        break;
    }
    case SDL_EVENT_ADD_VIRTUAL_USER:
        for (int i = 0; i < 4; i++) {
            if (controllers[i]->user_id == -1) {
                auto u = UserManagement.GetUserByPlayerIndex(i + 1);
                if (!u) {
                    break;
                }
                controllers[i]->user_id = u->user_id;
                controllers[i]->ConnectController(controllers[i]->m_sdl_gamepad);
                UserManagement.LoginUser(u, i + 1);
                break;
            }
        }
        break;
    case SDL_EVENT_REMOVE_VIRTUAL_USER:
        LOG_INFO(Input, "Remove user");
        for (int i = 3; i >= 0; i--) {
            if (controllers[i]->user_id != -1) {
                UserManagement.LogoutUser(UserManagement.GetUserByID(controllers[i]->user_id));
                controllers[i]->DisconnectController();
                controllers[i]->user_id = -1;
                break;
            }
        }
        break;
    case SDL_EVENT_RDOC_CAPTURE:
        if (VideoCore::IsRenderDocLoaded()) {
            VideoCore::TriggerCapture();
        } else {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        }
        break;
    case SDL_EVENT_SCREENSHOT_WITH_OVERLAYS:
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        break;
    default:
        break;
    }
}

void WindowSDL::InitTimers() {
    for (int i = 0; i < 4; ++i) {
        SDL_AddTimer(4, &PollController, controllers[i]);
    }
    SDL_AddTimer(33, Input::MousePolling, (void*)controllers[0]);
}

void WindowSDL::RequestKeyboard() {
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StartTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
    keyboard_grab++;
}

void WindowSDL::ReleaseKeyboard() {
    ASSERT(keyboard_grab > 0);
    keyboard_grab--;
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StopTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
}

void WindowSDL::OnResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
}

Uint32 wheelOffCallback(void* og_event, Uint32 timer_id, Uint32 interval) {
    SDL_Event off_event = *(SDL_Event*)og_event;
    off_event.type = SDL_EVENT_MOUSE_WHEEL_OFF;
    SDL_PushEvent(&off_event);
    delete (SDL_Event*)og_event;
    return 0;
}

void WindowSDL::OnKeyboardMouseInput(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    // Touchpad swipe emulation intercepts left-button (touchscreen) presses on the first pad,
    // independent of the mouse mode. Everything else passes through.
    if (Input::IsTouchpadSwipeEnabled() &&
        (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event->button.button == SDL_BUTTON_LEFT) {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            Input::TouchpadSwipeOnFingerDown(controllers[0], event->button.x, event->button.y);
        } else {
            Input::TouchpadSwipeOnFingerUp(controllers[0], event->button.x, event->button.y);
        }
        return;
    }

    // get the event's id, if it's keyup or keydown
    const bool input_down = event->type == SDL_EVENT_KEY_DOWN ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event->type == SDL_EVENT_MOUSE_WHEEL;
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // if it's a wheel event, make a timer that turns it off after a set time
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        const SDL_Event* copy = new SDL_Event(*event);
        SDL_AddTimer(33, wheelOffCallback, (void*)copy);
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    // update bindings
    if (inputs_changed) {
        Input::ActivateOutputsFromInputs();
    }
}

void WindowSDL::OnGamepadEvent(const SDL_Event* event) {
    bool input_down = event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
                      event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // the touchpad button shouldn't be rebound to anything else,
    // as it would break the entire touchpad handling
    // You can still bind other things to it though
    if (event->gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gbutton.which)]->Button(
            OrbisPadButtonDataOffset::TouchPad, input_down);
        return;
    }

    u8 gamepad;

    switch (event->type) {
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        switch ((SDL_SensorType)event->gsensor.sensor) {
        case SDL_SENSOR_GYRO:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateSensorGyro(event->gsensor.data);
            }
            break;
        case SDL_SENSOR_ACCEL:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateAcceleration(event->gsensor.data);
            }
            break;
        default:
            break;
        }
        return;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gtouchpad.which)]
            ->SetTouchpadState(event->gtouchpad.finger,
                               event->type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP, event->gtouchpad.x,
                               event->gtouchpad.y);
        return;
    default:
        break;
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    if (inputs_changed) {
        // update bindings
        Input::ActivateOutputsFromInputs();
    }
}

#ifndef __APPLE__
void SetWindowIcon(SDL_Window* window, const std::vector<u8>& png) {
    int imageWidth = 0;
    int imageHeight = 0;
    constexpr int numChannels = 4;
    unsigned char* imageData = stbi_load_from_memory(png.data(), png.size(), &imageWidth,
                                                     &imageHeight, nullptr, numChannels);
    if (imageData == nullptr) {
        LOG_ERROR(Core, "Failed to load window icon image: {}", stbi_failure_reason());
        return;
    }
    SCOPE_EXIT {
        stbi_image_free(imageData);
    };

    SDL_Surface* surface = SDL_CreateSurfaceFrom(imageWidth, imageHeight, SDL_PIXELFORMAT_RGBA32,
                                                 imageData, imageWidth * numChannels);
    if (surface == nullptr) {
        LOG_ERROR(Core, "Failed to create SDL surface for window icon: {}", SDL_GetError());
    }
    if (!SDL_SetWindowIcon(window, surface)) {
        LOG_ERROR(Core, "Failed to set SDL window icon: {}", SDL_GetError());
    }
    SDL_DestroySurface(surface);
}
#endif

void SetDefaultWindowIcon(SDL_Window* window) {
    const auto resource = cmrc::res::get_filesystem();
    const auto file = resource.open("src/resources/gr2port.png");
    const std::vector<u8> texData = std::vector<u8>(file.begin(), file.end());
    SetWindowIcon(window, texData);
}

} // namespace Frontend
