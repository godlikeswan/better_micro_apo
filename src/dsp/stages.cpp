#include "dsp/stages.h"
#include "pitch.h"

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

bool WantsLooper(const ApoSettings& settings)
{
    const EffectPreset& preset = ActivePreset(settings);
    for (int index = 0; index < preset.effectCount; ++index)
    {
        const EffectInstance& effect = preset.effects[index];
        if (effect.type == EffectType::Looper && EffectEnabled(effect))
        {
            return true;
        }
    }
    return false;
}

// Tuner analysis rejects sub-audio rumble before pitch detection. These affect
// only the detector's mono copy, never the audio buffer.
constexpr double kTunerRumbleHighPassHz = 45.0;
constexpr double kTunerMinPitchHz = 55.0;

double TunerRumbleHighPassCoeff(double sampleRate)
{
    constexpr double pi = 3.14159265358979323846;
    return 1.0 / (1.0 + 2.0 * pi * kTunerRumbleHighPassHz / sampleRate);
}

// Mono excitation feeds the common mode. The stereo output taps contain a
// strong common-mode component, so an alternating input would project almost
// zero energy onto them once Density and Width are raised.
constexpr int kFdnInputSigns[8] = {1, 1, 1, 1, 1, 1, 1, 1};
constexpr int kFdnOutputSigns[8] = {1, -1, 1, -1, -1, 1, -1, 1};

// Feedback needed for a roughly -60 dB decay over maxLengthMs, given the
// average delay-line loop time in seconds.
double FeedbackForDecay(double maxLengthMs, double loopSeconds)
{
    const double seconds =
        std::max(maxLengthMs, apo::kReverbMinMaxLengthMs) / 1000.0;
    const double loop = std::max(loopSeconds, 0.001);
    return std::clamp(std::pow(10.0, -3.0 * loop / seconds), 0.05, 0.995);
}

constexpr double kPi = 3.14159265358979323846;

// FftPlan::Inverse already divides by the FFT size. Hann analysis/synthesis
// overlap at a quarter-window hop sums to about 2, so halving the panes gives
// a unit-gain wet return before the user's Level is applied.
constexpr double kVocoderOutputScale = 0.5;

double WrapPosition(double value, size_t size)
{
    if (size == 0) {
        return 0.0;
    }
    const double span = static_cast<double>(size);
    value = std::fmod(value, span);
    if (value < 0.0) {
        value += span;
    }
    return value;
}

double WrapPi(double angle)
{
    angle = std::fmod(angle + kPi, 2.0 * kPi);
    if (angle < 0.0) {
        angle += 2.0 * kPi;
    }
    return angle - kPi;
}

// LFO phase in cycles, wrapped to [0, 1).
double WrapPhase(double phase)
{
    phase = std::fmod(phase, 1.0);
    return phase < 0.0 ? phase + 1.0 : phase;
}

double ModulationValue(double phase, ModulationWaveform waveform)
{
    switch (waveform)
    {
    case ModulationWaveform::Triangle:
        if (phase < 0.25) {
            return 4.0 * phase;
        }
        if (phase < 0.75) {
            return 2.0 - 4.0 * phase;
        }
        return 4.0 * phase - 4.0;
    case ModulationWaveform::Square:
        return phase < 0.5 ? 1.0 : -1.0;
    case ModulationWaveform::Sine:
    default:
        return std::sin(2.0 * kPi * phase);
    }
}

