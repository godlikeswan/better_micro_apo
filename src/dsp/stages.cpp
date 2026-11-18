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

void DynamicsStage::Configure(const AudioFormat& format)
{
    m_channels = format.channels;

    // Reserve the largest lookahead the settings clamp allows, so a live
    // Update can lengthen the delay without allocating.
    const double sampleRate = EffectiveSampleRate(format);
    m_delayCapacityFrames = static_cast<UINT32>(
        apo::kDynamicsMaxLookaheadMs * 0.001 * sampleRate + 0.5);
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

void DynamicsStage::Update(const EffectInstance& effect,
                           const AudioFormat& format)
{
    const bool wasEnabled = m_settings.enabled;
    const double oldLookahead = m_settings.lookaheadMs;
    const DynamicsMode oldMode = m_settings.mode;
    m_settings = effect.dynamics;
    ApplyCoefficients(format);

    // A bypass change, a different delay length, or a mode flip invalidates the
    // buffered tail and the smoothed gain.
    if (wasEnabled != m_settings.enabled ||
        oldLookahead != m_settings.lookaheadMs || oldMode != m_settings.mode)
    {
        Reset();
    }
}

void DynamicsStage::ApplyCoefficients(const AudioFormat& format)
{
    const double sampleRate = EffectiveSampleRate(format);

    m_thresholdDb = m_settings.thresholdDb;
    m_inputGain = DecibelsToLinear(m_settings.inputDb);
    m_outputGain = DecibelsToLinear(m_settings.outputDb);
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

    // The hold must last until the detected frame has passed the lookahead
    // delay, so add the delay length to the user's hold time.
    const double holdSeconds = m_settings.holdMs * 0.001;
    m_holdFrames =
        static_cast<UINT32>(holdSeconds * sampleRate + 0.5) + m_delayFrames;
}

void DynamicsStage::Reset()
{
    m_gainDb = 0.0;
    m_heldTargetDb = 0.0;
    m_holdCounter = 0;
    m_gateHoldActive = false;
    m_gateHoldSpent = false;
    m_delayWrite = 0;
    std::fill(m_delay.begin(), m_delay.end(), 0.0);
}

void DynamicsStage::Process(double* samples, UINT32 frames, UINT32 channels)
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

        // Input gain feeds both the signal and the linked peak detector.
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            values[channel] *= m_inputGain;
        }

        const double peak = PeakOfFrame(values, channels);
        const double levelDb = LinearToDecibels(peak);
        const double delta = levelDb - m_thresholdDb;
        double targetDb =
            DynamicsGainDb(levelDb, m_thresholdDb, m_settings.ratio,
                           m_settings.mode);

        bool holding = false;
        if (m_settings.mode == DynamicsMode::CompressorLimiter)
        {
            // Latch a deeper gain change so a short peak keeps its target
            // active long enough for attack to reach it. Hold includes the
            // lookahead, so the counter is measured at the output.
            if (std::fabs(targetDb) > std::fabs(m_heldTargetDb) + 1e-6)
            {
                m_heldTargetDb = targetDb;
                m_holdCounter = m_holdFrames;
            }
            holding = m_holdCounter > 0;
            if (holding)
            {
                --m_holdCounter;
                targetDb = m_heldTargetDb;
            }
            if (!holding)
            {
                m_heldTargetDb = 0.0;
            }
        }
        else
        {
            // Gate/Expander hold delays closing. Stay open briefly after the
            // signal drops below the threshold, then let the closing target
            // take over. Any signal above the threshold cancels the hold and
            // opens immediately.
            if (delta >= 0.0)
            {
                m_gateHoldActive = false;
                m_gateHoldSpent = false;
                m_holdCounter = 0;
                m_heldTargetDb = 0.0;
            }
            else
            {
                if (!m_gateHoldActive && !m_gateHoldSpent)
                {
                    m_gateHoldActive = true;
                    m_holdCounter = m_holdFrames;
                    m_heldTargetDb = 0.0;
                }
                if (m_gateHoldActive)
                {
                    if (m_holdCounter > 0)
                    {
                        --m_holdCounter;
                        targetDb = m_heldTargetDb;
                        holding = true;
                    }
                    if (m_holdCounter == 0)
                    {
                        m_gateHoldActive = false;
                        m_gateHoldSpent = true;
                    }
                }
            }
        }

        // Compressor/Limiter: attack engages, release recovers.
        // Expander/Gate: attack opens, release closes.
        const bool deepening =
            std::fabs(targetDb) > std::fabs(m_gainDb) + 1e-6;
        const bool useAttack =
            (m_settings.mode == DynamicsMode::CompressorLimiter) ? deepening
                                                                 : !deepening;
        m_gainDb += (targetDb - m_gainDb) *
                    (useAttack ? m_attackCoeff : m_releaseCoeff);
        const double gain = DecibelsToLinear(m_gainDb) * m_outputGain;

        if (lookahead)
        {
            // The detector ran on the incoming frame; apply the resulting gain
            // to the frame that is one lookahead behind it.
            double* delayed =
                m_delay.data() + static_cast<size_t>(m_delayWrite) * channels;
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                const double current = values[channel];
                values[channel] = delayed[channel] * gain;
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
                values[channel] *= gain;
            }
        }
    }
}

