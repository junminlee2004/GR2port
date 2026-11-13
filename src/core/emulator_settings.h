// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/logging/log.h"
#include "common/types.h"

#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())

enum HideCursorState : int {
    Never,
    Idle,
    Always,
};

enum UsbBackendType : int {
    Real,
    SkylandersPortal,
    InfinityBase,
    DimensionsToypad,
};

enum class ConfigMode {
    Default,
    Global,
    Clean,
};

enum AudioBackend : int {
    SDL,
    OpenAL,
    // Add more backends as needed
};

enum OpenALHrtfMode : int {
    HrtfAuto, // Let OpenAL Soft decide (on for headphone-like stereo outputs)
    HrtfOn,   // Force HRTF binaural rendering
    HrtfOff,  // Never use HRTF
};

enum OpenALOutputMode : int {
    OutputAuto,       // Let OpenAL Soft negotiate with the device
    OutputStereo,     // Force stereo output
    OutputQuad,       // Force quadraphonic output
    OutputSurround51, // Force 5.1 surround output
    OutputSurround71, // Force 7.1 surround output
};

template <typename T>
struct Setting {
    T default_value{};
    T value{};
    std::optional<T> game_specific_value{};

    Setting() = default;
    // Single-argument ctor: initialises both default_value and value so
    // that CleanMode can always recover the intended factory default.
    /*implicit*/ Setting(T init) : default_value(std::move(init)), value(default_value) {}

    /// Return the active value under the given mode.
    T get(ConfigMode mode = ConfigMode::Default) const {
        switch (mode) {
        case ConfigMode::Default:
            return game_specific_value.value_or(value);
        case ConfigMode::Global:
            return value;
        case ConfigMode::Clean:
            return default_value;
        }
        return value;
    }

    /// Write v to the base layer.
    /// Set proper value as base or game_specific
    void set(const T& v, bool game_specific = false) {
        if (game_specific) {
            game_specific_value = v;
        } else {
            value = v;
        }
    }

    /// Discard the game-specific override; subsequent get(Default) will
    /// fall back to the base value.
    void reset_game_specific() {
        game_specific_value = std::nullopt;
    }
};

template <typename T>
void to_json(nlohmann::json& j, const Setting<T>& s) {
    j = s.value;
}

template <typename T>
void from_json(const nlohmann::json& j, Setting<T>& s) {
    s.value = j.get<T>();
}

struct OverrideItem {
    const char* key;
    std::function<void(void* group_ptr, const nlohmann::json& entry,
                       std::vector<std::string>& changed)>
        apply;
    /// Return the value that should be written to the per-game config file.
    /// Falls back to base value if no game-specific override is set.
    std::function<nlohmann::json(const void* group_ptr)> get_for_save;

    /// Clear game_specific_value for this field.
    std::function<void(void* group_ptr)> reset_game_specific;
};

template <typename Struct, typename T>
inline OverrideItem make_override(const char* key, Setting<T> Struct::* member) {
    return OverrideItem{
        key,
        [member, key](void* base, const nlohmann::json& entry, std::vector<std::string>& changed) {
            Struct* obj = reinterpret_cast<Struct*>(base);
            Setting<T>& dst = obj->*member;
            try {
                T newValue = entry.get<T>();
                if (dst.value != newValue) {
                    std::ostringstream oss;
                    oss << key << " ( " << dst.value << " -> " << newValue << " )";
                    changed.push_back(oss.str());
                }
                dst.game_specific_value = newValue;
            } catch (const std::exception& e) {
                LOG_ERROR(Config, "[make_override] error parsing {}: {}", key, e.what());
                LOG_ERROR(Config, "[make_override] Entry was: {}", entry.dump());
                LOG_ERROR(Config, "[make_override] Type name: {}", entry.type_name());
            }
        },

        // --- get_for_save -------------------------------------------
        // Returns game_specific_value when present, otherwise base value.
        // This means a freshly-opened game-specific dialog still shows
        // useful (current-global) values rather than empty entries.
        [member](const void* base) -> nlohmann::json {
            const Struct* obj = reinterpret_cast<const Struct*>(base);
            const Setting<T>& src = obj->*member;
            return nlohmann::json(src.game_specific_value.value_or(src.value));
        },

        // --- reset_game_specific ------------------------------------
        [member](void* base) {
            Struct* obj = reinterpret_cast<Struct*>(base);
            (obj->*member).reset_game_specific();
        }};
}

// -------------------------------
// Support types
// -------------------------------
struct GameInstallDir {
    std::filesystem::path path;
    bool enabled;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GameInstallDir, path, enabled)

// -------------------------------
// General settings
// -------------------------------
struct GeneralSettings {
    Setting<std::vector<GameInstallDir>> install_dirs;
    Setting<std::filesystem::path> addon_install_dir;
    Setting<std::filesystem::path> home_dir;
    Setting<std::filesystem::path> font_dir;