size_t NextPowerOfTwo(size_t value)
{
    size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

int FftSizeIndex(size_t size)
{
    // The plan array starts at 512 (index 0), not at 1 (index 0). Returning
    // log2(size) made a 4096-point frame look for plan 12 and silently skipped
    // every phase-vocoder analysis.
    int index = 0;
    size_t value = 512;
    while (value < size) {
        value <<= 1;
        ++index;
    }
    return index;
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
    m_settings = (m_mode == DynamicsMode::CompressorLimiter)
                     ? effect.compressor
                     : effect.gate;
    ApplyCoefficients(format);

    // A bypass change or a different delay length invalidates the buffered
    // tail and the smoothed gain. The transfer half is fixed for this stage.
    if (wasEnabled != m_settings.enabled ||
        oldLookahead != m_settings.lookaheadMs)
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

double DynamicsStage::MonitorValue() const
{
    if (!m_settings.enabled) {
        return 0.0;
    }
    // Gain reduction and gate closure are both useful engagement cues; both
    // show up as a nonzero |m_gainDb|. 24 dB spans the useful visual range.
    return std::clamp(std::fabs(m_gainDb) / 24.0, 0.0, 1.0);
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
            DynamicsGainDb(levelDb, m_thresholdDb, m_settings.ratio, m_mode);

        bool holding = false;
        if (m_mode == DynamicsMode::CompressorLimiter)
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
            (m_mode == DynamicsMode::CompressorLimiter) ? deepening
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

    // Reserve the largest lookahead the settings clamp allows so a live Update
    // can change the delay length without allocating in the audio path.
    const double sampleRate = EffectiveSampleRate(format);
    m_delayCapacityFrames = static_cast<UINT32>(
        apo::kDynamicsMaxLookaheadMs * 0.001 * sampleRate + 0.5);
    if (m_delayCapacityFrames > 0 && m_channels > 0)
    {
        const size_t count =
            static_cast<size_t>(m_delayCapacityFrames) * m_channels;
        m_delay.assign(count, 0.0);
        m_delayBand.assign(count, 0.0);
    }
    else
    {
        m_delay.clear();
        m_delayBand.clear();
        m_delayCapacityFrames = 0;
    }

    ApplyCoefficients(format);
    Reset();
}

void EqualizerStage::Update(const EffectInstance& effect,
                          const AudioFormat& format)
{
    const bool wasEnabled = m_settings.enabled;
    const EqualizerDynamicsMode oldMode = m_settings.mode;
    const double oldFrequency = m_settings.frequencyHz;
    const double oldQ = m_settings.q;
    const double oldLookahead = m_settings.lookaheadMs;

    m_settings = effect.equalizer;
    ApplyCoefficients(format);

    // Filter shape, dynamic mode, or delay length changes invalidate the
    // stored band and delay state.
    if (wasEnabled != m_settings.enabled || oldMode != m_settings.mode ||
        oldFrequency != m_settings.frequencyHz || oldQ != m_settings.q ||
        oldLookahead != m_settings.lookaheadMs)
    {
        Reset();
    }
}

void EqualizerStage::ApplyCoefficients(const AudioFormat& format)
{
    constexpr double kLocalPi = 3.14159265358979323846;
    const double sampleRate = EffectiveSampleRate(format);

    // Topology-preserving state-variable filter (Simper). `k` is the damping
    // and equals 1/Q; the bandpass output `v1` has a peak gain of Q, so it is
    // scaled by k to normalize the band to unity at the center frequency.
    double frequency = m_settings.frequencyHz;
    const double maxFrequency = sampleRate * 0.49;
    frequency = std::max(20.0, std::min(frequency, maxFrequency));
    const double q = std::max(0.1, m_settings.q);
    const double g = std::tan(kLocalPi * frequency / sampleRate);
    m_k = 1.0 / q;
    m_a1 = 1.0 / (1.0 + g * (g + m_k));
    m_a2 = g * m_a1;
    m_a3 = g * m_a2;

    m_thresholdDb = m_settings.thresholdDb;
    m_thresholdLinear = DecibelsToLinear(m_thresholdDb);
    m_ratio = std::max(1.0, m_settings.ratio);
    m_rangeDb = std::max(0.0, m_settings.rangeDb);
    m_levelDb = m_settings.levelDb;
    m_mode = m_settings.mode;
    m_attackCoeff = TimeCoefficient(m_settings.attackMs, sampleRate);
    m_releaseCoeff = TimeCoefficient(m_settings.releaseMs, sampleRate);

    // A separate fast-attack / slow-release envelope smooths the band peak so
    // the gain does not pulse at the signal's own zero crossings. The attack is
    // scaled to the selected band's period: a fixed 1 ms is only about 0.12
    // cycles at 120 Hz but 5 cycles at 5 kHz, which averages short high-band
    // bursts away before they can reach the threshold. Keeping it near half a
    // cycle makes the detector respond to transients at any frequency; low
    // bands keep the original 1 ms.
    const double bandPeriodMs = 1000.0 / std::max(m_settings.frequencyHz, 20.0);
    const double detectorAttackMs =
        std::clamp(0.5 * bandPeriodMs, 0.05, 1.0);
    m_detectorAttack = TimeCoefficient(detectorAttackMs, sampleRate);
    m_detectorRelease = TimeCoefficient(100.0, sampleRate);

    UINT32 delayFrames = static_cast<UINT32>(
        m_settings.lookaheadMs * 0.001 * sampleRate + 0.5);
    if (delayFrames > m_delayCapacityFrames)
    {
        delayFrames = m_delayCapacityFrames;
    }
    m_delayFrames = delayFrames;

    // Hold is measured at the output, so cover the lookahead as well.
    m_holdFrames =
        static_cast<UINT32>(m_settings.holdMs * 0.001 * sampleRate + 0.5) +
        m_delayFrames;
}

void EqualizerStage::Reset()
{
    m_gainDb = 0.0;
    m_heldTargetDb = 0.0;
    m_expandHeldTargetDb = 0.0;
    m_expandLatchActive = false;
    m_holdCounter = 0;
    m_detectorEnvelope = 0.0;
    m_delayWrite = 0;
    std::fill(m_ic1.begin(), m_ic1.end(), 0.0);
    std::fill(m_ic2.begin(), m_ic2.end(), 0.0);
    std::fill(m_band.begin(), m_band.end(), 0.0);
    std::fill(m_delay.begin(), m_delay.end(), 0.0);
    std::fill(m_delayBand.begin(), m_delayBand.end(), 0.0);
}

double EqualizerStage::MonitorGainDb() const
{
    if (!m_settings.enabled ||
        m_settings.mode == EqualizerDynamicsMode::None)
    {
        return 0.0;
    }
    return m_gainDb;
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

    const bool lookahead =
        m_mode != EqualizerDynamicsMode::None && m_delayFrames > 0 &&
        m_delay.size() >= static_cast<size_t>(m_delayFrames) * channels &&
        m_delayBand.size() >= static_cast<size_t>(m_delayFrames) * channels;
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
        if (m_mode == EqualizerDynamicsMode::Compressor)
        {
            // Cut loud lows, taming a strong proximity effect up close.
            if (level > m_thresholdLinear)
            {
                const double levelDb = 20.0 * std::log10(level);
                const double amount = (levelDb - m_thresholdDb) * slope;
                if (amount > 0.0)
                {
                    targetDb = -std::min(amount, m_rangeDb);
                }
            }
        }
        else if (m_mode == EqualizerDynamicsMode::Expander)
        {
            // Lift quiet lows, restoring body when the mic is farther away.
            // Silence has nothing to lift, so only non-zero bands count.
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

        if (m_mode == EqualizerDynamicsMode::None)
        {
            m_gainDb = 0.0;
        }
        else if (m_mode == EqualizerDynamicsMode::Compressor)
        {
            // Latch a deeper cut so a short peak keeps its gain long enough for
            // attack to act; hold includes the lookahead delay.
            if (std::fabs(targetDb) > std::fabs(m_heldTargetDb) + 1e-6)
            {
                m_heldTargetDb = targetDb;
                m_holdCounter = m_holdFrames;
            }
            if (m_holdCounter > 0)
            {
                --m_holdCounter;
                targetDb = m_heldTargetDb;
            }
            else
            {
                m_heldTargetDb = 0.0;
            }

            const bool deepening =
                std::fabs(targetDb) > std::fabs(m_gainDb) + 1e-6;
            m_gainDb += (targetDb - m_gainDb) *
                        (deepening ? m_attackCoeff : m_releaseCoeff);
        }
        else
        {
            // Expander Hold latches the lowest lift target. A drop happens
            // whenever the band gets louder, even well below the threshold, so
            // the hold does not depend on a threshold crossing. The latched low
            // value pins the target until the timer (Hold plus Lookahead)
            // expires; a further drop re-latches an even lower value and
            // restarts it. Threshold still sets the target's size, but no
            // longer starts the hold.
            if (!m_expandLatchActive ||
                targetDb < m_expandHeldTargetDb - 1e-6)
            {
                m_expandHeldTargetDb = targetDb;
                m_holdCounter = m_holdFrames;
                m_expandLatchActive = true;
            }
            if (m_holdCounter > 0)
            {
                --m_holdCounter;
                targetDb = m_expandHeldTargetDb;
            }
            else
            {
                m_expandLatchActive = false;
            }

            // Opening toward 0 dB is fast; engaging the boost is slow.
            const bool deepening =
                std::fabs(targetDb) > std::fabs(m_gainDb) + 1e-6;
            m_gainDb += (targetDb - m_gainDb) *
                        (deepening ? m_releaseCoeff : m_attackCoeff);
        }

        const double bandGain =
            DecibelsToLinear(m_gainDb + m_levelDb) - 1.0;
        if (lookahead)
        {
            // The detector ran on the incoming band; apply its gain to the
            // matching band from the frame one lookahead behind it.
            double* delayed =
                m_delay.data() + static_cast<size_t>(m_delayWrite) * channels;
            double* delayedBand =
                m_delayBand.data() + static_cast<size_t>(m_delayWrite) * channels;
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                const double current = values[channel];
                const double currentBand = m_band[channel];
                values[channel] =
                    delayed[channel] + delayedBand[channel] * bandGain;
                delayed[channel] = current;
                delayedBand[channel] = currentBand;
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
                values[channel] += m_band[channel] * bandGain;
            }
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
    m_treatAsMono =
        effect.type == EffectType::Input && m_settings.treatAsMono;
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
    if (m_treatAsMono && channels > 1)
    {
        for (UINT32 frame = 0; frame < frames; ++frame)
        {
            double* values = samples + static_cast<size_t>(frame) * channels;
            const double first = values[0];
            for (UINT32 channel = 1; channel < channels; ++channel)
            {
                values[channel] = first;
            }
        }
    }

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
    constexpr double kLocalPi = 3.14159265358979323846;

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
                0.5 - 0.5 * std::cos(2.0 * kLocalPi *
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
    static constexpr int kFdnTunings[] = {1237, 1361, 1499, 1619,
                                          1741, 1867, 2003, 2143};
    static constexpr int kStereoSpread = 23;
    constexpr double kMaxPredelayMs = 200.0;

    m_channels = std::max<UINT32>(1, format.channels);
    m_sampleRate = EffectiveSampleRate(format);
    const double scale = m_sampleRate / 44100.0;

    // Comb/allpass engine. Buffers are fixed for the stream; live controls
    // only change coefficients.
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

    double combSum = 0.0;
    for (const Comb& comb : m_tanks[0].combs)
    {
        combSum += comb.buffer.size();
    }
    m_combAverageSeconds =
        combSum / m_tanks[0].combs.size() / m_sampleRate;

    // Feedback delay network. Eight incommensurate loops keep the resonances
    // from piling up at one pitch.
    m_fdnLines.resize(kFdnLineCount);
    double fdnSum = 0.0;
    for (size_t index = 0; index < m_fdnLines.size(); ++index)
    {
        const size_t length = static_cast<size_t>(
            std::max(1.0, std::round(kFdnTunings[index] * scale)));
        m_fdnLines[index].buffer.assign(length, 0.0);
        m_fdnLines[index].index = 0;
        m_fdnLines[index].store = 0.0;
        fdnSum += length;
    }
    m_fdnAverageSeconds =
        fdnSum / static_cast<double>(m_fdnLines.size()) / m_sampleRate;

    // Reserve the maximum predelay at Configure time so a live Update only
    // moves the read offset and never allocates.
    m_predelayCapacityFrames = static_cast<size_t>(
        std::ceil(m_sampleRate * kMaxPredelayMs / 1000.0)) + 1;
    m_predelay.assign(m_predelayCapacityFrames * m_channels, 0.0);
    m_wetLow.assign(m_channels, 0.0);

    ApplyCoefficients();
    Reset();
}

void ReverbStage::Update(const EffectInstance& effect,
                         const AudioFormat& format)
{
    (void)format;
    const bool wasEnabled = m_settings.enabled;
    const ReverbAlgorithm wasAlgorithm = m_settings.algorithm;
    m_settings = effect.reverb;
    ApplyCoefficients();
    if (wasEnabled != m_settings.enabled || wasAlgorithm != m_settings.algorithm)
    {
        // Drop both tails so switching engine or re-enabling does not replay
        // old audio through a different network.
        Reset();
    }
}

void ReverbStage::ApplyCoefficients()
{
    const double size = std::clamp(m_settings.size / 100.0, 0.0, 1.0);
    const double damping =
        std::clamp(m_settings.damping / 100.0, 0.0, 1.0);
    const double width = std::clamp(m_settings.width / 100.0, 0.0, 1.0);
    const double density =
        std::clamp(m_settings.density / 100.0, 0.0, 1.0);

    m_level = std::clamp(m_settings.level / 100.0, 0.0, 1.0);
    m_size = size;
    m_width = width;
    m_density = density;
    m_damping = 0.4 * damping;
    m_lowGain = 1.0 + 0.35 * size;
    m_wetLowCoeff = 0.05;
    m_monoMix = 0.75 - 0.5 * width;

    if (m_settings.algorithm == ReverbAlgorithm::FeedbackDelayNetwork)
    {
        m_fdnGain = FeedbackForDecay(m_settings.maxLengthMs,
                                     m_fdnAverageSeconds);

        // Build an orthogonal mixing matrix from pairwise rotations. A linear
        // blend of identity and Hadamard has a zero eigenvalue at 50%, which
        // kills most modes and makes the tail collapse. Rotations keep every
        // mode's magnitude, so Density changes diffusion without changing
        // loudness.
        double matrix[kFdnLineCount * kFdnLineCount] = {};
        for (size_t index = 0; index < kFdnLineCount; ++index)
        {
            matrix[index * kFdnLineCount + index] = 1.0;
        }
        const double angle = density * (std::acos(-1.0) / 4.0);
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        auto rotate = [&](size_t first, size_t second) {
            for (size_t row = 0; row < kFdnLineCount; ++row)
            {
                const double a = matrix[row * kFdnLineCount + first];
                const double b = matrix[row * kFdnLineCount + second];
                matrix[row * kFdnLineCount + first] = a * cosine - b * sine;
                matrix[row * kFdnLineCount + second] = a * sine + b * cosine;
            }
        };
        rotate(0, 1);
        rotate(2, 3);
        rotate(4, 5);
        rotate(6, 7);
        rotate(1, 2);
        rotate(3, 4);
        rotate(5, 6);
        rotate(7, 0);

        for (size_t row = 0; row < kFdnLineCount; ++row)
        {
            for (size_t column = 0; column < kFdnLineCount; ++column)
            {
                m_fdnMatrix[row * kFdnLineCount + column] =
                    matrix[row * kFdnLineCount + column] * m_fdnGain;
            }
        }

        for (size_t index = 0; index < kFdnLineCount; ++index)
        {
            const double sign = kFdnOutputSigns[index];
            const double base = 1.0 - 0.5 * width;
            const double spread = 0.5 * width;
            m_fdnLeftTap[index] = base + spread * sign;
            m_fdnRightTap[index] = base - spread * sign;
        }

        // Damping is the main high-frequency control. Size mostly trades
        // low-end body: low values cut it, high values boost it.
        m_damping = 0.02 + 0.60 * damping + 0.15 * size;
        m_lowGain = 0.2 + 1.8 * size;
        m_wetLowCoeff = 0.15;
        m_wetScale = m_level * 4.0 * (0.8 + 0.2 * damping);
    }
    else
    {
        // Size shapes resonance and low-end weight; Max Length caps decay.
        m_feedback = std::min(
            0.7 + 0.28 * size,
            FeedbackForDecay(m_settings.maxLengthMs, m_combAverageSeconds));
        m_allpassFeedback = 0.25 + 0.45 * density;
        // Width's default mono blend halves the excitation compared with the
        // old fixed mix, so the wet scaling is doubled to preserve its level.
        m_wetScale = m_level * 5.0 * (1.0 - 0.35 * size) *
                     (0.75 + 0.25 * damping);
    }

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
    for (FdnLine& line : m_fdnLines)
    {
        std::fill(line.buffer.begin(), line.buffer.end(), 0.0);
        line.index = 0;
        line.store = 0.0;
    }

    std::fill(m_predelay.begin(), m_predelay.end(), 0.0);
    std::fill(m_wetLow.begin(), m_wetLow.end(), 0.0);
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
    const double next = input + buffered * m_allpassFeedback;
    allpass.buffer[allpass.index] = (std::fabs(next) < 1e-20) ? 0.0 : next;

    if (++allpass.index >= allpass.buffer.size())
    {
        allpass.index = 0;
    }
    return buffered - input;
}

void ReverbStage::ProcessFdn(double excitation, double& left, double& right)
{
    double outputs[kFdnLineCount] = {};
    double feedback[kFdnLineCount] = {};

    for (size_t index = 0; index < kFdnLineCount; ++index)
    {
        FdnLine& line = m_fdnLines[index];
        if (line.buffer.empty())
        {
            continue;
        }
        outputs[index] = line.buffer[line.index];
        line.store = outputs[index] * (1.0 - m_damping) +
                     line.store * m_damping;
        if (std::fabs(line.store) < 1e-20)
        {
            line.store = 0.0;
        }
        feedback[index] = line.store;
    }

    double mixed[kFdnLineCount] = {};
    for (size_t row = 0; row < kFdnLineCount; ++row)
    {
        double value = 0.0;
        for (size_t column = 0; column < kFdnLineCount; ++column)
        {
            value += m_fdnMatrix[row * kFdnLineCount + column] *
                     feedback[column];
        }
        mixed[row] = value;
    }

    for (size_t index = 0; index < kFdnLineCount; ++index)
    {
        FdnLine& line = m_fdnLines[index];
        if (line.buffer.empty())
        {
            continue;
        }
        const double next = mixed[index] +
                            excitation * kFdnInputSigns[index] * 0.5;
        line.buffer[line.index] = (std::fabs(next) < 1e-20) ? 0.0 : next;
        if (++line.index >= line.buffer.size())
        {
            line.index = 0;
        }
        left += outputs[index] * m_fdnLeftTap[index];
        right += outputs[index] * m_fdnRightTap[index];
    }
}

void ReverbStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled || m_settings.level <= 0.0 ||
        samples == nullptr || frames == 0 || channels == 0 ||
        channels != m_channels || m_tanks.size() != channels ||
        m_fdnLines.size() != kFdnLineCount || m_predelayCapacityFrames == 0)
    {
        return;
    }

    const bool fdn =
        m_settings.algorithm == ReverbAlgorithm::FeedbackDelayNetwork;

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
            // Narrow settings favor the mono sum; wide settings keep more of
            // the individual channel so the two tails decorrelate.
            const double excitation =
                (values[channel] * (1.0 - m_monoMix) + mono * m_monoMix) *
                0.015;
            m_predelay[m_predelayWrite * channels + channel] = excitation;
        }

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            double wet = 0.0;
            if (fdn)
            {
                // The network is shared. Build its stereo output once, then
                // fold it down for mono or alternate it for multichannel.
                if (channel == 0)
                {
                    double delayedMono = 0.0;
                    for (UINT32 source = 0; source < channels; ++source)
                    {
                        delayedMono +=
                            m_predelay[readIndex * channels + source];
                    }
                    delayedMono /= static_cast<double>(channels);

                    double left = 0.0;
                    double right = 0.0;
                    ProcessFdn(delayedMono, left, right);
                    m_fdnLeft = left;
                    m_fdnRight = right;
                }
                wet = (channels == 1)
                          ? (m_fdnLeft + m_fdnRight) * 0.5
                          : ((channel % 2 == 0) ? m_fdnLeft : m_fdnRight);
            }
            else
            {
                const double delayed =
                    m_predelay[readIndex * channels + channel];
                Tank& tank = m_tanks[channel];
                for (Comb& comb : tank.combs)
                {
                    wet += ProcessComb(comb, delayed);
                }
                for (Allpass& allpass : tank.allpasses)
                {
                    wet = ProcessAllpass(allpass, wet);
                }
            }

            if (m_wetLow.size() > channel)
            {
                m_wetLow[channel] +=
                    (wet - m_wetLow[channel]) * m_wetLowCoeff;
                // wet = high + low. Size scales only the low band, so low
                // settings sound tight and high settings gain body.
                wet = (wet - m_wetLow[channel]) +
                      m_wetLow[channel] * m_lowGain;
            }
            values[channel] += wet * m_wetScale;
        }

        if (++m_predelayWrite >= m_predelayCapacityFrames)
        {
            m_predelayWrite = 0;
        }
    }
}

void EchoStage::Configure(const AudioFormat& format)
{
    m_channels = std::max<UINT32>(1, format.channels);
    m_sampleRate = EffectiveSampleRate(format);

    // Allocate the full Time range so a live retune only moves the read offset.
    const size_t frames = static_cast<size_t>(
        std::ceil(m_sampleRate * apo::kEchoMaxTimeMs / 1000.0));
    m_capacityFrames = std::max<size_t>(frames + 2, 2);
    m_delay.assign(m_capacityFrames * m_channels, 0.0);
    m_feedbackLow.assign(m_channels, 0.0);

    ApplyCoefficients();
    Reset();
}

void EchoStage::Update(const EffectInstance& effect,
                       const AudioFormat& format)
{
    (void)format;
    const bool wasEnabled = m_settings.enabled;
    const EchoMode wasMode = m_settings.mode;
    m_settings = effect.echo;
    ApplyCoefficients();
    if (wasEnabled != m_settings.enabled || wasMode != m_settings.mode)
    {
        Reset();
    }
}

void EchoStage::ApplyCoefficients()
{
    const double damping = std::clamp(m_settings.damping / 100.0, 0.0, 1.0);
    const double frames =
        std::ceil(m_settings.timeMs * 0.001 * m_sampleRate);
    m_delayFrames = static_cast<size_t>(std::clamp(
        frames, 1.0, static_cast<double>(m_capacityFrames - 1)));
    m_feedback = std::clamp(m_settings.feedback / 100.0, 0.0, 0.98);
    m_level = std::clamp(m_settings.level / 100.0, 0.0, 1.0);

    if (damping <= 0.0)
    {
        m_dampPole = 0.0;
    }
    else
    {
        // Damping 100 leaves a 200 Hz loop; 0 is a transparent feedback path.
        const double cutoff =
            200.0 + 19800.0 * std::pow(1.0 - damping, 2.0);
        m_dampPole = std::clamp(
            std::exp(-2.0 * kPi * cutoff / m_sampleRate), 0.0, 0.99);
    }
}

