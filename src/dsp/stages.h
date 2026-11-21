#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "apo_settings.h"
#include "dsp/dsp.h"

namespace apo::dsp {

// Shared implementation for the two standalone dynamics effects. The effect
// type fixes the transfer half at construction; the settings carry no mode.
// One linked peak detector drives compression/limiting above the threshold or
// expansion/gating below it. Input gain feeds the detector and the signal;
// output gain is applied after the dynamics. Optional lookahead delays the
// audio so the gain change can begin before a transient reaches the output.
class DynamicsStage final : public Stage
{
public:
    explicit DynamicsStage(DynamicsMode mode) : m_mode(mode) {}

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_delayFrames > 0;
    }
    double MonitorValue() const override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyCoefficients(const AudioFormat& format);

    DynamicsSettings m_settings;
    DynamicsMode m_mode = DynamicsMode::CompressorLimiter;
    double m_thresholdDb = 0.0;
    double m_inputGain = 1.0;
    double m_outputGain = 1.0;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    double m_gainDb = 0.0;

    // Latched gain change for Compressor/Limiter. Hold keeps the deepest
    // reduction active so a short peak still gets the full attack ramp.
    double m_heldTargetDb = 0.0;
    UINT32 m_holdFrames = 0;
    UINT32 m_holdCounter = 0;

    // Gate/Expander hold keeps the open state briefly after the signal falls
    // below the threshold. It is cancelled as soon as the signal returns.
    bool m_gateHoldActive = false;
    bool m_gateHoldSpent = false;

    // Delay line used by the lookahead. Sized for the maximum allowed lookahead
    // at Configure time so Update never allocates.
    std::vector<double> m_delay;
    UINT32 m_delayCapacityFrames = 0;
    UINT32 m_delayFrames = 0;
    UINT32 m_delayWrite = 0;
    UINT32 m_channels = 0;
};

// Frequency-selective equalizer for the proximity effect. A state-variable
// bandpass isolates a band around `frequencyHz`. `levelDb` always shifts that
// band; when Dynamic is Compressor or Expander, its envelope also cuts loud lows
// or lifts quiet lows. A broad Q behaves like a dynamic low shelf.
class EqualizerStage final : public Stage
{
public:
    EqualizerStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;
    double MonitorGainDb() const override;

private:
    void ApplyCoefficients(const AudioFormat& format);

    EqualizerSettings m_settings;
    double m_a1 = 0.0;
    double m_a2 = 0.0;
    double m_a3 = 0.0;
    double m_k = 1.0;
    double m_thresholdDb = 0.0;
    double m_thresholdLinear = 1.0;
    double m_ratio = 1.0;
    double m_rangeDb = 0.0;
    double m_levelDb = 0.0;
    EqualizerDynamicsMode m_mode = EqualizerDynamicsMode::None;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    double m_detectorAttack = 0.0;
    double m_detectorRelease = 0.0;
    double m_detectorEnvelope = 0.0;
    double m_gainDb = 0.0;
    double m_heldTargetDb = 0.0;

    // Expander Hold latches the lowest lift target for Hold + Lookahead; a
    // threshold crossing is not required to start it.
    double m_expandHeldTargetDb = 0.0;
    bool m_expandLatchActive = false;

    UINT32 m_holdFrames = 0;
    UINT32 m_holdCounter = 0;

    UINT32 m_delayCapacityFrames = 0;
    UINT32 m_delayFrames = 0;
    UINT32 m_delayWrite = 0;
    UINT32 m_channels = 0;
    std::vector<double> m_ic1;
    std::vector<double> m_ic2;
    std::vector<double> m_band;
    std::vector<double> m_delay;
    std::vector<double> m_delayBand;
};

// Flat gain, used both by the fixed Input row and by makeup Gain effects. The
// Input row can also copy channel 0 to every other channel; standalone Gain
// never does.
class GainStage final : public Stage
{
public:
    GainStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override {}
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyGain();

    GainSettings m_settings;
    double m_gain = 1.0;
    bool m_treatAsMono = false;
};