    Setting<int> volume_slider{100};
    Setting<bool> neo_mode{false};
    Setting<bool> dev_kit_mode{false};
    Setting<int> extra_dmem_in_mbytes{0};
    Setting<int> extra_fmem_in_mbytes{0};
    Setting<bool> trophy_popup_disabled{false};
    Setting<double> trophy_notification_duration{6.0};
    Setting<std::string> trophy_notification_side{"right"};
    Setting<bool> show_splash{false};
    Setting<bool> show_fps_counter{false};
    Setting<int> console_language{1};
    Setting<int> settings_window_scale{1000};
    Setting<bool> redzone_patches{false};
    Setting<bool> legacy_cpu_patches{false};
    // The launcher tells a player with an NVIDIA RTX GPU to update a driver too old for DLSS.
    Setting<bool> nvidia_driver_notice{true};
    // return a vector of override descriptors (runtime, but tiny)
    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<GeneralSettings>("volume_slider", &GeneralSettings::volume_slider),
            make_override<GeneralSettings>("neo_mode", &GeneralSettings::neo_mode),
            make_override<GeneralSettings>("dev_kit_mode", &GeneralSettings::dev_kit_mode),
            make_override<GeneralSettings>("extra_dmem_in_mbytes",
                                           &GeneralSettings::extra_dmem_in_mbytes),
            make_override<GeneralSettings>("extra_fmem_in_mbytes",
                                           &GeneralSettings::extra_fmem_in_mbytes),
            make_override<GeneralSettings>("trophy_popup_disabled",
                                           &GeneralSettings::trophy_popup_disabled),
            make_override<GeneralSettings>("trophy_notification_duration",
                                           &GeneralSettings::trophy_notification_duration),
            make_override<GeneralSettings>("show_splash", &GeneralSettings::show_splash),
            make_override<GeneralSettings>("trophy_notification_side",
                                           &GeneralSettings::trophy_notification_side),
            make_override<GeneralSettings>("console_language", &GeneralSettings::console_language),
            make_override<GeneralSettings>("redzone_patches", &GeneralSettings::redzone_patches)};
    }
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GeneralSettings, install_dirs, addon_install_dir, home_dir,
                                   font_dir, volume_slider, neo_mode, dev_kit_mode,
                                   extra_dmem_in_mbytes, extra_fmem_in_mbytes,
                                   trophy_popup_disabled, trophy_notification_duration, show_splash,
                                   trophy_notification_side, show_fps_counter, console_language,
                                   settings_window_scale, redzone_patches, legacy_cpu_patches,
                                   nvidia_driver_notice)

// -------------------------------
// Network settings
// -------------------------------
struct NetworkSettings {
    Setting<bool> connected_to_network{false};
    Setting<bool> shad_net_enabled{false};
    Setting<std::string> shadnet_server{"srv.shadps4.net:31313"};
    Setting<std::string> shadnet_webapi_server{"http://srv.shadps4.net:31315"};
    Setting<bool> enable_upnp{true};
    Setting<int> p2p_port{3658}; // 0 is any free port, 3658 is the default PS4 port. With UPnP
                                 // enabled it is forwarded on the router.
    Setting<bool> disable_https{false};
    Setting<bool> enable_device_discovery{false};

    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<NetworkSettings>("connected_to_network",
                                           &NetworkSettings::connected_to_network),
            make_override<NetworkSettings>("shad_net_enabled", &NetworkSettings::shad_net_enabled),
            make_override<NetworkSettings>("shadnet_server", &NetworkSettings::shadnet_server),
            make_override<NetworkSettings>("shadnet_webapi_server",
                                           &NetworkSettings::shadnet_webapi_server),
            make_override<NetworkSettings>("enable_upnp", &NetworkSettings::enable_upnp),
            make_override<NetworkSettings>("p2p_port", &NetworkSettings::p2p_port),
            make_override<NetworkSettings>("disable_https", &NetworkSettings::disable_https),
            make_override<NetworkSettings>("enable_device_discovery",
                                           &NetworkSettings::enable_device_discovery)};
    }
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NetworkSettings, connected_to_network, shad_net_enabled,
                                   shadnet_server, shadnet_webapi_server, enable_upnp, p2p_port,
                                   disable_https, enable_device_discovery)

// -------------------------------
// Log settings
// -------------------------------
struct LogSettings {
    Setting<bool> append{false}; // specific
    Setting<bool> enable{true};  // specific
    Setting<std::string> filter{""};
    Setting<std::string> flush_level{""};
    Setting<u32> max_skip_duration{5'000};
    Setting<bool> separate{false}; // specific
    Setting<unsigned long long> size_limit{100_MB};
    Setting<bool> skip_duplicate{true};
    Setting<bool> sync{true};
#ifdef _WIN32
    Setting<std::string> type{"wincolor"};
#endif

    // return a vector of override descriptors (runtime, but tiny)
    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<LogSettings>("append", &LogSettings::append),
            make_override<LogSettings>("enable", &LogSettings::enable),
            make_override<LogSettings>("filter", &LogSettings::filter),
            make_override<LogSettings>("flush_level", &LogSettings::flush_level),
            make_override<LogSettings>("max_skip_duration", &LogSettings::max_skip_duration),
            make_override<LogSettings>("separate", &LogSettings::separate),
            make_override<LogSettings>("size_limit", &LogSettings::size_limit),
            make_override<LogSettings>("skip_duplicate", &LogSettings::skip_duplicate),
            make_override<LogSettings>("sync", &LogSettings::sync),
#ifdef _WIN32
            make_override<LogSettings>("type", &LogSettings::type),
#endif
        };
    }
};
#ifdef _WIN32
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LogSettings, append, enable, filter, flush_level,
                                   max_skip_duration, separate, size_limit, skip_duplicate, sync,
                                   type)