void EchoStage::Reset()
{
    std::fill(m_delay.begin(), m_delay.end(), 0.0);
    std::fill(m_feedbackLow.begin(), m_feedbackLow.end(), 0.0);
    m_write = 0;
}

void EchoStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled || samples == nullptr || frames == 0 ||
        channels == 0 || channels != m_channels ||
        m_capacityFrames < 2 || m_delay.size() < m_capacityFrames * channels ||
        m_feedbackLow.size() < channels)
    {
        return;
    }

    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;
        const size_t readIndex =
            (m_write + m_capacityFrames - m_delayFrames) % m_capacityFrames;

        // Snapshot the damped taps before writing any feedback into the delay.
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            const double delayed =
                m_delay[readIndex * channels + channel];
            m_feedbackLow[channel] +=
                (delayed - m_feedbackLow[channel]) * (1.0 - m_dampPole);
        }

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            const UINT32 source =
                (m_settings.mode == EchoMode::PingPong)
                    ? ((channel + 1) % channels)
                    : channel;
            m_delay[m_write * channels + channel] =
                values[channel] + m_feedbackLow[source] * m_feedback;
        }

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            const double delayed =
                m_delay[readIndex * channels + channel];
            values[channel] += delayed * m_level;
        }

        if (++m_write >= m_capacityFrames)
        {
            m_write = 0;
        }
    }
}

namespace {

constexpr double kChorusBaseDelayMs = 15.0;

} // namespace

void ChorusStage::Configure(const AudioFormat& format)
{
    m_channels = std::max<UINT32>(1, format.channels);
    m_sampleRate = EffectiveSampleRate(format);
    const double maxDelayMs = kChorusBaseDelayMs + apo::kChorusMaxDepthMs;
    const size_t frames = static_cast<size_t>(
        std::ceil(m_sampleRate * maxDelayMs / 1000.0));
    m_capacityFrames = std::max<size_t>(frames + 8, 16);

    m_voices.resize(apo::kChorusMaxVoices);
    for (Voice& voice : m_voices)
    {
        voice.buffer.assign(m_capacityFrames * m_channels, 0.0);
        voice.phase = 0.0;
    }
    m_wet.assign(m_channels, 0.0);
    m_input.assign(m_channels, 0.0);

    ApplyCoefficients();
    Reset();
}

void ChorusStage::Update(const EffectInstance& effect,
                         const AudioFormat& format)
{
    (void)format;
    const bool wasEnabled = m_settings.enabled;
    const int wasVoices = m_settings.voices;
    m_settings = effect.chorus;
    ApplyCoefficients();
    if (wasEnabled != m_settings.enabled || wasVoices != m_settings.voices)
    {
        Reset();
    }
}

void ChorusStage::ApplyCoefficients()
{
    const double depthFrames = m_settings.depthMs * 0.001 * m_sampleRate;
    m_activeVoices = std::clamp(m_settings.voices, apo::kChorusMinVoices,
                                apo::kChorusMaxVoices);
    m_baseDelayFrames =
        kChorusBaseDelayMs * 0.001 * m_sampleRate + depthFrames * 0.5;
    m_modDepthFrames = depthFrames * 0.5;
    m_phaseStep = 2.0 * kPi * m_settings.rateHz / m_sampleRate;

    const double width = std::clamp(m_settings.width / 100.0, 0.0, 1.0);
    m_voiceSpread = 2.0 * kPi * width;
    m_channelSpread = 0.25 * m_voiceSpread;
    m_level = std::clamp(m_settings.level / 100.0, 0.0, 1.0);
}

void ChorusStage::Reset()
{
    std::fill(m_wet.begin(), m_wet.end(), 0.0);
    std::fill(m_input.begin(), m_input.end(), 0.0);
    m_write = 0;
    for (size_t index = 0; index < m_voices.size(); ++index)
    {
        Voice& voice = m_voices[index];
        std::fill(voice.buffer.begin(), voice.buffer.end(), 0.0);
        // Voice spread is applied in Process so Width can change live without
        // replaying the buffered tail.
        voice.phase = 0.0;
    }
}

double ChorusStage::Read(const Voice& voice, UINT32 channel,
                         double position) const
{
    const size_t capacity = m_capacityFrames;
    if (capacity == 0 || voice.buffer.size() < capacity * m_channels)
    {
        return 0.0;
    }
    const double wrapped = WrapPosition(position, capacity);
    const size_t first = static_cast<size_t>(std::floor(wrapped));
    const size_t second = (first + 1) % capacity;
    const double fraction = wrapped - static_cast<double>(first);
    const double a = voice.buffer[first * m_channels + channel];
    const double b = voice.buffer[second * m_channels + channel];
    return a + (b - a) * fraction;
}

void ChorusStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled || samples == nullptr || frames == 0 ||
        channels == 0 || channels != m_channels || m_capacityFrames < 16 ||
        m_voices.size() < static_cast<size_t>(m_activeVoices) ||
        m_wet.size() < channels || m_input.size() < channels)
    {
        return;
    }

    const double wetScale = m_level / static_cast<double>(m_activeVoices);
    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            m_input[channel] = values[channel];
            m_wet[channel] = 0.0;
        }

        const double divisor = static_cast<double>(m_activeVoices);
        for (int voiceIndex = 0; voiceIndex < m_activeVoices; ++voiceIndex)
        {
            Voice& voice = m_voices[static_cast<size_t>(voiceIndex)];
            const double voiceOffset =
                m_voiceSpread * (static_cast<double>(voiceIndex) / divisor);
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                const double phase = voice.phase + voiceOffset +
                                     channel * m_channelSpread;
                double delay =
                    m_baseDelayFrames + std::sin(phase) * m_modDepthFrames;
                delay = std::clamp(
                    delay, 2.0, static_cast<double>(m_capacityFrames - 2));
                m_wet[channel] += Read(voice, channel, delay);
            }
            voice.phase += m_phaseStep;
            if (voice.phase >= 2.0 * kPi)
            {
                voice.phase -= 2.0 * kPi;
            }
        }

        for (int voiceIndex = 0; voiceIndex < m_activeVoices; ++voiceIndex)
        {
            Voice& voice = m_voices[static_cast<size_t>(voiceIndex)];
            const size_t offset = m_write * channels;
            for (UINT32 channel = 0; channel < channels; ++channel)
            {
                voice.buffer[offset + channel] = m_input[channel];
            }
        }

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            values[channel] += m_wet[channel] * wetScale;
        }

        if (++m_write >= m_capacityFrames)
        {
            m_write = 0;
        }
    }
}

void TremoloStage::Configure(const AudioFormat& format)
{
    m_channels = std::max<UINT32>(1, format.channels);
    m_sampleRate = EffectiveSampleRate(format);
    m_state.assign(m_channels, ChannelState{});
    ApplyCoefficients();
    Reset();
}

void TremoloStage::Update(const EffectInstance& effect,
                          const AudioFormat& format)
{
    (void)format;
    m_settings = effect.tremolo;
    ApplyCoefficients();
}

void TremoloStage::ApplyCoefficients()
{
    m_phaseStep = m_settings.rateHz / m_sampleRate;
    m_depth = std::clamp(m_settings.depth / 100.0, 0.0, 1.0);
    m_spreadCycles =
        0.5 * std::clamp(m_settings.stereoSpread / 100.0, 0.0, 1.0);
    // Smooth the gain a little so a square LFO changes level over about 2 ms
    // instead of stepping between samples.
    m_smoothCoeff = 1.0 - std::exp(-2.0 * kPi * 80.0 / m_sampleRate);
}

void TremoloStage::Reset()
{
    for (ChannelState& state : m_state)
    {
        state.phase = 0.0;
        state.gain = 1.0;
    }
}

void TremoloStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled || samples == nullptr || frames == 0 ||
        channels == 0 || channels != m_channels ||
        m_state.size() < channels)
    {
        return;
    }

    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<std::size_t>(frame) * channels;
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            ChannelState& state = m_state[channel];
            state.phase = WrapPhase(state.phase + m_phaseStep);
            const double channelPhase =
                WrapPhase(state.phase + ((channel & 1U) ? m_spreadCycles : 0.0));
            const double lfo =
                ModulationValue(channelPhase, m_settings.waveform);
            const double targetGain = 1.0 - m_depth * (0.5 - 0.5 * lfo);
            state.gain += (targetGain - state.gain) * m_smoothCoeff;
            values[channel] *= state.gain;
        }
    }
}

void VibratoStage::Configure(const AudioFormat& format)
{
    m_channels = std::max<UINT32>(1, format.channels);
    m_sampleRate = EffectiveSampleRate(format);

    // Size the buffer for the deepest, slowest setting so a live Depth or Rate
    // change only retunes the LFO and never allocates. Square has the largest
    // excursion because its pitch offset is held for a whole half-cycle.
    const double maxRatio =
        std::pow(2.0, apo::kVibratoMaxDepthCents / 1200.0);
    const double minRatio = 1.0 / maxRatio;
    const double squareDuty = (1.0 - minRatio) / (maxRatio - minRatio);
    const double maxAmplitude = m_sampleRate * (maxRatio - 1.0) * squareDuty /
                                apo::kModulationMinRateHz;
    m_capacityFrames = std::max<std::size_t>(
        16, static_cast<std::size_t>(std::ceil(2.0 * maxAmplitude + 32.0)));
    m_baseDelay = static_cast<double>(m_capacityFrames) * 0.5;

    m_channelsState.assign(m_channels, DelayChannel{});
    for (DelayChannel& channel : m_channelsState)
    {
        channel.buffer.assign(m_capacityFrames, 0.0);
    }
    ApplyCoefficients();
    Reset();
}

void VibratoStage::Update(const EffectInstance& effect,
                          const AudioFormat& format)
{
    (void)format;
    m_settings = effect.vibrato;
    ApplyCoefficients();
}

void VibratoStage::ApplyCoefficients()
{
    const double rate =
        std::clamp(m_settings.rateHz, apo::kModulationMinRateHz,
                   apo::kModulationMaxRateHz);
    m_phaseStep = rate / m_sampleRate;
    const double cents =
        std::clamp(m_settings.depthCents, 0.0, apo::kVibratoMaxDepthCents);
    m_ratioHigh = std::pow(2.0, cents / 1200.0);
    m_ratioLow = 1.0 / m_ratioHigh;

    if (cents <= 0.0)
    {
        m_ratioMean = 1.0;
        m_squareDuty = 0.5;
    }
    else if (m_settings.waveform == ModulationWaveform::Square)
    {
        // Hold the two exact pitch ratios long enough that their mean is 1.
        // The read head then returns to the same place every cycle.
        m_squareDuty = std::clamp(
            (1.0 - m_ratioLow) / (m_ratioHigh - m_ratioLow), 0.0, 1.0);
        m_ratioMean = 1.0;
    }
    else
    {
        // Sine and Triangle get a centred ratio so their average pitch is
        // unchanged; their endpoints stay within a few cents of Depth.
        const double meanAbs =
            m_settings.waveform == ModulationWaveform::Sine ? 2.0 / kPi : 0.5;
        m_ratioMean =
            1.0 + 0.5 * ((m_ratioHigh - 1.0) - (1.0 - m_ratioLow)) * meanAbs;
        m_squareDuty = 0.5;
    }

    m_spreadCycles =
        0.5 * std::clamp(m_settings.stereoSpread / 100.0, 0.0, 1.0);
}

void VibratoStage::Reset()
{
    for (DelayChannel& channel : m_channelsState)
    {
        std::fill(channel.buffer.begin(), channel.buffer.end(), 0.0);
        channel.write = 0;
        channel.phase = 0.0;
        channel.delay = m_baseDelay;
    }
}

double VibratoStage::Read(const DelayChannel& channel, double position) const
{
    const std::size_t capacity = m_capacityFrames;
    if (capacity == 0 || channel.buffer.size() < capacity)
    {
        return 0.0;
    }
    const double wrapped = WrapPosition(position, capacity);
    const std::size_t first = static_cast<std::size_t>(std::floor(wrapped));
    const std::size_t second = (first + 1) % capacity;
    const double fraction = wrapped - static_cast<double>(first);
    const double a = channel.buffer[first];
    const double b = channel.buffer[second];
    return a + (b - a) * fraction;
}

void VibratoStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (!m_settings.enabled || samples == nullptr || frames == 0 ||
        channels == 0 || channels != m_channels ||
        m_channelsState.size() < channels || m_capacityFrames < 16)
    {
        return;
    }

    const double maxDelay = static_cast<double>(m_capacityFrames - 2);
    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<std::size_t>(frame) * channels;
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            DelayChannel& state = m_channelsState[channel];
            state.phase = WrapPhase(state.phase + m_phaseStep);
            const double channelPhase =
                WrapPhase(state.phase + ((channel & 1U) ? m_spreadCycles : 0.0));
            const double input = values[channel];

            if (m_settings.depthCents <= 0.0)
            {
                // Keep the history fresh so raising Depth does not replay an
                // old tail, but pass the current sample through untouched.
                state.buffer[state.write] = input;
                state.delay = m_baseDelay;
                values[channel] = input;
            }
            else
            {
                double advance = 1.0;
                if (m_settings.waveform == ModulationWaveform::Square)
                {
                    // The duty-compensated square holds +Depth cents for the
                    // first part of each cycle and -Depth cents for the rest.
                    advance = channelPhase < m_squareDuty ? m_ratioHigh
                                                          : m_ratioLow;
                }
                else
                {
                    const double lfo =
                        ModulationValue(channelPhase, m_settings.waveform);
                    const double ratio =
                        1.0 + (lfo >= 0.0 ? (m_ratioHigh - 1.0) * lfo
                                          : (1.0 - m_ratioLow) * lfo);
                    advance = 1.0 + ratio - m_ratioMean;
                }

                // Integrating the pitch ratio keeps the fractional delay
                // continuous: the read head never jumps at a Square transition.
                state.delay = std::clamp(state.delay + 1.0 - advance, 1.0,
                                         maxDelay);
                const double readPosition =
                    WrapPosition(static_cast<double>(state.write) - state.delay,
                                 m_capacityFrames);
                values[channel] = Read(state, readPosition);
                state.buffer[state.write] = input;
            }

            if (++state.write >= m_capacityFrames)
            {
                state.write = 0;
            }
        }
    }
}

void PitchShifterStage::Configure(const AudioFormat& format)
{
    m_channels = std::max<UINT32>(1, format.channels);
    m_sampleRate = EffectiveSampleRate(format);
    m_maxWindow = apo::kPitchMaxWindowSamples;
    m_maxFft = NextPowerOfTwo(m_maxWindow);

    const size_t grainCapacity = 6 * m_maxWindow + 8;
    m_grainChannels.resize(m_channels);
    for (GrainChannel& channel : m_grainChannels)
    {
        channel.input.assign(grainCapacity, 0.0);
        channel.write = 0;
        channel.position[0] = 0.0;
        channel.position[1] = 0.0;
        channel.phase[0] = 0.0;
        channel.phase[1] = 0.0;
    }

    const size_t vocoderCapacity = 2 * m_maxFft + 8;
    m_vocoderChannels.resize(m_channels);
    for (VocoderChannel& channel : m_vocoderChannels)
    {
        channel.input.assign(vocoderCapacity, 0.0);
        channel.overlap.assign(vocoderCapacity, 0.0);
        channel.lastPhase.assign(m_maxFft / 2 + 1, 0.0);
        channel.synthPhase.assign(m_maxFft / 2 + 1, 0.0);
        channel.magnitudes.assign(m_maxFft / 2 + 1, 0.0);
        channel.frequencySum.assign(m_maxFft / 2 + 1, 0.0);
        channel.phaseReal.assign(m_maxFft / 2 + 1, 0.0);
        channel.phaseImag.assign(m_maxFft / 2 + 1, 0.0);
        channel.targetCount.assign(m_maxFft / 2 + 1, 0);
        channel.phaseValid.assign(m_maxFft / 2 + 1, 0);
        channel.write = 0;
        channel.overlapWrite = 0;
        channel.frameCounter = 0;
    }

    m_fftPlans.clear();
    m_windows.clear();
    for (size_t size = 512; size <= m_maxFft; size <<= 1)
    {
        m_fftPlans.push_back(std::make_unique<FftPlan>(size));
        std::vector<double> window(size);
        for (size_t index = 0; index < size; ++index)
        {
            window[index] = 0.5 - 0.5 * std::cos(
                2.0 * kPi * static_cast<double>(index) /
                static_cast<double>(size - 1));
        }
        m_windows.push_back(std::move(window));
    }
    m_real.assign(m_maxFft, 0.0);
    m_imag.assign(m_maxFft, 0.0);
    m_wet.assign(m_channels, 0.0);
    m_inputFrame.assign(m_channels, 0.0);

    ApplyCoefficients();
    Reset();
}

void PitchShifterStage::Update(const EffectInstance& effect,
                               const AudioFormat& format)
{
    (void)format;
    const bool wasEnabled = m_settings.enabled;
    const PitchAlgorithm wasAlgorithm = m_settings.algorithm;
    const size_t wasWindow = m_windowSamples;
    m_settings = effect.pitchShifter;
    ApplyCoefficients();
    if (wasEnabled != m_settings.enabled ||
        wasAlgorithm != m_settings.algorithm || wasWindow != m_windowSamples)
    {
        Reset();
    }
}

void PitchShifterStage::ApplyCoefficients()
{
    m_ratio = std::pow(2.0, m_settings.shiftSemitones / 12.0);
    m_level = std::clamp(m_settings.level / 100.0, 0.0, 1.0);
    m_dryLevel = std::clamp(m_settings.dryLevel / 100.0, 0.0, 1.0);
    m_windowSamples = static_cast<size_t>(
        apo::ValidPitchWindow(m_settings.windowSamples));

    if (m_settings.algorithm == PitchAlgorithm::PhaseVocoder)
    {
        m_fftSize = std::min(m_maxFft, NextPowerOfTwo(m_windowSamples));
    }
    else
    {
        m_fftSize = m_windowSamples;
    }
    m_fftSize = std::clamp<size_t>(m_fftSize, 512, m_maxFft);
    m_fftIndex = FftSizeIndex(m_fftSize);

    m_grainHopFrames = std::max<size_t>(1, m_windowSamples / 2);
    m_vocoderHopFrames = std::max<size_t>(1, m_fftSize / 4);
    // A grain lasts the full Window; the other grain starts half a Window
    // later. Wrapping after a hop made both grains restart twice as often and
    // produced a pitched-up splice instead of a shift.
    m_grainPhaseStep = 1.0 / static_cast<double>(m_windowSamples);
    // Synthesis phase advances by the analysis hop for each output bin. The
    // pitch ratio is applied by moving magnitudes to new bins, not by adding
    // an extra phase term to the same bins.
    m_synthPhaseStep = 2.0 * kPi * static_cast<double>(m_vocoderHopFrames) /
                       static_cast<double>(m_fftSize);
}

void PitchShifterStage::Reset()
{
    for (GrainChannel& channel : m_grainChannels)
    {
        ResetGrain(channel);
    }
    for (VocoderChannel& channel : m_vocoderChannels)
    {
        ResetVocoder(channel);
    }
}

void PitchShifterStage::ResetGrain(GrainChannel& channel) const
{
    std::fill(channel.input.begin(), channel.input.end(), 0.0);
    channel.write = 0;
    // The read head that starts half a Window late must still stay behind the
    // write pointer after an upward shift has advanced it by (ratio - 1) *
    // Window. The 64-sample margin also covers the WSOLA search.
    const double startLag =
        static_cast<double>(m_windowSamples) * std::max(m_ratio, 1.0) + 64.0;
    channel.position[0] = WrapPosition(-startLag, channel.input.size());
    // The second grain began one hop earlier in output time, so at this
    // instant it is (1 - ratio) hops older in the input history.
    channel.position[1] = WrapPosition(
        channel.position[0] -
            static_cast<double>(m_grainHopFrames) * (1.0 - m_ratio),
        channel.input.size());
    channel.phase[0] = 0.0;
    channel.phase[1] = 0.5;
}

void PitchShifterStage::ResetVocoder(VocoderChannel& channel) const
{
    std::fill(channel.input.begin(), channel.input.end(), 0.0);
    std::fill(channel.overlap.begin(), channel.overlap.end(), 0.0);
    std::fill(channel.lastPhase.begin(), channel.lastPhase.end(), 0.0);
    std::fill(channel.synthPhase.begin(), channel.synthPhase.end(), 0.0);
    std::fill(channel.magnitudes.begin(), channel.magnitudes.end(), 0.0);
    std::fill(channel.frequencySum.begin(), channel.frequencySum.end(), 0.0);
    std::fill(channel.phaseReal.begin(), channel.phaseReal.end(), 0.0);
    std::fill(channel.phaseImag.begin(), channel.phaseImag.end(), 0.0);
    std::fill(channel.targetCount.begin(), channel.targetCount.end(), 0);
    std::fill(channel.phaseValid.begin(), channel.phaseValid.end(),
              static_cast<unsigned char>(0));
    channel.write = 0;
    channel.overlapWrite = 0;
    channel.frameCounter = 0;
}

double PitchShifterStage::ReadGrain(const GrainChannel& channel,
                                    double position) const
{
    const size_t capacity = channel.input.size();
    if (capacity == 0)
    {
        return 0.0;
    }
    const double wrapped = WrapPosition(position, capacity);
    const size_t first = static_cast<size_t>(std::floor(wrapped));
    const size_t second = (first + 1) % capacity;
    const double fraction = wrapped - static_cast<double>(first);
    const double a = channel.input[first];
    const double b = channel.input[second];
    return a + (b - a) * fraction;
}

double PitchShifterStage::GrainMaxLag() const
{
    // Grain restarts keep the lag between one and two Window lengths even at
    // the ±24-semitone extremes. This ceiling prevents a stalled read head
    // from turning into the multi-second delay the old full-capacity clamp
    // allowed on downshifts.
    const double window = static_cast<double>(m_windowSamples);
    const double hop = static_cast<double>(m_grainHopFrames);
    return window * std::max(m_ratio, 1.0) +
           hop * std::max(1.0, std::abs(1.0 - m_ratio)) + 128.0;
}

double PitchShifterStage::LimitedPosition(const GrainChannel& channel,
                                          double position,
                                          double writePosition) const
{
    const size_t capacity = channel.input.size();
    if (capacity == 0)
    {
        return 0.0;
    }
    const double write = WrapPosition(writePosition, capacity);
    double lag = WrapPosition(write - position, capacity);
    lag = std::clamp(lag, 2.0, GrainMaxLag());
    return WrapPosition(write - lag, capacity);
}

double PitchShifterStage::SearchAlignment(const GrainChannel& channel,
                                          double nominal, double reference,
                                          double writePosition) const
{
    constexpr int kSearchSamples = 48;
    const size_t length = std::min<size_t>(
        std::max<size_t>(16, m_windowSamples / 4), 256);
    const double referencePosition =
        LimitedPosition(channel, reference, writePosition);
    double bestPosition = nominal;
    double bestError = 1e300;

    for (int offset = -kSearchSamples; offset <= kSearchSamples; ++offset)
    {
        const double candidate = LimitedPosition(
            channel, nominal + static_cast<double>(offset), writePosition);
        double error = 0.0;
        for (size_t index = 0; index < length; ++index)
        {
            const double delta =
                ReadGrain(channel, candidate + static_cast<double>(index)) -
                ReadGrain(channel, referencePosition +
                                       static_cast<double>(index));
            error += delta * delta;
        }
        if (error < bestError)
        {
            bestError = error;
            bestPosition = candidate;
        }
    }
    return bestPosition;
}

double PitchShifterStage::ProcessGrain(GrainChannel& channel, double input,
                                       bool wsola)
{
    const size_t capacity = channel.input.size();
    if (capacity == 0)
    {
        return 0.0;
    }

    const double oldPosition[kGrainCount] = {channel.position[0],
                                             channel.position[1]};
    const double oldPhase[kGrainCount] = {channel.phase[0],
                                          channel.phase[1]};
    double wet = 0.0;
    for (size_t grain = 0; grain < kGrainCount; ++grain)
    {
        const double window =
            0.5 - 0.5 * std::cos(2.0 * kPi * oldPhase[grain]);
        wet += ReadGrain(channel, oldPosition[grain]) * window;
    }

    channel.input[channel.write] = input;
    channel.write = (channel.write + 1) % capacity;
    const double writePosition = static_cast<double>(channel.write);

    for (size_t grain = 0; grain < kGrainCount; ++grain)
    {
        double position = oldPosition[grain] + m_ratio;
        double phase = oldPhase[grain] + m_grainPhaseStep;
        if (phase >= 1.0)
        {
            phase -= 1.0;
            const size_t other = (grain + 1) % kGrainCount;
            // Start the new grain one hop ahead of the still-active grain in
            // input time. That jump supplies the missing (1 - ratio) samples
            // each hop, so a downshift no longer walks the read pointer to the
            // far end of the history and sticks there.
            double restart =
                oldPosition[other] +
                static_cast<double>(m_grainHopFrames) * (1.0 - m_ratio);
            if (wsola)
            {
                restart = SearchAlignment(channel, restart,
                                          oldPosition[other], writePosition);
            }
            position = restart;
        }
        channel.position[grain] =
            LimitedPosition(channel, position, writePosition);
        channel.phase[grain] = phase;
    }
    return wet;
}

