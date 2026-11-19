#pragma once

#include <windows.h>

#include <atlbase.h>
#include <atlcom.h>

#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>

#include <string>
#include <vector>
#include <cstdint>

#include "apo_ids.h"
#include "apo_settings.h"
#include "dsp/dsp.h"
#include "dsp/stages.h"
#include "runtime_settings.h"
#include "stage_capture.h"
#include "stage_monitor.h"

// Minimal capture APO.
//
// DSP chain: the per-endpoint effect list, processed in list order. The stages
// themselves live in src/dsp; StagePool owns a fixed pool of them and rebuilds
// the active order from the settings snapshot without allocating.
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
    void ProcessBuffer(const BYTE* source, BYTE* destination, UINT32 frames,
                       apo::StageMonitorSink* monitor);

    // Maps the GUI's per-endpoint settings file read-only. The APO polls the
    // generation counter in APOProcess and retunes the chain in place.
    void OpenRuntimeSettings();
    void CloseRuntimeSettings();
    void PollRuntimeSettings();

    // Stage-local noise capture. The tap is inserted only for the duration of
    // one GUI request; no settings or bindings are changed.
    void OpenStageCapture();
    void CloseStageCapture();
    void PollStageCapture();
    void OpenStageMonitor();
    void CloseStageMonitor();
    void RebuildChain();
    bool FindCaptureTarget(int presetIndex, int effectId, size_t& chainIndex,
                           int& fftSize) const;

    std::wstring m_endpointId;
    apo::ApoSettings m_settings;
    apo::NoiseProfile m_noiseProfile;

    apo::dsp::Chain m_chain;
    apo::dsp::StagePool m_stagePool;
    apo::StageCaptureView m_captureView;
    apo::StageCaptureTap m_captureTap;
    apo::StageMonitor m_monitor;
    bool m_captureActive = false;
    size_t m_captureChainIndex = 0;
    LONG m_capturePresetIndex = -1;
    LONG m_captureEffectId = -1;
    LONG m_captureRequestId = 0;
    apo::dsp::AudioFormat m_formatInfo;
    std::vector<double> m_scratch;
    UINT32 m_maxFrames = 0;

    HANDLE m_runtimeFile = nullptr;
    HANDLE m_runtimeMapping = nullptr;
    const apo::RuntimeSettingsFile* m_runtimeView = nullptr;
    std::uint64_t m_runtimeGeneration = 0;
};