#else
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LogSettings, append, enable, filter, flush_level,
                                   max_skip_duration, separate, size_limit, skip_duplicate, sync)
#endif

// -------------------------------
// Debug settings
// -------------------------------
struct DebugSettings {
    Setting<bool> debug_dump{false};         // specific
    Setting<std::string> config_version{""}; // specific

    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<DebugSettings>("debug_dump", &DebugSettings::debug_dump)};
    }
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(DebugSettings, debug_dump, config_version)

// -------------------------------
// Input settings
// -------------------------------

// The highest gyro deadzone the settings offer, in degrees per second.
inline constexpr int MaxGyroDeadzone = 20;

struct InputSettings {
    Setting<int> cursor_state{HideCursorState::Idle};      // specific
    Setting<int> cursor_hide_timeout{5};                   // specific
    Setting<int> usb_device_backend{UsbBackendType::Real}; // specific
    Setting<bool> use_special_pad{false};
    Setting<int> special_pad_class{1};
    Setting<bool> motion_controls_enabled{true}; // specific
    // Handheld gyro fixes: a device held upright (Steam Deck, ROG Ally) measures the game's yaw
    // on its roll channel and vice versa; the inversions compose with the swap.
    Setting<bool> gyro_swap_yaw_roll{false}; // specific
    Setting<bool> gyro_invert_yaw{false};    // specific
    Setting<bool> gyro_invert_x{false};      // specific
    Setting<bool> gyro_invert_roll{false};   // specific
    // A gyro sensor's rotation slower than this many degrees per second counts as none; 0 turns
    // the deadzone off.
    Setting<int> gyro_deadzone{0}; // specific
    Setting<bool> use_unified_input_config{true};
    Setting<std::string> default_controller_id{""};
    Setting<bool> background_controller_input{false}; // specific
    Setting<bool> ime_accessibility_enabled{false};   // specific
    Setting<bool> ime_url_mail_short_panel{false};    // specific
    Setting<bool> is_circle_enter{false};             // specific
    Setting<s32> camera_id{-1};
    Setting<bool> use_mice_as_mice{false};
    Setting<bool> use_keyboard_as_keyboard{false};
    Setting<bool> vibration_enabled{true}; // specific

    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<InputSettings>("cursor_state", &InputSettings::cursor_state),
            make_override<InputSettings>("cursor_hide_timeout",
                                         &InputSettings::cursor_hide_timeout),
            make_override<InputSettings>("usb_device_backend", &InputSettings::usb_device_backend),
            make_override<InputSettings>("motion_controls_enabled",
                                         &InputSettings::motion_controls_enabled),
            make_override<InputSettings>("gyro_swap_yaw_roll", &InputSettings::gyro_swap_yaw_roll),
            make_override<InputSettings>("gyro_invert_yaw", &InputSettings::gyro_invert_yaw),
            make_override<InputSettings>("gyro_invert_x", &InputSettings::gyro_invert_x),
            make_override<InputSettings>("gyro_invert_roll", &InputSettings::gyro_invert_roll),
            make_override<InputSettings>("gyro_deadzone", &InputSettings::gyro_deadzone),
            make_override<InputSettings>("background_controller_input",
                                         &InputSettings::background_controller_input),
            make_override<InputSettings>("ime_accessibility_enabled",
                                         &InputSettings::ime_accessibility_enabled),
            make_override<InputSettings>("ime_url_mail_short_panel",
                                         &InputSettings::ime_url_mail_short_panel),
            make_override<InputSettings>("is_circle_enter", &InputSettings::is_circle_enter),
            make_override<InputSettings>("camera_id", &InputSettings::camera_id),
            make_override<InputSettings>("use_mice_as_mice", &InputSettings::use_mice_as_mice),
            make_override<InputSettings>("use_keyboard_as_keyboard",
                                         &InputSettings::use_keyboard_as_keyboard),
            make_override<InputSettings>("vibration_enabled", &InputSettings::vibration_enabled)};
    }
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(InputSettings, cursor_state, cursor_hide_timeout,
                                   usb_device_backend, use_special_pad, special_pad_class,
                                   motion_controls_enabled, gyro_swap_yaw_roll, gyro_invert_yaw,
                                   gyro_invert_x, gyro_invert_roll, gyro_deadzone,
                                   use_unified_input_config, default_controller_id,
                                   background_controller_input, ime_accessibility_enabled,
                                   ime_url_mail_short_panel, camera_id, is_circle_enter,
                                   use_mice_as_mice, use_keyboard_as_keyboard, vibration_enabled)
