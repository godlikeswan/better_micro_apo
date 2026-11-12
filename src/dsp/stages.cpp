#include "dsp/stages.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    m_channels = format.channels;

    // Reserve the largest lookahead the settings clamp allows, so a live
    // Update can lengthen the delay without allocating.
    constexpr double kMaxLookaheadMs = 300.0;
    const double sampleRate = EffectiveSampleRate(format);
    m_delayCapacityFrames =
        static_cast<UINT32>(kMaxLookaheadMs * 0.001 * sampleRate + 0.5);
    if (m_delayCapacityFrames > 0 && m_channels > 0)
    {
        m_delay.assign(
            static_cast<size_t>(m_delayCapacityFrames) * m_channels, 0.0);
    }
    else
    {
        m_delay.clear();
        m_delayCapacityFrames = 0;
    }

    ApplyCoefficients(format);
    Reset();
}

void GateStage::Update(const ApoSettings& settings, const AudioFormat& format)
{
    const bool wasEnabled = m_settings.enabled;
    const double oldLookahead = m_settings.lookaheadMs;
    m_settings = settings.gate;
    ApplyCoefficients(format);

    // Changing the delay length or bypass state invalidates the buffered tail.
    if (wasEnabled != m_settings.enabled ||
        oldLookahead != m_settings.lookaheadMs)
    {
        Reset();
    }
}

void GateStage::ApplyCoefficients(const AudioFormat& format)
{
    const double sampleRate = EffectiveSampleRate(format);

    m_thresholdLinear = DecibelsToLinear(m_settings.thresholdDb);
    m_floorGain = DecibelsToLinear(-m_settings.rangeDb);
    m_attackCoeff = TimeCoefficient(m_settings.attackMs, sampleRate);
    m_releaseCoeff = TimeCoefficient(m_settings.releaseMs, sampleRate);

    const double lookaheadSeconds = m_settings.lookaheadMs * 0.001;
    UINT32 delayFrames =
        static_cast<UINT32>(lookaheadSeconds * sampleRate + 0.5);
    if (delayFrames > m_delayCapacityFrames)
    {
        delayFrames = m_delayCapacityFrames;
    }
    m_delayFrames = delayFrames;

    // The detector runs ahead of the delayed audio, so extend the hold by the
    // lookahead to keep the same hold time at the output.
    const double holdSeconds = m_settings.holdMs * 0.001;
    m_holdFrames = static_cast<UINT32>(holdSeconds * sampleRate + 0.5) +
                   m_delayFrames;
}

void GateStage::Reset()
{
    m_holdCounter = 0;
    m_open = true;
    m_gain = 1.0;
    m_delayWrite = 0;
    std::fill(m_delay.begin(), m_delay.end(), 0.0);
}

void GateStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled)
    {
        return;
    }

    const bool lookahead =
        m_delayFrames > 0 && channels == m_channels &&
        m_delay.size() >= static_cast<size_t>(m_delayFrames) * channels;

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

        if (lookahead)
        {
            // The detector ran on the incoming frame; apply the resulting gain
            // to the frame that is one lookahead behind it.
            double* delayed =
                m_delay.data() + static_cast<size_t>(m_delayWrite) * channels;
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                const double current = values[channel];
                values[channel] = delayed[channel] * m_gain;
                delayed[channel] = current;
            }

            if (++m_delayWrite >= m_delayFrames)
            {
                m_delayWrite = 0;
            }
        }
        else
        {
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                values[channel] *= m_gain;
            }
        }
    }
}

void CompressorStage::Configure(const AudioFormat& format)
{
    ApplyCoefficients(format);
    Reset();
}

void CompressorStage::Update(const ApoSettings& settings,
                             const AudioFormat& format)
{
    const bool wasEnabled = m_settings.enabled;
    m_settings = settings.compressor;
    ApplyCoefficients(format);
    if (wasEnabled != m_settings.enabled)
    {
        Reset();
    }
}

void CompressorStage::ApplyCoefficients(const AudioFormat& format)
{
    const double sampleRate = EffectiveSampleRate(format);

    m_thresholdLinear = DecibelsToLinear(m_settings.thresholdDb);
    m_attackCoeff = TimeCoefficient(m_settings.attackMs, sampleRate);
    m_releaseCoeff = TimeCoefficient(m_settings.releaseMs, sampleRate);
}

