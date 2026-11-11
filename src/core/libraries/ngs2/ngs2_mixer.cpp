// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "core/libraries/ngs2/ngs2_mixer.h"
#include "core/libraries/ngs2/ngs2_vag_coefs.h"

extern "C" {
#include <decoder.h>
#include <libatrac9.h>
}

namespace Libraries::Ngs2::Mixer {

namespace {

constexpr u32 MaxChannels = 8;
constexpr u32 MaxPorts = 16;
constexpr u32 MaxMatrixLevels = MaxChannels * MaxChannels;
constexpr u32 MaxFilters = 16;
constexpr u32 MaxEnvelopePoints = 8;
// Decoded frames a sampler voice holds: more than one grain at the highest pitch plus one codec
// frame.
constexpr u32 FifoFrames = 8192;
// The pitch converter steps through the decoded frames in 2.14 fixed point and weighs four of them
// for each sample it makes, with one of 512 sets of weights by the fraction of its position.
constexpr u32 PitchOne = 1u << 14;
constexpr u32 PitchTaps = 4;
constexpr u32 PitchPhases = 512;
// Samples of the fades that start, pause and end a voice.
constexpr u32 FadeSamples = 64;

constexpr s32 ErrorInvalidVoice = static_cast<s32>(0x804A0300);
constexpr s32 ErrorInvalidOutAddress = static_cast<s32>(0x804A0053);
constexpr s32 ErrorInvalidStateSize = static_cast<s32>(0x804A0054);

enum RackId : u32 {
    RackSampler = 0x1000,
    RackSubmixer = 0x2000,
    RackReverb = 0x2001,
    RackMastering = 0x3000,
    RackCustomSubmixer = 0x4002,
};

// The modules a custom rack is made of.
enum ModuleId : u32 {
    ModuleDelay = 0x1d,
};

// Voice state flags as the game reads them.
enum StateFlag : u32 {
    StateInUse = 0x1,
    StatePlaying = 0x2,
    StatePaused = 0x4,
    StateStopped = 0x8,
};

// Events kicked by the game, applied by the next render.
enum Request : u32 {
    RequestPlay = 1u << 0,
    RequestStop = 1u << 1,
    RequestStopImmediate = 1u << 2,
    RequestKill = 1u << 3,
    RequestPause = 1u << 4,
    RequestResume = 1u << 5,
};

// What an event does to the grain it is applied in.
enum class Transition : u32 {
    None,
    // Fades out over the last samples of the grain: the voice pauses after it.
    FadeOutTail,
    // Fades in over the first samples of the grain: the voice resumes.
    FadeIn,
    // The grain is silent: the voice is paused.
    Silent,
    // Fades out over the first samples of the grain and is silent after them: the voice ends.
    FadeOutHead,
};

enum ParamId : u32 {
    ParamMatrixLevels = 0x1,
    ParamPortVolume = 0x2,
    ParamPortMatrix = 0x3,
    ParamPatch = 0x5,
    ParamEvent = 0x6,
    ParamSamplerSetup = 0x10000000,
    ParamSamplerBlocks = 0x10000001,
    ParamSamplerPitch = 0x10000005,
    ParamSamplerEnvelope = 0x10000006,
    ParamSamplerFilter = 0x1000000a,
    ParamSubmixerSetup = 0x20000000,
    ParamSubmixerEnvelope = 0x20000001,
    ParamSubmixerCompressor = 0x20000002,
    ParamSubmixerUserFx = 0x20000004,
    ParamSubmixerFilter = 0x20000006,
    ParamReverbSetup = 0x20010000,
    ParamReverbI3dl2 = 0x20010001,
    ParamMasteringSetup = 0x30000000,
    ParamMasteringLfe = 0x30000002,
    ParamMasteringLimiter = 0x30000003,
    ParamMasteringGain = 0x30000004,
    ParamMasteringOutput = 0x30000005,
    ParamDelay = 0x40001d00,
};

enum WaveformType : u32 {
    WaveformPcmI8 = 0x10,
    WaveformPcmU8 = 0x11,
    WaveformPcmI16L = 0x12,
    WaveformPcmF64B = 0x1b,
    WaveformVag = 0x1c,
    WaveformAtrac9 = 0x40,
};

struct ParamHeader {
    u16 size;
    s16 next;
    u32 id;
};

struct WaveformFormat {
    u32 type;
    u32 channels;
    u32 sample_rate;
    u32 config;
    u32 frame_offset;
    u32 frame_margin;
};

struct WaveformBlock {
    u32 data_offset;
    u32 data_size;
    u32 repeats;
    u32 skip_samples;
    u32 samples;
    u32 reserved;
    u64 user_data;
};

struct EnvelopePoint {
    u32 curve;
    u32 duration;
    float height;
};

struct SamplerState {
    u32 flags;
    float envelope_height;
    float peak_height;
    u32 reserved;
    u64 decoded_samples;
    u64 decoded_size;
    u64 user_data;
    const void* waveform;
};

struct FxContext {
    float** channels;
    u64 user_data[3];
    u32 flags;
    u32 num_channels;
    u32 grain_samples;
    u32 sample_rate;
};
using FxHandler = s32 PS4_SYSV_ABI (*)(FxContext* context);

// What the effect handler of the game is told about the voice. The paused and stopped bits are
// those of the voice state.
enum FxFlag : u32 {
    FxFirst = 0x3,
    FxStoppedNow = 0x8,
    FxKilled = 0x20,
};

template <typename T>
T Read(const void* base, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const u8*>(base) + offset, sizeof(T));
    return value;
}

struct Port {
    Voice* dest{};
    u32 input{};
    float volume{1.0f};
    s32 matrix{};
    // The volume of the last render: a grain runs from it to the volume of now. A port just
    // patched has none.
    float last_volume{1.0f};
    bool fresh{true};
};

struct Matrix {
    u32 count{};
    std::array<float, MaxMatrixLevels> levels{};
    // The levels of the last render, as last_volume of a port.
    std::array<float, MaxMatrixLevels> last_levels{};
};

// A filter of a voice: two poles and two zeros, shaped by the type the game picks.
struct Filter {
    // Where in the voice it runs: 0 on its source, 1 after its effects, 2 and up on the way out
    // of a port.
    u32 location{};
    u32 type{};
    // The channels it leaves alone.
    u32 skip_channels{};
    // Type 0x20 takes the five coefficients themselves here.
    std::array<float, 5> values{};
    bool set{};
    // The values changed: the coefficients are due.
    bool changed{};
    // The voice started: the history is void.
    bool restart{};
    // What the output takes of the input, the two inputs before it and the two outputs before.
    std::array<float, 5> coefs{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    // For each channel the last input, the one before, the last output, the one before.
    std::array<std::array<float, 4>, MaxChannels> history{};
};

// The compressor of a mixing voice: above the threshold the level rises by the ratio only.
struct Compressor {
    // Bit 0 turns it off, bit 1 measures the level as a mean and not as a peak, bit 3 keeps the
    // fourth channel (the low frequency effects) apart from the others.
    u32 flags{};
    float threshold{};
    float ratio{};
    // The part of the threshold below it over which the ratio sets in.
    float knee{};
    float attack_time{};
    float release_time{};
    // The gain after it.
    float level{};
    bool set{};
    bool changed{};
    bool restart{};
    // What is left of a gain step after one sample, on the way down and on the way up.
    float attack{};
    float release{};
    std::array<float, MaxChannels> gain{};
};

// What a mastering voice does to its mix on the way to the buffer of the game.
struct Mastering {
    // The buffer of the render call it writes.
    u32 output{};
    // The low pass on the low frequency effects channel: four poles in two steps.
    bool lfe_filter{};
    u32 lfe_frequency{120};
    bool lfe_changed{};
    std::array<std::array<float, 5>, 2> lfe_coefs{};
    std::array<std::array<float, 4>, 2> lfe_history{};
    // The gain of each channel, and that of the last render: a grain runs from the one to the
    // other.
    std::array<float, MaxChannels> gain{};
    std::array<float, MaxChannels> last_gain{};
    bool restart{};
};

constexpr u32 ReverbTaps = 5;
constexpr u32 ReverbCombs = 16;
constexpr u32 ReverbStages = 4;
constexpr u32 DelayTaps = 8;

// One of the loops of the late reverb: a delay line that feeds itself, quieter and duller each
// turn.
struct ReverbComb {
    u32 offset{};
    u32 length{};
    u32 pos{};
    // What a turn of the line keeps.
    float feedback{};
    // The low pass of the loop: what it takes of the line and of its own last value.
    std::array<float, 2> damp{};
    // What a turn takes of the line itself and of its low pass.
    std::array<float, 2> blend{};
    // What the line takes of the input and of the input before it.
    std::array<float, 2> shape{};
    float damp_state{};
    float last_input{};
};

// One of the all-pass steps that smear the late reverb of a channel.
struct ReverbStage {
    u32 offset{};
    u32 length{};
    u32 pos{};
    float gain{};
};

// The reverb of a reverb voice. Its input goes as one channel through a delay line with five
// taps: four are the early reflections, the fifth feeds sixteen loops, the late reverb. Each
// output channel takes the early reflections and its own mix of the loops, smeared.
struct Reverb {
    // What the game asked for, within the bounds the reverb takes; the levels as gains.
    float wet{};
    float dry{};
    float room{};
    float room_hf{};
    u32 pattern{};
    float decay_time{};
    float decay_hf_ratio{};
    float reflections{};
    float reflections_delay{};
    float reverb{};
    float reverb_delay{};
    float diffusion{};
    float density{};
    float hf_reference{};
    // The values changed: the next render fades from the old early reflections to the new and
    // shapes the rest anew.
    bool changed{};
    // The voice started: the lines are void.
    bool restart{};

    std::vector<float> early_line;
    u32 early_pos{};
    bool early_on{};
    std::array<u32, ReverbTaps> early_delay{};
    std::array<float, ReverbTaps> early_gain{};
    // The gains of the early reflections and of the late reverb on the way out.
    float early_level{1.0f};
    float late_level{1.0f};
    // The density the loops were last sized for.
    float last_density{};
    std::array<ReverbComb, ReverbCombs> combs{};
    std::vector<float> comb_line;
    std::array<std::array<ReverbStage, ReverbStages>, MaxChannels> stages{};
    std::array<std::vector<float>, MaxChannels> stage_lines;
    // The low pass of the room on the way out.
    Filter room_filter;
    // The gains of the last render: a grain runs from them to those of now.
    std::array<float, MaxChannels> last_wet{};
    std::array<float, MaxChannels> last_dry{};
    std::vector<float> work;
};

// The delay of a delay voice. Each channel but the low frequency effects has a line of its own:
// up to eight taps of the line, low passed, mix with the input on the way out and feed the line
// again.
struct Delay {
    // What the rack option allows: the most taps, and the longest time in milliseconds.
    u32 max_taps{};
    float max_time{};
    // What the game asked for, within the bounds the delay takes.
    bool set{};
    float dry{};
    float wet{};
    float level{};
    float feedback{};
    float cutoff{};
    u32 taps{};
    std::array<float, DelayTaps> tap_level{};
    std::array<float, DelayTaps> tap_time{};
    std::array<float, MaxChannels> input_gain{};
    // The channels that pass untouched, a bit per channel.
    u32 skip_channels{};
    bool low_pass{};
    // The values changed: the next render places the taps anew.
    bool changed{};
    // The voice started: the lines are void.
    bool restart{};