// -------------------------------
// Audio settings
// -------------------------------
struct AudioSettings {
    Setting<u32> audio_backend{AudioBackend::SDL};
    Setting<std::string> sdl_mic_device{"Default Device"};
    Setting<std::string> sdl_main_output_device{"Default Device"};
    Setting<std::string> sdl_padSpk_output_device{"Default Device"};
    Setting<std::string> openal_mic_device{"Default Device"};
    Setting<std::string> openal_main_output_device{"Default Device"};
    Setting<std::string> openal_padSpk_output_device{"Default Device"};
    Setting<u32> openal_hrtf{OpenALHrtfMode::HrtfAuto};
    Setting<u32> openal_output_mode{OpenALOutputMode::OutputAuto};

    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<AudioSettings>("audio_backend", &AudioSettings::audio_backend),
            make_override<AudioSettings>("sdl_mic_device", &AudioSettings::sdl_mic_device),
            make_override<AudioSettings>("sdl_main_output_device",
                                         &AudioSettings::sdl_main_output_device),
            make_override<AudioSettings>("sdl_padSpk_output_device",
                                         &AudioSettings::sdl_padSpk_output_device),
            make_override<AudioSettings>("openal_mic_device", &AudioSettings::openal_mic_device),
            make_override<AudioSettings>("openal_main_output_device",
                                         &AudioSettings::openal_main_output_device),
            make_override<AudioSettings>("openal_padSpk_output_device",
                                         &AudioSettings::openal_padSpk_output_device),
            make_override<AudioSettings>("openal_hrtf", &AudioSettings::openal_hrtf),
            make_override<AudioSettings>("openal_output_mode", &AudioSettings::openal_output_mode)};
    }
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AudioSettings, audio_backend, sdl_mic_device,
                                   sdl_main_output_device, sdl_padSpk_output_device,
                                   openal_mic_device, openal_main_output_device,
                                   openal_padSpk_output_device, openal_hrtf, openal_output_mode)

// -------------------------------
// GPU settings
// -------------------------------
struct GPUSettings {
    Setting<u32> window_width{1280};
    Setting<u32> window_height{720};
    Setting<u32> internal_screen_width{1280};
    Setting<u32> internal_screen_height{720};
    // Record the draw scheduler's Vulkan commands and issue its queue submits on a dedicated
    // thread; the GPU command thread stores each call as a closure instead of calling the driver.
    Setting<bool> vk_record_thread{true};
    // The faulting thread performs its readback and the GPU command thread never waits for it:
    // retired GPU writes are read straight out of a host visible arena, otherwise a copy lane with
    // a queue of its own copies them; a GPU write marked in the window meanwhile keeps its pages
    // GPU modified.
    Setting<bool> readback_offload{false};
    // While a deferred operation waits out its GPU tick, attempt the pending
    // pop (a lock plus a fence-query ioctl) once per this many draw-rate
    // polls instead of every draw. 0 polls every call.
    Setting<u32> pending_pop_throttle{64};
    // Serve guest-visible backing writes from a per-thread memo of the last
    // resolved physical chunks, revalidated by the memory map generation; the
    // map descent runs only on a miss.
    Setting<bool> backing_write_memo{true};
    // Bakes the color write mask into the pipeline's blend state instead of
    // declaring it dynamic. The mask is already a pipeline key field, so the
    // pipeline count is unchanged.
    Setting<bool> static_color_write_mask{true};
    // Hands a heap descriptor set out again once the tick that recorded it retired,
    // instead of allocating a fresh one per push and resetting whole pools.
    // Pools are never reset in this mode.
    Setting<bool> desc_heap_recycle{true};
    Setting<u32> vblank_frequency{60};
    Setting<bool> full_screen{false};
    Setting<std::string> full_screen_mode{"Windowed"};
    Setting<std::string> present_mode{"Mailbox"};
    Setting<bool> hdr_allowed{false};

#define GPU_OVERRIDE(field) make_override<GPUSettings>(#field, &GPUSettings::field)
    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<GPUSettings>("full_screen", &GPUSettings::full_screen),
            make_override<GPUSettings>("full_screen_mode", &GPUSettings::full_screen_mode),
            make_override<GPUSettings>("present_mode", &GPUSettings::present_mode),
            make_override<GPUSettings>("window_height", &GPUSettings::window_height),
            make_override<GPUSettings>("window_width", &GPUSettings::window_width),
            make_override<GPUSettings>("hdr_allowed", &GPUSettings::hdr_allowed),
            GPU_OVERRIDE(vk_record_thread),
            GPU_OVERRIDE(readback_offload),
            GPU_OVERRIDE(pending_pop_throttle),
            GPU_OVERRIDE(backing_write_memo),
            GPU_OVERRIDE(static_color_write_mask),
            GPU_OVERRIDE(desc_heap_recycle),
            make_override<GPUSettings>("vblank_frequency", &GPUSettings::vblank_frequency),
        };
    }
#undef GPU_OVERRIDE
};
// nlohmann's field macros take at most 63 names, so the GPU settings are
// serialized in two groups; new settings go at the end of the second.
// clang-format off
#define GPU_SETTINGS_JSON_FIELDS_A \
    window_width, window_height, internal_screen_width, \
    internal_screen_height, vblank_frequency, full_screen, full_screen_mode, \
    present_mode, hdr_allowed, backing_write_memo