void CompressorStage::Reset()
{
    m_envelopeDb = 0.0;
}

void CompressorStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled)
    {
        return;
    }

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

void DynamicStage::Configure(const AudioFormat& format)
{
    m_channels = format.channels;
    m_ic1.assign(m_channels, 0.0);
    m_ic2.assign(m_channels, 0.0);
    m_band.assign(m_channels, 0.0);

    ApplyCoefficients(format);
    Reset();
}

void DynamicStage::Update(const ApoSettings& settings,
                          const AudioFormat& format)
{
    const bool wasEnabled = m_settings.enabled;
    const double oldFrequency = m_settings.frequencyHz;
    const double oldQ = m_settings.q;

    m_settings = settings.dynamics;
    ApplyCoefficients(format);

    // Changing the filter shape invalidates the filter state.
    if (wasEnabled != m_settings.enabled ||
        oldFrequency != m_settings.frequencyHz || oldQ != m_settings.q)
    {
        Reset();
    }
}

void DynamicStage::ApplyCoefficients(const AudioFormat& format)
{
    constexpr double kPi = 3.14159265358979323846;
    const double sampleRate = EffectiveSampleRate(format);

    // Topology-preserving state-variable filter (Simper). `k` is the damping
    // and equals 1/Q; the bandpass output `v1` has a peak gain of Q, so it is
    // scaled by k to normalize the band to unity at the center frequency.
    double frequency = m_settings.frequencyHz;
    const double maxFrequency = sampleRate * 0.49;
    frequency = std::max(20.0, std::min(frequency, maxFrequency));
    const double q = std::max(0.1, m_settings.q);
    const double g = std::tan(kPi * frequency / sampleRate);
    m_k = 1.0 / q;
    m_a1 = 1.0 / (1.0 + g * (g + m_k));
    m_a2 = g * m_a1;
    m_a3 = g * m_a2;

    m_thresholdDb = m_settings.thresholdDb;
    m_thresholdLinear = DecibelsToLinear(m_thresholdDb);
    m_ratio = std::max(1.0, m_settings.ratio);
    m_rangeDb = std::max(0.0, m_settings.rangeDb);
    m_expand = m_settings.expand;
    m_attackCoeff = TimeCoefficient(m_settings.attackMs, sampleRate);
    m_releaseCoeff = TimeCoefficient(m_settings.releaseMs, sampleRate);

    // A separate fast-attack / slow-release envelope smooths the band peak so
    // the gain does not pulse at the signal's own zero crossings.
    m_detectorAttack = TimeCoefficient(1.0, sampleRate);
    m_detectorRelease = TimeCoefficient(100.0, sampleRate);
}

void DynamicStage::Reset()
{
    m_gainDb = 0.0;
    m_detectorEnvelope = 0.0;
    std::fill(m_ic1.begin(), m_ic1.end(), 0.0);
    std::fill(m_ic2.begin(), m_ic2.end(), 0.0);
    std::fill(m_band.begin(), m_band.end(), 0.0);
}

void DynamicStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled)
    {
        return;
    }

    if (channels != m_channels || m_ic1.size() < channels)
    {
        return;
    }

    const double slope = 1.0 - 1.0 / m_ratio;

    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;

        // Isolate the band around the configured frequency and track its peak
        // across channels so the stereo image stays intact.
        double peak = 0.0;
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            const double input = values[channel];
            const double v3 = input - m_ic2[channel];
            const double v1 = m_a1 * m_ic1[channel] + m_a2 * v3;
            const double v2 = m_ic2[channel] + m_a2 * m_ic1[channel] +
                              m_a3 * v3;
            m_ic1[channel] = 2.0 * v1 - m_ic1[channel];
            m_ic2[channel] = 2.0 * v2 - m_ic2[channel];

            const double band = m_k * v1;
            m_band[channel] = band;
            const double magnitude = std::fabs(band);
            if (magnitude > peak)
            {
                peak = magnitude;
            }
        }

        const double detectorCoefficient =
            (peak > m_detectorEnvelope) ? m_detectorAttack
                                        : m_detectorRelease;
        m_detectorEnvelope +=
            (peak - m_detectorEnvelope) * detectorCoefficient;
        const double level = m_detectorEnvelope;

        double targetDb = 0.0;
        if (m_expand)
        {
            // Extend: lift the band when it is quiet, so quiet lows (the
            // proximity effect at a distance) become fuller. Silence has
            // nothing to lift, so only non-zero bands above the floor count.
            if (level > 0.0 && level < m_thresholdLinear)
            {
                const double levelDb = 20.0 * std::log10(level);
                const double amount = (m_thresholdDb - levelDb) * slope;
                if (amount > 0.0)
                {
                    targetDb = std::min(amount, m_rangeDb);
                }
            }
        }
        else if (level > m_thresholdLinear)
        {
            // Reduce: cut the band when it is loud, taming a strong
            // proximity effect up close.
            const double levelDb = 20.0 * std::log10(level);
            const double amount = (levelDb - m_thresholdDb) * slope;
            if (amount > 0.0)
            {
                targetDb = -std::min(amount, m_rangeDb);
            }
        }

        // Attack when the effect deepens, release when it returns to zero.
        const double coefficient =
            (std::fabs(targetDb) > std::fabs(m_gainDb)) ? m_attackCoeff
                                                        : m_releaseCoeff;
        m_gainDb += (targetDb - m_gainDb) * coefficient;

        const double bandGain = DecibelsToLinear(m_gainDb) - 1.0;
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            values[channel] += m_band[channel] * bandGain;
        }
    }
}

void GainStage::Configure(const AudioFormat& format)
{
    (void)format;
    ApplyGain();
}

void GainStage::Update(const ApoSettings& settings,
                       const AudioFormat& format)
{
    (void)format;
    m_settings = settings;
    ApplyGain();
}

void GainStage::ApplyGain()
{
    m_gain = (m_settings.makeupEnabled && m_settings.makeupDb != 0.0)
                 ? DecibelsToLinear(m_settings.makeupDb)
                 : 1.0;
}

void GainStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (m_gain == 1.0)
    {
        return;
    }

    const size_t count = static_cast<size_t>(frames) * channels;
    for (size_t i = 0; i < count; ++i)
    {
        samples[i] *= m_gain;
    }
}

NoiseStage::NoiseStage(const NoiseSettings& settings,
                       const NoiseProfile& profile)
    : m_settings(settings), m_profile(profile), m_fft(kNoiseFftSize)
{
}

void NoiseStage::Configure(const AudioFormat& format)
{
    constexpr size_t N = kNoiseFftSize;
    constexpr size_t H = kNoiseHopSize;
    constexpr double kPi = 3.14159265358979323846;

    m_channels = format.channels;
    m_maxFrames = (format.maxFrames > 0) ? format.maxFrames : 1;
    const double sampleRate = EffectiveSampleRate(format);

    m_window.resize(N);
    for (size_t index = 0; index < N; ++index)
    {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(index) /
                                 static_cast<double>(N));
        m_window[index] = std::sqrt(std::max(0.0, hann));
    }

    m_profileBins.assign(N / 2 + 1, 0.0);
    const size_t profileBins = m_profile.magnitudes.size();
    if (profileBins != 0)
    {
        const double profileRate =
            (m_profile.sampleRate > 0)
                ? static_cast<double>(m_profile.sampleRate)
                : sampleRate;
        for (size_t bin = 0; bin <= N / 2; ++bin)
        {
            double position =
                static_cast<double>(bin) * sampleRate / profileRate;
            position = std::max(
                0.0, std::min(position,
                              static_cast<double>(profileBins - 1)));
            const size_t low = static_cast<size_t>(position);
            const size_t high = std::min(profileBins - 1, low + 1);
            const double fraction = position - static_cast<double>(low);
            m_profileBins[bin] =
                m_profile.magnitudes[low] * (1.0 - fraction) +
                m_profile.magnitudes[high] * fraction;
        }
    }

    m_floorGain = DecibelsToLinear(-m_settings.reductionDb);
    const double hopSeconds =
        static_cast<double>(H) / std::max(1.0, sampleRate);
    m_gainAttack = 1.0 - std::exp(-hopSeconds / 0.020);
    m_gainRelease = 1.0 - std::exp(-hopSeconds / 0.150);

    const size_t capacity =
        (static_cast<size_t>(m_maxFrames) + N) * m_channels;
    m_input.assign(capacity, 0.0);
    m_output.assign(capacity, 0.0);
    m_history.assign(N * m_channels, 0.0);
    m_ola.assign(N * m_channels, 0.0);
    m_spectraReal.assign(N * m_channels, 0.0);
    m_spectraImag.assign(N * m_channels, 0.0);
    m_frameReal.assign(N, 0.0);
    m_frameImag.assign(N, 0.0);
    m_gain.assign(N / 2 + 1, 1.0);
    m_rawGain.assign(N / 2 + 1, 1.0);

    Reset();
}

