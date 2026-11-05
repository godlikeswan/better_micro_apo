#pragma once

#include <windows.h>

#include <atlbase.h>
#include <atlcom.h>

#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>

#include <string>

#include "apo_ids.h"
#include "apo_settings.h"

// Minimal capture APO.
//
// DSP chain: input gain -> peak compressor -> makeup gain.
// The DSP settings are loaded per endpoint from
// C:\ProgramData\better_micro_apo\devices\{endpoint-guid}.ini.
//
// The APO contract itself (Initialize, registration properties, format
// negotiation, LockForProcess/UnlockForProcess, Reset, Calc*Frames and the
// reference-counting rules the engine expects) comes from Microsoft's
// CBaseAudioProcessingObject. This class adds the DSP and the
// IAudioSystemEffects marker.
//
// COM identity is owned by ATL, exactly like the SwapAPO sample: the audio
// engine creates APOs with an outer (controlling) unknown, and CComCoClass
// picks CComAggObject for that case, which implements the aggregation rules
// the engine expects.
class BetterMicroApo :
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<BetterMicroApo, &CLSID_BetterMicroApo>,
    public CBaseAudioProcessingObject,
    public IAudioSystemEffects
{
public:
    BetterMicroApo()
        : CBaseAudioProcessingObject(&sm_RegProperties.m_Properties)
    {
    }

    DECLARE_NO_REGISTRY()
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(BetterMicroApo)
        COM_INTERFACE_ENTRY(IAudioSystemEffects)
        COM_INTERFACE_ENTRY(IAudioProcessingObjectRT)
        COM_INTERFACE_ENTRY(IAudioProcessingObject)
        COM_INTERFACE_ENTRY(IAudioProcessingObjectConfiguration)
    END_COM_MAP()

    HRESULT FinalConstruct();
    void FinalRelease();

    // IAudioProcessingObject
    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHOD(Reset)() override;

    // IAudioProcessingObjectConfiguration
    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections,
                              APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                              UINT32 u32NumOutputConnections,
                              APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHOD(UnlockForProcess)() override;

    // IAudioProcessingObjectRT
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections,
                                 APO_CONNECTION_PROPERTY** ppInputConnections,
                                 UINT32 u32NumOutputConnections,
                                 APO_CONNECTION_PROPERTY** ppOutputConnections) override;

    // Registration data used by CBaseAudioProcessingObject and DllRegisterServer.
    static const CRegAPOProperties<1> sm_RegProperties;

private:
    enum class SampleFormat
    {
        Unknown,
        Float32,
        Pcm16,
        Pcm24,
        Pcm32
    };

    static bool IsSupportedFormat(const UNCOMPRESSEDAUDIOFORMAT& format);
    static const wchar_t* FormatName(SampleFormat format);

    void ConfigureDsp();
    void UpdateEnvelope(double peak);
    double ReadSample(const BYTE* buffer, size_t index) const;
    void WriteSample(BYTE* buffer, size_t index, double value) const;
    void ProcessBuffer(const BYTE* source, BYTE* destination, UINT32 frames);

    std::wstring m_endpointId;
    apo::ApoSettings m_settings;

    UINT32 m_channels = 0;
    UINT32 m_sampleRate = 0;
    UINT32 m_bytesPerSample = 0;
    SampleFormat m_format = SampleFormat::Unknown;

    // Compressor state and coefficients.
    double m_inputGain = 1.0;
    double m_makeupGain = 1.0;
    double m_thresholdLinear = 1.0;
    double m_attackCoeff = 0.0;
    double m_releaseCoeff = 0.0;
    double m_envelopeDb = 0.0;
};