#define GPU_SETTINGS_JSON_FIELDS_B \
    static_color_write_mask, desc_heap_recycle, pending_pop_throttle, \
    readback_offload, vk_record_thread
// clang-format on
template <
    typename BasicJsonType,
    nlohmann::detail::enable_if_t<nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void to_json(BasicJsonType& nlohmann_json_j, const GPUSettings& nlohmann_json_t) {
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, GPU_SETTINGS_JSON_FIELDS_A))
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, GPU_SETTINGS_JSON_FIELDS_B))
}
template <
    typename BasicJsonType,
    nlohmann::detail::enable_if_t<nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
void from_json(const BasicJsonType& nlohmann_json_j, GPUSettings& nlohmann_json_t) {
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, GPU_SETTINGS_JSON_FIELDS_A))
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, GPU_SETTINGS_JSON_FIELDS_B))
}

// -------------------------------
// Vulkan settings
// -------------------------------
struct VulkanSettings {
    Setting<s32> gpu_id{-1};
    Setting<bool> renderdoc_enabled{false};
    Setting<bool> vkvalidation_enabled{false};
    Setting<bool> vkvalidation_core_enabled{true};
    Setting<bool> vkvalidation_sync_enabled{false};
    Setting<bool> vkvalidation_gpu_enabled{false};
    Setting<bool> vkcrash_diagnostic_enabled{false};
    Setting<bool> vkhost_markers{false};

    std::vector<OverrideItem> GetOverrideableFields() const {
        return std::vector<OverrideItem>{
            make_override<VulkanSettings>("gpu_id", &VulkanSettings::gpu_id),
            make_override<VulkanSettings>("renderdoc_enabled", &VulkanSettings::renderdoc_enabled),
            make_override<VulkanSettings>("vkvalidation_enabled",
                                          &VulkanSettings::vkvalidation_enabled),
            make_override<VulkanSettings>("vkvalidation_core_enabled",
                                          &VulkanSettings::vkvalidation_core_enabled),
            make_override<VulkanSettings>("vkvalidation_sync_enabled",
                                          &VulkanSettings::vkvalidation_sync_enabled),
            make_override<VulkanSettings>("vkvalidation_gpu_enabled",
                                          &VulkanSettings::vkvalidation_gpu_enabled),
            make_override<VulkanSettings>("vkcrash_diagnostic_enabled",
                                          &VulkanSettings::vkcrash_diagnostic_enabled),
            make_override<VulkanSettings>("vkhost_markers", &VulkanSettings::vkhost_markers),
        };
    }
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(VulkanSettings, gpu_id, renderdoc_enabled, vkvalidation_enabled,
                                   vkvalidation_core_enabled, vkvalidation_sync_enabled,
                                   vkvalidation_gpu_enabled, vkcrash_diagnostic_enabled,
                                   vkhost_markers)

// -------------------------------
// Main manager
// -------------------------------
class EmulatorSettingsImpl {
public:
    EmulatorSettingsImpl();
    ~EmulatorSettingsImpl();

    static std::shared_ptr<EmulatorSettingsImpl> GetInstance();
    static void SetInstance(std::shared_ptr<EmulatorSettingsImpl> instance);

    bool Save(const std::string& serial = "");
    bool Load(const std::string& serial = "");
    void SetDefaultValues();
    bool TransferSettings();

    // Config mode
    ConfigMode GetConfigMode() const {
        return m_configMode;
    }
    void SetConfigMode(ConfigMode mode) {
        m_configMode = mode;
    }

    //
    // Game-specific override management
    /// Clears all per-game overrides.  Call this when a game exits so
    /// the emulator reverts to global settings.
    void ClearGameSpecificOverrides();

    // general accessors
    bool AddGameInstallDir(const std::filesystem::path& dir, bool enabled = true);
    std::vector<std::filesystem::path> GetGameInstallDirs() const;
    void SetAllGameInstallDirs(const std::vector<GameInstallDir>& dirs);
    void RemoveGameInstallDir(const std::filesystem::path& dir);
    void SetGameInstallDirEnabled(const std::filesystem::path& dir, bool enabled);
    void SetGameInstallDirs(const std::vector<std::filesystem::path>& dirs_config);
    const std::vector<bool> GetGameInstallDirsEnabled();
    const std::vector<GameInstallDir>& GetAllGameInstallDirs() const;

    /// The folder the settings name for dumped system fonts, or empty.
    std::filesystem::path GetFontsDir();
    void SetFontsDir(const std::filesystem::path& dir);
    /// The folder the settings name for additional content, or empty.
    std::filesystem::path GetAddonInstallDir();
    void SetAddonInstallDir(const std::filesystem::path& dir);

private:
    GeneralSettings m_general{};
    NetworkSettings m_network{};
    LogSettings m_log{};
    DebugSettings m_debug{};
    InputSettings m_input{};
    AudioSettings m_audio{};
    GPUSettings m_gpu{};
    VulkanSettings m_vulkan{};
    ConfigMode m_configMode{ConfigMode::Default};

    // Runtime-only override: when true, IsShadNetEnabled() reports false for the
    // rest of this run regardless of the persisted setting
    std::atomic<bool> m_shadnet_session_disabled{false};