// Spectral noise reduction. The profile is measured once from the device and
// subtracted from the short-time spectrum; a linked gain keeps the channel
// image intact. The stage delays the stream by about one FFT window.
class NoiseStage final : public Stage
{
public:
    explicit NoiseStage(const NoiseProfile& profile);

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    // Replaces the analysis bins from a live capture without reallocating.
    void SetProfileBins(const std::vector<double>& magnitudes,
                        std::size_t binCount, uint32_t sampleRate);
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_hasProfile;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyProfileBins(const std::vector<double>& magnitudes,
                          std::size_t binCount, uint32_t sampleRate);
    void ProcessHop();

    NoiseSettings m_settings;
    NoiseProfile m_profile;
    bool m_hasProfile = false;
    std::size_t m_sizeIndex = 0;
    size_t m_fftSize = static_cast<size_t>(apo::kNoiseDefaultFftSize);
    size_t m_hopSize = m_fftSize / 2;
    std::vector<FftPlan> m_fftPlans;
    std::vector<std::vector<double>> m_windows;
    std::vector<double> m_profileBins;
    std::vector<double> m_gain;
    std::vector<double> m_rawGain;
    double m_floorGain = 0.25;
    double m_gainAttack = 0.5;
    double m_gainRelease = 0.1;
    double m_sampleRate = 44100.0;

    UINT32 m_channels = 0;
    UINT32 m_maxFrames = 0;
    UINT32 m_inputFrames = 0;
    UINT32 m_outputFrames = 0;

    std::vector<double> m_input;
    std::vector<double> m_output;
    std::vector<double> m_history;
    std::vector<double> m_ola;
    std::vector<double> m_spectraReal;
    std::vector<double> m_spectraImag;
    std::vector<double> m_frameReal;
    std::vector<double> m_frameImag;
};

// Soft saturation with a selectable curve: tanh, arctan, cubic polynomial,
// logarithmic, exponential, or error function. `driveDb` sets the level into
// the curve, `curvature` how hard it bends, and `outputDb` the level after it.
// Every curve is odd and maps full scale to full scale, so there is no DC
// offset to remove. The curve math lives in dsp/transfer.h so the GUI can draw
// the same shape.
class DistortionStage final : public Stage
{
public:
    DistortionStage() = default;

    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    double Shape(double value) const;

    DistortionSettings m_settings;
    // Curve coefficients derived from the function and curvature in Update, so
    // the real-time path only evaluates the curve itself.
    DistortionCurve m_curve;
    double m_drive = 1.0;
    double m_output = 1.0;
};

// Digital reverb with two switchable engines: classic parallel combs into
// series allpasses, or a feedback delay network with a mixing matrix. Both
// share predelay, wet level, damping, and stereo output shaping. The dry path
// always stays at unity; `level` only changes the wet return.
class ReverbStage final : public Stage
{
public:
    ReverbStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_settings.level > 0.0;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    static constexpr size_t kFdnLineCount = 8;

    struct Comb
    {
        std::vector<double> buffer;
        size_t index = 0;
        double store = 0.0;
    };

    struct Allpass
    {
        std::vector<double> buffer;
        size_t index = 0;
    };

    struct Tank
    {
        std::vector<Comb> combs;
        std::vector<Allpass> allpasses;
    };

    struct FdnLine
    {
        std::vector<double> buffer;
        size_t index = 0;
        double store = 0.0;
    };

    void ApplyCoefficients();
    double ProcessComb(Comb& comb, double input) const;
    double ProcessAllpass(Allpass& allpass, double input) const;
    void ProcessFdn(double excitation, double& left, double& right);