void NoiseStage::Update(const ApoSettings& settings,
                        const AudioFormat& format)
{
    (void)format;
    const bool wasEnabled = m_settings.enabled;
    m_settings = settings.noise;
    m_floorGain = DecibelsToLinear(-m_settings.reductionDb);
    if (wasEnabled != m_settings.enabled)
    {
        // Drop the buffered analysis so re-enabling does not replay stale audio.
        Reset();
    }
}

void NoiseStage::Reset()
{
    m_inputFrames = 0;

    // Prime with one hop minus one frame of silence. Together with the first
    // hop of the analysis, this keeps the output queue from running dry when
    // the engine's buffer size is not a multiple of the hop.
    m_outputFrames = static_cast<UINT32>(kNoiseHopSize - 1);
    if (!m_output.empty() && m_channels != 0)
    {
        std::fill(m_output.begin(),
                  m_output.begin() +
                      static_cast<size_t>(m_outputFrames) * m_channels,
                  0.0);
    }

    std::fill(m_history.begin(), m_history.end(), 0.0);
    std::fill(m_ola.begin(), m_ola.end(), 0.0);
    std::fill(m_spectraReal.begin(), m_spectraReal.end(), 0.0);
    std::fill(m_spectraImag.begin(), m_spectraImag.end(), 0.0);
    std::fill(m_gain.begin(), m_gain.end(), 1.0);
    std::fill(m_rawGain.begin(), m_rawGain.end(), 1.0);
}