    static std::shared_ptr<EmulatorSettingsImpl> s_instance;
    static std::mutex s_mutex;

    /// Apply overrideable fields from groupJson into group.game_specific_value.
    template <typename Group>
    void ApplyGroupOverrides(Group& group, const nlohmann::json& groupJson,
                             std::vector<std::string>& changed) {
        for (auto& item : group.GetOverrideableFields()) {
            if (!groupJson.contains(item.key))
                continue;
            item.apply(&group, groupJson.at(item.key), changed);
        }
    }

    // Write all overrideable fields from group into out (for game-specific save). A field that
    // equals the group's base value is written as null, which removes its key in the merge.
    template <typename Group>
    static void SaveGroupGameSpecific(const Group& group, nlohmann::json& out) {
        const nlohmann::json global = group;
        for (auto& item : group.GetOverrideableFields()) {
            out[item.key] = item.get_for_save(&group);
            if (out[item.key] == global.at(item.key)) {
                out[item.key] = nullptr;
            }
        }
    }

    // Discard every game-specific override in group.
    template <typename Group>
    static void ClearGroupOverrides(Group& group) {
        for (auto& item : group.GetOverrideableFields())
            item.reset_game_specific(&group);
    }

    // Call fn on the group stored under the given config section and return its result, or a
    // value-initialized result for an unknown section.
    template <typename Self, typename Fn>
    static auto VisitGroup(Self& self, std::string_view section, Fn&& fn) {
        if (section == "General")
            return fn(self.m_general);
        if (section == "Network")
            return fn(self.m_network);
        if (section == "Log")
            return fn(self.m_log);
        if (section == "Debug")
            return fn(self.m_debug);
        if (section == "Input")
            return fn(self.m_input);
        if (section == "Audio")
            return fn(self.m_audio);
        if (section == "GPU")
            return fn(self.m_gpu);
        if (section == "Vulkan")
            return fn(self.m_vulkan);
        return decltype(fn(self.m_general)){};
    }

    static void PrintChangedSummary(const std::vector<std::string>& changed);
    void MigrateNetworkKeys(const nlohmann::json& general, const nlohmann::json& network);
    static void SyncLegacyNetworkKeys(const nlohmann::json& old_general, nlohmann::json& general,
                                      const nlohmann::json& network);

public:
    // Used by the launcher to back up / restore settings (e.g. per-game settings dialog)
    EmulatorSettingsImpl& operator=(const EmulatorSettingsImpl& other) {
        if (this != &other) {
            m_shadnet_session_disabled.store(other.m_shadnet_session_disabled.load());
            m_general = other.m_general;
            m_network = other.m_network;
            m_log = other.m_log;
            m_debug = other.m_debug;
            m_input = other.m_input;
            m_audio = other.m_audio;
            m_gpu = other.m_gpu;
            m_vulkan = other.m_vulkan;
            m_configMode = other.m_configMode;
        }
        return *this;
    }

    // Add these getters to access overrideable fields
    std::vector<OverrideItem> GetGeneralOverrideableFields() const {
        return m_general.GetOverrideableFields();
    }
    std::vector<OverrideItem> GetNetworkOverrideableFields() const {
        return m_network.GetOverrideableFields();
    }
    std::vector<OverrideItem> GetDebugOverrideableFields() const {
        return m_debug.GetOverrideableFields();
    }
    std::vector<OverrideItem> GetInputOverrideableFields() const {
        return m_input.GetOverrideableFields();
    }
    std::vector<OverrideItem> GetAudioOverrideableFields() const {
        return m_audio.GetOverrideableFields();
    }
    std::vector<OverrideItem> GetGPUOverrideableFields() const {
        return m_gpu.GetOverrideableFields();
    }
    std::vector<OverrideItem> GetVulkanOverrideableFields() const {
        return m_vulkan.GetOverrideableFields();
    }
    std::vector<std::string> GetAllOverrideableKeys() const;

    /// Return the fields of a config section ("GPU", "Input", ...) by JSON key, each with its
    /// value in the current config mode. Null for an unknown section.
    nlohmann::json GetGroupValues(std::string_view section) const;
    /// Write the given keys of a config section as global values, or as per-game overrides when
    /// specific is set. Keys that cannot be written this way (unknown, or not overrideable for a
    /// per-game write) are skipped, and a value of the wrong JSON type rejects the whole call.
    /// Returns whether every given key was applied.
    bool SetGroupValues(std::string_view section, const nlohmann::json& values, bool specific);

#define SETTING_FORWARD(group, Name, field)                                                        \
    auto Get##Name() const {                                                                       \
        return (group).field.get(m_configMode);                                                    \
    }                                                                                              \
    void Set##Name(const decltype((group).field.value)& v, bool specific = false) {                \
        (group).field.set(v, specific);                                                            \
    }
#define SETTING_FORWARD_BOOL(group, Name, field)                                                   \
    bool Is##Name() const {                                                                        \
        return (group).field.get(m_configMode);                                                    \
    }                                                                                              \
    void Set##Name(bool v, bool specific = false) {                                                \
        (group).field.set(v, specific);                                                            \
    }
