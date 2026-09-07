#include "LoopbackCaptureWrap.h"
#include "Utils.h"

using namespace Microsoft::WRL;

Napi::Object LoopbackCaptureWrap::Init(Napi::Env env, Napi::Object exports)
{
   Napi::Function func = DefineClass(env, "LoopbackCapture", {
                                                                 InstanceMethod("start", &LoopbackCaptureWrap::Start),
                                                                 InstanceMethod("startSystemAudio", &LoopbackCaptureWrap::StartSystemAudio),
                                                                 InstanceMethod("stop", &LoopbackCaptureWrap::Stop),
                                                             });

   Napi::FunctionReference *constructor = new Napi::FunctionReference();
   *constructor = Napi::Persistent(func);
   constructor->SuppressDestruct();
   env.SetInstanceData(constructor);

   exports.Set("LoopbackCapture", func);
   return exports;
}

LoopbackCaptureWrap::LoopbackCaptureWrap(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<LoopbackCaptureWrap>(info)
{
}

LoopbackCaptureWrap::~LoopbackCaptureWrap()
{
   // Best-effort cleanup if the JS object is GC'd without an explicit
   // stop() call. StopCaptureAsync() is a no-op (returns E_NOT_VALID_STATE)
   // if capture already isn't running.
   if (m_capture)
   {
      m_capture->StopCaptureAsync();
      m_capture.Reset();
   }
}

bool LoopbackCaptureWrap::PrepareStart(const Napi::CallbackInfo &info, Napi::Function callback, Napi::ThreadSafeFunction &outTsfn)
{
   Napi::Env env = info.Env();

   if (m_capture)
   {
      Napi::Error::New(env, "Capture already started on this instance").ThrowAsJavaScriptException();
      return false;
   }

   outTsfn = Napi::ThreadSafeFunction::New(
       env,
       callback,
       "LoopbackCaptureCallback",
       0, // unlimited queue
       1  // one thread reference; CLoopbackCapture releases it on finish/failure
   );

   m_capture = Make<CLoopbackCapture>();
   return true;
}

Napi::Value LoopbackCaptureWrap::Start(const Napi::CallbackInfo &info)
{
   Napi::Env env = info.Env();

   if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsBoolean() || !info[2].IsFunction())
   {
      Napi::TypeError::New(env,
                           "Expected start(processId: number, includeProcessTree: boolean, onData: (chunk: Buffer) => void)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }

   DWORD processId = static_cast<DWORD>(info[0].As<Napi::Number>().Uint32Value());
   bool includeProcessTree = info[1].As<Napi::Boolean>().Value();
   Napi::Function callback = info[2].As<Napi::Function>();

   Napi::ThreadSafeFunction tsfn;
   if (!PrepareStart(info, callback, tsfn))
   {
      return env.Undefined();
   }

   HRESULT hr = m_capture->StartCaptureAsync(processId, includeProcessTree, tsfn);
   if (FAILED(hr))
   {
      m_capture.Reset();
      Napi::Error::New(env, "Failed to start loopback capture (HRESULT " + HResultToString(hr) + ")")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }

   return env.Undefined();
}

Napi::Value LoopbackCaptureWrap::StartSystemAudio(const Napi::CallbackInfo &info)
{
   Napi::Env env = info.Env();

   if (info.Length() < 1 || !info[0].IsFunction())
   {
      Napi::TypeError::New(env,
                           "Expected startSystemAudio(onData: (chunk: Buffer) => void)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }

   Napi::Function callback = info[0].As<Napi::Function>();

   Napi::ThreadSafeFunction tsfn;
   if (!PrepareStart(info, callback, tsfn))
   {
      return env.Undefined();
   }

   HRESULT hr = m_capture->StartSystemCaptureAsync(tsfn);
   if (FAILED(hr))
   {
      m_capture.Reset();
      Napi::Error::New(env, "Failed to start system audio capture (HRESULT " + HResultToString(hr) + ")")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }

   return env.Undefined();
}

Napi::Value LoopbackCaptureWrap::Stop(const Napi::CallbackInfo &info)
{
   Napi::Env env = info.Env();

   if (!m_capture)
   {
      return env.Undefined();
   }

   // Note: StopCaptureAsync() blocks the calling (JS) thread until the MF
   // work queue confirms capture has fully stopped. This mirrors the
   // original console-app behavior; if you need a non-blocking stop(),
   // wrap this call in a Napi::AsyncWorker instead.
   HRESULT hr = m_capture->StopCaptureAsync();
   m_capture.Reset();

   if (FAILED(hr))
   {
      Napi::Error::New(env, "Failed to stop loopback capture (HRESULT " + HResultToString(hr) + ")")
          .ThrowAsJavaScriptException();
   }

   return env.Undefined();
}