void EqualizerStage::Configure(const AudioFormat& format)
{
    m_channels = format.channels;
    m_ic1.assign(m_channels, 0.0);
    m_ic2.assign(m_channels, 0.0);
    m_band.assign(m_channels, 0.0);

    ApplyCoefficients(format);
    Reset();
}

void EqualizerStage::Update(const EffectInstance& effect,
                          const AudioFormat& format)
{
    const bool wasEnabled = m_settings.enabled;
    const double oldFrequency = m_settings.frequencyHz;
    const double oldQ = m_settings.q;

    m_settings = effect.equalizer;
    ApplyCoefficients(format);

    // Changing the filter shape invalidates the filter state.
    if (wasEnabled != m_settings.enabled ||
        oldFrequency != m_settings.frequencyHz || oldQ != m_settings.q)
    {
        Reset();
    }
}

void EqualizerStage::ApplyCoefficients(const AudioFormat& format)
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

void EqualizerStage::Reset()
{
    m_gainDb = 0.0;
    m_detectorEnvelope = 0.0;
    std::fill(m_ic1.begin(), m_ic1.end(), 0.0);
    std::fill(m_ic2.begin(), m_ic2.end(), 0.0);
    std::fill(m_band.begin(), m_band.end(), 0.0);
}

void EqualizerStage::Process(double* samples, UINT32 frames, UINT32 channels)
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

void GainStage::Update(const EffectInstance& effect,
                       const AudioFormat& format)
{
    (void)format;
    m_settings = effect.gain;
    ApplyGain();
}

