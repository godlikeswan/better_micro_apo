#include "dsp/stages.h"

#include <cmath>

namespace apo::dsp {
namespace {

double PeakOfFrame(const double* frame, UINT32 channels)
{
    double peak = 0.0;
    for (UINT32 channel = 0; channel < channels; ++channel)
    {
        const double magnitude = std::fabs(frame[channel]);
        if (magnitude > peak)
        {
            peak = magnitude;
        }
    }
    return peak;
}

double EffectiveSampleRate(const AudioFormat& format)
{
    return (format.sampleRate > 0) ? static_cast<double>(format.sampleRate)
                                   : 48000.0;
}

} // namespace

void GateStage::Configure(const AudioFormat& format)
{
    const double sampleRate = EffectiveSampleRate(format);

    m_thresholdLinear = DecibelsToLinear(m_settings.thresholdDb);
    m_floorGain = DecibelsToLinear(-m_settings.rangeDb);
    m_attackCoeff = TimeCoefficient(m_settings.attackMs, sampleRate);
    m_releaseCoeff = TimeCoefficient(m_settings.releaseMs, sampleRate);

    const double holdSeconds = m_settings.holdMs * 0.001;
    m_holdFrames = static_cast<UINT32>(holdSeconds * sampleRate + 0.5);

    Reset();
}

void GateStage::Reset()
{
    m_holdCounter = 0;
    m_open = true;
    m_gain = 1.0;
}

void GateStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;
        const double peak = PeakOfFrame(values, channels);

        if (peak >= m_thresholdLinear)
        {
            m_holdCounter = m_holdFrames;
            m_open = true;
        }
        else if (m_holdCounter > 0)
        {
            --m_holdCounter;
            m_open = true;
        }
        else
        {
            m_open = false;
        }

        const double target = m_open ? 1.0 : m_floorGain;
        const double coefficient =
            (target > m_gain) ? m_attackCoeff : m_releaseCoeff;
        m_gain += (target - m_gain) * coefficient;

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            values[channel] *= m_gain;
        }
    }
}

void CompressorStage::Configure(const AudioFormat& format)
{
    const double sampleRate = EffectiveSampleRate(format);

    m_thresholdLinear = DecibelsToLinear(m_settings.thresholdDb);
    m_attackCoeff = TimeCoefficient(m_settings.attackMs, sampleRate);
    m_releaseCoeff = TimeCoefficient(m_settings.releaseMs, sampleRate);

    Reset();
}

void CompressorStage::Reset()
{
    m_envelopeDb = 0.0;
}

void CompressorStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    const double ratio = (m_settings.ratio > 0.0) ? m_settings.ratio : 1.0;

    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;
        const double peak = PeakOfFrame(values, channels);

        double targetDb = 0.0;
        if (peak > m_thresholdLinear)
        {
            const double levelDb = 20.0 * std::log10(peak);
            targetDb =
                (m_settings.thresholdDb - levelDb) * (1.0 - 1.0 / ratio);
            if (targetDb > 0.0)
            {
                targetDb = 0.0;
            }
        }

        // Attack when we need more reduction, release when we need less.
        const double coefficient =
            (targetDb < m_envelopeDb) ? m_attackCoeff : m_releaseCoeff;
        m_envelopeDb += (targetDb - m_envelopeDb) * coefficient;

        const double gain = DecibelsToLinear(m_envelopeDb);
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            values[channel] *= gain;
        }
    }
}

void GainStage::Configure(const AudioFormat& format)
{
    (void)format;
    m_gain = DecibelsToLinear(m_gainDb);
}

void GainStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    const size_t count = static_cast<size_t>(frames) * channels;
    for (size_t i = 0; i < count; ++i)
    {
        samples[i] *= m_gain;
    }
}

void MonoStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (channels < 2)
    {
        return;
    }

    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;
        const double left = values[0];
        for (UINT32 channel = 1; channel < channels; ++channel)
        {
            values[channel] = left;
        }
    }
}

} // namespace apo::dsp
