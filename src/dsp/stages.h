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
    void Reset() override;
    bool NeedsSilence() const override { return m_delayFrames > 0; }
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    GateSettings m_settings;
    double m_thresholdLinear = 1.0;
    double m_floorGain = 1.0;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    UINT32 m_holdFrames = 0;
    UINT32 m_holdCounter = 0;
    bool m_open = true;
    double m_gain = 1.0;

    // Delay line used by the lookahead. Empty when lookahead is zero.
    std::vector<double> m_delay;
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
    void Reset() override;
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    CompressorSettings m_settings;
    double m_thresholdLinear = 1.0;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    double m_envelopeDb = 0.0;
};

// Flat gain, used for makeup gain.
class GainStage final : public Stage
{
public:
    explicit GainStage(double gainDb) : m_gainDb(gainDb) {}

    void Configure(const AudioFormat& format) override;
    void Reset() override {}
    void Process(double* samples, UINT32 frames, UINT32 channels) override;

private:
    double m_gainDb = 0.0;
    double m_gain = 1.0;
};

// Copies the processed left channel to every other channel.
class MonoStage final : public Stage
{
public:
    void Process(double* samples, UINT32 frames, UINT32 channels) override;
};

} // namespace apo::dsp