    ReverbSettings m_settings;
    std::vector<Tank> m_tanks;
    std::vector<FdnLine> m_fdnLines;
    double m_fdnMatrix[kFdnLineCount * kFdnLineCount]{};
    double m_fdnLeftTap[kFdnLineCount]{};
    double m_fdnRightTap[kFdnLineCount]{};
    std::vector<double> m_predelay;
    std::vector<double> m_wetLow;
    size_t m_predelayCapacityFrames = 0;
    size_t m_predelayFrames = 0;
    size_t m_predelayWrite = 0;
    UINT32 m_channels = 0;
    double m_sampleRate = 44100.0;
    double m_combAverageSeconds = 0.03;
    double m_fdnAverageSeconds = 0.04;
    double m_feedback = 0.0;
    double m_allpassFeedback = 0.5;
    double m_damping = 0.0;
    double m_fdnGain = 0.0;
    double m_monoMix = 0.5;
    double m_level = 0.0;
    double m_size = 0.5;
    double m_width = 0.5;
    double m_density = 0.5;
    double m_lowGain = 1.0;
    double m_wetLowCoeff = 0.05;
    double m_wetScale = 0.0;
    double m_fdnLeft = 0.0;
    double m_fdnRight = 0.0;
};

// Stereo/ping-pong echo. The dry path stays at unity and the wet return is a
// damped feedback delay line. All delay memory is allocated in Configure(); a
// live Time change only moves the read offset.
class EchoStage final : public Stage
{
public:
    EchoStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_level > 0.0;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyCoefficients();

    EchoSettings m_settings;
    std::vector<double> m_delay;
    std::vector<double> m_feedbackLow;
    size_t m_capacityFrames = 0;
    size_t m_delayFrames = 1;
    size_t m_write = 0;
    UINT32 m_channels = 0;
    double m_sampleRate = 48000.0;
    double m_feedback = 0.0;
    double m_dampPole = 0.0;
    double m_level = 0.0;
};

// Modulated fractional delays summed back into the dry signal. Each voice owns
// a fixed delay buffer for the stream; a live Voices change only selects how
// many are mixed, and Width spreads their LFO phases across channels.
class ChorusStage final : public Stage
{
public:
    ChorusStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_level > 0.0;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    struct Voice
    {
        std::vector<double> buffer;
        double phase = 0.0;
    };

    void ApplyCoefficients();
    double Read(const Voice& voice, UINT32 channel, double position) const;

    ChorusSettings m_settings;
    std::vector<Voice> m_voices;
    std::vector<double> m_wet;
    std::vector<double> m_input;
    size_t m_capacityFrames = 0;
    size_t m_write = 0;
    UINT32 m_channels = 0;
    double m_sampleRate = 48000.0;
    double m_baseDelayFrames = 0.0;
    double m_modDepthFrames = 0.0;
    double m_phaseStep = 0.0;
    double m_voiceSpread = 0.0;
    double m_channelSpread = 0.0;
    double m_level = 0.0;
    int m_activeVoices = apo::kChorusDefaultVoices;
};

// Level modulation with a shared LFO shape. Rate sets the cycle speed, Depth
// sets how far the level drops, and Stereo spread offsets the right channel's
// LFO so the pulsation moves between speakers. Depth 0 leaves the audio alone.
class TremoloStage final : public Stage
{
public:
    TremoloStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    struct ChannelState
    {
        double phase = 0.0;
        double gain = 1.0;
    };

    void ApplyCoefficients();

    TremoloSettings m_settings;
    std::vector<ChannelState> m_state;
    double m_phaseStep = 0.0;
    double m_spreadCycles = 0.0;
    double m_depth = 0.0;
    double m_smoothCoeff = 0.0;
    UINT32 m_channels = 0;
    double m_sampleRate = 48000.0;
};

// Pitch modulation driven by the same LFO shape. The fractional delay is
// integrated from the requested pitch ratio, so Square holds a sustained
// pitch offset for each half-cycle instead of only bending during transitions.
// Depth 0 passes the input through while keeping the delay memory current.
class VibratoStage final : public Stage
{
public:
    VibratoStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_settings.depthCents > 0.0;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    struct DelayChannel
    {
        std::vector<double> buffer;
        size_t write = 0;
        double phase = 0.0;
        double delay = 0.0;
    };

    void ApplyCoefficients();
    double Read(const DelayChannel& channel, double position) const;

