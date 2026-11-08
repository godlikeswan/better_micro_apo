#pragma once

#include <windows.h>

#include <atlbase.h>
#include <atlcom.h>

#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>

#include <string>
#include <vector>

#include "apo_ids.h"
#include "apo_settings.h"
#include "dsp/dsp.h"

// Minimal capture APO.
//
// DSP chain: optional gate -> linked peak compressor -> makeup gain ->
// optional mono copy. The stages live in src/dsp; ConfigureDsp() assembles the
// chain, so adding or reordering a stage only touches that function and the
// settings model.
//
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
    static bool IsSupportedFormat(const UNCOMPRESSEDAUDIOFORMAT& format);

    void ConfigureDsp();
    void ProcessBuffer(const BYTE* source, BYTE* destination, UINT32 frames);

    std::wstring m_endpointId;
    apo::ApoSettings m_settings;
    apo::NoiseProfile m_noiseProfile;

    apo::dsp::Chain m_chain;
    apo::dsp::AudioFormat m_formatInfo;
    std::vector<double> m_scratch;
    UINT32 m_maxFrames = 0;
};
