#pragma once

#include <napi.h>
#include <wrl\client.h>

#include "LoopbackCapture.h"

// Thin N-API wrapper around the COM-based CLoopbackCapture. Kept as a
// separate class (rather than making CLoopbackCapture itself inherit from
// Napi::ObjectWrap) because WRL's RuntimeClass and Napi::ObjectWrap each
// assume they own the object's lifetime/deletion; composing them avoids
// that conflict.
class LoopbackCaptureWrap : public Napi::ObjectWrap<LoopbackCaptureWrap>
{
public:
   static Napi::Object Init(Napi::Env env, Napi::Object exports);

   explicit LoopbackCaptureWrap(const Napi::CallbackInfo &info);
   ~LoopbackCaptureWrap();

private:
   // JS: capture.start(processId: number, includeProcessTree: boolean, onData: (chunk: Buffer) => void): void
   Napi::Value Start(const Napi::CallbackInfo &info);

   // JS: capture.startSystemAudio(onData: (chunk: Buffer) => void): void
   // Captures everything going out through the current default playback
   // device -- no process id needed.
   Napi::Value StartSystemAudio(const Napi::CallbackInfo &info);

   // JS: capture.stop(): void
   Napi::Value Stop(const Napi::CallbackInfo &info);

   // Shared by Start/StartSystemAudio: checks there's no capture already
   // running on this instance and builds the ThreadSafeFunction from the
   // JS callback. Returns false (and throws) if the checks fail.
   bool PrepareStart(const Napi::CallbackInfo &info, Napi::Function callback, Napi::ThreadSafeFunction &outTsfn);

   Microsoft::WRL::ComPtr<CLoopbackCapture> m_capture;
};