    VibratoSettings m_settings;
    std::vector<DelayChannel> m_channelsState;
    size_t m_capacityFrames = 0;
    double m_baseDelay = 0.0;
    double m_phaseStep = 0.0;
    double m_spreadCycles = 0.0;
    double m_ratioHigh = 1.0;
    double m_ratioLow = 1.0;
    double m_ratioMean = 1.0;
    double m_squareDuty = 0.5;
    UINT32 m_channels = 0;
    double m_sampleRate = 48000.0;
};

// Pitch shifting with three engines sharing one preallocated sample history.
// Granular uses two crossfading grains, WSOLA adds a bounded waveform-similarity
// search at each grain restart, and the phase vocoder uses radix-2 FFT plans
// created at Configure time. No engine allocates in Update() or Process().
class PitchShifterStage final : public Stage
{
public:
    PitchShifterStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_level > 0.0;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    static constexpr size_t kGrainCount = 2;

    struct GrainChannel
    {
        std::vector<double> input;
        size_t write = 0;
        double position[kGrainCount]{};
        double phase[kGrainCount]{};
    };

    struct VocoderChannel
    {
        std::vector<double> input;
        std::vector<double> overlap;
        // Analysis phase is tracked per input bin; synthesis phase is
        // accumulated per output bin. They cannot share one array.
        std::vector<double> lastPhase;
        std::vector<double> synthPhase;
        std::vector<double> magnitudes;
        // Magnitude-weighted target bin and source phase for each synthesis
        // bin, plus a count so an empty bin can re-lock cleanly.
        std::vector<double> frequencySum;
        std::vector<double> phaseReal;
        std::vector<double> phaseImag;
        std::vector<int> targetCount;
        std::vector<unsigned char> phaseValid;
        size_t write = 0;
        size_t overlapWrite = 0;
        size_t frameCounter = 0;
    };

    void ApplyCoefficients();
    void ResetGrain(GrainChannel& channel) const;
    void ResetVocoder(VocoderChannel& channel) const;
    double ReadGrain(const GrainChannel& channel, double position) const;
    double GrainMaxLag() const;
    double LimitedPosition(const GrainChannel& channel, double position,
                           double writePosition) const;
    double SearchAlignment(const GrainChannel& channel, double nominal,
                           double reference, double writePosition) const;
    double ProcessGrain(GrainChannel& channel, double input, bool wsola);
    void AnalyzeVocoder(VocoderChannel& channel, size_t writeIndex);
    double ProcessVocoder(VocoderChannel& channel, double input);

    PitchShifterSettings m_settings;
    std::vector<GrainChannel> m_grainChannels;
    std::vector<VocoderChannel> m_vocoderChannels;
    std::vector<std::unique_ptr<FftPlan>> m_fftPlans;
    std::vector<std::vector<double>> m_windows;
    std::vector<double> m_real;
    std::vector<double> m_imag;
    std::vector<double> m_wet;
    std::vector<double> m_inputFrame;
    UINT32 m_channels = 0;
    double m_sampleRate = 48000.0;
    double m_ratio = 0.5;
    double m_level = 0.5;
    double m_dryLevel = 1.0;
    double m_grainPhaseStep = 0.0;
    double m_synthPhaseStep = 0.0;
    size_t m_maxWindow = apo::kPitchMaxWindowSamples;
    size_t m_maxFft = 16384;
    size_t m_windowSamples = apo::kPitchDefaultWindowSamples;
    size_t m_fftSize = 4096;
    size_t m_grainHopFrames = 2048;
    size_t m_vocoderHopFrames = 1024;
    int m_fftIndex = 3;
};

