#pragma once

#include <memory>

#include <AudioClient.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <guiddef.h>
#include <mfapi.h>

#include <wrl/implements.h>
#include <wil/com.h>
#include <wil/result.h>

#include <napi.h>

#include "Common.h"

using namespace Microsoft::WRL;

// One chunk of captured PCM data, handed off across the thread boundary via
// the ThreadSafeFunction. Allocated on the MF work-queue thread, freed on
// the JS thread once the callback has consumed it.
struct AudioDataPacket
{
   std::unique_ptr<BYTE[]> data;
   UINT32 byteCount = 0;
};

class CLoopbackCapture : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler>
{
public:
   CLoopbackCapture() = default;
   ~CLoopbackCapture();

   // tsfn is expected to already have been created (and its thread count
   // accounted for) by the caller; CLoopbackCapture takes ownership of
   // exactly one "thread" reference on it and releases that reference
   // itself once capture has fully finished (see OnFinishCapture).

   // Captures audio rendered by a specific process (and optionally its
   // child process tree), using the AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
   // virtual device (Windows 10 2004+).
   HRESULT StartCaptureAsync(DWORD processId, bool includeProcessTree, Napi::ThreadSafeFunction tsfn);

   // Captures everything going out through the current default audio
   // render (playback) device -- i.e. "all system audio", the same
   // technique OBS/Discord-style loopback recorders use. No process
   // filtering is applied.
   HRESULT StartSystemCaptureAsync(Napi::ThreadSafeFunction tsfn);

   HRESULT StopCaptureAsync();

   METHODASYNCCALLBACK(CLoopbackCapture, StartCapture, OnStartCapture);
   METHODASYNCCALLBACK(CLoopbackCapture, StopCapture, OnStopCapture);
   METHODASYNCCALLBACK(CLoopbackCapture, SampleReady, OnSampleReady);
   METHODASYNCCALLBACK(CLoopbackCapture, FinishCapture, OnFinishCapture);

   // IActivateAudioInterfaceCompletionHandler
   STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation *operation);

private:
   // NB: All states >= Initialized will allow some methods
   // to be called successfully on the Audio Client
   enum class DeviceState
   {
      Uninitialized,
      Error,
      Initialized,
      Starting,
      Capturing,
      Stopping,
      Stopped,
   };

   enum class CaptureMode
   {
      Process,
      System,
   };

   HRESULT OnStartCapture(IMFAsyncResult *pResult);
   HRESULT OnStopCapture(IMFAsyncResult *pResult);
   HRESULT OnFinishCapture(IMFAsyncResult *pResult);
   HRESULT OnSampleReady(IMFAsyncResult *pResult);

   HRESULT InitializeLoopbackCapture();
   HRESULT OnAudioSampleRequested();

   // Shared driver for both public Start*CaptureAsync entry points: sets
   // up the TSFN bookkeeping, activates the right kind of audio interface
   // (m_captureMode), then kicks off the MF work item to actually start
   // pulling samples.
   HRESULT StartCaptureCommon(Napi::ThreadSafeFunction tsfn);

   // Process-specific loopback (existing behavior): asynchronous, goes
   // through ActivateAudioInterfaceAsync / ActivateCompleted.
   HRESULT ActivateProcessAudioInterface(DWORD processId, bool includeProcessTree);

   // Full system loopback: synchronous, activates IAudioClient directly on
   // the default render endpoint.
   HRESULT ActivateSystemAudioInterface();

   // Common tail end of both activation paths once m_AudioClient holds a
   // freshly-activated (but not yet Initialize()'d) IAudioClient.
   HRESULT FinishAudioClientInitialization();

   HRESULT FinishCaptureAsync();

   HRESULT SetDeviceStateErrorIfFailed(HRESULT hr);

   // Copies [data, data+byteCount) and queues it to be delivered to the
   // stored JS callback. Safe to call from any thread.
   void EmitAudioData(const BYTE *data, UINT32 byteCount);

   // Releases the ThreadSafeFunction's thread reference exactly once.
   void ReleaseThreadSafeFunction();

   wil::com_ptr_nothrow<IAudioClient> m_AudioClient;
   WAVEFORMATEX m_CaptureFormat{};
   UINT32 m_BufferFrames = 0;
   wil::com_ptr_nothrow<IAudioCaptureClient> m_AudioCaptureClient;
   wil::com_ptr_nothrow<IMFAsyncResult> m_SampleReadyAsyncResult;

   wil::unique_event_nothrow m_SampleReadyEvent;
   MFWORKITEM_KEY m_SampleReadyKey = 0;
   wil::critical_section m_CritSec;
   DWORD m_dwQueueID = 0;
   DWORD m_cbHeaderSize = 0;
   DWORD m_cbDataSize = 0;

   // These two members are used to communicate between the main thread
   // and the ActivateCompleted callback.
   HRESULT m_activateResult = E_UNEXPECTED;

   DeviceState m_DeviceState{DeviceState::Uninitialized};
   wil::unique_event_nothrow m_hActivateCompleted;
   wil::unique_event_nothrow m_hCaptureStopped;

   // Which activation path StartCaptureCommon should take, and the
   // process-mode parameters (unused in System mode).
   CaptureMode m_captureMode = CaptureMode::Process;
   DWORD m_targetProcessId = 0;
   bool m_includeProcessTree = false;

   // JS interop: the callback supplied by the JS caller, wrapped so it can
   // be invoked safely from the MF work-queue threads.
   Napi::ThreadSafeFunction m_tsfn;
   bool m_tsfnActive = false;
};