void GainStage::ApplyGain()
{
    m_gain = (m_settings.enabled && m_settings.gainDb != 0.0)
                 ? DecibelsToLinear(m_settings.gainDb)
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

NoiseStage::NoiseStage(const NoiseProfile& profile)
    : m_profile(profile)
{
}

void NoiseStage::ApplyProfileBins(const std::vector<double>& magnitudes,
                                  std::size_t binCount, uint32_t sampleRate)
{
    std::fill(m_profileBins.begin(), m_profileBins.end(), 0.0);
    m_hasProfile = !magnitudes.empty() && binCount != 0 && sampleRate != 0;
    if (!m_hasProfile || m_profileBins.size() != kNoiseMaxBins)
    {
        return;
    }

    const size_t count = std::min(binCount, magnitudes.size());
    const int profileFft =
        (count >= 2) ? static_cast<int>((count - 1) * 2) : 0;
    if (profileFft <= 0 || ValidNoiseFftSize(profileFft) != profileFft)
    {
        m_hasProfile = false;
        return;
    }

    const double profileRate = static_cast<double>(sampleRate);
    for (size_t bin = 0; bin <= m_fftSize / 2; ++bin)
    {
        const double frequency = static_cast<double>(bin) * m_sampleRate /
                                 static_cast<double>(m_fftSize);
        double position =
            frequency * static_cast<double>(profileFft) / profileRate;
        position = std::max(
            0.0, std::min(position, static_cast<double>(count - 1)));
        const size_t low = static_cast<size_t>(position);
        const size_t high = std::min(count - 1, low + 1);
        const double fraction = position - static_cast<double>(low);
        m_profileBins[bin] = magnitudes[low] * (1.0 - fraction) +
                             magnitudes[high] * fraction;
    }
}

void NoiseStage::SetProfileBins(const std::vector<double>& magnitudes,
                                std::size_t binCount, uint32_t sampleRate)
{
    const bool hadProfile = m_hasProfile;
    ApplyProfileBins(magnitudes, binCount, sampleRate);
    if (hadProfile != m_hasProfile)
    {
        // The stage is about to start or stop delaying the stream. Prime or
        // drop its analysis tail so the first buffer is not stale.
        Reset();
    }
}

void NoiseStage::Configure(const AudioFormat& format)
{
    constexpr double kPi = 3.14159265358979323846;

    m_channels = format.channels;
    m_maxFrames = (format.maxFrames > 0) ? format.maxFrames : 1;
    m_sampleRate = EffectiveSampleRate(format);

    const size_t planCount = kNoiseFftSizes.size();
    m_fftPlans.clear();
    m_fftPlans.reserve(planCount);
    m_windows.assign(planCount, {});
    for (size_t index = 0; index < planCount; ++index)
    {
        const size_t size = static_cast<size_t>(kNoiseFftSizes[index]);
        m_fftPlans.emplace_back(size);
        m_windows[index].resize(size);
        for (size_t sample = 0; sample < size; ++sample)
        {
            const double hann =
                0.5 - 0.5 * std::cos(2.0 * kPi *
                                     static_cast<double>(sample) /
                                     static_cast<double>(size));
            m_windows[index][sample] = std::sqrt(std::max(0.0, hann));
        }
    }

    m_sizeIndex = static_cast<size_t>(
        NoiseFftSizeIndex(ValidNoiseFftSize(m_settings.fftSize)));
    m_fftSize = static_cast<size_t>(kNoiseFftSizes[m_sizeIndex]);
    m_hopSize = m_fftSize / 2;
    m_settings.fftSize = static_cast<int>(m_fftSize);

    m_profileBins.assign(kNoiseMaxBins, 0.0);
    ApplyProfileBins(m_profile.magnitudes, m_profile.binCount,
                     m_profile.sampleRate);

    m_floorGain = DecibelsToLinear(-m_settings.reductionDb);
    const double hopSeconds =
        static_cast<double>(m_hopSize) / std::max(1.0, m_sampleRate);
    m_gainAttack = 1.0 - std::exp(-hopSeconds / 0.020);
    m_gainRelease = 1.0 - std::exp(-hopSeconds / 0.150);

    const size_t maxSize = static_cast<size_t>(kNoiseFftSizes.back());
    const size_t capacity =
        (static_cast<size_t>(m_maxFrames) + maxSize) * m_channels;
    m_input.assign(capacity, 0.0);
    m_output.assign(capacity, 0.0);
    m_history.assign(maxSize * m_channels, 0.0);
    m_ola.assign(maxSize * m_channels, 0.0);
    m_spectraReal.assign(maxSize * m_channels, 0.0);
    m_spectraImag.assign(maxSize * m_channels, 0.0);
    m_frameReal.assign(maxSize, 0.0);
    m_frameImag.assign(maxSize, 0.0);
    m_gain.assign(kNoiseMaxBins, 1.0);
    m_rawGain.assign(kNoiseMaxBins, 1.0);

    Reset();
}

void NoiseStage::Update(const EffectInstance& effect,
                        const AudioFormat& format)
{
    (void)format;
    const bool wasEnabled = m_settings.enabled;
    const size_t oldSize = m_fftSize;
    m_settings = effect.noise;
    m_sizeIndex = static_cast<size_t>(
        NoiseFftSizeIndex(ValidNoiseFftSize(m_settings.fftSize)));
    m_fftSize = static_cast<size_t>(kNoiseFftSizes[m_sizeIndex]);
    m_hopSize = m_fftSize / 2;
    m_settings.fftSize = static_cast<int>(m_fftSize);

    m_floorGain = DecibelsToLinear(-m_settings.reductionDb);
    const double hopSeconds =
        static_cast<double>(m_hopSize) / std::max(1.0, m_sampleRate);
    m_gainAttack = 1.0 - std::exp(-hopSeconds / 0.020);
    m_gainRelease = 1.0 - std::exp(-hopSeconds / 0.150);
    ApplyProfileBins(m_profile.magnitudes, m_profile.binCount,
                     m_profile.sampleRate);

    if (wasEnabled != m_settings.enabled || oldSize != m_fftSize)
    {
        // Drop the buffered analysis when the chain shape changes so stale
        // audio from the old size is not replayed.
        Reset();
    }
}

void NoiseStage::Reset()
{
    m_inputFrames = 0;

    // Prime with one hop minus one frame of silence. Together with the first
    // hop of the analysis, this keeps the output queue from running dry when
    // the engine's buffer size is not a multiple of the hop.
    m_outputFrames = static_cast<UINT32>(m_hopSize - 1);
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
    const size_t N = m_fftSize;
    const size_t H = m_hopSize;
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
            m_frameReal[index] = history[index] * m_windows[m_sizeIndex][index];
            m_frameImag[index] = 0.0;
        }
        m_fftPlans[m_sizeIndex].Forward(m_frameReal.data(), m_frameImag.data());
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
        m_fftPlans[m_sizeIndex].Inverse(m_frameReal.data(), m_frameImag.data());

        double* ola = m_ola.data() + channel * N;
        for (size_t index = 0; index < N; ++index)
        {
            ola[index] += m_frameReal[index] * m_windows[m_sizeIndex][index];
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
    const size_t N = m_fftSize;
    const size_t H = m_hopSize;

    if (!m_settings.enabled || !m_hasProfile || samples == nullptr ||
        frames == 0 || channels == 0 || channels != m_channels)
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

void MonoStage::Update(const EffectInstance& effect,
                       const AudioFormat& format)
{
    (void)format;
    m_enabled = effect.mono.enabled;
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

void DistortionStage::Update(const EffectInstance& effect,
                             const AudioFormat& format)
{
    (void)format;
    m_settings = effect.distortion;

    // Fold the function and curvature into one coefficient pair so the
    // real-time path only evaluates the curve itself.
    m_curve = MakeDistortionCurve(m_settings.function, m_settings.curvature);
    m_drive = DecibelsToLinear(m_settings.driveDb);
    m_output = DecibelsToLinear(m_settings.outputDb);
}

double DistortionStage::Shape(double value) const
{
    return ShapeDistortion(m_curve, m_settings.function, value);
}

void DistortionStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled || samples == nullptr || frames == 0 ||
        channels == 0)
    {
        return;
    }

    const size_t count = static_cast<size_t>(frames) * channels;
    for (size_t i = 0; i < count; ++i)
    {
        samples[i] = Shape(samples[i] * m_drive) * m_output;
    }
}

void ReverbStage::Configure(const AudioFormat& format)
{
    static constexpr int kCombTunings[] = {1116, 1188, 1277, 1356,
                                           1422, 1491, 1557, 1617};
    static constexpr int kAllpassTunings[] = {556, 441, 341, 225};
    static constexpr int kStereoSpread = 23;
    constexpr double kMaxPredelayMs = 200.0;

    m_channels = std::max<UINT32>(1, format.channels);
    m_sampleRate = EffectiveSampleRate(format);
    const double scale = m_sampleRate / 44100.0;

    m_tanks.clear();
    m_tanks.resize(m_channels);
    for (UINT32 channel = 0; channel < m_channels; ++channel)
    {
        Tank& tank = m_tanks[channel];
        const int spread = (channel % 2 == 0) ? 0 : kStereoSpread;

        tank.combs.resize(8);
        for (size_t index = 0; index < tank.combs.size(); ++index)
        {
            const size_t length = static_cast<size_t>(
                std::max(1.0, std::round(kCombTunings[index] * scale) +
                                  spread));
            tank.combs[index].buffer.assign(length, 0.0);
        }

        tank.allpasses.resize(4);
        for (size_t index = 0; index < tank.allpasses.size(); ++index)
        {
            const size_t length = static_cast<size_t>(
                std::max(1.0, std::round(kAllpassTunings[index] * scale) +
                                  spread));
            tank.allpasses[index].buffer.assign(length, 0.0);
        }
    }

    // Reserve the maximum predelay at Configure time so a live Update only
    // moves the read offset and never allocates.
    m_predelayCapacityFrames = static_cast<size_t>(
        std::ceil(m_sampleRate * kMaxPredelayMs / 1000.0)) + 1;
    m_predelay.assign(m_predelayCapacityFrames * m_channels, 0.0);

    ApplyCoefficients();
    Reset();
}

void ReverbStage::Update(const EffectInstance& effect,
                         const AudioFormat& format)
{
    (void)format;
    const bool wasEnabled = m_settings.enabled;
    m_settings = effect.reverb;
    ApplyCoefficients();
    if (wasEnabled != m_settings.enabled)
    {
        // Drop the tail so re-enabling does not replay old audio.
        Reset();
    }
}

void ReverbStage::ApplyCoefficients()
{
    const double size = std::clamp(m_settings.size / 100.0, 0.0, 1.0);
    const double damping =
        std::clamp(m_settings.damping / 100.0, 0.0, 1.0);
    const double mix = std::clamp(m_settings.mix / 100.0, 0.0, 1.0);

    m_feedback = 0.7 + 0.28 * size;
    m_damping = 0.4 * damping;

    // The comb bank has a lot of gain, so the wet path needs its own scaling.
    // Lower it as the room grows or the tail gets brighter so the reverb stays
    // near the dry level instead of clipping at extreme settings.
    m_wetGain = mix * 2.5 * (1.0 - 0.35 * size) * (0.75 + 0.25 * damping);
    m_dryGain = 1.0 - mix;

    if (m_predelayCapacityFrames == 0)
    {
        m_predelayFrames = 0;
        return;
    }

    const double predelayMs = std::clamp(m_settings.predelayMs, 0.0, 200.0);
    size_t frames = static_cast<size_t>(
        std::round(predelayMs * m_sampleRate / 1000.0));
    if (frames >= m_predelayCapacityFrames)
    {
        frames = m_predelayCapacityFrames - 1;
    }
    m_predelayFrames = frames;
}

void ReverbStage::Reset()
{
    for (Tank& tank : m_tanks)
    {
        for (Comb& comb : tank.combs)
        {
            std::fill(comb.buffer.begin(), comb.buffer.end(), 0.0);
            comb.index = 0;
            comb.store = 0.0;
        }
        for (Allpass& allpass : tank.allpasses)
        {
            std::fill(allpass.buffer.begin(), allpass.buffer.end(), 0.0);
            allpass.index = 0;
        }
    }

    std::fill(m_predelay.begin(), m_predelay.end(), 0.0);
    m_predelayWrite = 0;
}

double ReverbStage::ProcessComb(Comb& comb, double input) const
{
    const double output = comb.buffer[comb.index];
    comb.store = output * (1.0 - m_damping) + comb.store * m_damping;

    const double next = input + comb.store * m_feedback;
    comb.buffer[comb.index] = (std::fabs(next) < 1e-20) ? 0.0 : next;
    if (std::fabs(comb.store) < 1e-20)
    {
        comb.store = 0.0;
    }

    if (++comb.index >= comb.buffer.size())
    {
        comb.index = 0;
    }
    return output;
}

double ReverbStage::ProcessAllpass(Allpass& allpass, double input) const
{
    const double buffered = allpass.buffer[allpass.index];
    const double next = input + buffered * 0.5;
    allpass.buffer[allpass.index] = (std::fabs(next) < 1e-20) ? 0.0 : next;

    if (++allpass.index >= allpass.buffer.size())
    {
        allpass.index = 0;
    }
    return buffered - input;
}

void ReverbStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled || m_settings.mix <= 0.0 || samples == nullptr ||
        frames == 0 || channels == 0 || channels != m_channels ||
        m_tanks.size() != channels || m_predelayCapacityFrames == 0)
    {
        return;
    }

    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;

        double sum = 0.0;
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            sum += values[channel];
        }
        const double mono = sum / static_cast<double>(channels);

        const size_t readIndex =
            (m_predelayWrite + m_predelayCapacityFrames - m_predelayFrames) %
            m_predelayCapacityFrames;

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            // Feed each tank its own channel plus a little of the mono sum.
            // A mono source therefore excites both tanks and gets a wide tail,
            // while a true stereo source keeps its image.
            const double excitation = (values[channel] + mono) * 0.015;
            m_predelay[m_predelayWrite * channels + channel] = excitation;
            const double delayed =
                m_predelay[readIndex * channels + channel];

            Tank& tank = m_tanks[channel];
            double wet = 0.0;
            for (Comb& comb : tank.combs)
            {
                wet += ProcessComb(comb, delayed);
            }
            for (Allpass& allpass : tank.allpasses)
            {
                wet = ProcessAllpass(allpass, wet);
            }

            values[channel] =
                values[channel] * m_dryGain + wet * m_wetGain;
        }

        if (++m_predelayWrite >= m_predelayCapacityFrames)
        {
            m_predelayWrite = 0;
        }
    }
}