// Monitoring-only pitch detector. It copies the mono sum of the incoming audio
// into a preallocated analysis window, detects a pitch with the selected engine,
// and publishes frequency/clarity through the stage monitor. Samples are never
// modified, so the Tuner adds no gain, filtering, or latency.
class TunerStage final : public Stage
{
public:
    TunerStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    void MonitorTuner(TunerMonitorData& data) const override
    {
        data = m_monitor;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ResetAnalysis();
    bool DetectAutocorrelation(double& frequencyHz, double& confidence);
    bool DetectFftPeak(double& frequencyHz, double& confidence);
    void ApplyDetection(double frequencyHz, double confidence);

    TunerSettings m_settings;
    double m_sampleRate = 48000.0;
    double m_highPassCoeff = 0.99;
    double m_highPassInput = 0.0;
    double m_highPassOutput = 0.0;
    std::size_t m_sampleSize = static_cast<std::size_t>(kTunerDefaultSampleSize);
    std::size_t m_filled = 0;
    std::vector<double> m_window;
    std::vector<double> m_mono;
    std::vector<double> m_decimated;
    std::vector<double> m_lagValues;
    std::vector<double> m_real;
    std::vector<double> m_imag;
    std::vector<FftPlan> m_fftPlans;
    double m_lastMidi = 0.0;
    bool m_hasLastMidi = false;
    double m_timeSeconds = 0.0;
    std::vector<double> m_averageMidi;
    std::vector<double> m_averageTime;
    std::size_t m_averageWrite = 0;
    std::size_t m_averageCount = 0;
    TunerMonitorData m_monitor;
};

// Five independent loop tracks over one shared undo bank. Track buffers, the
// undo bank and provisional-gesture snapshots are allocated once when the stage
// pool is configured, so Process() never allocates. The first synced take sets a
// shared reference length. Later synced takes write into one fixed span for the
// whole take and choose the nearest allowed multiple only when recording finishes, so
// the write cursor never jumps when a predicted snap length changes. Free
// tracks keep their own exact-length playhead.
class LooperStage final : public Stage
{
public:
    void SetStorageId(const std::wstring& endpointId)
    {
        m_storageId = endpointId;
    }
    // Writes the committed takes before the APO tears its stage pool down.
    void SaveBank();
    // Restores a saved take bank after Configure() allocated fresh buffers.
    bool LoadBank();
    // Re-establishes the command baseline after Chain::Reset() clears it. The
    // first gesture after an APO restart must not look like a stale command.
    void PrimeCommandSerial();
    // Same, for a live rebind: the incoming settings are not in m_settings yet.
    void PrimeCommandSerial(const LooperSettings& settings);

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override;
    void MonitorLooper(LooperMonitorData& data) const override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    struct TrackState
    {
        LooperState state = LooperState::Empty;
        std::size_t length = 0;
        std::size_t position = 0;
        std::size_t recordPosition = 0;
        std::uint64_t recordFrames = 0;
        // Open-recording state. `writeSpan` is fixed when the take starts;
        // `writeBase` and `recordStartGlobal` let FinishRecord() remap the ring
        // into the final snapped loop. `startDelayFrames` waits for a snapped
        // reference boundary instead of writing a few early frames at the end.
        std::size_t writeSpan = 0;
        std::uint64_t writeBase = 0;
        std::uint64_t recordStartGlobal = 0;
        std::uint64_t startDelayFrames = 0;
        // Playback origin for this synced track. It is chosen when the take
        // finishes so a 2x loop still starts at the left edge of its bar.
        std::uint64_t phaseOffset = 0;
        bool synced = true;
        bool overdub = false;
        bool pendingStop = false;
        bool isReference = false;
    };

    void ClearAll();
    void ClearTrack(int track);
    void SetReference(int track, std::size_t masterFrames,
                      std::uint64_t syncOrigin);
    void ClearReference();
    void SaveProvisionalSnapshot(int track);
    void RestoreProvisionalSnapshot(int track);
    void ApplyCommand(LooperCommand command, int track);
    void StartRecord(int track);
    void FinishRecord(int track);
    std::size_t Phase(int track, std::size_t length) const;
    std::size_t SyncedWriteSpan() const;
    std::uint64_t StartSnapDelay() const;
    std::size_t SnapSyncedFrames(std::uint64_t recordedFrames) const;
    std::size_t VisualSyncedFrames(std::uint64_t recordedFrames) const;
    std::size_t RecordingTarget(const TrackState& state) const;
    void RemapSyncedTake(int track, TrackState& state,
                         std::size_t finalLength);