void PitchShifterStage::AnalyzeVocoder(VocoderChannel& channel,
                                       size_t writeIndex)
{
    const size_t bins = m_fftSize / 2 + 1;
    const size_t nyquist = m_fftSize / 2;
    if (m_fftIndex < 0 || m_fftIndex >= static_cast<int>(m_fftPlans.size()) ||
        m_windows.size() <= static_cast<size_t>(m_fftIndex) ||
        m_real.size() < m_fftSize || m_imag.size() < m_fftSize ||
        channel.lastPhase.size() < bins || channel.synthPhase.size() < bins ||
        channel.magnitudes.size() < bins || channel.frequencySum.size() < bins ||
        channel.phaseReal.size() < bins || channel.phaseImag.size() < bins ||
        channel.targetCount.size() < bins || channel.phaseValid.size() < bins)
    {
        return;
    }

    const size_t capacity = channel.input.size();
    const double start = static_cast<double>(writeIndex) + 1.0 -
                         static_cast<double>(m_fftSize);
    const std::vector<double>& window = m_windows[static_cast<size_t>(m_fftIndex)];
    for (size_t index = 0; index < m_fftSize; ++index)
    {
        const size_t source = static_cast<size_t>(
            WrapPosition(start + static_cast<double>(index), capacity));
        m_real[index] = channel.input[source] * window[index];
        m_imag[index] = 0.0;
    }

    const FftPlan& plan = *m_fftPlans[static_cast<size_t>(m_fftIndex)];
    plan.Forward(m_real.data(), m_imag.data());

    std::fill(channel.magnitudes.begin(), channel.magnitudes.begin() + bins,
              0.0);
    std::fill(channel.frequencySum.begin(),
              channel.frequencySum.begin() + bins, 0.0);
    std::fill(channel.phaseReal.begin(), channel.phaseReal.begin() + bins, 0.0);
    std::fill(channel.phaseImag.begin(), channel.phaseImag.begin() + bins, 0.0);
    std::fill(channel.targetCount.begin(), channel.targetCount.begin() + bins,
              0);

    // Analysis: estimate the true frequency of each bin from its phase
    // advance, then spread its magnitude across the two nearest shifted bins.
    // Rounding to one bin made the output jump whenever a partial moved across
    // a bin boundary, which was audible as a window-size-dependent click.
    for (size_t bin = 0; bin < bins; ++bin)
    {
        const double magnitude = std::hypot(m_real[bin], m_imag[bin]);
        double trueBin = static_cast<double>(bin);
        double phase = 0.0;
        if (bin == 0 || bin == nyquist)
        {
            channel.lastPhase[bin] = 0.0;
        }
        else
        {
            phase = std::atan2(m_imag[bin], m_real[bin]);
            const double expected = m_synthPhaseStep * static_cast<double>(bin);
            const double residual =
                WrapPi(phase - channel.lastPhase[bin] - expected);
            trueBin = std::clamp(
                static_cast<double>(bin) +
                    residual * static_cast<double>(m_fftSize) /
                        (2.0 * kPi *
                         static_cast<double>(m_vocoderHopFrames)),
                0.0, static_cast<double>(nyquist));
            channel.lastPhase[bin] = phase;
        }

        const double target = trueBin * m_ratio;
        const double low = std::floor(target);
        const double fraction = target - low;
        const double phaseReal = std::cos(phase) * magnitude;
        const double phaseImag = std::sin(phase) * magnitude;
        auto deposit = [&](size_t targetBin, double weight) {
            if (weight <= 0.0 || targetBin > nyquist)
            {
                return;
            }
            const double weighted = magnitude * weight;
            channel.magnitudes[targetBin] += weighted;
            channel.frequencySum[targetBin] += weighted * target;
            channel.phaseReal[targetBin] += phaseReal * weight;
            channel.phaseImag[targetBin] += phaseImag * weight;
            ++channel.targetCount[targetBin];
        };
        deposit(static_cast<size_t>(std::max(0.0, low)), 1.0 - fraction);
        deposit(static_cast<size_t>(low) + 1, fraction);
    }

    // Synthesis uses the magnitude-weighted true bin for phase advance, and
    // locks to the weighted analysis phase when a bin becomes active again.
    // That keeps newly populated bins from starting with an arbitrary phase.
    for (size_t bin = 0; bin < bins; ++bin)
    {
        const double magnitude = channel.magnitudes[bin];
        if (magnitude <= 1e-12)
        {
            channel.synthPhase[bin] = WrapPi(
                channel.synthPhase[bin] +
                m_synthPhaseStep * static_cast<double>(bin));
            channel.phaseValid[bin] = 0;
            m_real[bin] = 0.0;
            m_imag[bin] = 0.0;
            continue;
        }

        if (channel.targetCount[bin] > 0 && channel.phaseValid[bin] == 0)
        {
            channel.synthPhase[bin] = std::atan2(channel.phaseImag[bin],
                                                 channel.phaseReal[bin]);
            channel.phaseValid[bin] = 1;
        }
        else
        {
            const double targetBin = channel.frequencySum[bin] / magnitude;
            channel.synthPhase[bin] = WrapPi(
                channel.synthPhase[bin] + m_synthPhaseStep * targetBin);
        }
        m_real[bin] = magnitude * std::cos(channel.synthPhase[bin]);
        m_imag[bin] = magnitude * std::sin(channel.synthPhase[bin]);
    }
    for (size_t bin = 1; bin < nyquist; ++bin)
    {
        m_real[m_fftSize - bin] = m_real[bin];
        m_imag[m_fftSize - bin] = -m_imag[bin];
    }

    plan.Inverse(m_real.data(), m_imag.data());
    for (size_t index = 0; index < m_fftSize; ++index)
    {
        const double value =
            m_real[index] * kVocoderOutputScale * window[index];
        const size_t target = (channel.overlapWrite + index) % capacity;
        channel.overlap[target] += value;
    }
}

double PitchShifterStage::ProcessVocoder(VocoderChannel& channel,
                                         double input)
{
    const size_t capacity = channel.input.size();
    if (capacity == 0 || channel.overlap.size() < capacity)
    {
        return 0.0;
    }

    const size_t writeIndex = channel.write;
    channel.input[writeIndex] = input;
    channel.write = (writeIndex + 1) % capacity;
    ++channel.frameCounter;
    if (channel.frameCounter % m_vocoderHopFrames == 0)
    {
        AnalyzeVocoder(channel, writeIndex);
    }

    const double wet = channel.overlap[channel.overlapWrite];
    channel.overlap[channel.overlapWrite] = 0.0;
    channel.overlapWrite = (channel.overlapWrite + 1) % capacity;
    return wet;
}

void PitchShifterStage::Process(double* samples, UINT32 frames,
                                UINT32 channels)
{
    if (!m_settings.enabled || samples == nullptr || frames == 0 ||
        channels == 0 || channels != m_channels ||
        m_grainChannels.size() != channels ||
        m_vocoderChannels.size() != channels ||
        m_wet.size() < channels || m_inputFrame.size() < channels)
    {
        return;
    }

    const bool vocoder = m_settings.algorithm == PitchAlgorithm::PhaseVocoder;
    const bool wsola = m_settings.algorithm == PitchAlgorithm::Wsola;
    for (UINT32 frame = 0; frame < frames; ++frame)
    {
        double* values = samples + static_cast<size_t>(frame) * channels;
        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            m_inputFrame[channel] = values[channel];
            m_wet[channel] = 0.0;
        }

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            if (vocoder)
            {
                m_wet[channel] = ProcessVocoder(
                    m_vocoderChannels[channel], m_inputFrame[channel]);
            }
            else
            {
                m_wet[channel] = ProcessGrain(m_grainChannels[channel],
                                              m_inputFrame[channel], wsola);
            }
        }

        for (UINT32 channel = 0; channel < channels; ++channel)
        {
            values[channel] =
                values[channel] * m_dryLevel + m_wet[channel] * m_level;
        }
    }
}

void TunerStage::Configure(const AudioFormat& format)
{
    m_sampleRate = EffectiveSampleRate(format);
    m_highPassCoeff = TunerRumbleHighPassCoeff(m_sampleRate);
    const std::size_t maxSize =
        static_cast<std::size_t>(kTunerSampleSizes.back());

    m_window.assign(maxSize, 0.0);
    m_mono.assign(maxSize, 0.0);
    m_decimated.assign(maxSize, 0.0);
    m_lagValues.assign(maxSize / 2 + 1, 0.0);
    m_real.assign(maxSize, 0.0);
    m_imag.assign(maxSize, 0.0);

    constexpr double kLocalPi = 3.14159265358979323846;
    for (std::size_t i = 0; i < maxSize; ++i) {
        m_window[i] =
            0.5 - 0.5 * std::cos(2.0 * kLocalPi * static_cast<double>(i) /
                                 static_cast<double>(maxSize - 1));
    }

    m_fftPlans.clear();
    m_fftPlans.reserve(kTunerSampleSizes.size());
    for (int size : kTunerSampleSizes) {
        m_fftPlans.emplace_back(static_cast<std::size_t>(size));
    }

    // The fastest detection rate is the smallest sample size with a quarter
    // window hop. Size the moving average once for the longest smoothing
    // window at that rate so ApplyDetection never allocates.
    const std::size_t minHop =
        std::max<std::size_t>(1, static_cast<std::size_t>(
                                     kTunerSampleSizes.front()) / 4);
    const std::size_t averageCapacity =
        static_cast<std::size_t>(std::ceil(apo::kTunerMaxAverageSeconds *
                                           m_sampleRate /
                                           static_cast<double>(minHop))) +
        4;
    m_averageMidi.assign(averageCapacity, 0.0);
    m_averageTime.assign(averageCapacity, 0.0);
    m_averageWrite = 0;
    m_averageCount = 0;

    m_sampleSize =
        static_cast<std::size_t>(ValidTunerSampleSize(m_settings.sampleSize));
    Reset();
}

void TunerStage::Update(const EffectInstance& effect, const AudioFormat& format)
{
    const double sampleRate = EffectiveSampleRate(format);
    const bool shapeChanged =
        m_settings.sampleSize != effect.tuner.sampleSize ||
        m_settings.algorithm != effect.tuner.algorithm ||
        m_sampleRate != sampleRate;
    m_settings = effect.tuner;
    m_sampleRate = sampleRate;
    m_highPassCoeff = TunerRumbleHighPassCoeff(m_sampleRate);
    m_sampleSize =
        static_cast<std::size_t>(ValidTunerSampleSize(m_settings.sampleSize));
    if (shapeChanged) {
        ResetAnalysis();
    }
}

void TunerStage::Reset()
{
    ResetAnalysis();
}

void TunerStage::ResetAnalysis()
{
    m_filled = 0;
    m_highPassInput = 0.0;
    m_highPassOutput = 0.0;
    m_lastMidi = 0.0;
    m_hasLastMidi = false;
    m_timeSeconds = 0.0;
    m_averageWrite = 0;
    m_averageCount = 0;
    m_monitor = {};
}

void TunerStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (samples == nullptr || frames == 0 || channels == 0 ||
        m_sampleSize == 0) {
        return;
    }

    m_timeSeconds += static_cast<double>(frames) / m_sampleRate;
    for (UINT32 frame = 0; frame < frames; ++frame) {
        double value = 0.0;
        const double* frameSamples =
            samples + static_cast<std::size_t>(frame) * channels;
        for (UINT32 channel = 0; channel < channels; ++channel) {
            value += frameSamples[channel];
        }
        const double mono = value / static_cast<double>(channels);
        // One-pole rumble high-pass on the analysis copy only. The sample
        // buffer is never touched, so this adds no filtering or latency.
        const double filtered =
            m_highPassCoeff * (m_highPassOutput + mono - m_highPassInput);
        m_highPassInput = mono;
        m_highPassOutput = filtered;
        m_mono[m_filled++] = filtered;

        if (m_filled < m_sampleSize) {
            continue;
        }

        double frequencyHz = 0.0;
        double confidence = 0.0;
        const bool detected =
            m_settings.algorithm == TunerAlgorithm::FftPeak
                ? DetectFftPeak(frequencyHz, confidence)
                : DetectAutocorrelation(frequencyHz, confidence);
        ApplyDetection(detected ? frequencyHz : 0.0,
                       detected ? confidence : 0.0);

        // Overlapping windows keep the display responsive without allocating;
        // a quarter-window hop is about 43 ms for 8192 samples at 48 kHz.
        const std::size_t hop = std::max<std::size_t>(1, m_sampleSize / 4);
        const std::size_t keep = m_sampleSize - hop;
        std::memmove(m_mono.data(), m_mono.data() + hop,
                     keep * sizeof(double));
        m_filled = keep;
    }
}

