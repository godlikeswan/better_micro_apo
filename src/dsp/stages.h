#pragma once

#include <memory>
#include <vector>

#include "apo_settings.h"
#include "dsp/dsp.h"

namespace apo::dsp {

// Downward gate with a linked peak detector. Below the threshold the signal is
// attenuated by at most `rangeDb`; `holdMs` keeps it open after the level
// falls, and attack/release smooth the gain changes. Optional lookahead delays
// the audio while the detector watches the incoming signal, so the gate can
// open before a transient reaches the output.
class GateStage final : public Stage
{
public:
    GateStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_delayFrames > 0;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyCoefficients(const AudioFormat& format);

    GateSettings m_settings;
    double m_thresholdLinear = 1.0;
    double m_floorGain = 1.0;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    UINT32 m_holdFrames = 0;
    UINT32 m_holdCounter = 0;
    bool m_open = true;
    double m_gain = 1.0;

    // Delay line used by the lookahead. Sized for the maximum allowed lookahead
    // at Configure time so Update never allocates.
    std::vector<double> m_delay;
    UINT32 m_delayCapacityFrames = 0;
    UINT32 m_delayFrames = 0;
    UINT32 m_delayWrite = 0;
    UINT32 m_channels = 0;
};

// Linked peak compressor. The detector uses the loudest channel of each frame,
// so a stereo source keeps its image.
class CompressorStage final : public Stage
{
public:
    CompressorStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyCoefficients(const AudioFormat& format);

    CompressorSettings m_settings;
    double m_thresholdLinear = 1.0;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    double m_envelopeDb = 0.0;
};

// Frequency-selective dynamics for the proximity effect. A state-variable
// bandpass isolates a band around `frequencyHz`; its envelope drives a cut of
// loud lows (compress) or a boost of quiet lows (expand) applied back to that
// band only. A broad Q behaves like a dynamic low shelf.
class DynamicStage final : public Stage
{
public:
    DynamicStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyCoefficients(const AudioFormat& format);

    DynamicSettings m_settings;
    double m_a1 = 0.0;
    double m_a2 = 0.0;
    double m_a3 = 0.0;
    double m_k = 1.0;
    double m_thresholdDb = 0.0;
    double m_thresholdLinear = 1.0;
    double m_ratio = 1.0;
    double m_rangeDb = 0.0;
    bool m_expand = false;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    double m_detectorAttack = 0.0;
    double m_detectorRelease = 0.0;
    double m_detectorEnvelope = 0.0;
    double m_gainDb = 0.0;
    UINT32 m_channels = 0;
    std::vector<double> m_ic1;
    std::vector<double> m_ic2;
    std::vector<double> m_band;
};

// Flat gain, used for makeup gain.
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

// Copies the processed left channel to every other channel.
class MonoStage final : public Stage
{
public:
    MonoStage() = default;

    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    bool m_enabled = false;
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

// Room/hall reverb built from the Freeverb comb/allpass network. It runs last
// so a mono source copied to both channels gets a wide stereo tail. `mix`
// crossfades dry and wet, `size` sets the decay, `damping` absorbs highs in the
// tail, and `predelayMs` delays the excitation.
class ReverbStage final : public Stage
{
public:
    ReverbStage() = default;

    void Configure(const AudioFormat& format) override;
    void Update(const EffectInstance& effect, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override
    {
        return m_settings.enabled && m_settings.mix > 0.0;
    }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
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

    void ApplyCoefficients();
    double ProcessComb(Comb& comb, double input) const;
    double ProcessAllpass(Allpass& allpass, double input) const;

    ReverbSettings m_settings;
    std::vector<Tank> m_tanks;
    std::vector<double> m_predelay;
    size_t m_predelayCapacityFrames = 0;
    size_t m_predelayFrames = 0;
    size_t m_predelayWrite = 0;
    UINT32 m_channels = 0;
    double m_sampleRate = 44100.0;
    double m_feedback = 0.0;
    double m_damping = 0.0;
    double m_wetGain = 0.0;
    double m_dryGain = 1.0;
};

// Owns every stage instance the effect list can reference. Configure() runs
// from LockForProcess and allocates a fixed number of stages per type; Update()
// runs from the real-time path, binds instances by id, retunes them in place and
// fills a non-owning Chain without allocating.
class StagePool
{
public:
    void Configure(const AudioFormat& format, const NoiseProfile& profile);
    void Clear();
    void Update(const ApoSettings& settings, const AudioFormat& format,
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
};

} // namespace apo::dsp
