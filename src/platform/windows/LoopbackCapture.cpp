#include <shlobj.h>
#include <wchar.h>
#include <iostream>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <combaseapi.h>
#include <audioclientactivationparams.h>

#include "LoopbackCapture.h"

#define BITS_PER_BYTE 8

namespace
{
   // CoCreateInstance/CoInitializeEx are needed for the full-system-audio
   // path (IMMDeviceEnumerator), which -- unlike ActivateAudioInterfaceAsync
   // -- requires COM to be initialized on the calling thread. Node.js does
   // not guarantee this for us, so make sure it's done (once) on whichever
   // thread calls in. RPC_E_CHANGED_MODE means some other component already
   // initialized COM on this thread with a different concurrency model,
   // which is fine -- COM is still usable, we just didn't set the mode.
   void EnsureComInitializedForThisThread()
   {
      thread_local bool comInitialized = false;
      if (!comInitialized)
      {
         HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
         if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
         {
            comInitialized = true;
         }
      }
   }
}

HRESULT CLoopbackCapture::SetDeviceStateErrorIfFailed(HRESULT hr)
{
   if (FAILED(hr))
   {
      m_DeviceState = DeviceState::Error;
   }
   return hr;
}

HRESULT CLoopbackCapture::InitializeLoopbackCapture()
{
   // Create events for sample ready or user stop
   RETURN_IF_FAILED(m_SampleReadyEvent.create(wil::EventOptions::None));

   // Initialize MF
   RETURN_IF_FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE));

   // Register MMCSS work queue
   DWORD dwTaskID = 0;
   RETURN_IF_FAILED(MFLockSharedWorkQueue(L"Capture", 0, &dwTaskID, &m_dwQueueID));

   // Set the capture event work queue to use the MMCSS queue
   m_xSampleReady.SetQueueID(m_dwQueueID);

   // Create the completion event as auto-reset
   RETURN_IF_FAILED(m_hActivateCompleted.create(wil::EventOptions::None));

   // Create the capture-stopped event as auto-reset
   RETURN_IF_FAILED(m_hCaptureStopped.create(wil::EventOptions::None));

   return S_OK;
}

CLoopbackCapture::~CLoopbackCapture()
{
   // Make sure we never leave a dangling thread-reference on the
   // ThreadSafeFunction if the object is torn down unexpectedly (e.g. an
   // early failure before OnFinishCapture ever runs).
   ReleaseThreadSafeFunction();

   if (m_dwQueueID != 0)
   {
      MFUnlockWorkQueue(m_dwQueueID);
   }
}

HRESULT CLoopbackCapture::ActivateProcessAudioInterface(DWORD processId, bool includeProcessTree)
{
   return SetDeviceStateErrorIfFailed([&]() -> HRESULT
                                      {
			AUDIOCLIENT_ACTIVATION_PARAMS audioclientActivationParams = {};
			audioclientActivationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
			audioclientActivationParams.ProcessLoopbackParams.ProcessLoopbackMode = includeProcessTree ?
				PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
			audioclientActivationParams.ProcessLoopbackParams.TargetProcessId = processId;

			PROPVARIANT activateParams = {};
			activateParams.vt = VT_BLOB;
			activateParams.blob.cbSize = sizeof(audioclientActivationParams);
			activateParams.blob.pBlobData = (BYTE*)&audioclientActivationParams;

			wil::com_ptr_nothrow<IActivateAudioInterfaceAsyncOperation> asyncOp;
			RETURN_IF_FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, this, &asyncOp));

			// Wait for activation completion
			m_hActivateCompleted.wait();

			return m_activateResult; }());
}

//
//  ActivateSystemAudioInterface()
//
//  Full-system loopback: grab the current default render (playback) device
//  and activate an IAudioClient on it directly. This is the classic WASAPI
//  loopback-recording technique -- synchronous, no process filtering.
//
HRESULT CLoopbackCapture::ActivateSystemAudioInterface()
{
   return SetDeviceStateErrorIfFailed([&]() -> HRESULT
                                      {
			EnsureComInitializedForThisThread();

			wil::com_ptr_nothrow<IMMDeviceEnumerator> deviceEnumerator;
			RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&deviceEnumerator)));

			wil::com_ptr_nothrow<IMMDevice> device;
			RETURN_IF_FAILED(deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));

			RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
				reinterpret_cast<void**>(m_AudioClient.put())));

			return FinishAudioClientInitialization(); }());
}