bool TunerStage::DetectAutocorrelation(double& frequencyHz, double& confidence)
{
    // Decimating to about 12 kHz keeps the lag search cheap while still
    // covering the upper end of the note map.
    const int decimation =
        std::max(1, static_cast<int>(std::ceil(m_sampleRate / 12000.0)));
    const std::size_t count = m_sampleSize / static_cast<std::size_t>(decimation);
    if (count < 64) {
        return false;
    }
    const double rate = m_sampleRate / static_cast<double>(decimation);
    for (std::size_t i = 0; i < count; ++i) {
        double average = 0.0;
        const std::size_t start = i * static_cast<std::size_t>(decimation);
        for (int k = 0; k < decimation; ++k) {
            average += m_mono[start + static_cast<std::size_t>(k)];
        }
        m_decimated[i] = average / static_cast<double>(decimation);
    }

    double energy = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        energy += m_decimated[i] * m_decimated[i];
    }
    const double rms = std::sqrt(energy / static_cast<double>(count));
    if (!std::isfinite(rms) || rms < 1e-4) {
        return false;
    }

    const double minHz =
        std::max(kTunerMinPitchHz, rate * 2.0 / static_cast<double>(count));
    const double maxHz = std::min(4000.0, rate * 0.45);
    if (!(maxHz > minHz)) {
        return false;
    }

    const std::size_t maxLagLimit = count / 2 - 1;
    const auto clampLag = [maxLagLimit](double value) {
        const long long rounded = std::lround(value);
        return static_cast<std::size_t>(
            std::clamp(rounded, 2LL, static_cast<long long>(maxLagLimit)));
    };
    const std::size_t minLag = clampLag(std::floor(rate / maxHz));
    const std::size_t maxLag = clampLag(std::ceil(rate / minHz));
    if (maxLag <= minLag) {
        return false;
    }

    // Cumulative-mean normalized difference (the YIN cost function). Its
    // minimum at a lag gives the period; the first clear local minimum avoids
    // jumping to an octave of the true pitch.
    double running = 0.0;
    double bestValue = std::numeric_limits<double>::infinity();
    std::size_t bestLag = minLag;
    for (std::size_t lag = 1; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (std::size_t i = 0; i + lag < count; ++i) {
            const double difference = m_decimated[i] - m_decimated[i + lag];
            sum += difference * difference;
        }
        running += sum;
        const double normalized =
            running > 1e-12
                ? sum * static_cast<double>(lag) / running
                : 1.0;
        m_lagValues[lag] = normalized;
        if (lag >= minLag && normalized < bestValue) {
            bestValue = normalized;
            bestLag = lag;
        }
    }

    constexpr double kThreshold = 0.18;
    std::size_t chosen = bestLag;
    for (std::size_t lag = minLag; lag < maxLag; ++lag) {
        if (m_lagValues[lag] <= kThreshold &&
            m_lagValues[lag] <= m_lagValues[lag - 1] &&
            m_lagValues[lag] <= m_lagValues[lag + 1]) {
            chosen = lag;
            break;
        }
    }

    double value = m_lagValues[chosen];
    if (!std::isfinite(value) || value > 0.35) {
        return false;
    }

    // A strong upper harmonic can make the first clear dip sit at half or a
    // third of the true period. Promote an integer-longer dip only when it is
    // much deeper, so ordinary period-doubling noise cannot pull the reading
    // down an octave. Comparable dips prefer the longer one once a correction
    // has started, which handles missing fundamentals cleanly.
    std::size_t promoted = chosen;
    double promotedValue = value;
    bool corrected = false;
    constexpr int factors[] = {2, 3};
    for (int factor : factors) {
        const std::size_t candidate =
            chosen * static_cast<std::size_t>(factor);
        if (candidate > maxLag || candidate + 1 >= m_lagValues.size()) {
            continue;
        }
        const double candidateValue = m_lagValues[candidate];
        if (!std::isfinite(candidateValue) || candidateValue > 0.25) {
            continue;
        }
        const bool localMinimum =
            candidateValue <= m_lagValues[candidate - 1] &&
            candidateValue <= m_lagValues[candidate + 1];
        const bool substantiallyDeeper = candidateValue < promotedValue * 0.6;
        const bool comparableAndLonger =
            corrected && candidate > promoted &&
            candidateValue <= promotedValue + 0.02;
        if (localMinimum && (substantiallyDeeper || comparableAndLonger)) {
            promoted = candidate;
            promotedValue = candidateValue;
            corrected = true;
        }
    }
    chosen = promoted;
    value = promotedValue;

    const double left = m_lagValues[chosen - 1];
    const double right = m_lagValues[chosen + 1];
    const double denominator = left - 2.0 * value + right;
    double delta = 0.0;
    if (std::fabs(denominator) > 1e-12) {
        delta = std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5);
    }
    const double period = static_cast<double>(chosen) + delta;
    if (period <= 0.0) {
        return false;
    }

    frequencyHz = rate / period;
    confidence = std::clamp(1.0 - value / 0.35, 0.0, 1.0);
    return frequencyHz >= minHz && frequencyHz <= maxHz;
}

bool TunerStage::DetectFftPeak(double& frequencyHz, double& confidence)
{
    const std::size_t size = m_sampleSize;
    std::size_t planIndex = 0;
    for (std::size_t i = 0; i < kTunerSampleSizes.size(); ++i) {
        if (kTunerSampleSizes[i] == static_cast<int>(size)) {
            planIndex = i;
            break;
        }
    }
    if (planIndex >= m_fftPlans.size()) {
        return false;
    }

    double energy = 0.0;
    for (std::size_t i = 0; i < size; ++i) {
        energy += m_mono[i] * m_mono[i];
        m_real[i] = m_mono[i] * m_window[i];
        m_imag[i] = 0.0;
    }
    const double rms = std::sqrt(energy / static_cast<double>(size));
    if (!std::isfinite(rms) || rms < 1e-4) {
        return false;
    }

    m_fftPlans[planIndex].Forward(m_real.data(), m_imag.data());

    const double binHz = m_sampleRate / static_cast<double>(size);
    // Include the bin at the lower limit while excluding sub-audio rumble.
    const int firstBin =
        std::max(1, static_cast<int>(std::ceil(kTunerMinPitchHz / binHz)) - 1);
    const int lastBin = std::min(
        static_cast<int>(size / 2) - 1,
        static_cast<int>(std::ceil(4000.0 / binHz)));
    if (lastBin <= firstBin) {
        return false;
    }

    auto magnitudeAt = [&](int bin) {
        return std::hypot(m_real[static_cast<std::size_t>(bin)],
                          m_imag[static_cast<std::size_t>(bin)]);
    };
    int peakBin = firstBin;
    double peak = 0.0;
    double total = 0.0;
    for (int bin = firstBin; bin <= lastBin; ++bin) {
        const double magnitude = magnitudeAt(bin);
        total += magnitude;
        if (magnitude > peak) {
            peak = magnitude;
            peakBin = bin;
        }
    }
    if (peak <= 1e-9) {
        return false;
    }

    // If the largest bin is an upper harmonic, a weaker fundamental can still
    // be a clear peak at half or a third of its frequency. Prefer a reasonably
    // strong subharmonic so the readout stops hopping between octaves.
    int selectedBin = peakBin;
    double selectedMagnitude = peak;
    constexpr int divisors[] = {3, 2};
    for (int divisor : divisors) {
        const double subharmonicHz =
            static_cast<double>(peakBin) * binHz /
            static_cast<double>(divisor);
        if (subharmonicHz < kTunerMinPitchHz) {
            continue;
        }
        const int targetBin =
            static_cast<int>(std::lround(subharmonicHz / binHz));
        int bestBin = -1;
        double bestMagnitude = 0.0;
        const int low = std::max(firstBin, targetBin - 2);
        const int high = std::min(lastBin, targetBin + 2);
        for (int bin = low; bin <= high; ++bin) {
            const double magnitude = magnitudeAt(bin);
            if (magnitude > bestMagnitude) {
                bestMagnitude = magnitude;
                bestBin = bin;
            }
        }
        if (bestBin >= 0 && bestBin < selectedBin &&
            bestMagnitude >= peak * 0.25) {
            selectedBin = bestBin;
            selectedMagnitude = bestMagnitude;
        }
    }

    const double average = total / static_cast<double>(lastBin - firstBin + 1);
    const double baseConfidence =
        std::clamp((peak / std::max(average, 1e-9) - 1.0) / 5.0, 0.0, 1.0);
    // A promoted subharmonic is less certain than the strongest bin itself.
    confidence = baseConfidence *
                 (0.5 + 0.5 * std::clamp(selectedMagnitude / peak, 0.0, 1.0));

    const double left = std::log(magnitudeAt(selectedBin - 1) + 1e-12);
    const double center = std::log(selectedMagnitude + 1e-12);
    const double right = std::log(magnitudeAt(selectedBin + 1) + 1e-12);
    const double denominator = left - 2.0 * center + right;
    double delta = 0.0;
    if (std::fabs(denominator) > 1e-12) {
        delta = std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5);
    }
    frequencyHz = static_cast<double>(selectedBin + delta) * binHz;
    return std::isfinite(frequencyHz) &&
           frequencyHz >= kTunerMinPitchHz && frequencyHz <= 4000.0;
}

void TunerStage::ApplyDetection(double frequencyHz, double confidence)
{
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0 ||
        !std::isfinite(confidence) || confidence <= 0.0) {
        m_monitor = {};
        m_hasLastMidi = false;
        m_averageWrite = 0;
        m_averageCount = 0;
        return;
    }

    const double midi =
        apo::pitch::FrequencyToMidi(frequencyHz, m_settings.referenceHz);
    // An octave is a plausible correction, not a new note: smooth it with the
    // rest of the movement instead of snapping. Only a much larger jump, such
    // as switching instruments, starts a new average.
    if (!m_hasLastMidi || std::fabs(midi - m_lastMidi) >= 24.0) {
        m_lastMidi = midi;
        m_averageWrite = 0;
        m_averageCount = 0;
    }

    const double amount =
        std::clamp(m_settings.smoothing / 100.0, 0.0, 1.0);
    // A cubic curve keeps the 50% setting near the old quick response while
    // 100% averages about two seconds of detections.
    const double windowSeconds =
        apo::kTunerMaxAverageSeconds * amount * amount * amount;
    if (windowSeconds > 0.0 && !m_averageMidi.empty()) {
        const std::size_t capacity = m_averageMidi.size();
        m_averageMidi[m_averageWrite] = midi;
        m_averageTime[m_averageWrite] = m_timeSeconds;
        m_averageWrite = (m_averageWrite + 1) % capacity;
        m_averageCount = std::min(capacity, m_averageCount + 1);

        double sum = 0.0;
        std::size_t used = 0;
        for (std::size_t i = 0; i < m_averageCount; ++i) {
            const std::size_t index =
                (m_averageWrite + capacity - 1 - i) % capacity;
            // Entries are chronological from newest to oldest, so the first
            // stale one ends the scan.
            if (m_timeSeconds - m_averageTime[index] > windowSeconds) {
                break;
            }
            sum += m_averageMidi[index];
            ++used;
        }
        if (used > 0) {
            m_lastMidi = sum / static_cast<double>(used);
        }
    } else {
        m_lastMidi = midi;
    }
    m_hasLastMidi = true;

    const double smoothedHz =
        apo::pitch::MidiToFrequency(m_lastMidi, m_settings.referenceHz);
    m_monitor.frequencyHz = static_cast<float>(smoothedHz);
    m_monitor.confidence = static_cast<float>(std::clamp(confidence, 0.0, 1.0));
    m_monitor.valid =
        std::isfinite(smoothedHz) && smoothedHz > 0.0;
}

void LooperStage::Configure(const AudioFormat& format)
{
    m_sampleRate = EffectiveSampleRate(format);
    m_channels = std::max<UINT32>(1, format.channels);
    m_maxFrames = static_cast<std::size_t>(
        std::ceil(apo::kLooperMaxLengthSeconds * m_sampleRate));

    const std::size_t sampleCount = m_maxFrames * m_channels;
    for (std::vector<float>& buffer : m_buffers) {
        buffer.assign(sampleCount, 0.0f);
    }
    m_undoBuffer.assign(sampleCount, 0.0f);
    m_preProvisionalTrackBuffer.assign(sampleCount, 0.0f);
    m_preProvisionalUndoBuffer.assign(sampleCount, 0.0f);
    m_inputFrame.assign(m_channels, 0.0);
    m_layerFrame.assign(m_channels, 0.0);
    ClearAll();
}

void LooperStage::SaveBank()
{
    if (m_storageId.empty() || m_maxFrames == 0)
    {
        return;
    }

    // An unfinished keyboard gesture is not a committed take. Writing the
    // snapshot from before that press keeps the bank exactly as the user left
    // it before the gesture started.
    if (m_provisional)
    {
        RestoreProvisionalSnapshot(m_provisionalTrack);
    }

    apo::LooperBankData bank{};
    bank.valid = true;
    bank.sampleRate = static_cast<std::uint32_t>(m_sampleRate);
    bank.channels = m_channels;
    bank.maxFrames = static_cast<std::uint32_t>(m_maxFrames);
    bank.globalFrames = m_globalFrames;
    bank.referenceTrack = m_referenceTrack;
    bank.masterFrames = m_masterFrames;
    bank.syncOrigin = m_syncOrigin;

    for (int track = 0; track < apo::kLooperTrackCount; ++track)
    {
        const TrackState& state = m_tracks[static_cast<size_t>(track)];
        const std::size_t frames = std::min(state.length, m_maxFrames);
        const std::size_t samples = frames * m_channels;
        apo::LooperBankTrack& saved = bank.tracks[static_cast<size_t>(track)];
        saved.state = state.state;
        saved.length = frames;
        saved.position = std::min(state.position, m_maxFrames);
        saved.recordPosition = std::min(state.recordPosition, m_maxFrames);
        saved.recordFrames = state.recordFrames;
        saved.synced = state.synced;
        saved.overdub = state.overdub;
        saved.pendingStop = state.pendingStop;
        saved.isReference = state.isReference;
        if (samples != 0 && samples <= m_buffers[static_cast<size_t>(track)].size())
        {
            bank.buffers[static_cast<size_t>(track)].assign(
                m_buffers[static_cast<size_t>(track)].begin(),
                m_buffers[static_cast<size_t>(track)].begin() +
                    static_cast<std::ptrdiff_t>(samples));
        }
    }

    bank.undoValid = m_undoValid;
    bank.undoTrack = m_undoTrack;
    bank.undoLength = m_undoLength;
    bank.undoState = m_undoState;
    bank.undoReferenceTrack = m_undoReferenceTrack;
    bank.undoMasterFrames = m_undoMasterFrames;
    bank.undoSyncOrigin = m_undoSyncOrigin;
    const std::size_t undoSamples = std::min(m_undoLength, m_maxFrames) *
                                    m_channels;
    if (m_undoValid && undoSamples != 0 &&
        undoSamples <= m_undoBuffer.size())
    {
        bank.undoBuffer.assign(
            m_undoBuffer.begin(),
            m_undoBuffer.begin() + static_cast<std::ptrdiff_t>(undoSamples));
        bank.undoLength = undoSamples / m_channels;
    }

    apo::SaveLooperBank(m_storageId, bank);
}

