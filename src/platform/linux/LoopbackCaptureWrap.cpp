#include "LoopbackCaptureWrap.h"
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace
{
   bool ReadProcessId(Napi::Value value, const char *name, pid_t &pid)
   {
      if (!value.IsNumber())
      {
         Napi::TypeError::New(value.Env(), std::string(name) + " must be a number").ThrowAsJavaScriptException();
         return false;
      }
      const double number = value.As<Napi::Number>().DoubleValue();
      if (!std::isfinite(number) || number < 1 || number > std::numeric_limits<pid_t>::max() ||
          std::floor(number) != number)
      {
         Napi::RangeError::New(value.Env(), std::string(name) + " must be an integer between 1 and " +
                                                std::to_string(std::numeric_limits<pid_t>::max()))
             .ThrowAsJavaScriptException();
         return false;
      }
      pid = static_cast<pid_t>(number);
      return true;
   }
} // namespace
Napi::Object LoopbackCaptureWrap::Init(Napi::Env env, Napi::Object exports)
{
   Napi::Function constructor =
       DefineClass(env, "LoopbackCapture",
                   {
                       InstanceMethod("start", &LoopbackCaptureWrap::Start),
                       InstanceMethod("startSystemAudio", &LoopbackCaptureWrap::StartSystemAudio),
                       InstanceMethod("stop", &LoopbackCaptureWrap::Stop),
                   });
   auto *reference = new Napi::FunctionReference();
   *reference = Napi::Persistent(constructor);
   reference->SuppressDestruct();
   env.SetInstanceData(reference);
   exports.Set("LoopbackCapture", constructor);
   return exports;
}
LoopbackCaptureWrap::LoopbackCaptureWrap(const Napi::CallbackInfo &info) : Napi::ObjectWrap<LoopbackCaptureWrap>(info)
{
}
LoopbackCaptureWrap::~LoopbackCaptureWrap()
{
   if (m_capture)
   {
      m_capture->StopCapture();
   }
}
Napi::Value LoopbackCaptureWrap::Start(const Napi::CallbackInfo &info)
{
   Napi::Env env = info.Env();
   if (info.Length() != 2 || !info[1].IsFunction())
   {
      Napi::TypeError::New(env, "Expected start(processId: number, onData: (chunk: Buffer) => void)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }
   if (m_capture)
   {
      Napi::Error::New(env, "Capture already started on this instance").ThrowAsJavaScriptException();
      return env.Undefined();
   }

   pid_t processId;
   if (!ReadProcessId(info[0], "processId", processId))
      return env.Undefined();

   Napi::Function callback = info[1].As<Napi::Function>();
   Napi::ThreadSafeFunction tsfn =
       Napi::ThreadSafeFunction::New(env, callback, "LoopbackCaptureCallback",
                                     64, // Bound queued audio if the JS consumer falls behind.
                                     1);

   m_capture = std::make_unique<CLoopbackCapture>();
   std::string error;

   if (!m_capture->StartProcessCapture(processId, tsfn, error))
   {
      m_capture.reset();
      Napi::Error::New(env, error).ThrowAsJavaScriptException();
   }
   return env.Undefined();
}
Napi::Value LoopbackCaptureWrap::StartSystemAudio(const Napi::CallbackInfo &info)
{
   Napi::Env env = info.Env();
   if (info.Length() < 1 || info.Length() > 2 || !info[0].IsFunction())
   {
      Napi::TypeError::New(env, "Expected startSystemAudio(onData: (chunk: Buffer) => void, excludeProcessId?: number)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }
   if (m_capture)
   {
      Napi::Error::New(env, "Capture already started on this instance").ThrowAsJavaScriptException();
      return env.Undefined();
   }
   std::optional<pid_t> excludeProcessId;
   if (info.Length() == 2 && !info[1].IsUndefined())
   {
      pid_t pid;
      if (!ReadProcessId(info[1], "excludeProcessId", pid))
         return env.Undefined();
      excludeProcessId = pid;
   }
   Napi::Function callback = info[0].As<Napi::Function>();
   Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(env, callback, "LoopbackCaptureCallback", 64, 1);
   m_capture = std::make_unique<CLoopbackCapture>();
   std::string error;
   if (!m_capture->StartSystemCapture(tsfn, error, excludeProcessId))
   {
      m_capture.reset();
      Napi::Error::New(env, error).ThrowAsJavaScriptException();
   }
   return env.Undefined();
}
Napi::Value LoopbackCaptureWrap::Stop(const Napi::CallbackInfo &info)
{
   if (m_capture)
   {
      m_capture->StopCapture();
      m_capture.reset();
   }
   return info.Env().Undefined();
}