//
//  ActivateCompleted()
//
//  Callback implementation of ActivateAudioInterfaceAsync function.  This will be called on MTA thread
//  when results of the activation are available.
//
HRESULT CLoopbackCapture::ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation)
{
   m_activateResult = SetDeviceStateErrorIfFailed([&]() -> HRESULT
                                                  {
			// Check for a successful activation result
			HRESULT hrActivateResult = E_UNEXPECTED;
			wil::com_ptr_nothrow<IUnknown> punkAudioInterface;
			RETURN_IF_FAILED(operation->GetActivateResult(&hrActivateResult, &punkAudioInterface));
			RETURN_IF_FAILED(hrActivateResult);

			// Get the pointer for the Audio Client
			RETURN_IF_FAILED(punkAudioInterface.copy_to(&m_AudioClient));

			return FinishAudioClientInitialization(); }());

   // Let ActivateAudioInterface know that m_activateResult has the result of the activation attempt.
   m_hActivateCompleted.SetEvent();
   return S_OK;
}

//
//  FinishAudioClientInitialization()
//
//  Common tail of both activation paths: m_AudioClient has just been
//  activated (process-loopback or system-default-render) but not yet
//  Initialize()'d. Sets the capture format, initializes the client in
//  loopback mode, and wires up the sample-ready event.
//
HRESULT CLoopbackCapture::FinishAudioClientInitialization()
{
   // The app can also call m_AudioClient->GetMixFormat instead to get the capture format.
   // 16 - bit PCM format.
   m_CaptureFormat.wFormatTag = WAVE_FORMAT_PCM;
   m_CaptureFormat.nChannels = 2;
   m_CaptureFormat.nSamplesPerSec = 48000;
   m_CaptureFormat.wBitsPerSample = 16;
   m_CaptureFormat.nBlockAlign = m_CaptureFormat.nChannels * m_CaptureFormat.wBitsPerSample / BITS_PER_BYTE;
   m_CaptureFormat.nAvgBytesPerSec = m_CaptureFormat.nSamplesPerSec * m_CaptureFormat.nBlockAlign;

   // Initialize the AudioClient in Shared Mode with the user specified buffer.
   // AUDCLNT_STREAMFLAGS_LOOPBACK is what makes this a capture of what the
   // device is rendering (system audio) rather than a microphone input; for
   // process-loopback activations it's required by the virtual device too.
   RETURN_IF_FAILED(m_AudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                              0,
                                              0,
                                              //  AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                              &m_CaptureFormat,
                                              nullptr));

   // Get the maximum size of the AudioClient Buffer
   RETURN_IF_FAILED(m_AudioClient->GetBufferSize(&m_BufferFrames));

   // Get the capture client
   RETURN_IF_FAILED(m_AudioClient->GetService(IID_PPV_ARGS(&m_AudioCaptureClient)));

   // Create Async callback for sample events
   RETURN_IF_FAILED(MFCreateAsyncResult(nullptr, &m_xSampleReady, nullptr, &m_SampleReadyAsyncResult));

   // Tell the system which event handle it should signal when an audio buffer is ready to be processed by the client
   RETURN_IF_FAILED(m_AudioClient->SetEventHandle(m_SampleReadyEvent.get()));

   // Everything is ready.
   m_DeviceState = DeviceState::Initialized;

   return S_OK;
}

HRESULT CLoopbackCapture::StartCaptureAsync(DWORD processId, bool includeProcessTree, Napi::ThreadSafeFunction tsfn)
{
   m_captureMode = CaptureMode::Process;
   m_targetProcessId = processId;
   m_includeProcessTree = includeProcessTree;

   return StartCaptureCommon(tsfn);
}

HRESULT CLoopbackCapture::StartSystemCaptureAsync(Napi::ThreadSafeFunction tsfn)
{
   m_captureMode = CaptureMode::System;

   return StartCaptureCommon(tsfn);
}

//
//  StartCaptureCommon()
//
//  Shared driver for both public entry points above. Takes ownership of
//  the TSFN thread-reference, activates the appropriate audio interface for
//  m_captureMode, and (on success) kicks off the MF work item that starts
//  pulling samples.
//
HRESULT CLoopbackCapture::StartCaptureCommon(Napi::ThreadSafeFunction tsfn)
{
   // Stash the callback handle before doing anything that might fail, and
   // mark it active so EmitAudioData/ReleaseThreadSafeFunction know there's
   // a live thread-reference to account for.
   m_tsfn = tsfn;
   m_tsfnActive = true;

   HRESULT hr = [&]() -> HRESULT
   {
      RETURN_IF_FAILED(InitializeLoopbackCapture());

      if (m_captureMode == CaptureMode::Process)
      {
         RETURN_IF_FAILED(ActivateProcessAudioInterface(m_targetProcessId, m_includeProcessTree));
      }
      else
      {
         RETURN_IF_FAILED(ActivateSystemAudioInterface());
      }

      // We should be in the initialzied state if this is the first time through getting ready to capture.
      if (m_DeviceState == DeviceState::Initialized)
      {
         m_DeviceState = DeviceState::Starting;
         return MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xStartCapture, nullptr);
      }

      return S_OK;
   }();

   if (FAILED(hr))
   {
      // Nothing will ever reach OnFinishCapture to release the TSFN
      // thread-reference for us, so do it here.
      ReleaseThreadSafeFunction();
   }

   return hr;
}