#define SETTING_FORWARD_BOOL_READONLY(group, Name, field)                                          \
    bool Is##Name() const {                                                                        \
        return (group).field.get(m_configMode);                                                    \
    }

    // General settings
    SETTING_FORWARD(m_general, VolumeSlider, volume_slider)
    SETTING_FORWARD_BOOL(m_general, Neo, neo_mode)
    SETTING_FORWARD_BOOL(m_general, DevKit, dev_kit_mode)
    SETTING_FORWARD(m_general, ExtraDmemInMBytes, extra_dmem_in_mbytes)
    SETTING_FORWARD(m_general, ExtraFmemInMBytes, extra_fmem_in_mbytes)
    bool IsShadNetEnabled() const {
        return m_network.shad_net_enabled.get(m_configMode) &&
               !m_shadnet_session_disabled.load(std::memory_order_relaxed);
    }
    void SetShadNetEnabled(bool v, bool specific = false) {
        m_network.shad_net_enabled.set(v, specific);
    }
    bool IsShadNetEnabledSetting() const {
        return m_network.shad_net_enabled.get(m_configMode);
    }
    void SetShadNetSessionDisabled(bool v) {
        m_shadnet_session_disabled.store(v, std::memory_order_relaxed);
    }
    bool IsShadNetSessionDisabled() const {
        return m_shadnet_session_disabled.load(std::memory_order_relaxed);
    }
    SETTING_FORWARD_BOOL(m_general, TrophyPopupDisabled, trophy_popup_disabled)
    SETTING_FORWARD(m_general, TrophyNotificationDuration, trophy_notification_duration)
    SETTING_FORWARD(m_general, TrophyNotificationSide, trophy_notification_side)
    SETTING_FORWARD_BOOL(m_general, ShowSplash, show_splash)
    SETTING_FORWARD_BOOL(m_general, ShowFpsCounter, show_fps_counter)
    SETTING_FORWARD(m_general, ConsoleLanguage, console_language)
    SETTING_FORWARD(m_general, SettingsWindowScale, settings_window_scale)
    SETTING_FORWARD_BOOL(m_general, RedZonePatchingEnabled, redzone_patches)
    SETTING_FORWARD_BOOL(m_general, LegacyCpuPatchesEnabled, legacy_cpu_patches)
    SETTING_FORWARD_BOOL(m_general, NvidiaDriverNotice, nvidia_driver_notice)

    // Network settings
    SETTING_FORWARD_BOOL(m_network, ConnectedToNetwork, connected_to_network)
    SETTING_FORWARD(m_network, ShadNetServer, shadnet_server)
    std::string GetShadNetWebApiServer() const {
        // Strip trailing slashes if exists
        std::string url = m_network.shadnet_webapi_server.get(m_configMode);
        while (!url.empty() && url.back() == '/') {
            url.pop_back();
        }
        return url;
    }
    void SetShadNetWebApiServer(const std::string& v, bool is_specific = false) {
        m_network.shadnet_webapi_server.set(v, is_specific);
    }

    SETTING_FORWARD_BOOL(m_network, UPnPEnabled, enable_upnp)
    SETTING_FORWARD(m_network, P2PPort, p2p_port)
    SETTING_FORWARD_BOOL(m_network, ForcedHttpsDisabled, disable_https)
    SETTING_FORWARD_BOOL(m_network, DeviceDiscoveryEnabled, enable_device_discovery)

    // Log settings
    SETTING_FORWARD_BOOL(m_log, LogAppend, append)
    SETTING_FORWARD_BOOL(m_log, LogEnable, enable)
    SETTING_FORWARD(m_log, LogFilter, filter)
    SETTING_FORWARD(m_log, LogFlushLevel, flush_level)
    SETTING_FORWARD(m_log, LogMaxSkipDuration, max_skip_duration)
    SETTING_FORWARD_BOOL(m_log, LogSeparate, separate)
    SETTING_FORWARD(m_log, LogSizeLimit, size_limit)
    SETTING_FORWARD_BOOL(m_log, LogSkipDuplicate, skip_duplicate)
    SETTING_FORWARD_BOOL(m_log, LogSync, sync)
#ifdef _WIN32
    SETTING_FORWARD(m_log, LogType, type)