void NoiseStage::ProcessHop()
{
    constexpr size_t N = kNoiseFftSize;
    constexpr size_t H = kNoiseHopSize;
    const size_t channels = m_channels;

    for (size_t channel = 0; channel < channels; ++channel)
    {
        double* history = m_history.data() + channel * N;
        std::memmove(history, history + H, (N - H) * sizeof(double));
        for (size_t index = 0; index < H; ++index)
        {
            history[N - H + index] =
                m_input[index * channels + channel];
        }
    }

    for (size_t channel = 0; channel < channels; ++channel)
    {
        const double* history = m_history.data() + channel * N;
        for (size_t index = 0; index < N; ++index)
        {
            m_frameReal[index] = history[index] * m_window[index];
            m_frameImag[index] = 0.0;
        }
        m_fft.Forward(m_frameReal.data(), m_frameImag.data());
        std::memcpy(m_spectraReal.data() + channel * N, m_frameReal.data(),
                    N * sizeof(double));
        std::memcpy(m_spectraImag.data() + channel * N, m_frameImag.data(),
                    N * sizeof(double));
    }

    for (size_t bin = 0; bin <= N / 2; ++bin)
    {
        double power = 0.0;
        for (size_t channel = 0; channel < channels; ++channel)
        {
            const double real = m_spectraReal[channel * N + bin];
            const double imag = m_spectraImag[channel * N + bin];
            power += real * real + imag * imag;
        }

        const double magnitude =
            std::sqrt(power / static_cast<double>(channels));
        const double noise = m_profileBins[bin];

        double gain = 1.0;
        if (magnitude > 1e-12)
        {
            const double ratio = noise / magnitude;
            gain = std::sqrt(std::max(0.0, 1.0 - ratio * ratio));
            if (gain < m_floorGain)
            {
                gain = m_floorGain;
            }
        }
        m_rawGain[bin] = gain;
    }

    for (size_t bin = 0; bin <= N / 2; ++bin)
    {
        const double left = m_rawGain[bin > 0 ? bin - 1 : 0];
        const double center = m_rawGain[bin];
        const double right = m_rawGain[bin < N / 2 ? bin + 1 : N / 2];
        const double target = 0.25 * left + 0.5 * center + 0.25 * right;
        const double coefficient =
            (target < m_gain[bin]) ? m_gainAttack : m_gainRelease;
        m_gain[bin] += (target - m_gain[bin]) * coefficient;
    }

    for (size_t channel = 0; channel < channels; ++channel)
    {
        double* real = m_spectraReal.data() + channel * N;
        double* imag = m_spectraImag.data() + channel * N;
        for (size_t bin = 0; bin <= N / 2; ++bin)
        {
            const double gain = m_gain[bin];
            real[bin] *= gain;
            imag[bin] *= gain;
            if (bin != 0 && bin != N / 2)
            {
                real[N - bin] *= gain;
                imag[N - bin] *= gain;
            }
        }
    }

    for (size_t channel = 0; channel < channels; ++channel)
    {
        std::memcpy(m_frameReal.data(), m_spectraReal.data() + channel * N,
                    N * sizeof(double));
        std::memcpy(m_frameImag.data(), m_spectraImag.data() + channel * N,
                    N * sizeof(double));
        m_fft.Inverse(m_frameReal.data(), m_frameImag.data());

        double* ola = m_ola.data() + channel * N;
        for (size_t index = 0; index < N; ++index)
        {
            ola[index] += m_frameReal[index] * m_window[index];
        }
    }

    for (size_t index = 0; index < H; ++index)
    {
        for (size_t channel = 0; channel < channels; ++channel)
        {
            m_output[(static_cast<size_t>(m_outputFrames) + index) *
                         channels +
                     channel] = m_ola[channel * N + index];
        }
    }
    m_outputFrames += static_cast<UINT32>(H);

    for (size_t channel = 0; channel < channels; ++channel)
    {
        double* ola = m_ola.data() + channel * N;
        std::memmove(ola, ola + H, (N - H) * sizeof(double));
        std::fill(ola + (N - H), ola + N, 0.0);
    }

    m_inputFrames -= static_cast<UINT32>(H);
    std::memmove(m_input.data(), m_input.data() + H * channels,
                 static_cast<size_t>(m_inputFrames) * channels *
                     sizeof(double));
}

void NoiseStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    constexpr size_t N = kNoiseFftSize;
    constexpr size_t H = kNoiseHopSize;

    if (!m_settings.enabled || samples == nullptr || frames == 0 ||
        channels == 0 || channels != m_channels)
    {
        return;
    }

    const size_t capacityFrames =
        static_cast<size_t>(m_maxFrames) + N;
    if (m_input.size() < capacityFrames * channels ||
        m_output.size() < capacityFrames * channels ||
        static_cast<size_t>(m_inputFrames) + frames > capacityFrames)
    {
        return;
    }

    std::memcpy(
        m_input.data() + static_cast<size_t>(m_inputFrames) * channels,
        samples, static_cast<size_t>(frames) * channels * sizeof(double));
    m_inputFrames += frames;

    while (m_inputFrames >= H)
    {
        ProcessHop();
    }

    const size_t available =
        std::min<size_t>(m_outputFrames, frames);
    std::memcpy(samples, m_output.data(),
                available * channels * sizeof(double));
    if (available < frames)
    {
        std::fill(samples + available * channels,
                  samples + static_cast<size_t>(frames) * channels, 0.0);
    }

    const size_t remaining =
        static_cast<size_t>(m_outputFrames) - available;
    if (remaining != 0)
    {
        std::memmove(m_output.data(), m_output.data() + available * channels,
                     remaining * channels * sizeof(double));
    }
    m_outputFrames = static_cast<UINT32>(remaining);
}

void MonoStage::Update(const ApoSettings& settings, const AudioFormat& format)
{
    (void)format;
    m_enabled = settings.mono;
}

void MonoStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_enabled || channels < 2)
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