//
//  OnStartCapture()
//
//  Callback method to start capture
//
HRESULT CLoopbackCapture::OnStartCapture(IMFAsyncResult *pResult)
{
   return SetDeviceStateErrorIfFailed([&]() -> HRESULT
                                      {
			// Start the capture
			RETURN_IF_FAILED(m_AudioClient->Start());

			m_DeviceState = DeviceState::Capturing;
			MFPutWaitingWorkItem(m_SampleReadyEvent.get(), 0, m_SampleReadyAsyncResult.get(), &m_SampleReadyKey);

			return S_OK; }());
}

//
//  StopCaptureAsync()
//
//  Stop capture asynchronously via MF Work Item
//
HRESULT CLoopbackCapture::StopCaptureAsync()
{
   RETURN_HR_IF(E_NOT_VALID_STATE, (m_DeviceState != DeviceState::Capturing) &&
                                       (m_DeviceState != DeviceState::Error));

   m_DeviceState = DeviceState::Stopping;

   RETURN_IF_FAILED(MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xStopCapture, nullptr));

   // Wait for capture to stop
   m_hCaptureStopped.wait();

   return S_OK;
}

//
//  OnStopCapture()
//
//  Callback method to stop capture
//
HRESULT CLoopbackCapture::OnStopCapture(IMFAsyncResult *pResult)
{
   // Stop capture by cancelling Work Item
   // Cancel the queued work item (if any)
   if (0 != m_SampleReadyKey)
   {
      MFCancelWorkItem(m_SampleReadyKey);
      m_SampleReadyKey = 0;
   }

   m_AudioClient->Stop();
   m_SampleReadyAsyncResult.reset();

   return FinishCaptureAsync();
}

//
//  FinishCaptureAsync()
//
//
HRESULT CLoopbackCapture::FinishCaptureAsync()
{
   // We should be flushing when this is called
   return MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, 0, &m_xFinishCapture, nullptr);
}

//
//  OnFinishCapture()
//
//  Because of the asynchronous nature of the MF Work Queues and the DataWriter, there could still be
//  a sample processing.  So this will get called to finalize the WAV header.
//
HRESULT CLoopbackCapture::OnFinishCapture(IMFAsyncResult *pResult)
{
   m_DeviceState = DeviceState::Stopped;

   // No more audio will be produced; release our hold on the JS callback
   // now rather than waiting for the destructor.
   ReleaseThreadSafeFunction();

   m_hCaptureStopped.SetEvent();

   return S_OK;
}

//
//  OnSampleReady()
//
//  Callback method when ready to fill sample buffer
//
HRESULT CLoopbackCapture::OnSampleReady(IMFAsyncResult *pResult)
{
   if (SUCCEEDED(OnAudioSampleRequested()))
   {
      // Re-queue work item for next sample
      if (m_DeviceState == DeviceState::Capturing)
      {
         // Re-queue work item for next sample
         return MFPutWaitingWorkItem(m_SampleReadyEvent.get(), 0, m_SampleReadyAsyncResult.get(), &m_SampleReadyKey);
      }
   }
   else
   {
      m_DeviceState = DeviceState::Error;
   }

   return S_OK;
}

static bool IsBufferSilent(const BYTE *pData, UINT32 numFrames, WAVEFORMATEX *format, double silenceThresholdDb = -50.0)
{
   if (format->wBitsPerSample != 16)
   {
      // You can add support for 24-bit, float, etc., but let's keep it simple.
      throw std::runtime_error("Only 16-bit PCM supported in this example");
   }

   const int16_t *samples = reinterpret_cast<const int16_t *>(pData);
   int numSamples = numFrames * format->nChannels;

   double threshold = pow(10.0, silenceThresholdDb / 20.0) * 32768.0;

   for (int i = 0; i < numSamples; ++i)
   {
      if (std::abs(samples[i]) > threshold)
      {
         return false; // Not silent
      }
   }

   return true; // All samples below threshold
}