bool LooperStage::LoadBank()
{
    if (m_storageId.empty() || m_maxFrames == 0)
    {
        return false;
    }

    apo::LooperBankData bank{};
    if (!apo::LoadLooperBank(m_storageId, bank))
    {
        return false;
    }

    // A device format change makes saved sample positions meaningless. Drop the
    // stale bank instead of resampling recorded audio or playing it at the
    // wrong speed.
    if (bank.sampleRate != static_cast<std::uint32_t>(m_sampleRate) ||
        bank.channels != m_channels ||
        bank.maxFrames != static_cast<std::uint32_t>(m_maxFrames))
    {
        DeleteFileW(apo::LooperBankPath(m_storageId).c_str());
        return false;
    }

    ClearAll();
    m_globalFrames = bank.globalFrames;
    m_referenceTrack = bank.referenceTrack;
    m_masterFrames = static_cast<std::size_t>(bank.masterFrames);
    m_syncOrigin = bank.syncOrigin;

    for (int track = 0; track < apo::kLooperTrackCount; ++track)
    {
        const apo::LooperBankTrack& saved =
            bank.tracks[static_cast<size_t>(track)];
        TrackState& state = m_tracks[static_cast<size_t>(track)];
        state.state = saved.state;
        state.length = static_cast<std::size_t>(saved.length);
        state.position = static_cast<std::size_t>(saved.position);
        state.recordPosition = static_cast<std::size_t>(saved.recordPosition);
        state.recordFrames = saved.recordFrames;
        state.synced = saved.synced;
        state.overdub = saved.overdub;
        state.pendingStop = saved.pendingStop;
        state.isReference = (track == m_referenceTrack);

        std::vector<float>& buffer = m_buffers[static_cast<size_t>(track)];
        std::fill(buffer.begin(), buffer.end(), 0.0f);
        const std::vector<float>& source = bank.buffers[static_cast<size_t>(track)];
        if (!source.empty() && source.size() <= buffer.size())
        {
            std::copy(source.begin(), source.end(), buffer.begin());
        }
    }

    m_undoValid = bank.undoValid;
    m_undoTrack = bank.undoTrack;
    m_undoLength = static_cast<std::size_t>(bank.undoLength);
    m_undoState = bank.undoState;
    m_undoReferenceTrack = bank.undoReferenceTrack;
    m_undoMasterFrames = static_cast<std::size_t>(bank.undoMasterFrames);
    m_undoSyncOrigin = bank.undoSyncOrigin;
    std::fill(m_undoBuffer.begin(), m_undoBuffer.end(), 0.0f);
    if (!bank.undoBuffer.empty() && bank.undoBuffer.size() <= m_undoBuffer.size())
    {
        std::copy(bank.undoBuffer.begin(), bank.undoBuffer.end(),
                  m_undoBuffer.begin());
    }
    return true;
}

void LooperStage::PrimeCommandSerial()
{
    // Update() normally adopts the current serial as a baseline once. Reset()
    // clears that flag, so a fresh stream would otherwise swallow the user's
    // first gesture after the bank has been restored.
    m_lastCommandSerial = m_settings.commandSerial;
    m_hasCommandSerial = true;
}

void LooperStage::Update(const EffectInstance& effect, const AudioFormat& format)
{
    (void)format;
    m_settings = effect.looper;

    // The first snapshot after binding establishes the serial baseline. A stale
    // command left in a runtime file must not fire when an APO first maps it.
    if (!m_hasCommandSerial) {
        m_lastCommandSerial = m_settings.commandSerial;
        m_hasCommandSerial = true;
        return;
    }

    if (m_settings.commandSerial == m_lastCommandSerial) {
        return;
    }
    m_lastCommandSerial = m_settings.commandSerial;
    const int track = static_cast<int>(std::clamp(
        m_settings.commandTrack, 0, apo::kLooperTrackCount - 1));
    ApplyCommand(m_settings.command, track);
}

void LooperStage::Reset()
{
    ClearAll();
    m_hasCommandSerial = false;
    m_lastCommandSerial = 0;
}

bool LooperStage::NeedsSilence() const
{
    for (const TrackState& track : m_tracks) {
        if (track.state != LooperState::Empty) {
            return true;
        }
    }
    return false;
}

void LooperStage::MonitorLooper(LooperMonitorData& data) const
{
    for (int track = 0; track < apo::kLooperTrackCount; ++track) {
        data.states[track] = static_cast<int>(m_tracks[track].state);
    }
}

void LooperStage::ClearTrack(int track)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }

    TrackState& state = m_tracks[track];
    const std::size_t samples = state.length * m_channels;
    if (samples > 0 && samples <= m_buffers[track].size()) {
        std::fill(m_buffers[track].begin(),
                  m_buffers[track].begin() +
                      static_cast<std::ptrdiff_t>(samples),
                  0.0f);
    }
    if (state.isReference) {
        ClearReference();
    }
    state = TrackState{};
}

void LooperStage::ClearAll()
{
    m_provisional = false;
    m_provisionalTrack = -1;
    std::fill(m_preProvisionalTrackBuffer.begin(),
              m_preProvisionalTrackBuffer.end(), 0.0f);
    std::fill(m_preProvisionalUndoBuffer.begin(),
              m_preProvisionalUndoBuffer.end(), 0.0f);

    for (int track = 0; track < apo::kLooperTrackCount; ++track) {
        ClearTrack(track);
    }
    m_undoTrack = -1;
    m_undoLength = 0;
    m_undoState = LooperState::Empty;
    m_undoReferenceTrack = -1;
    m_undoMasterFrames = 0;
    m_undoSyncOrigin = 0;
    m_undoValid = false;
    m_globalFrames = 0;
    ClearReference();
}

void LooperStage::SetReference(int track, std::size_t masterFrames,
                               std::uint64_t syncOrigin)
{
    m_referenceTrack = track;
    m_masterFrames = masterFrames;
    m_syncOrigin = syncOrigin;
    for (int index = 0; index < apo::kLooperTrackCount; ++index) {
        m_tracks[index].isReference = (index == track);
    }
}

void LooperStage::ClearReference()
{
    m_referenceTrack = -1;
    m_masterFrames = 0;
    m_syncOrigin = 0;
    for (TrackState& track : m_tracks) {
        track.isReference = false;
    }
}

void LooperStage::SaveProvisionalSnapshot(int track)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }

    m_preProvisionalTracks = m_tracks;
    m_preProvisionalReferenceTrack = m_referenceTrack;
    m_preProvisionalMasterFrames = m_masterFrames;
    m_preProvisionalSyncOrigin = m_syncOrigin;
    m_preProvisionalGlobalFrames = m_globalFrames;

    const std::size_t trackSamples = m_tracks[track].length * m_channels;
    std::fill(m_preProvisionalTrackBuffer.begin(),
              m_preProvisionalTrackBuffer.end(), 0.0f);
    if (trackSamples > 0 &&
        trackSamples <= m_preProvisionalTrackBuffer.size() &&
        trackSamples <= m_buffers[track].size())
    {
        std::copy_n(m_buffers[track].begin(),
                    static_cast<std::ptrdiff_t>(trackSamples),
                    m_preProvisionalTrackBuffer.begin());
    }

    m_preProvisionalUndoValid = m_undoValid;
    m_preProvisionalUndoTrack = m_undoTrack;
    m_preProvisionalUndoLength = m_undoLength;
    m_preProvisionalUndoState = m_undoState;
    m_preProvisionalUndoReferenceTrack = m_undoReferenceTrack;
    m_preProvisionalUndoMasterFrames = m_undoMasterFrames;
    m_preProvisionalUndoSyncOrigin = m_undoSyncOrigin;
    const std::size_t undoSamples = m_undoLength * m_channels;
    std::fill(m_preProvisionalUndoBuffer.begin(),
              m_preProvisionalUndoBuffer.end(), 0.0f);
    if (m_undoValid && undoSamples > 0 &&
        undoSamples <= m_preProvisionalUndoBuffer.size() &&
        undoSamples <= m_undoBuffer.size())
    {
        std::copy_n(m_undoBuffer.begin(),
                    static_cast<std::ptrdiff_t>(undoSamples),
                    m_preProvisionalUndoBuffer.begin());
    }
}

void LooperStage::RestoreProvisionalSnapshot(int track)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }

    std::fill(m_buffers[track].begin(), m_buffers[track].end(), 0.0f);
    const std::size_t trackSamples = m_preProvisionalTracks[track].length *
                                     m_channels;
    if (trackSamples > 0 &&
        trackSamples <= m_preProvisionalTrackBuffer.size() &&
        trackSamples <= m_buffers[track].size())
    {
        std::copy_n(m_preProvisionalTrackBuffer.begin(),
                    static_cast<std::ptrdiff_t>(trackSamples),
                    m_buffers[track].begin());
    }

    m_tracks = m_preProvisionalTracks;
    m_referenceTrack = m_preProvisionalReferenceTrack;
    m_masterFrames = m_preProvisionalMasterFrames;
    m_syncOrigin = m_preProvisionalSyncOrigin;
    m_globalFrames = m_preProvisionalGlobalFrames;

    std::fill(m_undoBuffer.begin(), m_undoBuffer.end(), 0.0f);
    const std::size_t undoSamples = m_preProvisionalUndoLength * m_channels;
    if (m_preProvisionalUndoValid && undoSamples > 0 &&
        undoSamples <= m_preProvisionalUndoBuffer.size() &&
        undoSamples <= m_undoBuffer.size())
    {
        std::copy_n(m_preProvisionalUndoBuffer.begin(),
                    static_cast<std::ptrdiff_t>(undoSamples),
                    m_undoBuffer.begin());
    }
    m_undoValid = m_preProvisionalUndoValid;
    m_undoTrack = m_preProvisionalUndoTrack;
    m_undoLength = m_preProvisionalUndoLength;
    m_undoState = m_preProvisionalUndoState;
    m_undoReferenceTrack = m_preProvisionalUndoReferenceTrack;
    m_undoMasterFrames = m_preProvisionalUndoMasterFrames;
    m_undoSyncOrigin = m_preProvisionalUndoSyncOrigin;

    m_provisional = false;
    m_provisionalTrack = -1;
}

std::size_t LooperStage::Phase(int track, std::size_t length) const
{
    if (length == 0 || track < 0 || track >= apo::kLooperTrackCount) {
        return 0;
    }
    const TrackState& state = m_tracks[track];
    if (!state.synced) {
        return state.position % length;
    }
    if (m_globalFrames < m_syncOrigin) {
        return 0;
    }
    return static_cast<std::size_t>((m_globalFrames - m_syncOrigin) % length);
}

std::size_t LooperStage::SnapSyncedFrames(std::uint64_t recordedFrames) const
{
    if (m_maxFrames == 0) {
        return 0;
    }
    if (m_masterFrames == 0) {
        const std::uint64_t frames =
            std::min<std::uint64_t>(recordedFrames, m_maxFrames);
        return static_cast<std::size_t>(frames == 0 ? 1 : frames);
    }

    const double ratio = apo::NearestLooperAutoSnapRatio(
        static_cast<double>(recordedFrames) /
        static_cast<double>(m_masterFrames));
    const double frames = std::round(
        static_cast<double>(m_masterFrames) * ratio);
    const double clamped = std::clamp(frames, 1.0,
                                      static_cast<double>(m_maxFrames));
    return static_cast<std::size_t>(clamped);
}

std::size_t LooperStage::RecordingTarget(const TrackState& state) const
{
    if (state.overdub && state.length > 0) {
        return state.length;
    }
    if (state.synced && m_masterFrames > 0) {
        return SnapSyncedFrames(state.recordFrames);
    }
    return m_maxFrames;
}

