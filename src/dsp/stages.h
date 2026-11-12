#pragma once

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
    explicit GateStage(const GateSettings& settings) : m_settings(settings) {}

    void Configure(const AudioFormat& format) override;
    void Update(const ApoSettings& settings, const AudioFormat& format) override;
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
    explicit CompressorStage(const CompressorSettings& settings)
        : m_settings(settings)
    {
    }

    void Configure(const AudioFormat& format) override;
    void Update(const ApoSettings& settings, const AudioFormat& format) override;
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
    explicit DynamicStage(const DynamicSettings& settings)
        : m_settings(settings)
    {
    }

    void Configure(const AudioFormat& format) override;
    void Update(const ApoSettings& settings, const AudioFormat& format) override;
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
    explicit GainStage(const ApoSettings& settings) : m_settings(settings) {}

    void Configure(const AudioFormat& format) override;
    void Update(const ApoSettings& settings, const AudioFormat& format) override;
    void Reset() override {}
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ApplyGain();

    ApoSettings m_settings;
    double m_gain = 1.0;
};

// Spectral noise reduction. The profile is measured once from the device and
// subtracted from the short-time spectrum; a linked gain keeps the channel
// image intact. The stage delays the stream by about one FFT window.
class NoiseStage final : public Stage
{
public:
    NoiseStage(const NoiseSettings& settings, const NoiseProfile& profile);

    void Configure(const AudioFormat& format) override;
    void Update(const ApoSettings& settings, const AudioFormat& format) override;
    void Reset() override;
    bool NeedsSilence() const override { return m_settings.enabled; }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    void ProcessHop();

    NoiseSettings m_settings;
    NoiseProfile m_profile;
    FftPlan m_fft;
    std::vector<double> m_window;
    std::vector<double> m_profileBins;
    std::vector<double> m_gain;
    std::vector<double> m_rawGain;
    double m_floorGain = 0.25;
    double m_gainAttack = 0.5;
    double m_gainRelease = 0.1;

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
    explicit MonoStage(bool enabled) : m_enabled(enabled) {}

    void Update(const ApoSettings& settings, const AudioFormat& format) override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    bool m_enabled = false;
};

} // namespace apo::dsp
