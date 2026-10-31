#include "LoopbackCaptureWrap.h"
#include "Utils.h"

#include <cmath>
#include <limits>

using namespace Microsoft::WRL;

namespace
{
   bool ReadProcessId(Napi::Value value, const char *name, DWORD &processId)
   {
      if (!value.IsNumber())
      {
         Napi::TypeError::New(value.Env(), std::string(name) + " must be a number")
             .ThrowAsJavaScriptException();
         return false;
      }

      const double number = value.As<Napi::Number>().DoubleValue();
      if (!std::isfinite(number) || number < 1 || number > (std::numeric_limits<DWORD>::max)() || std::floor(number) != number)
      {
         Napi::RangeError::New(value.Env(), std::string(name) + " must be an integer between 1 and 4294967295")
             .ThrowAsJavaScriptException();
         return false;
      }

      processId = static_cast<DWORD>(number);
      return true;
   }
}

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

   if (info.Length() != 2 || !info[1].IsFunction())
   {
      Napi::TypeError::New(env,
                           "Expected start(processId: number, onData: (chunk: Buffer) => void)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }

   DWORD processId;
   if (!ReadProcessId(info[0], "processId", processId))
   {
      return env.Undefined();
   }
   Napi::Function callback = info[1].As<Napi::Function>();

   Napi::ThreadSafeFunction tsfn;
   if (!PrepareStart(info, callback, tsfn))
   {
      return env.Undefined();
   }

   HRESULT hr = m_capture->StartCaptureAsync(processId, tsfn);
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

   if (info.Length() < 1 || info.Length() > 2 || !info[0].IsFunction())
   {
      Napi::TypeError::New(env,
                           "Expected startSystemAudio(onData: (chunk: Buffer) => void, excludeProcessId?: number)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }

   std::optional<DWORD> excludeProcessId;
   if (info.Length() == 2 && !info[1].IsUndefined())
   {
      DWORD processId;
      if (!ReadProcessId(info[1], "excludeProcessId", processId))
      {
         return env.Undefined();
      }
      excludeProcessId = processId;
   }

   Napi::Function callback = info[0].As<Napi::Function>();

   Napi::ThreadSafeFunction tsfn;
   if (!PrepareStart(info, callback, tsfn))
   {
      return env.Undefined();
   }

   HRESULT hr = m_capture->StartSystemCaptureAsync(tsfn, excludeProcessId);
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