//
//  EmitAudioData()
//
//  Copies the given PCM bytes and hands them off to the stored JS callback
//  via the ThreadSafeFunction. Safe to call from the MF work-queue thread.
//
void CLoopbackCapture::EmitAudioData(const BYTE *data, UINT32 byteCount)
{
   if (!m_tsfnActive || byteCount == 0 || data == nullptr)
   {
      return;
   }

   auto packet = std::make_unique<AudioDataPacket>();
   packet->data = std::make_unique<BYTE[]>(byteCount);
   memcpy(packet->data.get(), data, byteCount);
   packet->byteCount = byteCount;

   // NonBlockingCall marshals the call onto the JS thread's event loop and
   // invokes the callback below there. `data` here is the AudioDataPacket*
   // we hand off; ownership transfers to the JS-thread callback.
   napi_status status = m_tsfn.NonBlockingCall(
       packet.get(),
       [](Napi::Env env, Napi::Function jsCallback, AudioDataPacket *pkt)
       {
          std::unique_ptr<AudioDataPacket> owned(pkt);
          Napi::Buffer<BYTE> buffer = Napi::Buffer<BYTE>::Copy(env, owned->data.get(), owned->byteCount);
          jsCallback.Call({buffer});
       });

   if (status == napi_ok)
   {
      // The TSFN callback above now owns the packet.
      packet.release();
   }
   // If the queue is full or the TSFN has already been aborted, we simply
   // drop this chunk of audio rather than blocking the capture thread.
}

//
//  ReleaseThreadSafeFunction()
//
//  Releases our single thread-reference on m_tsfn exactly once. Safe to
//  call multiple times (e.g. once from a failure path and again from the
//  destructor) and safe to call from any thread.
//
void CLoopbackCapture::ReleaseThreadSafeFunction()
{
   if (m_tsfnActive)
   {
      m_tsfnActive = false;
      m_tsfn.Release();
   }
}

//
//  OnAudioSampleRequested()
//
//  Called when audio device fires m_SampleReadyEvent
//
HRESULT CLoopbackCapture::OnAudioSampleRequested()
{
   UINT32 FramesAvailable = 0;
   BYTE *Data = nullptr;
   DWORD dwCaptureFlags;
   UINT64 u64DevicePosition = 0;
   UINT64 u64QPCPosition = 0;
   DWORD cbBytesToCapture = 0;

   auto lock = m_CritSec.lock();

   // If this flag is set, we have already queued up the async call to finialize the WAV header
   // So we don't want to grab or write any more data that would possibly give us an invalid size
   if (m_DeviceState == DeviceState::Stopping)
   {
      return S_OK;
   }

   // A word on why we have a loop here;
   // Suppose it has been 10 milliseconds or so since the last time
   // this routine was invoked, and that we're capturing 48000 samples per second.
   //
   // The audio engine can be reasonably expected to have accumulated about that much
   // audio data - that is, about 480 samples.
   //
   // However, the audio engine is free to accumulate this in various ways:
   // a. as a single packet of 480 samples, OR
   // b. as a packet of 80 samples plus a packet of 400 samples, OR
   // c. as 48 packets of 10 samples each.
   //
   // In particular, there is no guarantee that this routine will be
   // run once for each packet.
   //
   // So every time this routine runs, we need to read ALL the packets
   // that are now available;
   //
   // We do this by calling IAudioCaptureClient::GetNextPacketSize
   // over and over again until it indicates there are no more packets remaining.
   while (SUCCEEDED(m_AudioCaptureClient->GetNextPacketSize(&FramesAvailable)) && FramesAvailable > 0)
   {
      cbBytesToCapture = FramesAvailable * m_CaptureFormat.nBlockAlign;

      // Get sample buffer
      RETURN_IF_FAILED(m_AudioCaptureClient->GetBuffer(&Data, &FramesAvailable, &dwCaptureFlags, &u64DevicePosition, &u64QPCPosition));

      // Hand the buffer off to JS via the ThreadSafeFunction
      if (m_DeviceState != DeviceState::Stopping && !IsBufferSilent(Data, FramesAvailable, &m_CaptureFormat, -70))
      {
         size_t byteCount = FramesAvailable * m_CaptureFormat.nBlockAlign;
         EmitAudioData(Data, static_cast<UINT32>(byteCount));
      }

      // Release buffer back
      m_AudioCaptureClient->ReleaseBuffer(FramesAvailable);

      // Increase the size of our 'data' chunk.  m_cbDataSize needs to be accurate
      m_cbDataSize += cbBytesToCapture;
   }

   return S_OK;
}