#endif

    // Audio settings
    SETTING_FORWARD(m_audio, AudioBackend, audio_backend)
    SETTING_FORWARD(m_audio, SDLMicDevice, sdl_mic_device)
    SETTING_FORWARD(m_audio, SDLMainOutputDevice, sdl_main_output_device)
    SETTING_FORWARD(m_audio, SDLPadSpkOutputDevice, sdl_padSpk_output_device)
    SETTING_FORWARD(m_audio, OpenALMicDevice, openal_mic_device)
    SETTING_FORWARD(m_audio, OpenALMainOutputDevice, openal_main_output_device)
    SETTING_FORWARD(m_audio, OpenALPadSpkOutputDevice, openal_padSpk_output_device)
    SETTING_FORWARD(m_audio, OpenALHrtf, openal_hrtf)
    SETTING_FORWARD(m_audio, OpenALOutputMode, openal_output_mode)

    // Debug settings
    SETTING_FORWARD_BOOL(m_debug, DebugDump, debug_dump)
    SETTING_FORWARD(m_debug, ConfigVersion, config_version)

    // GPU Settings
    SETTING_FORWARD_BOOL(m_gpu, VkRecordThread, vk_record_thread)
    SETTING_FORWARD_BOOL(m_gpu, ReadbackOffload, readback_offload)
    SETTING_FORWARD(m_gpu, PendingPopThrottle, pending_pop_throttle)
    SETTING_FORWARD_BOOL(m_gpu, BackingWriteMemo, backing_write_memo)
    SETTING_FORWARD_BOOL(m_gpu, StaticColorWriteMask, static_color_write_mask)
    SETTING_FORWARD_BOOL(m_gpu, DescHeapRecycle, desc_heap_recycle)
    SETTING_FORWARD_BOOL(m_gpu, FullScreen, full_screen)
    SETTING_FORWARD(m_gpu, FullScreenMode, full_screen_mode)
    SETTING_FORWARD(m_gpu, PresentMode, present_mode)
    SETTING_FORWARD(m_gpu, WindowHeight, window_height)
    SETTING_FORWARD(m_gpu, WindowWidth, window_width)
    SETTING_FORWARD(m_gpu, InternalScreenHeight, internal_screen_height)
    SETTING_FORWARD(m_gpu, InternalScreenWidth, internal_screen_width)
    SETTING_FORWARD_BOOL(m_gpu, HdrAllowed, hdr_allowed)

    u32 GetVblankFrequency() {
        if (m_gpu.vblank_frequency.value < 30) {
            return 30;
        }
        return m_gpu.vblank_frequency.get();
    }
    void SetVblankFrequency(const u32& v, bool is_specific = false) {
        u32 val = v < 30 ? 30 : v;
        if (is_specific) {
            m_gpu.vblank_frequency.game_specific_value = val;
        } else {
            m_gpu.vblank_frequency.value = val;
        }
    }

    // Input Settings
    SETTING_FORWARD(m_input, CursorState, cursor_state)
    SETTING_FORWARD(m_input, CursorHideTimeout, cursor_hide_timeout)
    SETTING_FORWARD(m_input, UsbDeviceBackend, usb_device_backend)
    SETTING_FORWARD_BOOL(m_input, MotionControlsEnabled, motion_controls_enabled)
    SETTING_FORWARD_BOOL(m_input, GyroSwapYawRoll, gyro_swap_yaw_roll)
    SETTING_FORWARD_BOOL(m_input, GyroInvertYaw, gyro_invert_yaw)
    SETTING_FORWARD_BOOL(m_input, GyroInvertX, gyro_invert_x)
    SETTING_FORWARD_BOOL(m_input, GyroInvertRoll, gyro_invert_roll)
    SETTING_FORWARD(m_input, GyroDeadzone, gyro_deadzone)
    SETTING_FORWARD_BOOL(m_input, BackgroundControllerInput, background_controller_input)
    SETTING_FORWARD_BOOL(m_input, ImeAccessibilityEnabled, ime_accessibility_enabled)
    SETTING_FORWARD_BOOL(m_input, ImeUrlMailShortPanel, ime_url_mail_short_panel)
    SETTING_FORWARD(m_input, DefaultControllerId, default_controller_id)
    SETTING_FORWARD_BOOL(m_input, UsingSpecialPad, use_special_pad)
    SETTING_FORWARD(m_input, SpecialPadClass, special_pad_class)
    SETTING_FORWARD_BOOL(m_input, UseUnifiedInputConfig, use_unified_input_config)
    SETTING_FORWARD(m_input, CameraId, camera_id)
    SETTING_FORWARD_BOOL(m_input, CircleEnter, is_circle_enter)
    SETTING_FORWARD_BOOL(m_input, MiceUsedAsMice, use_mice_as_mice)
    SETTING_FORWARD_BOOL(m_input, KeyboardUsedAsKeyboard, use_keyboard_as_keyboard)
    SETTING_FORWARD_BOOL(m_input, VibrationEnabled, vibration_enabled)

    // Vulkan settings
    SETTING_FORWARD(m_vulkan, GpuId, gpu_id)
    SETTING_FORWARD_BOOL(m_vulkan, RenderdocEnabled, renderdoc_enabled)
    SETTING_FORWARD_BOOL(m_vulkan, VkValidationEnabled, vkvalidation_enabled)
    SETTING_FORWARD_BOOL(m_vulkan, VkValidationCoreEnabled, vkvalidation_core_enabled)
    SETTING_FORWARD_BOOL(m_vulkan, VkValidationSyncEnabled, vkvalidation_sync_enabled)
    SETTING_FORWARD_BOOL(m_vulkan, VkValidationGpuEnabled, vkvalidation_gpu_enabled)
    SETTING_FORWARD_BOOL(m_vulkan, VkCrashDiagnosticEnabled, vkcrash_diagnostic_enabled)
    SETTING_FORWARD_BOOL(m_vulkan, VkHostMarkersEnabled, vkhost_markers)

#undef SETTING_FORWARD
#undef SETTING_FORWARD_BOOL
#undef SETTING_FORWARD_BOOL_READONLY
};