void StagePool::Configure(const AudioFormat& format,
                          const NoiseProfile& profile)
{
    Clear();

    for (int typeIndex = 0;
         typeIndex < static_cast<int>(EffectType::Count); ++typeIndex)
    {
        const auto type = static_cast<EffectType>(typeIndex);
        const int capacity = EffectTypeCapacity(type);
        Slots& slots = m_slots[typeIndex];
        slots.stages.reserve(static_cast<size_t>(capacity));
        slots.boundIds.assign(static_cast<size_t>(capacity), 0);

        for (int slot = 0; slot < capacity; ++slot)
        {
            std::unique_ptr<Stage> stage;
            switch (type)
            {
            case EffectType::Gain:
                stage = std::make_unique<GainStage>();
                break;
            case EffectType::Noise:
                // Always allocate the noise stages. Without a profile they
                // pass audio through unchanged, and a live capture can fill
                // their bins without rebuilding the pool.
                stage = std::make_unique<NoiseStage>(profile);
                break;
            case EffectType::Dynamics:
                stage = std::make_unique<DynamicsStage>();
                break;
            case EffectType::Equalizer:
                stage = std::make_unique<EqualizerStage>();
                break;
            case EffectType::Mono:
                stage = std::make_unique<MonoStage>();
                break;
            case EffectType::Saturation:
                stage = std::make_unique<DistortionStage>();
                break;
            case EffectType::Reverb:
                stage = std::make_unique<ReverbStage>();
                break;
            default:
                break;
            }

            if (stage != nullptr)
            {
                stage->Configure(format);
                slots.stages.push_back(std::move(stage));
            }
        }
    }
}