    LooperSettings m_settings;
    std::wstring m_storageId;
    double m_sampleRate = 48000.0;
    UINT32 m_channels = 0;
    std::size_t m_maxFrames = 0;
    std::array<TrackState, apo::kLooperTrackCount> m_tracks{};
    std::array<std::vector<float>, apo::kLooperTrackCount> m_buffers{};
    std::vector<float> m_undoBuffer;
    // Scratch space for the one-time ring-to-loop copy at end of synced record.
    std::vector<float> m_remapBuffer;
    std::vector<double> m_inputFrame;
    std::vector<double> m_layerFrame;
    int m_undoTrack = -1;
    std::size_t m_undoLength = 0;
    LooperState m_undoState = LooperState::Empty;
    int m_undoReferenceTrack = -1;
    std::size_t m_undoMasterFrames = 0;
    std::uint64_t m_undoSyncOrigin = 0;
    // The previous take's playback origin must be restored along with its
    // samples and shared reference; otherwise undo can shift its alignment.
    std::uint64_t m_undoPhaseOffset = 0;
    bool m_undoValid = false;
    std::uint64_t m_globalFrames = 0;
    std::uint64_t m_syncOrigin = 0;
    std::size_t m_masterFrames = 0;
    int m_referenceTrack = -1;
    bool m_hasCommandSerial = false;
    std::uint64_t m_lastCommandSerial = 0;

    // A keyboard press starts a take before the 250 ms double-tap window closes.
    // Keep what that one press can change so a later double tap or long press
    // can discard it exactly. Discarding is scoped to the gesture's track; the
    // global frame clock and other playing tracks must never be rolled back.
    bool m_provisional = false;
    int m_provisionalTrack = -1;
    std::array<TrackState, apo::kLooperTrackCount> m_preProvisionalTracks{};
    std::vector<float> m_preProvisionalTrackBuffer;
    std::vector<float> m_preProvisionalUndoBuffer;
    int m_preProvisionalReferenceTrack = -1;
    std::size_t m_preProvisionalMasterFrames = 0;
    std::uint64_t m_preProvisionalSyncOrigin = 0;
    bool m_preProvisionalUndoValid = false;
    int m_preProvisionalUndoTrack = -1;
    std::size_t m_preProvisionalUndoLength = 0;
    LooperState m_preProvisionalUndoState = LooperState::Empty;
    int m_preProvisionalUndoReferenceTrack = -1;
    std::size_t m_preProvisionalUndoMasterFrames = 0;
    std::uint64_t m_preProvisionalUndoSyncOrigin = 0;
    std::uint64_t m_preProvisionalUndoPhaseOffset = 0;
};

// Owns every stage instance the effect list can reference. Configure() runs
// from LockForProcess and allocates a fixed number of stages per type, including
// Looper's large buffers even when the preset has no Looper. Update() runs from
// the real-time path, binds instances by id, retunes them in place and fills a
// non-owning Chain without allocating, loading banks or touching disk.
class StagePool
{
public:
    void Configure(const AudioFormat& format, const NoiseProfile& profile,
                   const ApoSettings& settings,
                   const std::wstring& endpointId);
    void Clear();
    // Saves every allocated Looper stage before Clear() destroys them.
    void SaveLooperBanks();
    // Restores banks on the setup path after the chain's first build and reset.
    // Live binds now preserve the loaded stage state instead of resetting it.
    void LoadLooperBanks();
    // Returns the number of active stages bound into `chain`.
    int Update(const ApoSettings& settings, const AudioFormat& format,
               Chain& chain);
    // Retunes every allocated noise stage from a live capture. No allocation.
    void UpdateProfileBins(const NoiseProfile& profile);

    bool Empty() const;

private:
    struct Slots
    {
        std::vector<std::unique_ptr<Stage>> stages;
        std::vector<int> boundIds;
    };

    Slots m_slots[static_cast<int>(EffectType::Count)];
    std::wstring m_looperStorageId;
};

} // namespace apo::dsp