    // The lines, one after the other, and where each channel writes next.
    u32 length{};
    std::vector<float> lines;
    std::array<u32, MaxChannels> pos{};
    std::array<u32, DelayTaps> tap_delay{};
    // The share of the taps that goes back into the line.
    float feed{};
    // The low pass of the taps: its pole and its last output per channel.
    float pole{};
    std::array<float, MaxChannels> low_state{};
    std::vector<float> work;
};

struct QueuedBlock {
    WaveformBlock block;
    const u8* data;
};

struct Envelope {
    std::array<EnvelopePoint, MaxEnvelopePoints> points{};
    u32 forward{};
    u32 release{};
    // The game gave the voice an envelope: its points shape the voice, and a stop ends it by
    // them.
    bool set{};
    // Playback: the point being run, the samples done of it, the height it started from.
    u32 point{};
    u32 done{};
    float from{};
    float height{1.0f};
    bool releasing{};
};

struct Sampler {
    WaveformFormat format{};
    float pitch{1.0f};
    std::vector<QueuedBlock> blocks;
    // The game marked the last block list as one that more blocks follow.
    bool more_follow{};
    u32 offset{};
    u32 repeats_done{};
    u32 skip{};
    u32 block_samples{};
    void* at9{};
    Atrac9CodecInfo at9_info{};
    u32 at9_frame{};
    std::array<std::array<float, 4>, MaxChannels> vag_history{};
    // Decoded frames, one plane of FifoFrames per channel.
    std::vector<float> fifo;
    u32 fifo_read{};
    u32 fifo_count{};
    // The fraction of the position between two frames, in 2.14 fixed point.
    u32 phase{};
    SamplerState state{};
};

} // namespace

struct Voice {
    Rack* rack{};
    u32 index{};
    u32 flags{};
    u32 requests{};
    u32 channels{2};
    std::array<Port, MaxPorts> ports{};
    std::vector<Matrix> matrices;
    Envelope envelope;
    std::array<Filter, MaxFilters> filters{};
    Compressor compressor;
    // Mixing voices: what the voices patched to this one sent in this render, a plane per channel.
    std::vector<float> input;
    u32 pending{};
    bool processed{};
    // The voice renders in this render, and how the events applied in it shape the grain.
    bool active{};
    Transition transition{};
    // The events being applied in this render.
    u32 events{};
    // The effect handler of the game and its three values.
    FxHandler fx{};
    std::array<u64, 3> fx_data{};
    bool fx_first{};
    // A mixing voice the game has controlled renders, in use or not, until an event ends it.
    bool linked{};
    Mastering mastering;
    std::unique_ptr<Sampler> sampler;
    std::unique_ptr<Reverb> reverb;
    std::unique_ptr<Delay> delay;
};

struct Rack {
    System* system{};
    u32 id{};
    u32 max_ports{};
    std::vector<Voice> voices;
};

struct System {
    std::mutex mutex;
    u32 sample_rate{};
    u32 grain{};
    std::vector<Rack*> racks;
    // The voice being rendered, a plane per channel, and its envelope gain per sample.
    std::vector<float> out;
    std::vector<float> gain;
    std::vector<float> decode;
};

namespace {

bool IsSampler(const Rack* rack) {
    return rack->id == RackSampler;
}

bool IsMastering(const Rack* rack) {
    return rack->id == RackMastering;
}

bool IsPcm(u32 type) {
    return type >= WaveformPcmI8 && type <= WaveformPcmF64B;
}

u32 PcmBytes(u32 type) {
    static constexpr std::array<u32, 12> bytes = {1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 8, 8};
    return bytes[type - WaveformPcmI8];
}

// One PCM sample of the waveform type as a float. Odd types from 0x13 up are big-endian.
float PcmSample(u32 type, const u8* data) {
    const u32 size = PcmBytes(type);
    std::array<u8, 8> raw{};
    const bool big = type >= WaveformPcmI16L && (type & 1) != 0;
    for (u32 i = 0; i < size; ++i) {
        raw[i] = big ? data[size - 1 - i] : data[i];
    }
    switch (type) {
    case WaveformPcmI8:
        return static_cast<float>(static_cast<s8>(raw[0])) * (1.0f / 128.0f);
    case WaveformPcmU8:
        return (static_cast<float>(raw[0]) - 128.0f) * (1.0f / 128.0f);
    case 0x12:
    case 0x13:
        return static_cast<float>(Read<s16>(raw.data(), 0)) * (1.0f / 32768.0f);
    case 0x14:
    case 0x15: {
        const s32 value = static_cast<s32>(raw[0] << 8 | raw[1] << 16 | raw[2] << 24) >> 8;
        return static_cast<float>(value) * (1.0f / 8388608.0f);
    }
    case 0x16:
    case 0x17:
        return static_cast<float>(Read<s32>(raw.data(), 0)) * (1.0f / 2147483648.0f);
    case 0x18:
    case 0x19:
        return Read<float>(raw.data(), 0);
    default:
        return static_cast<float>(Read<double>(raw.data(), 0));
    }
}

void ResetEnvelope(Envelope& envelope) {
    envelope.point = 0;
    envelope.done = 0;
    envelope.from = 0.0f;
    envelope.releasing = false;
    // Without points the voice plays at full height.
    envelope.height = envelope.forward == 0 ? 1.0f : 0.0f;
}

void ResetDecode(Sampler& sampler) {
    sampler.blocks.clear();
    sampler.more_follow = false;
    sampler.offset = 0;
    sampler.repeats_done = 0;
    sampler.skip = 0;
    sampler.block_samples = 0;
    sampler.at9_frame = 0;
    // The pitch converter starts with silence behind the first frame.
    if (!sampler.fifo.empty()) {
        for (size_t at = 0; at < sampler.fifo.size(); at += FifoFrames) {
            std::fill_n(sampler.fifo.begin() + at, PitchTaps, 0.0f);
        }
    }
    sampler.fifo_read = 0;
    sampler.fifo_count = PitchTaps;
    sampler.phase = 0;
    sampler.state = {};
}

// The voice is free again: nothing of its last use is left but its routing.
void EndVoice(Voice& voice) {
    voice.flags = 0;
    voice.requests = 0;
    if (voice.sampler) {
        ResetDecode(*voice.sampler);
    }
}

void StartBlock(Sampler& sampler) {
    const WaveformBlock& block = sampler.blocks.front().block;
    sampler.offset = 0;
    sampler.skip = block.skip_samples;
    sampler.block_samples = 0;
    sampler.at9_frame = 0;
    sampler.vag_history = {};
    // A block starts on a superframe, and so must the decoder.
    if (sampler.at9) {
        static_cast<Atrac9Handle*>(sampler.at9)->Frame.IndexInSuperframe = 0;
    }
    sampler.state.user_data = block.user_data;
}

void SetupSampler(Voice& voice, const void* param) {
    // A setup leaves nothing of the last use: no patches, no envelope, no filters.
    voice.ports = {};
    voice.envelope = {};
    voice.filters = {};
    if (!voice.sampler) {
        voice.sampler = std::make_unique<Sampler>();
    }
    Sampler& sampler = *voice.sampler;
    sampler.format = Read<WaveformFormat>(param, 8);
    sampler.pitch = 1.0f;
    voice.channels = std::clamp<u32>(sampler.format.channels, 1, MaxChannels);
    sampler.fifo.assign(static_cast<size_t>(voice.channels) * FifoFrames, 0.0f);
    ResetDecode(sampler);
    sampler.at9_info = {};
    if (sampler.format.type == WaveformAtrac9) {
        // The configuration word holds the four configuration bytes, first byte highest.
        std::array<u8, 4> config = {
            static_cast<u8>(sampler.format.config >> 24),
            static_cast<u8>(sampler.format.config >> 16),
            static_cast<u8>(sampler.format.config >> 8),
            static_cast<u8>(sampler.format.config),
        };
        if (sampler.at9) {
            Atrac9ReleaseHandle(sampler.at9);
        }
        sampler.at9 = Atrac9GetHandle();
        if (Atrac9InitDecoder(sampler.at9, config.data()) == 0) {
            Atrac9GetCodecInfo(sampler.at9, &sampler.at9_info);
        }
    }
}

void AddBlocks(Voice& voice, const void* param) {
    if (!voice.sampler) {
        return;
    }
    Sampler& sampler = *voice.sampler;
    const u8* data = Read<const u8*>(param, 8);
    const u32 flags = Read<u32>(param, 0x10);
    const u32 count = Read<u32>(param, 0x14);
    const auto* blocks = Read<const WaveformBlock*>(param, 0x18);
    sampler.more_follow = (flags & 1) != 0;
    const bool idle = sampler.blocks.empty();
    for (u32 i = 0; blocks && i < count; ++i) {
        sampler.blocks.push_back({blocks[i], data});
    }
    if (idle && !sampler.blocks.empty()) {
        StartBlock(sampler);
    }
}

void SetEnvelope(Envelope& envelope, const void* param) {
    const u32 forward = Read<u32>(param, 8);
    const u32 release = Read<u32>(param, 0xc);
    const auto* points = Read<const EnvelopePoint*>(param, 0x10);
    if (!points || forward + release > MaxEnvelopePoints) {
        envelope.forward = 0;
        envelope.release = 0;
    } else {
        envelope.forward = forward;
        envelope.release = release;
        std::copy_n(points, forward + release, envelope.points.begin());
        for (u32 i = 0; i < forward + release; ++i) {
            envelope.points[i].height = std::clamp(envelope.points[i].height, 0.0f, 1.0f);
        }
    }
    envelope.set = true;
    ResetEnvelope(envelope);
}

void SetFilter(Voice& voice, const void* param) {
    const u32 index = Read<u32>(param, 8);
    if (index >= MaxFilters) {
        return;
    }
    Filter& filter = voice.filters[index];
    filter.location = Read<u32>(param, 0xc);
    filter.type = Read<u32>(param, 0x10);
    filter.skip_channels = Read<u32>(param, 0x14);
    filter.values = Read<std::array<float, 5>>(param, 0x18);
    filter.set = true;
    filter.changed = true;
}

// A level in hundredths of a decibel as a gain.
float MillibelGain(float millibel) {
    if (millibel < -10000.0f) {
        return 0.0f;
    }
    return static_cast<float>(std::pow(10.0, static_cast<double>(millibel * 0.00049999997f)));
}

// The reverb values of the game: wet and dry gains, then the values of an I3DL2 reverb.
void SetReverb(Reverb& reverb, const void* param) {
    reverb.wet = std::clamp(Read<float>(param, 0x8), 0.0f, 1.0f);
    reverb.dry = std::clamp(Read<float>(param, 0xc), 0.0f, 1.0f);
    reverb.room = std::min(MillibelGain(static_cast<float>(Read<s32>(param, 0x10))), 1.0f);
    reverb.room_hf =
        MillibelGain(static_cast<float>(std::clamp(Read<s32>(param, 0x14), -10000, 0)));
    const u32 pattern = Read<u32>(param, 0x18);
    reverb.pattern = pattern < 16 ? pattern : 0;
    reverb.decay_time = std::clamp(Read<float>(param, 0x1c), 0.1f, 20.0f);
    reverb.decay_hf_ratio = std::clamp(Read<float>(param, 0x20), 0.01f, 2.0f);
    reverb.reflections =
        MillibelGain(static_cast<float>(std::clamp(Read<s32>(param, 0x24), -10000, 1000)));
    reverb.reflections_delay = std::clamp(Read<float>(param, 0x28), 0.0f, 0.3f);
    // The late reverb sits 24 dB below the level asked for.
    reverb.reverb =
        MillibelGain(static_cast<float>(std::clamp(Read<s32>(param, 0x2c), -10000, 2000) - 2400));
    reverb.reverb_delay = std::clamp(Read<float>(param, 0x30), 0.0f, 0.1f);
    reverb.diffusion = std::clamp(Read<float>(param, 0x34), 0.0f, 100.0f);
    reverb.density = std::clamp(Read<float>(param, 0x38), 0.0f, 100.0f);
    reverb.hf_reference = std::clamp(Read<float>(param, 0x3c), 20.0f, 20000.0f);
    reverb.changed = true;
}

// The delay values of the game: the gains of the input and of the taps on the way out, a gain
// over both, the share fed back, the corner of the low pass, the taps as level and time in
// milliseconds, a gain per channel into the lines, the channels to pass, flags. More taps than
// the rack allows change nothing.
void SetDelay(Delay& delay, const void* param) {
    const u32 taps = Read<u32>(param, 0x1c);
    if (taps > delay.max_taps) {
        return;
    }
    delay.dry = std::clamp(Read<float>(param, 0x8), 0.0f, 1.0f);
    delay.wet = std::clamp(Read<float>(param, 0xc), -1.0f, 1.0f);
    delay.level = std::clamp(Read<float>(param, 0x10), 0.0f, 1.0f);
    delay.feedback = std::clamp(Read<float>(param, 0x14), -1.0f, 1.0f);
    delay.cutoff = std::clamp(Read<float>(param, 0x18), 0.0f, 24000.0f);
    delay.taps = taps == 0 ? 1 : std::min(taps, DelayTaps);
    delay.tap_level = {};
    delay.tap_time = {};
    for (u32 tap = 0; tap < delay.taps; ++tap) {
        delay.tap_level[tap] = std::clamp(Read<float>(param, 0x20 + 8 * tap), -1.0f, 1.0f);
        delay.tap_time[tap] = std::clamp(Read<float>(param, 0x24 + 8 * tap), 0.0f, delay.max_time);
    }
    delay.skip_channels = Read<u32>(param, 0x80);
    const u32 flags = Read<u32>(param, 0x84);
    delay.low_pass = (flags & 1) != 0;
    // Without the flag for them the channels go into their lines as they are.
    for (u32 ch = 0; ch < MaxChannels; ++ch) {
        delay.input_gain[ch] = (flags & 2) != 0 ? Read<float>(param, 0x60 + 4 * ch) : 1.0f;
    }
    delay.set = true;
    delay.changed = true;
}

void KickEvent(Voice& voice, u32 event) {
    if (event == 0) {
        voice.flags = StateInUse;
        voice.requests = RequestPlay;
        ResetEnvelope(voice.envelope);
        for (Filter& filter : voice.filters) {
            filter.changed = true;
            filter.restart = true;
        }
        voice.compressor.changed = true;
        voice.compressor.restart = true;
        voice.mastering.lfe_changed = true;
        voice.mastering.restart = true;
        if (voice.reverb) {
            voice.reverb->changed = true;
            voice.reverb->restart = true;
        }
        if (voice.delay) {
            voice.delay->changed = true;
            voice.delay->restart = true;
        }
        return;
    }
    if ((voice.flags & StateInUse) == 0) {
        return;
    }
    switch (event) {
    case 1:
        voice.requests |= RequestStop;
        break;
    case 2:
        voice.requests |= RequestStopImmediate;
        break;
    case 3:
        voice.requests |= RequestKill;
        break;
    case 4:
        voice.requests = (voice.requests & ~RequestResume) | RequestPause;
        break;
    case 5:
        voice.requests = (voice.requests & ~RequestPause) | RequestResume;
        break;
    default:
        break;
    }
}

void ControlOne(Voice& voice, const void* param, Resolver resolve) {
    const u32 id = Read<ParamHeader>(param, 0).id;
    switch (id) {
    case ParamMatrixLevels: {
        const u32 matrix = Read<u32>(param, 8);
        const u32 count = std::min(Read<u32>(param, 0xc), MaxMatrixLevels);
        const float* levels = Read<const float*>(param, 0x10);
        if (matrix < voice.matrices.size() && levels) {
            voice.matrices[matrix].count = count;
            std::copy_n(levels, count, voice.matrices[matrix].levels.begin());
        }
        break;
    }
    case ParamPortVolume:
        if (const u32 port = Read<u32>(param, 8); port < voice.rack->max_ports) {
            voice.ports[port].volume = Read<float>(param, 0xc);
        }
        break;
    case ParamPortMatrix:
        if (const u32 port = Read<u32>(param, 8); port < voice.rack->max_ports) {
            voice.ports[port].matrix = Read<s32>(param, 0xc);
        }
        break;
    case ParamPatch:
        if (const u32 port = Read<u32>(param, 8); port < voice.rack->max_ports) {
            voice.ports[port].input = Read<u32>(param, 0xc);
            voice.ports[port].dest = resolve(Read<u64>(param, 0x10));
            voice.ports[port].fresh = true;
        }
        break;
    case ParamEvent:
        KickEvent(voice, Read<u32>(param, 8));
        break;
    case ParamSamplerSetup:
        if (IsSampler(voice.rack)) {
            SetupSampler(voice, param);
        }
        break;
    case ParamSamplerBlocks:
        AddBlocks(voice, param);
        break;
    case ParamSamplerPitch:
        if (voice.sampler) {
            voice.sampler->pitch = Read<float>(param, 8);
        }
        break;
    case ParamSamplerEnvelope:
    case ParamSubmixerEnvelope:
        SetEnvelope(voice.envelope, param);
        break;
    case ParamSamplerFilter:
    case ParamSubmixerFilter:
        SetFilter(voice, param);
        break;
    case ParamSubmixerCompressor: {
        Compressor& compressor = voice.compressor;
        compressor.flags = Read<u32>(param, 8);
        // A value that is not a number counts as zero.
        const auto limit = [](float value, float most) {
            return value >= 0.0f ? std::min(value, most) : 0.0f;
        };
        compressor.threshold = limit(Read<float>(param, 0xc), 1.0f);
        compressor.ratio = limit(Read<float>(param, 0x10), 1.0f);
        compressor.knee = limit(Read<float>(param, 0x14), 1.0f);
        compressor.attack_time = limit(Read<float>(param, 0x18), 1.0f);
        compressor.release_time = limit(Read<float>(param, 0x1c), 10.0f);
        compressor.level = limit(Read<float>(param, 0x20), 32.0f);
        compressor.set = true;
        compressor.changed = true;
        break;
    }
    case ParamSubmixerSetup:
    case ParamReverbSetup:
        voice.channels = std::clamp<u32>(Read<u32>(param, 8), 1, MaxChannels);
        voice.ports = {};
        break;
    case ParamReverbI3dl2:
        if (voice.reverb) {
            SetReverb(*voice.reverb, param);
        }
        break;
    case ParamMasteringSetup: {
        voice.channels = std::clamp<u32>(Read<u32>(param, 8), 1, MaxChannels);
        // A mastering voice starts with its low pass at 120 Hz, a limiter at full scale and no
        // gain.
        Mastering& mastering = voice.mastering;
        mastering.lfe_filter = true;
        mastering.lfe_frequency = 120;
        mastering.lfe_changed = true;
        mastering.gain.fill(1.0f);
        Compressor& limiter = voice.compressor;
        limiter = {};
        limiter.threshold = 1.0f;
        limiter.attack_time = 0.001f;
        limiter.release_time = 0.01f;
        limiter.level = 1.0f;
        limiter.set = true;
        limiter.changed = true;
        limiter.restart = true;
        break;
    }
    case ParamMasteringLfe:
        voice.mastering.lfe_filter = Read<u32>(param, 8) != 0;
        voice.mastering.lfe_frequency = Read<u32>(param, 0xc);
        voice.mastering.lfe_changed = true;
        break;
    case ParamMasteringLimiter: {
        const float threshold = Read<float>(param, 0xc);
        voice.compressor.flags = Read<u32>(param, 8) != 0 ? 0 : 1;
        voice.compressor.threshold = threshold >= 0.0f ? std::min(threshold, 4.0f) : 0.0f;
        voice.compressor.set = true;
        voice.compressor.changed = true;
        break;
    }
    case ParamMasteringGain: {
        // One level for the low frequency effects channel, one for the others.
        const auto limit = [](float value) { return value >= 0.0f ? std::min(value, 4.0f) : 0.0f; };
        voice.mastering.gain.fill(limit(Read<float>(param, 8)));
        voice.mastering.gain[3] = limit(Read<float>(param, 0xc));
        break;
    }
    case ParamSubmixerUserFx:
        voice.fx = Read<FxHandler>(param, 8);
        voice.fx_data = {Read<u64>(param, 0x10), Read<u64>(param, 0x18), Read<u64>(param, 0x20)};
        break;
    case ParamMasteringOutput:
        voice.mastering.output = Read<u32>(param, 8);
        break;
    case ParamDelay:
        if (voice.delay) {
            SetDelay(*voice.delay, param);
        }
        break;
    default:
        // A custom rack names its setup by its own id; like the others it starts with the channels.
        if ((id & 0xffff) == 0 && id >= 0x40000000) {
            voice.channels = std::clamp<u32>(Read<u32>(param, 8), 1, MaxChannels);
            voice.ports = {};
        }
        break;
    }
}

// Moves the frames of the FIFO to its start.
void CompactFifo(Sampler& sampler, u32 channels) {
    for (u32 ch = 0; ch < channels; ++ch) {
        float* plane = &sampler.fifo[static_cast<size_t>(ch) * FifoFrames];
        std::memmove(plane, plane + sampler.fifo_read, sampler.fifo_count * sizeof(float));
    }
    sampler.fifo_read = 0;
}

// Appends decoded frames (a plane per channel in system.decode) to the FIFO of the voice, less
// the samples the block skips at its start and those past its sample count.
void PushFrames(System& system, Voice& voice, u32 count) {
    Sampler& sampler = *voice.sampler;
    const WaveformBlock& block = sampler.blocks.front().block;
    u32 first = std::min(sampler.skip, count);
    sampler.skip -= first;
    u32 keep = count - first;
    if (block.samples != 0) {
        keep = std::min(keep, block.samples - std::min(block.samples, sampler.block_samples));
    }
    sampler.block_samples += keep;
    sampler.state.decoded_samples += count;
    if (keep == 0) {
        return;
    }
    if (sampler.fifo_read + sampler.fifo_count + keep > FifoFrames) {
        CompactFifo(sampler, voice.channels);
    }
    keep = std::min(keep, FifoFrames - sampler.fifo_count);
    for (u32 ch = 0; ch < voice.channels; ++ch) {
        float* plane = &sampler.fifo[static_cast<size_t>(ch) * FifoFrames];
        std::memcpy(plane + sampler.fifo_read + sampler.fifo_count,
                    &system.decode[static_cast<size_t>(ch) * 256 + first], keep * sizeof(float));
    }
    sampler.fifo_count += keep;
}

// The block in front has given all it has: its next repeat, the next block, or the end of the
// data. Returns false when no data is left.
bool NextBlock(Sampler& sampler) {
    const WaveformBlock& block = sampler.blocks.front().block;
    if (block.repeats == 0xffffffff || sampler.repeats_done < block.repeats) {
        ++sampler.repeats_done;
        StartBlock(sampler);
        return true;
    }
    sampler.repeats_done = 0;
    sampler.blocks.erase(sampler.blocks.begin());
    if (sampler.blocks.empty()) {
        return false;
    }
    StartBlock(sampler);
    return true;
}

// Decodes the next codec frame of the voice into its FIFO. Returns false when no data is left.
bool DecodeSome(System& system, Voice& voice) {
    Sampler& sampler = *voice.sampler;
    if (sampler.blocks.empty()) {
        return false;
    }
    const QueuedBlock& queued = sampler.blocks.front();
    const WaveformBlock& block = queued.block;
    const u32 left = block.data_size > sampler.offset ? block.data_size - sampler.offset : 0;
    if (left == 0 || !queued.data ||
        (block.samples != 0 && sampler.block_samples >= block.samples)) {
        return NextBlock(sampler);
    }
    const u8* src = queued.data + block.data_offset + sampler.offset;
    const u32 channels = voice.channels;
    float* decode = system.decode.data();
    sampler.state.waveform = src;
    if (sampler.format.type == WaveformAtrac9) {
        const Atrac9CodecInfo& info = sampler.at9_info;
        int used = 0;
        if (info.frameSamples <= 0 || info.frameSamples > 256 ||
            static_cast<u32>(info.channels) != channels ||
            Atrac9DecodeF32(sampler.at9, src, static_cast<int>(left), decode, &used, 1) != 0 ||
            used <= 0) {
            // Data the decoder rejects: the block gives nothing more.
            sampler.offset = block.data_size;
            return true;
        }
        // The decoder scales by 32767, the samples of the other types by 32768.
        const u32 total = channels * static_cast<u32>(info.frameSamples);
        for (u32 i = 0; i < total; ++i) {
            decode[i] *= 32767.0f / 32768.0f;
        }
        if (static_cast<u32>(info.frameSamples) != 256) {
            // The planes of the decoder are a frame apart, those of PushFrames 256 samples.
            for (u32 ch = channels; ch-- > 1;) {
                std::memmove(decode + ch * 256, decode + ch * info.frameSamples,
                             info.frameSamples * sizeof(float));
            }
        }
        sampler.offset += static_cast<u32>(used);
        if (++sampler.at9_frame == static_cast<u32>(info.framesInSuperframe)) {
            // The rest of a superframe is padding.
            const u32 size = static_cast<u32>(info.superframeSize);
            sampler.offset = (sampler.offset + size - 1) / size * size;
            sampler.at9_frame = 0;
            sampler.state.decoded_size += size;
        }
        PushFrames(system, voice, static_cast<u32>(info.frameSamples));
        if (sampler.at9_frame != 0 && block.samples != 0 &&
            sampler.block_samples >= block.samples) {
            // The block ends inside a superframe, which counts as decoded whole.
            sampler.state.decoded_size += static_cast<u32>(info.superframeSize);
            sampler.at9_frame = 0;
        }
    } else if (sampler.format.type == WaveformVag) {
        // Frames of 16 bytes and 28 samples, one per channel in turn. The predictor index is
        // the high nibble of byte 0 with the high nibble of byte 1 above it; flag 7 in the low
        // nibble of byte 1 is a silent frame.
        if (left < 16 * channels) {
            sampler.offset = block.data_size;
            return true;
        }
        for (u32 ch = 0; ch < channels; ++ch) {
            const u8* frame = src + 16 * ch;
            const u32 shift = frame[0] & 15;
            const u32 flag = frame[1] & 15;
            const u32 index = (frame[0] >> 4) | (frame[1] & 0xf0);
            const s16* coefs = VagCoefs[index < 128 ? index : 0];
            auto& history = sampler.vag_history[ch];
            for (u32 i = 0; i < 28; ++i) {
                float sample = 0.0f;
                if (flag < 7) {
                    const u32 nibble = (frame[2 + i / 2] >> ((i & 1) * 4)) & 15;
                    const s32 code = static_cast<s16>(nibble << 12) >> shift;
                    sample = static_cast<float>(code) * (1.0f / 32768.0f);
                    for (u32 tap = 0; tap < 4; ++tap) {
                        sample += history[tap] * (static_cast<float>(coefs[tap]) / 8192.0f);
                    }
                }
                history = {sample, history[0], history[1], history[2]};
                decode[ch * 256 + i] = sample;
            }
        }
        sampler.offset += 16 * channels;
        sampler.state.decoded_size += 16 * channels;
        PushFrames(system, voice, 28);
    } else if (IsPcm(sampler.format.type)) {
        const u32 bytes = PcmBytes(sampler.format.type);
        const u32 frames = std::min<u32>(left / (bytes * channels), 256);
        if (frames == 0) {
            sampler.offset = block.data_size;
            return true;
        }
        for (u32 i = 0; i < frames; ++i) {
            for (u32 ch = 0; ch < channels; ++ch) {
                decode[ch * 256 + i] =
                    PcmSample(sampler.format.type, src + (i * channels + ch) * bytes);
            }
        }
        sampler.offset += frames * bytes * channels;
        sampler.state.decoded_size += frames * bytes * channels;
        PushFrames(system, voice, frames);
    } else {
        // A type the mixer does not decode: silence for as long as the block says.
        sampler.offset = block.data_size;
    }
    return true;
}

// The coefficients of a filter from its type, frequency, Q and level.
void ShapeFilter(Filter& filter, u32 sample_rate) {
    const u32 type = filter.type;
    if (type == 0x20) {
        filter.coefs = filter.values;
        return;
    }
    if (type < 1 || type > 10) {
        filter.coefs = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        return;
    }
    const float turn = filter.values[0] / static_cast<float>(sample_rate);
    const float w = turn < 0.001f ? 0.00628318591f : std::min(turn, 0.499f) * 6.28318548f;
    const float q = filter.values[1] < 1e-4f ? 1e-4f : std::min(filter.values[1], 32.0f);
    const float sine = std::sin(w);
    const float cosine = std::cos(w);
    // b: what the output takes of the inputs. a: what it gives back of the outputs, a[0] the
    // scale of the lot.
    std::array<float, 3> b{};
    std::array<float, 3> a{};
    if (type <= 5) {
        const float alpha = sine / (q + q);
        a = {1.0f + alpha, -2.0f * cosine, 1.0f - alpha};
        switch (type) {
        case 1: // Low pass.
            b = {(1.0f - cosine) * 0.5f, 1.0f - cosine, (1.0f - cosine) * 0.5f};
            break;
        case 2: // High pass.
            b = {(1.0f + cosine) * 0.5f, -(1.0f + cosine), (1.0f + cosine) * 0.5f};
            break;
        case 3: // Band pass, the skirts at the level of the input.
            b = {sine * 0.5f, 0.0f, -sine * 0.5f};
            break;
        case 4: // Band pass, the peak at the level of the input.
            b = {alpha, 0.0f, -alpha};
            break;
        default: // Notch.
            b = {1.0f, -2.0f * cosine, 1.0f};
            break;
        }
    } else if (type <= 8) {
        // The level is the gain of the shelf or the peak.
        const float gain = std::sqrt(std::max(filter.values[2], 1e-4f));
        if (type == 8) {
            const float alpha = sine / (q + q);
            b = {1.0f + alpha * gain, -2.0f * cosine, 1.0f - alpha * gain};
            a = {1.0f + alpha / gain, -2.0f * cosine, 1.0f - alpha / gain};
        } else {
            const float slope = std::sqrt(gain) / q * sine;
            const float more = gain + 1.0f;
            const float less = gain - 1.0f;
            if (type == 6) { // Low shelf.
                b = {gain * (more - less * cosine + slope), 2.0f * gain * (less - more * cosine),
                     gain * (more - less * cosine - slope)};
                a = {more + less * cosine + slope, -2.0f * (less + more * cosine),
                     more + less * cosine - slope};
            } else { // High shelf.
                b = {gain * (more + less * cosine + slope), -2.0f * gain * (less + more * cosine),
                     gain * (more + less * cosine - slope)};
                a = {more - less * cosine + slope, 2.0f * (less - more * cosine),
                     more - less * cosine - slope};
            }
        }
    } else if (type == 9) { // Low pass of one pole.
        b = {sine, sine, 0.0f};
        a = {sine + cosine + 1.0f, sine - cosine - 1.0f, 0.0f};
    } else { // High pass of one pole.
        b = {cosine + 1.0f, -(cosine + 1.0f), 0.0f};
        a = {sine + cosine + 1.0f, sine - cosine - 1.0f, 0.0f};
    }
    const float scale = 1.0f / a[0];
    filter.coefs = {b[0] * scale, b[1] * scale, b[2] * scale, -a[1] * scale, -a[2] * scale};
}

// Runs a filter over the channels of the grain in planes.
void RunFilter(Filter& filter, float* planes, u32 channels, u32 grain) {
    const bool restart = filter.restart;
    filter.restart = false;
    for (u32 ch = 0; ch < channels; ++ch) {
        auto& history = filter.history[ch];
        if (filter.skip_channels & (1u << ch)) {
            history = {};
            continue;
        }
        if (restart) {
            history = {};
        }
        float* samples = planes + static_cast<size_t>(ch) * grain;
        if (filter.type == 0) {
            // Off, yet in step with the signal for the day it is turned on.
            const float last = samples[grain - 1];
            const float before = grain > 1 ? samples[grain - 2] : history[0];
            history = {last, before, last, before};
            continue;
        }
        const auto& c = filter.coefs;
        float x1 = history[0];
        float x2 = history[1];
        float y1 = history[2];
        float y2 = history[3];
        for (u32 i = 0; i < grain; ++i) {
            const float x = samples[i];
            const float y = c[0] * x + c[1] * x1 + c[2] * x2 + c[3] * y1 + c[4] * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            samples[i] = y;
        }
        history = {x1, x2, y1, y2};
    }
}

// Runs the filters of the voice that sit at this location over the grain in planes.
void RunFilters(System& system, Voice& voice, u32 location, float* planes) {
    for (Filter& filter : voice.filters) {
        if (!filter.set) {
            continue;
        }
        if (filter.changed) {
            ShapeFilter(filter, system.sample_rate);
            filter.changed = false;
        }
        if (filter.location != location) {
            continue;
        }
        RunFilter(filter, planes, voice.channels, system.grain);
    }
}

// Runs the compressor of the voice over the channels of the grain in system.out.
void RunCompressor(System& system, Voice& voice, u32 channels) {
    Compressor& compressor = voice.compressor;
    if (!compressor.set) {
        return;
    }
    if (compressor.restart) {
        compressor.gain.fill(1.0f);
        compressor.restart = false;
    }
    if (compressor.changed) {
        const auto left_after_sample = [&](float seconds) {
            const u32 samples = static_cast<u32>(static_cast<float>(system.sample_rate) * seconds);
            return samples != 0 ? std::exp(-1.0f / static_cast<float>(samples)) : 0.0f;
        };
        compressor.attack = left_after_sample(compressor.attack_time);
        compressor.release = left_after_sample(compressor.release_time);
        compressor.changed = false;
    }
    if (compressor.flags & 1) {
        compressor.gain.fill(1.0f);
        return;
    }
    const u32 grain = system.grain;
    const float threshold = compressor.threshold;
    const float knee_start = threshold * (1.0f - compressor.knee * 0.5f);
    // The level is taken, and the gain aimed, a stretch of 32 samples at a time.
    for (u32 at = 0; at < grain; at += 32) {
        const u32 count = std::min<u32>(32, grain - at);
        std::array<float, MaxChannels> levels{};
        for (u32 ch = 0; ch < channels; ++ch) {
            const float* samples = &system.out[static_cast<size_t>(ch) * grain + at];
            float level = 0.0f;
            if (compressor.flags & 2) {
                for (u32 i = 0; i < count; ++i) {
                    level += samples[i] * samples[i];
                }
                level = std::sqrt(level / static_cast<float>(count));
            } else {
                for (u32 i = 0; i < count; ++i) {
                    level = std::max(level, std::abs(samples[i]));
                }
            }
            levels[ch] = level;
        }
        // The channels move as one, by the loudest of them.
        const bool apart = (compressor.flags & 8) != 0;
        float loudest = 0.0f;
        for (u32 ch = 0; ch < channels; ++ch) {
            if (!apart || ch != 3) {
                loudest = std::max(loudest, levels[ch]);
            }
        }
        for (u32 ch = 0; ch < channels; ++ch) {
            if (!apart || ch != 3) {
                levels[ch] = loudest;
            }
        }
        for (u32 ch = 0; ch < channels; ++ch) {
            const float level = levels[ch];
            float aim = 1.0f;
            const float into_knee = level - knee_start;
            if (compressor.knee > 0.0f && into_knee > 0.0f && into_knee < threshold - knee_start) {
                const float part = into_knee / (threshold - knee_start);
                aim = (knee_start + into_knee * (part * 0.5f * (compressor.ratio - 1.0f) + 1.0f)) /
                      level;
            } else if (threshold < level) {
                aim = (threshold + (level - threshold) * compressor.ratio) / level;
            }
            float gain = compressor.gain[ch];
            const float left = gain <= aim ? compressor.release : compressor.attack;
            float* samples = &system.out[static_cast<size_t>(ch) * grain + at];
            for (u32 i = 0; i < count; ++i) {
                gain = aim + left * (gain - aim);
                samples[i] *= compressor.level * gain;
            }
            // A gain that steps no further has arrived.
            if (std::abs(aim - gain) < 0.01f && gain == aim + left * (gain - aim)) {
                gain = aim;
            }
            compressor.gain[ch] = gain;
        }
    }
}

// The shapes an envelope point takes from the height before it to its own, as the part of the
// way done at each of 256 steps: 1 exponential, 2 logarithmic, 3 an S, 4 an S turned over, 5 a
// quarter of a sine going up, 6 the same going down. Curve 0 is a line and has no table.
constexpr u32 CurveSteps = 256;
using CurveTable = std::array<float, CurveSteps + 2>;

const std::array<CurveTable, 7>& EnvelopeCurves() {
    static const auto curves = [] {
        std::array<CurveTable, 7> tables{};
        const double pi = 3.14159265358979323846;
        for (u32 i = 0; i <= CurveSteps; ++i) {
            const double x = static_cast<double>(i) / CurveSteps;
            tables[1][i] = static_cast<float>((std::pow(10.0, x) - 1.0) / 9.0);
            tables[2][i] = static_cast<float>(std::log10(1.0 + 9.0 * x));
            tables[3][i] = static_cast<float>((1.0 - std::cos(pi * x)) / 2.0);
            tables[4][i] = static_cast<float>(std::acos(1.0 - 2.0 * x) / pi);
            tables[5][i] = static_cast<float>(std::sin(pi * x / 2.0));
            tables[6][i] = static_cast<float>(1.0 - std::cos(pi * x / 2.0));
        }
        return tables;
    }();
    return curves;
}

// The envelope gain of each sample of the grain into system.gain. Returns false once the
// envelope has ended the voice: a release that ran out, or forward points that end at zero.
bool RunEnvelope(System& system, Voice& voice) {
    Envelope& envelope = voice.envelope;
    float* gain = system.gain.data();
    const u32 grain = system.grain;
    if ((voice.flags & StateStopped) && !envelope.releasing) {
        envelope.releasing = true;
        envelope.point = envelope.forward;
        envelope.done = 0;
        envelope.from = envelope.height;
    }
    const u32 end = envelope.releasing ? envelope.forward + envelope.release : envelope.forward;
    float height = envelope.height;
    bool alive = true;
    u32 at = 0;
    while (at < grain) {
        if (envelope.point >= end) {
            alive = !envelope.releasing && height != 0.0f;
            if (envelope.releasing) {
                height = 0.0f;
            }
            std::fill(gain + at, gain + grain, height);
            break;
        }
        const EnvelopePoint& point = envelope.points[envelope.point];
        const u32 count = envelope.done < point.duration
                              ? std::min(grain - at, point.duration - envelope.done)
                              : 0;
        const float rise = point.height - envelope.from;
        float next = height;
        if (point.curve == 0) {
            const float slope = rise / static_cast<float>(point.duration);
            for (u32 i = 0; i < count; ++i) {
                gain[at + i] = height + slope * static_cast<float>(i);
            }
            next = height + slope * static_cast<float>(count);
        } else if (rise != 0.0f) {
            const u32 shape = point.curve == 5 && rise < 0.0f ? 6 : std::min<u32>(point.curve, 5);
            const CurveTable& table = EnvelopeCurves()[shape];
            const float step = static_cast<float>(CurveSteps) / static_cast<float>(point.duration);
            float x = step * static_cast<float>(envelope.done);
            for (u32 i = 0; i < count; ++i) {
                const u32 index = static_cast<u32>(x);
                const float part = x - static_cast<float>(index);
                next = envelope.from +
                       rise * (table[index] + (table[index + 1] - table[index]) * part);
                gain[at + i] = next;
                x += step;
            }
        } else {
            std::fill_n(gain + at, count, height);
        }
        envelope.done += count;
        height = next;
        if (envelope.done >= point.duration) {
            height = point.height;
            envelope.from = height;
            envelope.done = 0;
            ++envelope.point;
        }
        at += count;
    }
    envelope.height = height;
    return alive;
}

// The weights of the pitch converter: a sinc under a Hann window four frames wide, each set
// scaled to add up to one.
const std::array<std::array<float, PitchTaps>, PitchPhases>& PitchWeights() {
    static const auto weights = [] {
        std::array<std::array<float, PitchTaps>, PitchPhases> table{};
        const double pi = 3.14159265358979323846;
        for (u32 phase = 0; phase < PitchPhases; ++phase) {
            const double t = static_cast<double>(phase) / PitchPhases;
            const std::array<double, PitchTaps> distance = {1.0 + t, t, 1.0 - t, 2.0 - t};
            std::array<double, PitchTaps> raw{};
            double sum = 0.0;
            for (u32 tap = 0; tap < PitchTaps; ++tap) {
                const double x = distance[tap];
                const double sinc = x == 0.0 ? 1.0 : std::sin(pi * x) / (pi * x);
                raw[tap] = sinc * 0.5 * (1.0 + std::cos(pi * x / 2.0));
                sum += raw[tap];
            }
            for (u32 tap = 0; tap < PitchTaps; ++tap) {
                table[phase][tap] = static_cast<float>(raw[tap] / sum);
            }
        }
        return table;
    }();
    return weights;
}

// One grain of the sampler voice into system.out at the rate of the system. Returns false when
// its data has run out before the grain was full.
bool RenderSampler(System& system, Voice& voice) {
    Sampler& sampler = *voice.sampler;
    const u32 grain = system.grain;
    const u32 rate =
        sampler.format.sample_rate != 0 ? sampler.format.sample_rate : system.sample_rate;
    // Frames stepped for each sample made, in 2.14 fixed point.
    const float ratio = sampler.pitch * 16384.0f * static_cast<float>(rate) /
                        static_cast<float>(system.sample_rate);
    const u32 step = ratio > 0.0f ? static_cast<u32>(std::min(ratio, 65535.0f)) : 0;
    // The FIFO starts with the four frames before the position: the grain reads those and the
    // frames it steps over.
    const u32 advance = (step * grain + sampler.phase) >> 14;
    bool data_left = true;
    while (sampler.fifo_count < PitchTaps + advance && data_left) {
        data_left = DecodeSome(system, voice);
    }
    const u32 have = sampler.fifo_count;
    if (have < PitchTaps + advance) {
        // Silence stands in for the frames the data did not have.
        if (sampler.fifo_read + PitchTaps + advance > FifoFrames) {
            CompactFifo(sampler, voice.channels);
        }
        for (u32 ch = 0; ch < voice.channels; ++ch) {
            float* plane = &sampler.fifo[static_cast<size_t>(ch) * FifoFrames + sampler.fifo_read];
            std::fill(plane + have, plane + PitchTaps + advance, 0.0f);
        }
        sampler.fifo_count = PitchTaps + advance;
    }
    const auto& weights = PitchWeights();
    for (u32 ch = 0; ch < voice.channels; ++ch) {
        const float* plane =
            &sampler.fifo[static_cast<size_t>(ch) * FifoFrames + sampler.fifo_read];
        float* out = &system.out[static_cast<size_t>(ch) * grain];
        u32 position = sampler.phase;
        for (u32 i = 0; i < grain; ++i) {
            const float* frames = plane + (position >> 14);
            const auto& weight = weights[(position >> 5) & (PitchPhases - 1)];
            out[i] = frames[0] * weight[0] + frames[1] * weight[1] + frames[2] * weight[2] +
                     frames[3] * weight[3];
            position += step;
        }
    }
    sampler.fifo_read += advance;
    sampler.fifo_count -= advance;
    sampler.phase = (step * grain + sampler.phase) & (PitchOne - 1);
    // Out of data: the voice is over, unless the game said more blocks follow.
    return data_left || have >= PitchTaps + advance || sampler.more_follow;
}

// The level a channel has in a channel of another layout when the game gives no matrix. The
// layouts are mono, stereo, 5.1 and 7.1, in the order left, right, centre, low frequency
// effects, the two at the sides, the two at the back; a mono channel is a centre.
float DefaultLevel(u32 in_channels, u32 out_channels, u32 in, u32 out) {
    const auto known = [](u32 channels) {
        return channels == 1 || channels == 2 || channels == 6 || channels == 8;
    };
    if (in_channels == out_channels || !known(in_channels) || !known(out_channels)) {
        return in == out ? 1.0f : 0.0f;
    }
    enum : u32 { Left, Right, Centre, Lfe, SideLeft, SideRight, BackLeft, BackRight };
    const u32 from = in_channels == 1 ? Centre : in;
    const u32 to = out_channels == 1 ? Centre : out;
    const bool kept = out_channels == 1 ? from == Centre : from < out_channels;
    if (kept) {
        return from == to ? 1.0f : 0.0f;
    }
    const bool left_side = from == Left || from == SideLeft || from == BackLeft;
    switch (from) {
    case Left:
    case Right:
        // Only mono lacks them.
        return 0.70710677f;
    case Centre:
        // Only stereo lacks it.
        return 0.70710677f;
    case Lfe:
        return 0.0f;
    case SideLeft:
    case SideRight:
        return out_channels == 1 || (to == Left) == left_side ? 0.5f : 0.0f;
    default:
        if (out_channels == 6) {
            return to == (left_side ? SideLeft : SideRight) ? 1.0f : 0.0f;
        }
        return out_channels == 1 || (to == Left) == left_side ? 0.5f : 0.0f;
    }
}

// Adds system.out through each patched port of the voice into the input of its destination.
void Send(System& system, Voice& voice) {
    const u32 grain = system.grain;
    const float per_sample = 1.0f / static_cast<float>(grain);
    // A voice that starts in this render has no last render to run from.
    const bool starts = (voice.events & RequestPlay) != 0;
    for (u32 p = 0; p < voice.rack->max_ports; ++p) {
        Port& port = voice.ports[p];
        Voice* dest = port.dest;
        if (!dest) {
            continue;
        }
        const bool jump = starts || port.fresh;
        const float volume_from = jump ? port.volume : port.last_volume;
        const u32 in_channels = voice.channels;
        const u32 out_channels = dest->channels;
        Matrix* matrix = port.matrix >= 0 && static_cast<u32>(port.matrix) < voice.matrices.size()
                             ? &voice.matrices[port.matrix]
                             : nullptr;
        if (matrix && matrix->count < in_channels * out_channels) {
            matrix = nullptr;
        }
        const bool heard = dest->active && !dest->input.empty();
        for (u32 o = 0; heard && o < out_channels; ++o) {
            float* to = &dest->input[static_cast<size_t>(o) * grain];
            for (u32 i = 0; i < in_channels; ++i) {
                // A level per destination channel and source channel, destination first.
                const u32 index = o * in_channels + i;
                const float level =
                    matrix ? matrix->levels[index] : DefaultLevel(in_channels, out_channels, i, o);
                const float level_from = matrix && !jump ? matrix->last_levels[index] : level;
                const float from_gain = volume_from * level_from;
                const float to_gain = port.volume * level;
                if (from_gain == 0.0f && to_gain == 0.0f) {
                    continue;
                }
                const float* from = &system.out[static_cast<size_t>(i) * grain];
                if (from_gain == to_gain) {
                    for (u32 s = 0; s < grain; ++s) {
                        to[s] += from[s] * to_gain;
                    }
                } else {
                    const float slope = (to_gain - from_gain) * per_sample;
                    for (u32 s = 0; s < grain; ++s) {
                        to[s] += from[s] * (from_gain + slope * static_cast<float>(s));
                    }
                }
            }
        }
        // The ports of a voice that share a matrix: only the first runs from its last levels.
        if (matrix) {
            matrix->last_levels = matrix->levels;
        }
        port.last_volume = port.volume;
        port.fresh = false;
    }
}

// The events kicked since the last render, before the voice renders: the state they lead to
// and the shape they give the grain.
void BeginEvents(Voice& voice) {
    voice.events = voice.requests;
    voice.requests = 0;
    voice.transition = Transition::None;
    voice.active = false;
    if ((voice.flags & StateInUse) == 0) {
        voice.events = 0;
        voice.active = voice.linked;
        return;
    }
    u32& events = voice.events;
    if (events & RequestPlay) {
        voice.fx_first = true;
    }
    if (events & RequestPause) {
        if (voice.flags & StatePaused) {
            events &= ~RequestPause;
        } else if (events & RequestPlay) {
            // Paused before it ever played: nothing to fade.
            events &= ~RequestPause;
            voice.flags |= StatePaused;
        } else {
            voice.transition = Transition::FadeOutTail;
        }
    }
    if (events & RequestResume) {
        if (voice.flags & StatePaused) {
            voice.flags &= ~StatePaused;
            voice.transition = Transition::FadeIn;
        } else {
            events &= ~RequestResume;
        }
    }
    if (events & RequestStop) {
        if (voice.envelope.set && (voice.flags & StatePaused) == 0) {
            // The envelope ends the voice: by its release points, or at once without any.
            events &= ~RequestStop;
        } else {
            voice.transition = Transition::FadeOutHead;
        }
        voice.flags |= StateStopped;
    }
    if (events & (RequestStopImmediate | RequestKill)) {
        voice.transition = Transition::FadeOutHead;
    }
    if (voice.flags & StatePaused) {
        return;
    }
    voice.flags |= StatePlaying;
    voice.active = true;
}

// After the voice rendered: a pause takes hold, a stop frees the voice.
void EndEvents(Voice& voice) {
    const u32 events = voice.events;
    voice.events = 0;
    if (events & RequestPause) {
        voice.flags = (voice.flags & ~StatePlaying) | StatePaused;
    }
    if (events & (RequestStop | RequestStopImmediate | RequestKill)) {
        EndVoice(voice);
        voice.linked = false;
    }
}

// Shapes the grain in system.out by the transition of the voice.
void ApplyTransition(System& system, const Voice& voice) {
    const u32 grain = system.grain;
    const u32 fade = std::min(FadeSamples, grain);
    for (u32 ch = 0; ch < voice.channels; ++ch) {
        float* out = &system.out[static_cast<size_t>(ch) * grain];
        switch (voice.transition) {
        case Transition::FadeOutTail:
            for (u32 i = 0; i < fade; ++i) {
                out[grain - fade + i] *= 1.0f - static_cast<float>(i) / fade;
            }
            break;
        case Transition::FadeIn:
            for (u32 i = 0; i < fade; ++i) {
                out[i] *= static_cast<float>(i) / fade;
            }
            break;
        case Transition::Silent:
            std::fill_n(out, grain, 0.0f);
            break;
        case Transition::FadeOutHead:
            for (u32 i = 0; i < fade; ++i) {
                out[i] *= 1.0f - static_cast<float>(i) / fade;
            }
            std::fill(out + fade, out + grain, 0.0f);
            break;
        default:
            break;
        }
    }
}

void ApplyGain(System& system, const Voice& voice) {
    for (u32 ch = 0; ch < voice.channels; ++ch) {
        float* out = &system.out[static_cast<size_t>(ch) * system.grain];
        for (u32 i = 0; i < system.grain; ++i) {
            out[i] *= system.gain[i];
        }
    }
}

// Calls the effect handler of the game on the grain in system.out.
void RunUserFx(System& system, Voice& voice) {
    if (!voice.fx) {
        return;
    }
    std::array<float*, MaxChannels> planes;
    for (u32 ch = 0; ch < MaxChannels; ++ch) {
        planes[ch] = &system.out[static_cast<size_t>(ch) * system.grain];
    }
    FxContext context{};
    context.channels = planes.data();
    std::copy(voice.fx_data.begin(), voice.fx_data.end(), context.user_data);
    context.flags = (voice.fx_first ? FxFirst : 0) | (voice.flags & (StatePaused | StateStopped)) |
                    ((voice.events & RequestStopImmediate) ? FxStoppedNow : 0) |
                    ((voice.events & RequestKill) ? FxKilled : 0);
    context.num_channels = voice.channels;
    context.grain_samples = system.grain;
    context.sample_rate = system.sample_rate;
    voice.fx(&context);
    voice.fx_first = false;
}

void RenderSamplerVoice(System& system, Voice& voice) {
    if (!voice.sampler) {
        return;
    }
    if (!voice.active) {
        if (voice.flags & StatePaused) {
            voice.sampler->state.envelope_height = 1.0f;
        }
        return;
    }
    const bool data_left = RenderSampler(system, voice);
    RunFilters(system, voice, 0, system.out.data());
    bool envelope_left = true;
    if (voice.envelope.set) {
        envelope_left = RunEnvelope(system, voice);
        ApplyGain(system, voice);
    }
    RunFilters(system, voice, 1, system.out.data());
    ApplyTransition(system, voice);
    Send(system, voice);
    voice.sampler->state.envelope_height = voice.envelope.set ? voice.envelope.height : 1.0f;
    if (!data_left || !envelope_left) {
        voice.events |= RequestKill;
    }
}

// The early reflection patterns the game picks from: when the three reflections after the first
// arrive, in milliseconds, and the gains of the four.
constexpr std::array<std::array<float, 4>, 8> EarlyTimes{{
    {0.0f, 11.0f, 23.0f, 35.0f},
    {0.0f, 14.75f, 26.55f, 38.03f},
    {0.0f, 6.0f, 17.0f, 29.0f},
    {0.0f, 8.0f, 20.0f, 53.0f},
    {0.0f, 11.0f, 17.0f, 30.0f},
    {0.0f, 16.0f, 30.0f, 55.0f},
    {0.0f, 20.0f, 29.0f, 40.0f},
    {0.0f, 37.0f, 47.0f, 62.0f},
}};
constexpr std::array<std::array<float, 4>, 8> EarlyGains{{
    {0.7f, 0.4f, 0.134f, 0.1f},
    {0.7f, 0.4f, 0.3f, 0.1f},
    {0.7f, 0.45f, -0.6f, 0.2f},
    {0.7f, 0.55f, -0.6f, 0.2f},
    {0.7f, -0.45f, 0.6f, 0.3f},
    {0.7f, 0.7f, 0.7f, 0.4f},
    {0.7f, 0.7f, -0.4f, 0.3f},
    {0.7f, 0.7f, -0.4f, 0.4f},
}};
// The lengths of the loops of the late reverb at full density, in milliseconds.
constexpr std::array<float, ReverbCombs> CombTimes{
    22.01f, 23.25f, 24.75f, 26.6f,  28.25f, 29.63f, 31.06f, 32.44f,
    33.69f, 34.95f, 36.29f, 37.65f, 38.99f, 40.35f, 41.67f, 43.05f,
};
// The loops each channel mixes, a bit per loop. The fourth channel, the low frequency effects,
// takes no reverb.
constexpr std::array<u32, MaxChannels> CombSets{
    0xaaaa, 0x5555, 0xcccc, 0x3333, 0x0ff0, 0x9ab2, 0x3c3c, 0x399c,
};
// The lengths of the all-pass steps in milliseconds, and the samples each channel has less.
constexpr std::array<float, ReverbStages> StageTimes{11.58f, 9.19f, 7.1f, 4.69f};
constexpr std::array<std::array<s32, ReverbStages>, MaxChannels> StageTrims{{
    {0, -32, -6, -11},
    {-10, -5, 0, -34},
    {-8, -3, -33, -5},
    {-7, -9, -5, -8},
    {-33, -8, -8, 0},
    {-11, -21, -10, -7},
    {-9, -10, -27, -3},
    {-6, -11, -9, -24},
}};

// size is the reverb size of the rack option: 1 holds 300 ms of early reflections, 0 holds 100.
std::unique_ptr<Reverb> MakeReverb(u32 sample_rate, u32 size) {
    auto reverb = std::make_unique<Reverb>();
    const float rate = static_cast<float>(sample_rate);
    reverb->early_line.assign(size == 1 ? 14400 : 4800, 0.0f);
    size_t loops = 0;
    for (const float time : CombTimes) {
        loops += static_cast<u32>(rate * time * 0.001f + 0.5f) | 1;
    }
    reverb->comb_line.assign(loops, 0.0f);
    size_t steps = 0;
    for (const float time : StageTimes) {
        steps += static_cast<u32>(rate * time * 0.001f + 0.5f);
    }
    for (auto& line : reverb->stage_lines) {
        line.assign(steps, 0.0f);
    }
    reverb->room_filter.skip_channels = 1u << 3;
    return reverb;
}

// Places the five taps of the early reflections.
void ShapeEarly(Reverb& reverb, float rate) {
    const s32 length = static_cast<s32>(reverb.early_line.size());
    const u32 pattern = reverb.pattern < 8 ? reverb.pattern : reverb.pattern - 8;
    const auto& times = EarlyTimes[pattern];
    const auto& gains = EarlyGains[pattern];
    const s32 first = std::min(
        static_cast<s32>(reverb.reflections_delay * rate * 1000.0f * 0.001f + 0.5f), length);
    s32 late = std::min(
        static_cast<s32>(
            (reverb.reflections_delay + reverb.reverb_delay) * rate * 1000.0f * 0.001f + 0.5f),
        length);
    // The patterns from 8 on are the first eight with the line to themselves.
    if (reverb.pattern >= 8) {
        late = length;
    }
    // The reflections end where the late reverb starts: a pattern too long for that is squeezed.
    const s32 span = static_cast<s32>(rate * times[3] * 0.001f + 0.5f);
    const float squeeze =
        late < first + span ? static_cast<float>(late - first) / static_cast<float>(span) : 1.0f;
    reverb.early_delay[0] = first;
    reverb.early_gain[0] = gains[0];
    u32 last = first;
    for (u32 tap = 1; tap < 4; ++tap) {
        const u32 delay = static_cast<s32>(squeeze * times[tap] * rate * 0.001f + 0.5f) + first;
        reverb.early_delay[tap] = delay;
        // Two reflections at one time: the later one is off.
        reverb.early_gain[tap] = delay != last ? gains[tap] : 0.0f;
        last = delay;
    }
    reverb.early_delay[4] = std::min(
        first + static_cast<s32>(reverb.reverb_delay * rate * 1000.0f * 0.001f + 0.5f), length);
    reverb.early_gain[4] = 1.0f;
    reverb.early_on = true;
}

// The taps of the early reflections over the input: four into direct, the fifth into feed. If
// write is true, the input goes into the line after them.
void RunEarly(Reverb& reverb, float* direct, float* feed, const float* in, u32 grain, bool write) {
    std::fill_n(direct, grain, 0.0f);
    std::fill_n(feed, grain, 0.0f);
    const u32 length = static_cast<u32>(reverb.early_line.size());
    float* line = reverb.early_line.data();
    for (u32 tap = 0; reverb.early_on && tap < ReverbTaps; ++tap) {
        float* out = tap == ReverbTaps - 1 ? feed : direct;
        const float gain = reverb.early_gain[tap];
        // The first samples come off the line, the rest out of this grain.
        const u32 old = std::min(reverb.early_delay[tap], grain);
        u32 at = (reverb.early_pos + length - reverb.early_delay[tap]) % length;
        for (u32 i = 0; i < old; ++i) {
            out[i] += line[at] * gain;
            at = at + 1 == length ? 0 : at + 1;
        }
        for (u32 i = old; i < grain; ++i) {
            out[i] += in[i - old] * gain;
        }
    }
    if (!write) {
        return;
    }
    for (u32 i = grain > length ? grain - length : 0; i < grain; ++i) {
        line[reverb.early_pos] = in[i];
        reverb.early_pos = reverb.early_pos + 1 == length ? 0 : reverb.early_pos + 1;
    }
}

// Sizes and tunes the loops of the late reverb. If clear is true, their lines start empty.
void ShapeLate(Reverb& reverb, float rate, bool clear) {
    // A lower density shortens the loops, down to a quarter.
    const float spread = reverb.density * -3.0f / 100.0f + 4.0f;
    const float ratio = reverb.decay_hf_ratio;
    u32 offset = 0;
    for (u32 i = 0; i < ReverbCombs; ++i) {
        ReverbComb& comb = reverb.combs[i];
        comb.length = static_cast<u32>(rate * CombTimes[i] / spread * 0.001f + 0.5f) | 1;
        comb.offset = offset;
        offset += comb.length;
        if (clear) {
            std::fill_n(&reverb.comb_line[comb.offset], comb.length, 0.0f);
        }
        if (comb.length < comb.pos) {
            comb.pos = 0;
        }
        // A turn of the loop loses what brings it down 60 dB over the decay time.
        comb.feedback =
            std::pow(10.0f, static_cast<float>(comb.length) / rate / reverb.decay_time * -3.0f);
        if (ratio > 1.0f) {
            // The highs outlast the lows: the lows get a fixed low pass on top of the line.
            const float high = std::pow(10.0f, (ratio - 1.0f) * std::log10(comb.feedback));
            comb.damp = {0.0506209731f, 0.949379027f};
            comb.blend = {1.0f, 1.0f - high};
        } else {
            float bend = ratio * ratio;
            if (ratio < 0.8f) {
                bend = (0.8f - ratio) * -0.857142925f + 0.8f;
                bend *= bend;
            }
            const float keep =
                std::min(std::log10(comb.feedback) * 2.30258512f * 0.25f * (1.0f - 1.0f / bend),
                         0.99977994f);
            comb.damp = {1.0f - keep, keep};
            comb.blend = {0.0f, 1.0f};
        }
        const float root = std::sqrt(ratio);
        const float tilt = (1.0f - root) / (root + 1.0f);
        comb.shape = {tilt / (1.0f - tilt) + 1.0f, -tilt / (1.0f - tilt)};
    }
}

void RunComb(ReverbComb& comb, float* line, const float* in, float* out, u32 grain) {
    line += comb.offset;
    for (u32 i = 0; i < grain; ++i) {
        const float shaped = in[i] * comb.shape[0] + comb.last_input * comb.shape[1];
        comb.last_input = in[i];
        const float delayed = line[comb.pos];
        comb.damp_state = delayed * comb.damp[0] + comb.damp_state * comb.damp[1];
        const float turn =
            (delayed * comb.blend[0] + comb.damp_state * comb.blend[1]) * comb.feedback;
        line[comb.pos] = shaped + turn;
        out[i] = turn;
        comb.pos = comb.pos + 1 >= comb.length ? 0 : comb.pos + 1;
    }
}

// Sizes the all-pass steps of a channel.
void ShapeStages(Reverb& reverb, u32 ch, float rate) {
    u32 offset = 0;
    for (u32 s = 0; s < ReverbStages; ++s) {
        ReverbStage& stage = reverb.stages[ch][s];
        const s32 length = static_cast<s32>(rate * StageTimes[s] * 0.001f + 0.5f);
        stage.offset = offset;
        offset += length;
        stage.length = length + StageTrims[ch][s];
        stage.gain = reverb.diffusion * 0.005f;
    }
}

void RunStages(Reverb& reverb, u32 ch, float* samples, u32 grain) {
    for (ReverbStage& stage : reverb.stages[ch]) {
        float* line = &reverb.stage_lines[ch][stage.offset];
        for (u32 i = 0; i < grain; ++i) {
            const float delayed = line[stage.pos];
            const float in = delayed * stage.gain + samples[i];
            line[stage.pos] = in;
            samples[i] = delayed - stage.gain * in;
            stage.pos = stage.pos + 1 >= stage.length ? 0 : stage.pos + 1;
        }
    }
}

// The samples of from into to, under a gain that runs from first to last over the grain. If add
// is true, they add to what to holds.
void Ramp(const float* from, float* to, u32 grain, float first, float last, bool add) {
    const float slope = (last - first) / static_cast<float>(grain);
    for (u32 i = 0; i < grain; ++i) {
        const float value = from[i] * (first + slope * static_cast<float>(i));
        to[i] = add ? to[i] + value : value;
    }
}

// The reverb of a reverb voice: its input through the reverb into system.out.
void RunReverb(System& system, Voice& voice) {
    Reverb& reverb = *voice.reverb;
    const u32 grain = system.grain;
    const u32 channels = voice.channels;
    const float rate = static_cast<float>(system.sample_rate);
    const float* in = voice.input.data();
    float* out = system.out.data();
    reverb.work.resize(static_cast<size_t>(6 + ReverbCombs) * grain);
    float* mono = reverb.work.data();
    float* direct = mono + grain;
    float* feed = direct + grain;
    float* old_direct = feed + grain;
    float* old_feed = old_direct + grain;
    float* smear = old_feed + grain;
    float* loops = smear + grain;

    // The reverb hears one channel: the sum of the input but for the low frequency effects.
    std::copy_n(in, grain, mono);
    const float share = channels == 8 ? 0.26699999f : channels == 6 ? 0.316000015f : 0.707000017f;
    if (channels == 8 || channels == 6 || channels == 2) {
        Ramp(in, mono, grain, share, share, false);
        for (u32 ch = 1; ch < channels; ++ch) {
            if (ch != 3) {
                Ramp(in + static_cast<size_t>(ch) * grain, mono, grain, share, share, true);
            }
        }
    }

    if (reverb.restart) {
        std::fill(reverb.early_line.begin(), reverb.early_line.end(), 0.0f);
        reverb.early_pos = 0;
        reverb.early_on = false;
        reverb.early_level = 1.0f;
        reverb.late_level = 1.0f;
        reverb.combs = {};
        std::fill(reverb.comb_line.begin(), reverb.comb_line.end(), 0.0f);
        reverb.stages = {};
        for (auto& line : reverb.stage_lines) {
            std::fill(line.begin(), line.end(), 0.0f);
        }
        reverb.room_filter.restart = true;
    }
    if (reverb.changed) {
        // The grain fades from the reflections as they were to the reflections as they are.
        RunEarly(reverb, old_direct, old_feed, mono, grain, false);
        ShapeEarly(reverb, rate);
        reverb.early_level = reverb.reflections * reverb.room;
        reverb.late_level = reverb.reverb * reverb.room;
        RunEarly(reverb, direct, feed, mono, grain, true);
        Ramp(direct, direct, grain, 0.0f, 1.0f, false);
        Ramp(feed, feed, grain, 0.0f, 1.0f, false);
        Ramp(old_direct, direct, grain, 1.0f, 0.0f, true);
        Ramp(old_feed, feed, grain, 1.0f, 0.0f, true);
        const bool clear = reverb.last_density != reverb.density;
        reverb.last_density = reverb.density;
        ShapeLate(reverb, rate, clear);
        // The low pass of the room: two equal steps of one pole, set by how much the room keeps
        // at the reference frequency.
        Filter& filter = reverb.room_filter;
        filter.type = 0;
        if (reverb.room_hf < 1.0f) {
            const float keep = reverb.room_hf;
            const float cosine = std::cos(reverb.hf_reference * 6.2831855f / rate);
            const float root =
                std::sqrt((keep + keep) * (1.0f - cosine) - keep * keep * (1.0f - cosine * cosine));
            const float pole = ((1.0f - keep * cosine) - root) / (1.0f - keep);
            filter.type = 0x20;
            filter.coefs = {(1.0f - pole) * (1.0f - pole), 0.0f, 0.0f, pole + pole, pole * -pole};
        }
    } else {
        RunEarly(reverb, direct, feed, mono, grain, true);
    }
    for (u32 i = 0; i < ReverbCombs; ++i) {
        RunComb(reverb.combs[i], reverb.comb_line.data(), feed,
                loops + static_cast<size_t>(i) * grain, grain);
    }
    for (u32 ch = 0; ch < channels; ++ch) {
        float* plane = out + static_cast<size_t>(ch) * grain;
        if (ch == 3) {
            std::copy_n(in + static_cast<size_t>(ch) * grain, grain, plane);
            continue;
        }
        if (reverb.changed) {
            ShapeStages(reverb, ch, rate);
        }
        // Every second loop of the channel is quieter at a lower density.
        std::fill_n(smear, grain, 0.0f);
        u32 taken = 0;
        for (u32 i = 0; i < ReverbCombs; ++i) {
            if ((CombSets[ch] & (1u << i)) == 0) {
                continue;
            }
            const float gain = taken & 1
                                   ? 1.0f
                                   : 1.0f / (static_cast<float>(taken) * 0.5f * 20.0f + 40.0f) *
                                             (reverb.density - 100.0f) +
                                         1.0f;
            Ramp(loops + static_cast<size_t>(i) * grain, smear, grain, gain, gain, true);
            ++taken;
        }
        RunStages(reverb, ch, smear, grain);
        Ramp(direct, plane, grain, reverb.early_level, reverb.early_level, false);
        Ramp(smear, plane, grain, reverb.late_level, reverb.late_level, true);
    }
    RunFilter(reverb.room_filter, out, channels, grain);
    for (u32 ch = 0; ch < channels; ++ch) {
        float* plane = out + static_cast<size_t>(ch) * grain;
        const float wet = ch == 3 ? 0.0f : reverb.wet;
        Ramp(plane, plane, grain, reverb.restart ? wet : reverb.last_wet[ch], wet, false);
        Ramp(in + static_cast<size_t>(ch) * grain, plane, grain,
             reverb.restart ? reverb.dry : reverb.last_dry[ch], reverb.dry, true);
        reverb.last_wet[ch] = wet;
        reverb.last_dry[ch] = reverb.dry;
    }
    reverb.changed = false;
    reverb.restart = false;
}

// The delay of a custom rack, or nullptr if the rack has none. The modules follow the rack
// option: their count at 0x88, then 40 bytes each from 0x90 that start with the address of the
// module option and the module id.
std::unique_ptr<Delay> MakeDelay(const void* option) {
    const u32 modules = std::min<u32>(Read<u32>(option, 0x88), 24);
    for (u32 i = 0; i < modules; ++i) {
        const size_t at = 0x90 + static_cast<size_t>(i) * 40;
        if (Read<u32>(option, at + 8) != ModuleDelay) {
            continue;
        }
        // The delay option: size, kind, the most taps, the longest time in milliseconds. The
        // game asks for the kind with a line per channel, the one the mixer makes. A delay
        // without an option takes eight taps and ten seconds.
        const void* module = Read<const void*>(option, at);
        auto delay = std::make_unique<Delay>();
        delay->max_taps = module ? Read<u32>(module, 8) : DelayTaps;
        delay->max_time = module ? Read<float>(module, 0xc) : 10000.0f;
        return delay;
    }
    return nullptr;
}

// Places the taps of the delay and tunes its low pass.
void ShapeDelay(Delay& delay, float rate, u32 grain) {
    float sum = 0.0f;
    for (u32 tap = 0; tap < delay.taps; ++tap) {
        // A tap reaches back a grain at the least: the line takes the grain after the taps read.
        delay.tap_delay[tap] =
            std::max(static_cast<u32>(rate * delay.tap_time[tap] * 0.001f + 0.5f), grain);
        sum += std::abs(delay.tap_level[tap]);
    }
    // Taps louder than one together feed back that much less.
    delay.feed = delay.feedback / std::max(sum, 1.0f);
    if (delay.low_pass) {
        const float turn = 2.0f - std::cos(delay.cutoff * 6.2831855f / rate);
        delay.pole = turn - std::sqrt(turn * turn - 1.0f);
    }
}

// The delay of a delay voice: its input through the delay into system.out.
void RunDelay(System& system, Voice& voice) {
    Delay& delay = *voice.delay;
    const u32 grain = system.grain;
    const size_t bytes = static_cast<size_t>(voice.channels) * grain * sizeof(float);
    if (!delay.set) {
        // The game set no delay yet: the input goes on as it is.
        std::memcpy(system.out.data(), voice.input.data(), bytes);
        return;
    }
    const float rate = static_cast<float>(system.sample_rate);
    if (delay.restart) {
        delay.restart = false;
        delay.length = static_cast<u32>(rate * delay.max_time * 0.001f + 0.5f);
        delay.lines.assign(static_cast<size_t>(delay.length) * MaxChannels, 0.0f);
        delay.pos = {};
        delay.low_state = {};
    }
    if (delay.changed) {
        delay.changed = false;
        ShapeDelay(delay, rate, grain);
    }
    delay.work.resize(grain);
    float* taps = delay.work.data();
    const u32 length = delay.length;
    const float dry = delay.level * delay.dry;
    const float wet = delay.level * delay.wet;
    for (u32 ch = 0; ch < voice.channels; ++ch) {
        const float* in = &voice.input[static_cast<size_t>(ch) * grain];
        float* out = &system.out[static_cast<size_t>(ch) * grain];
        // The low frequency effects and the channels the game names pass untouched.
        if (ch == 3 || (delay.skip_channels >> ch & 1) != 0) {
            std::copy_n(in, grain, out);
            continue;
        }
        float* line = &delay.lines[static_cast<size_t>(ch) * length];
        u32& pos = delay.pos[ch];
        std::fill_n(taps, grain, 0.0f);
        for (u32 tap = 0; tap < delay.taps; ++tap) {
            u32 at = (pos + length - delay.tap_delay[tap]) % length;
            for (u32 i = 0; i < grain; ++i) {
                taps[i] += delay.tap_level[tap] * line[at];
                at = at + 1 == length ? 0 : at + 1;
            }
        }
        if (delay.low_pass) {
            float state = delay.low_state[ch];
            for (u32 i = 0; i < grain; ++i) {
                state = (1.0f - delay.pole) * taps[i] + delay.pole * state;
                taps[i] = state;
            }
            delay.low_state[ch] = state;
        }
        for (u32 i = 0; i < grain; ++i) {
            line[pos] = delay.input_gain[ch] * in[i] + delay.feed * taps[i];
            pos = pos + 1 == length ? 0 : pos + 1;
        }
        for (u32 i = 0; i < grain; ++i) {
            out[i] = dry * in[i] + wet * taps[i];
        }
    }
}

// A mixing voice: its input through its effects into system.out, then on through its ports.
void RenderMixVoice(System& system, Voice& voice) {
    if (!voice.active) {
        return;
    }
    if (voice.reverb) {
        RunReverb(system, voice);
        ApplyTransition(system, voice);
        Send(system, voice);
        return;
    }
    if (voice.delay) {
        RunDelay(system, voice);
        ApplyTransition(system, voice);
        Send(system, voice);
        return;
    }
    if (voice.rack->id != RackSubmixer) {
        // A custom rack of modules the mixer does not make: nothing of it goes on.
        std::fill(system.out.begin(), system.out.end(), 0.0f);
        Send(system, voice);
        return;
    }
    std::memcpy(system.out.data(), voice.input.data(),
                static_cast<size_t>(voice.channels) * system.grain * sizeof(float));
    RunFilters(system, voice, 0, system.out.data());
    bool envelope_left = true;
    if (voice.envelope.set) {
        envelope_left = RunEnvelope(system, voice);
        ApplyGain(system, voice);
    }
    RunCompressor(system, voice, voice.channels);
    RunUserFx(system, voice);
    RunFilters(system, voice, 1, system.out.data());
    ApplyTransition(system, voice);
    Send(system, voice);
    if (!envelope_left) {
        voice.events |= RequestKill;
    }
}

float Clamp1(float value) {
    return std::clamp(value, -1.0f, 1.0f);
}

// The low pass of the low frequency effects channel: four poles with a tenth of a decibel of
// ripple below the corner.
void ShapeLfeFilter(Mastering& mastering, u32 sample_rate) {
    const double ripple = std::sqrt(std::pow(10.0, 0.01) - 1.0);
    const double spread = std::asinh(1.0 / ripple) / 4.0;
    const double pi = 3.14159265358979323846;
    const double warp = std::tan(pi * mastering.lfe_frequency / sample_rate);
    for (u32 step = 0; step < 2; ++step) {
        const double angle = pi * (2.0 * step + 1.0) / 8.0;
        const double real = std::sinh(spread) * std::sin(angle);
        const double size = std::hypot(real, std::cosh(spread) * std::cos(angle));
        const double w = size * warp;
        const double damp = w * 2.0 * real / size;
        const double scale = 1.0 / (1.0 + damp + w * w);
        // The gain of the whole filter sits in its first step.
        const double gain = step == 0 ? 1.0 / std::sqrt(1.0 + ripple * ripple) : 1.0;
        const double b = w * w * scale;
        mastering.lfe_coefs[step] = {
            static_cast<float>(b * gain), static_cast<float>(2.0 * b * gain),
            static_cast<float>(b * gain), static_cast<float>(2.0 * (1.0 - w * w) * scale),
            static_cast<float>(-(1.0 - damp + w * w) * scale)};
    }
    // The gain of the second step moves to the first, which keeps the second at 1, 2, 1.
    const float second = mastering.lfe_coefs[1][0];
    for (u32 i = 0; i < 3; ++i) {
        mastering.lfe_coefs[0][i] *= second;
        mastering.lfe_coefs[1][i] /= second;
    }
}

// A mastering voice: its mix through its low pass, limiter and gain into the buffer of the game.
void WriteOutput(System& system, Voice& voice, const RenderBuffer* buffers, u32 count) {
    Mastering& mastering = voice.mastering;
    if (!voice.active || mastering.output >= count || !buffers[mastering.output].data) {
        return;
    }
    const RenderBuffer& buffer = buffers[mastering.output];
    const u32 grain = system.grain;
    const u32 channels = std::min(buffer.channels, MaxChannels);
    if (channels == voice.channels) {
        std::memcpy(system.out.data(), voice.input.data(),
                    static_cast<size_t>(channels) * grain * sizeof(float));
    } else {
        // The buffer has another layout than the voice.
        std::fill(system.out.begin(), system.out.end(), 0.0f);
        for (u32 o = 0; o < channels; ++o) {
            float* to = &system.out[static_cast<size_t>(o) * grain];
            for (u32 i = 0; i < voice.channels; ++i) {
                const float level = DefaultLevel(voice.channels, channels, i, o);
                if (level == 0.0f) {
                    continue;
                }
                const float* from = &voice.input[static_cast<size_t>(i) * grain];
                for (u32 s = 0; s < grain; ++s) {
                    to[s] += from[s] * level;
                }
            }
        }
    }
    if (mastering.lfe_changed) {
        ShapeLfeFilter(mastering, system.sample_rate);
        mastering.lfe_changed = false;
    }
    if (mastering.restart || !mastering.lfe_filter) {
        mastering.lfe_history = {};
    }
    if (mastering.lfe_filter && voice.channels > 4 && channels > 3) {
        float* samples = &system.out[static_cast<size_t>(3) * grain];
        for (u32 step = 0; step < 2; ++step) {
            const auto& c = mastering.lfe_coefs[step];
            auto& history = mastering.lfe_history[step];
            float x1 = history[0];
            float x2 = history[1];
            float y1 = history[2];
            float y2 = history[3];
            for (u32 i = 0; i < grain; ++i) {
                const float x = samples[i];
                const float y = c[0] * x + c[1] * x1 + c[2] * x2 + c[3] * y1 + c[4] * y2;
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                samples[i] = y;
            }
            history = {x1, x2, y1, y2};
        }
    }
    RunCompressor(system, voice, channels);
    if (mastering.restart) {
        mastering.last_gain = mastering.gain;
        mastering.restart = false;
    }
    for (u32 ch = 0; ch < channels; ++ch) {
        const float from_gain = mastering.last_gain[ch];
        const float to_gain = mastering.gain[ch];
        if (from_gain == 1.0f && to_gain == 1.0f) {
            continue;
        }
        const float slope = (to_gain - from_gain) / static_cast<float>(grain);
        float* samples = &system.out[static_cast<size_t>(ch) * grain];
        for (u32 s = 0; s < grain; ++s) {
            samples[s] *= from_gain + slope * static_cast<float>(s);
        }
    }
    mastering.last_gain = mastering.gain;
    const u32 kept = voice.channels;
    voice.channels = channels;
    ApplyTransition(system, voice);
    voice.channels = kept;
    const u32 type = buffer.waveform_type;
    for (u32 ch = 0; ch < channels; ++ch) {
        const float* from = &system.out[static_cast<size_t>(ch) * grain];
        for (u32 s = 0; s < grain; ++s) {
            const size_t at = static_cast<size_t>(s) * buffer.channels + ch;
            if (type == 0x18) {
                static_cast<float*>(buffer.data)[at] = from[s];
            } else if (type == 0x12) {
                static_cast<s16*>(buffer.data)[at] = static_cast<s16>(Clamp1(from[s]) * 32767.0f);
            }
        }
    }
}

} // namespace

System* CreateSystem(u32 sample_rate, u32 grain_samples) {
    auto* system = new System;
    system->sample_rate = sample_rate;
    system->grain = grain_samples;
    system->out.assign(static_cast<size_t>(MaxChannels) * grain_samples, 0.0f);
    system->gain.assign(grain_samples, 1.0f);
    system->decode.assign(MaxChannels * 256, 0.0f);
    return system;
}

void DestroySystem(System* system) {
    for (Rack* rack : std::vector<Rack*>(system->racks)) {
        DestroyRack(rack);
    }
    delete system;
}

Rack* CreateRack(System* system, u32 rack_id, const void* option) {
    std::scoped_lock lock{system->mutex};
    auto* rack = new Rack;
    rack->system = system;
    rack->id = rack_id;
    // The option of the game: size, name, flags, grain, voices, input delay blocks, matrices,
    // ports. The defaults are those of a rack made without one.
    const u32 max_voices = option ? Read<u32>(option, 0x20) : 1;
    const u32 max_matrices = option ? Read<u32>(option, 0x28) : 1;
    rack->max_ports = std::min(option ? Read<u32>(option, 0x2c) : 1, MaxPorts);
    rack->voices.resize(max_voices);
    for (u32 i = 0; i < max_voices; ++i) {
        Voice& voice = rack->voices[i];
        voice.rack = rack;
        voice.index = i;
        voice.matrices.resize(max_matrices);
        if (!IsSampler(rack)) {
            voice.channels = MaxChannels;
            voice.input.assign(static_cast<size_t>(MaxChannels) * system->grain, 0.0f);
        }
        if (rack_id == RackReverb) {
            // The reverb option follows the rack option: channels, then the size.
            voice.reverb = MakeReverb(system->sample_rate, option ? Read<u32>(option, 0x84) : 1);
        }
        if (rack_id == RackCustomSubmixer && option) {
            voice.delay = MakeDelay(option);
        }
    }
    system->racks.push_back(rack);
    return rack;
}

void DestroyRack(Rack* rack) {
    System* system = rack->system;
    std::scoped_lock lock{system->mutex};
    std::erase(system->racks, rack);
    // Ports elsewhere that lead into the rack lead nowhere now.
    for (Rack* other : system->racks) {
        for (Voice& voice : other->voices) {
            for (Port& port : voice.ports) {
                if (port.dest && port.dest->rack == rack) {
                    port.dest = nullptr;
                }
            }
        }
    }
    for (Voice& voice : rack->voices) {
        if (voice.sampler && voice.sampler->at9) {
            Atrac9ReleaseHandle(voice.sampler->at9);
        }
    }
    delete rack;
}

u32 NumVoices(const Rack* rack) {
    return static_cast<u32>(rack->voices.size());
}

Voice* GetVoice(Rack* rack, u32 index) {
    return index < rack->voices.size() ? &rack->voices[index] : nullptr;
}

s32 Control(Voice* voice, const void* params, Resolver resolve) {
    if (!voice) {
        return ErrorInvalidVoice;
    }
    std::scoped_lock lock{voice->rack->system->mutex};
    voice->linked = !IsSampler(voice->rack);
    const u8* param = static_cast<const u8*>(params);
    while (param) {
        ControlOne(*voice, param, resolve);
        const s16 next = Read<ParamHeader>(param, 0).next;
        if (next == 0) {
            break;
        }
        param += next;
    }
    return 0;
}

s32 GetState(Voice* voice, void* out, size_t size) {
    if (!voice) {
        return ErrorInvalidVoice;
    }
    if (!out) {
        return ErrorInvalidOutAddress;
    }
    if (size < sizeof(u32)) {
        return ErrorInvalidStateSize;
    }
    std::scoped_lock lock{voice->rack->system->mutex};
    SamplerState state{};
    if (voice->sampler && voice->flags != 0) {
        state = voice->sampler->state;
    }
    state.flags = voice->flags;
    std::memset(out, 0, size);
    std::memcpy(out, &state, std::min(size, sizeof(state)));
    return 0;
}

s32 Render(System* system, const RenderBuffer* buffers, u32 count) {
    std::scoped_lock lock{system->mutex};
    for (u32 i = 0; i < count; ++i) {
        if (buffers[i].data) {
            std::memset(buffers[i].data, 0, buffers[i].size);
        }
    }
    // The inputs start empty; each mixing voice counts the mixing voices that feed it.
    for (Rack* rack : system->racks) {
        for (Voice& voice : rack->voices) {
            if (!voice.input.empty()) {
                std::fill(voice.input.begin(), voice.input.end(), 0.0f);
            }
            voice.pending = 0;
            voice.processed = false;
            BeginEvents(voice);
        }
    }
    for (Rack* rack : system->racks) {
        if (IsSampler(rack) || IsMastering(rack)) {
            continue;
        }
        for (Voice& voice : rack->voices) {
            for (u32 p = 0; p < rack->max_ports; ++p) {
                if (voice.ports[p].dest) {
                    ++voice.ports[p].dest->pending;
                }
            }
        }
    }
    for (Rack* rack : system->racks) {
        if (IsSampler(rack)) {
            for (Voice& voice : rack->voices) {
                if (voice.flags != 0) {
                    RenderSamplerVoice(*system, voice);
                }
            }
        }
    }
    // A mixing voice renders once every voice that feeds it has.
    bool progress = true;
    while (progress) {
        progress = false;
        for (Rack* rack : system->racks) {
            if (IsSampler(rack) || IsMastering(rack)) {
                continue;
            }
            for (Voice& voice : rack->voices) {
                if (voice.processed || voice.pending != 0) {
                    continue;
                }
                voice.processed = true;
                progress = true;
                RenderMixVoice(*system, voice);
                for (u32 p = 0; p < rack->max_ports; ++p) {
                    if (voice.ports[p].dest) {
                        --voice.ports[p].dest->pending;
                    }
                }
            }
        }
    }
    for (Rack* rack : system->racks) {
        if (IsMastering(rack)) {
            for (Voice& voice : rack->voices) {
                WriteOutput(*system, voice, buffers, count);
            }
        }
    }
    for (Rack* rack : system->racks) {
        for (Voice& voice : rack->voices) {
            if (voice.events != 0) {
                EndEvents(voice);
            }
        }
    }
    return 0;
}

} // namespace Libraries::Ngs2::Mixer