void LooperStage::StartRecord(int track)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }

    TrackState& state = m_tracks[track];
    if (state.state == LooperState::Recording) {
        return;
    }

    // One shared undo bank: every new take saves the track it is about to
    // change, whether it is a fresh loop or an overdub.
    const std::size_t undoSamples = state.length * m_channels;
    if (undoSamples > 0 && undoSamples <= m_undoBuffer.size() &&
        undoSamples <= m_buffers[track].size())
    {
        std::copy_n(m_buffers[track].begin(),
                    static_cast<std::ptrdiff_t>(undoSamples),
                    m_undoBuffer.begin());
    }
    m_undoTrack = track;
    m_undoLength = state.length;
    m_undoState = state.state;
    m_undoReferenceTrack = m_referenceTrack;
    m_undoMasterFrames = m_masterFrames;
    m_undoSyncOrigin = m_syncOrigin;
    m_undoValid = true;

    const bool overdub = state.state == LooperState::Playback &&
                         state.length > 0;
    const bool synced = !m_settings.tracks[track].freeLength;
    state.overdub = overdub;
    state.synced = synced;
    state.pendingStop = false;
    state.recordFrames = 0;
    state.recordPosition = 0;

    // A fresh take on the reference track starts a new reference. Keep the
    // shared clock intact for an overdub so its phase stays aligned.
    if (state.isReference && !overdub) {
        ClearReference();
    }

    if (!overdub) {
        std::fill(m_buffers[track].begin(), m_buffers[track].end(), 0.0f);
        state.length = 0;
        state.position = 0;
    }

    state.state = LooperState::Recording;
}

void LooperStage::FinishRecord(int track)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }

    TrackState& state = m_tracks[track];
    if (state.state != LooperState::Recording) {
        return;
    }

    const std::uint64_t recorded = state.recordFrames;
    if (state.overdub) {
        state.position = Phase(track, state.length);
    } else if (!state.synced) {
        state.length = static_cast<std::size_t>(
            std::min<std::uint64_t>(recorded, m_maxFrames));
        state.position = 0;
    } else {
        const std::size_t length = SnapSyncedFrames(recorded);
        state.length = length;
        if (m_masterFrames == 0) {
            const std::uint64_t phase = recorded % length;
            const std::uint64_t origin =
                (m_globalFrames >= phase) ? (m_globalFrames - phase) : 0;
            SetReference(track, length, origin);
        }
        state.position = Phase(track, length);
    }

    state.state = LooperState::Playback;
    state.recordFrames = 0;
    state.recordPosition = 0;
    state.overdub = false;
    if (state.pendingStop) {
        state.state = LooperState::Stopped;
        state.position = 0;
        state.pendingStop = false;
    }
}

void LooperStage::ApplyCommand(LooperCommand command, int track)
{
    if (track < 0 || track >= apo::kLooperTrackCount) {
        return;
    }

    // A provisional take is only authoritative once it is committed. Any
    // other command means the gesture resolved differently, so undo the take
    // before applying that command to the state the user actually started from.
    if (m_provisional) {
        if (command == LooperCommand::CommitProvisional) {
            m_provisional = false;
            m_provisionalTrack = -1;
            return;
        }
        if (command == LooperCommand::DiscardProvisional) {
            RestoreProvisionalSnapshot(m_provisionalTrack);
            return;
        }
        RestoreProvisionalSnapshot(m_provisionalTrack);
    }

    TrackState& state = m_tracks[track];
    switch (command)
    {
    case LooperCommand::StartRecordProvisional:
        SaveProvisionalSnapshot(track);
        StartRecord(track);
        m_provisional = true;
        m_provisionalTrack = track;
        break;

    case LooperCommand::StartRecord:
        StartRecord(track);
        break;

    case LooperCommand::FinishRecord:
        FinishRecord(track);
        break;

    case LooperCommand::Play:
        if (state.state == LooperState::Recording) {
            FinishRecord(track);
        } else if (state.state == LooperState::Stopped && state.length > 0) {
            state.state = LooperState::Playback;
            state.position = 0;
            state.pendingStop = false;
        }
        break;

    case LooperCommand::Stop:
        if (state.state == LooperState::Recording) {
            if (m_settings.tracks[track].stopAtLoopEnd && state.synced &&
                m_masterFrames > 0)
            {
                state.pendingStop = true;
            } else {
                FinishRecord(track);
                state.state = LooperState::Stopped;
                state.position = 0;
                state.pendingStop = false;
            }
        } else if (state.state == LooperState::Playback) {
            if (m_settings.tracks[track].stopAtLoopEnd) {
                state.pendingStop = true;
            } else {
                state.state = LooperState::Stopped;
                state.position = 0;
                state.pendingStop = false;
            }
        }
        break;

    case LooperCommand::Undo:
        if (!m_undoValid || m_undoTrack != track) {
            break;
        }
        {
            const std::size_t samples = m_undoLength * m_channels;
            if (samples > 0 && samples <= m_undoBuffer.size() &&
                samples <= m_buffers[track].size())
            {
                std::copy_n(m_undoBuffer.begin(),
                            static_cast<std::ptrdiff_t>(samples),
                            m_buffers[track].begin());
            }
            state.length = m_undoLength;
            state.position = 0;
            state.recordPosition = 0;
            state.recordFrames = 0;
            state.overdub = false;
            state.pendingStop = false;
            state.state = (m_undoLength > 0 &&
                           m_undoState != LooperState::Empty)
                              ? LooperState::Playback
                              : LooperState::Empty;
            if (m_undoReferenceTrack == track) {
                if (state.state == LooperState::Empty) {
                    ClearReference();
                } else {
                    SetReference(track, m_undoMasterFrames,
                                 m_undoSyncOrigin);
                }
            }
        }
        m_undoValid = false;
        m_undoTrack = -1;
        break;

    case LooperCommand::Clear:
        ClearTrack(track);
        if (m_undoTrack == track) {
            m_undoValid = false;
            m_undoTrack = -1;
        }
        break;

    case LooperCommand::CommitProvisional:
    case LooperCommand::DiscardProvisional:
        // Handled before the switch; with no provisional take these are no-ops.
        break;

    case LooperCommand::None:
    default:
        break;
    }
}

void LooperStage::Process(double* samples, UINT32 frames, UINT32 channels)
{
    if (m_channels == 0 || m_maxFrames == 0 || frames == 0 ||
        samples == nullptr)
    {
        return;
    }

    const UINT32 count = std::min(channels, m_channels);
    if (count == 0) {
        return;
    }

    for (UINT32 frame = 0; frame < frames; ++frame) {
        const std::size_t base = static_cast<std::size_t>(frame) * count;

        // Capture the raw input before any loop is mixed into `samples`, so no
        // track can record another track's return.
        for (UINT32 channel = 0; channel < count; ++channel) {
            m_inputFrame[channel] = samples[base + channel];
        }

        for (int track = 0; track < apo::kLooperTrackCount; ++track) {
            TrackState& state = m_tracks[track];
            const double gain = m_settings.tracks[track].volume / 100.0;

            if (state.state == LooperState::Recording) {
                const std::size_t target = RecordingTarget(state);
                std::size_t position = 0;
                if (state.overdub) {
                    position = Phase(track, state.length);
                } else if (state.synced && m_masterFrames > 0) {
                    const std::uint64_t elapsed =
                        (m_globalFrames >= m_syncOrigin)
                            ? (m_globalFrames - m_syncOrigin)
                            : 0;
                    position = static_cast<std::size_t>(elapsed % target);
                } else {
                    position = static_cast<std::size_t>(
                        state.recordFrames % target);
                }
                state.recordPosition = position;

                if (position < m_maxFrames) {
                    const std::size_t offset = position * count;
                    for (UINT32 channel = 0; channel < count; ++channel) {
                        float& destination = m_buffers[track][offset + channel];
                        // Always add. A wrapped pass layers over what was
                        // already in the buffer, for fresh takes and overdubs.
                        m_layerFrame[channel] = destination;
                        destination +=
                            static_cast<float>(m_inputFrame[channel]);
                    }
                } else {
                    for (UINT32 channel = 0; channel < count; ++channel) {
                        m_layerFrame[channel] = 0.0;
                    }
                }

                // Return only the layer that was there before this frame. The
                // current input is already in the dry signal, so returning the
                // post-write value would double it.
                for (UINT32 channel = 0; channel < count; ++channel) {
                    samples[base + channel] += m_layerFrame[channel] * gain;
                }

                const std::size_t monitorLength =
                    state.overdub ? state.length : target;
                state.position = (monitorLength > 0)
                                     ? (position + 1) % monitorLength
                                     : 0;
                ++state.recordFrames;
                if (state.pendingStop && state.recordFrames % target == 0) {
                    FinishRecord(track);
                }
            } else if (state.state == LooperState::Playback &&
                       state.length > 0)
            {
                const std::size_t position = Phase(track, state.length);
                const std::size_t offset = position * count;
                for (UINT32 channel = 0; channel < count; ++channel) {
                    samples[base + channel] +=
                        static_cast<double>(m_buffers[track][offset + channel]) *
                        gain;
                }
                state.position = (position + 1) % state.length;
                if (state.pendingStop && state.position == 0) {
                    state.state = LooperState::Stopped;
                    state.pendingStop = false;
                }
            }
        }

        ++m_globalFrames;
    }
}

void StagePool::Configure(const AudioFormat& format,
                          const NoiseProfile& profile,
                          const ApoSettings& settings,
                          const std::wstring& endpointId)
{
    Clear();
    m_looperStorageId = endpointId;
    const bool wantsLooper = WantsLooper(settings);

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
            case EffectType::Input:
            case EffectType::Gain:
                stage = std::make_unique<GainStage>();
                break;
            case EffectType::Noise:
                // Always allocate the noise stages. Without a profile they
                // pass audio through unchanged, and a live capture can fill
                // their bins without rebuilding the pool.
                stage = std::make_unique<NoiseStage>(profile);
                break;
            case EffectType::Gate:
                stage = std::make_unique<DynamicsStage>(
                    DynamicsMode::ExpanderGate);
                break;
            case EffectType::Compressor:
                stage = std::make_unique<DynamicsStage>(
                    DynamicsMode::CompressorLimiter);
                break;
            case EffectType::Equalizer:
                stage = std::make_unique<EqualizerStage>();
                break;
            case EffectType::Saturation:
                stage = std::make_unique<DistortionStage>();
                break;
            case EffectType::Reverb:
                stage = std::make_unique<ReverbStage>();
                break;
            case EffectType::Echo:
                stage = std::make_unique<EchoStage>();
                break;
            case EffectType::Chorus:
                stage = std::make_unique<ChorusStage>();
                break;
            case EffectType::Tremolo:
                stage = std::make_unique<TremoloStage>();
                break;
            case EffectType::Vibrato:
                stage = std::make_unique<VibratoStage>();
                break;
            case EffectType::PitchShifter:
                stage = std::make_unique<PitchShifterStage>();
                break;
            case EffectType::Tuner:
                stage = std::make_unique<TunerStage>();
                break;
            case EffectType::Looper:
                // Looper's 120 s banks are far larger than the other stages.
                // Allocate them only when the preset actually uses Looper; a
                // live add creates the slot in Update().
                if (wantsLooper)
                {
                    stage = std::make_unique<LooperStage>();
                }
                break;
            default:
                break;
            }

            if (stage != nullptr)
            {
                stage->Configure(format);
                if (type == EffectType::Looper)
                {
                    static_cast<LooperStage*>(stage.get())->SetStorageId(
                        endpointId);
                }
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

void StagePool::SaveLooperBanks()
{
    if (m_looperStorageId.empty())
    {
        return;
    }
    Slots& slots = m_slots[static_cast<int>(EffectType::Looper)];
    for (const auto& stage : slots.stages)
    {
        static_cast<LooperStage*>(stage.get())->SaveBank();
    }
}

void StagePool::LoadLooperBanks()
{
    if (m_looperStorageId.empty())
    {
        return;
    }
    Slots& slots = m_slots[static_cast<int>(EffectType::Looper)];
    for (const auto& stage : slots.stages)
    {
        LooperStage* looper = static_cast<LooperStage*>(stage.get());
        looper->LoadBank();
        looper->PrimeCommandSerial();
    }
}

int StagePool::Update(const ApoSettings& settings, const AudioFormat& format,
                      Chain& chain)
{
    chain.Clear();

    int used[static_cast<int>(EffectType::Count)] = {};
    int boundCount = 0;
    const auto wantsSpectrum = [](EffectType type) {
        return type == EffectType::Input || type == EffectType::Equalizer;
    };

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
        if (effect.type == EffectType::Looper && slots.stages.empty())
        {
            // Looper was absent when the pool was configured. Adding it later
            // is a setup-path settings change, so allocate its one slot now.
            auto stage = std::make_unique<LooperStage>();
            stage->Configure(format);
            stage->SetStorageId(m_looperStorageId);
            slots.stages.push_back(std::move(stage));
        }
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
        const bool rebound =
            slots.boundIds[static_cast<size_t>(ordinal)] != effect.id;
        if (rebound)
        {
            stage->Reset();
            slots.boundIds[static_cast<size_t>(ordinal)] = effect.id;
        }

        stage->Update(effect, format);
        if (effect.type == EffectType::Looper && rebound)
        {
            // A fresh binding has just cleared the stage. Restore the saved
            // take bank and re-arm the one-shot command baseline.
            LooperStage* looper = static_cast<LooperStage*>(stage);
            looper->LoadBank();
            looper->PrimeCommandSerial();
        }
        chain.Add(stage, StageMonitorBinding{effect.id, effect.type,
                                             wantsSpectrum(effect.type)});
        ++boundCount;
    }
    return boundCount;
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