void StagePool::Clear()
{
    for (Slots& slots : m_slots)
    {
        slots.stages.clear();
        slots.boundIds.clear();
    }
}

void StagePool::Update(const ApoSettings& settings, const AudioFormat& format,
                       Chain& chain)
{
    chain.Clear();

    int used[static_cast<int>(EffectType::Count)] = {};

    const EffectPreset& preset = ActivePreset(settings);
    for (int index = 0; index < preset.effectCount; ++index)
    {
        const EffectInstance& effect = preset.effects[index];
        if (!EffectEnabled(effect))
        {
            continue;
        }

        const int typeIndex = static_cast<int>(effect.type);
        if (typeIndex < 0 ||
            typeIndex >= static_cast<int>(EffectType::Count))
        {
            continue;
        }

        Slots& slots = m_slots[typeIndex];
        if (slots.stages.empty())
        {
            continue;
        }

        // The ordinal picks the pool slot for the n-th enabled instance of
        // this type in list order. A slot that used to hold another instance
        // is cleared before it takes over.
        const int ordinal = used[typeIndex]++;
        if (ordinal >= static_cast<int>(slots.stages.size()))
        {
            continue;
        }

        Stage* stage = slots.stages[static_cast<size_t>(ordinal)].get();
        if (slots.boundIds[static_cast<size_t>(ordinal)] != effect.id)
        {
            stage->Reset();
            slots.boundIds[static_cast<size_t>(ordinal)] = effect.id;
        }

        stage->Update(effect, format);
        chain.Add(stage);
    }
}

void StagePool::UpdateProfileBins(const NoiseProfile& profile)
{
    Slots& slots = m_slots[static_cast<int>(EffectType::Noise)];
    for (const auto& stage : slots.stages)
    {
        static_cast<NoiseStage*>(stage.get())->SetProfileBins(
            profile.magnitudes, profile.binCount, profile.sampleRate);
    }
}

bool StagePool::Empty() const
{
    for (const Slots& slots : m_slots)
    {
        if (!slots.stages.empty())
        {
            return false;
        }
    }
    return true;
}

} // namespace apo::dsp
