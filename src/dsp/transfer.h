#pragma once

#include <algorithm>
#include <cmath>

#include "apo_settings.h"

// Transfer functions shared by the DSP stages and the GUI's curve displays.
// Keeping the math in one place means a drawn curve always matches what the
// stage applies. Everything here is inline and allocation-free.
namespace apo::dsp {

inline double DecibelsToLinear(double decibels)
{
    return std::pow(10.0, decibels / 20.0);
}

inline double LinearToDecibels(double linear)
{
    return (linear > 0.0) ? 20.0 * std::log10(linear) : -160.0;
}

// Precomputed shape of one distortion curve. `sharpness` sets how quickly the
// curve bends; `norm` scales it so full scale in maps to full scale out.
struct DistortionCurve
{
    double sharpness = 1.0;
    double norm = 1.0;
};

inline DistortionCurve MakeDistortionCurve(int function, double curvature)
{
    const double amount = std::clamp(curvature / 100.0, 0.0, 1.0);
    DistortionCurve curve;
    switch (function)
    {
    case kDistortionArctan:
        curve.sharpness = 0.5 * std::pow(20.0, amount);
        curve.norm = std::atan(curve.sharpness);
        break;
    case kDistortionCubic:
        // Blend from linear to the classic cubic soft clip; the result stays
        // monotonic for every curvature.
        curve.sharpness = amount;
        curve.norm = 1.0;
        break;
    case kDistortionLog:
        curve.sharpness = 0.02 * std::pow(1000.0, amount);
        curve.norm = std::log1p(curve.sharpness);
        break;
    case kDistortionExp:
        curve.sharpness = 0.02 * std::pow(1000.0, amount);
        curve.norm = -std::expm1(-curve.sharpness);
        break;
    case kDistortionErf:
        curve.sharpness = 0.25 * std::pow(24.0, amount);
        curve.norm = std::erf(curve.sharpness);
        break;
    case kDistortionTanh:
    default:
        curve.sharpness = 0.5 * std::pow(16.0, amount);
        curve.norm = std::tanh(curve.sharpness);
        break;
    }
    return curve;
}

inline double ShapeDistortion(const DistortionCurve& curve, int function,
                              double value)
{
    switch (function)
    {
    case kDistortionArctan:
        return std::atan(curve.sharpness * value) / curve.norm;
    case kDistortionCubic: {
        // The polynomial leaves [-1, 1] outside the unit interval, so the
        // driven signal is clipped first.
        const double input = std::clamp(value, -1.0, 1.0);
        const double soft = 1.5 * input - 0.5 * input * input * input;
        return (1.0 - curve.sharpness) * input + curve.sharpness * soft;
    }
    case kDistortionLog: {
        const double shaped =
            std::log1p(curve.sharpness * std::fabs(value)) / curve.norm;
        return std::copysign(shaped, value);
    }
    case kDistortionExp: {
        const double shaped =
            -std::expm1(-curve.sharpness * std::fabs(value)) / curve.norm;
        return std::copysign(shaped, value);
    }
    case kDistortionErf:
        return std::erf(curve.sharpness * value) / curve.norm;
    case kDistortionTanh:
    default:
        return std::tanh(curve.sharpness * value) / curve.norm;
    }
}

// Settled input/output transfer of the compressor in dBFS.
inline double CompressorTransferDb(double inputDb, double thresholdDb,
                                   double ratio)
{
    if (inputDb <= thresholdDb)
    {
        return inputDb;
    }
    return thresholdDb + (inputDb - thresholdDb) / std::max(ratio, 1.0);
}

// Settled transfer of the gate in dBFS. Attack, hold, and release shape the
// transition over time, so this is the curve the gate converges to.
inline double GateTransferDb(double inputDb, double thresholdDb,
                             double rangeDb)
{
    return (inputDb >= thresholdDb) ? inputDb : inputDb - rangeDb;
}

// Magnitude of the normalized band-pass that the dynamic stage isolates. The
// analog prototype matches the topology-preserving state-variable filter
// closely enough for a display: unity at the center frequency and a slope set
// by Q.
inline double BandpassMagnitudeDb(double frequencyHz, double centerHz,
                                  double q)
{
    const double ratio = frequencyHz / std::max(centerHz, 1.0);
    const double damping = 1.0 / std::max(q, 0.1);
    const double real = 1.0 - ratio * ratio;
    const double imaginary = damping * ratio;
    const double denominator = std::sqrt(real * real + imaginary * imaginary);
    return LinearToDecibels(denominator > 0.0 ? imaginary / denominator
                                              : 0.0);
}

} // namespace apo::dsp
