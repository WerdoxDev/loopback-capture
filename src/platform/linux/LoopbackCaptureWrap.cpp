#include "LoopbackCaptureWrap.h"
#include <memory>
#include <string>
Napi::Object LoopbackCaptureWrap::Init(Napi::Env env, Napi::Object exports)
{
   Napi::Function constructor = DefineClass(env, "LoopbackCapture", {
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
LoopbackCaptureWrap::LoopbackCaptureWrap(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<LoopbackCaptureWrap>(info)
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
   if (info.Length() < 3 || !info[0].IsNumber() || !info[1].IsBoolean() || !info[2].IsFunction())
   {
      Napi::TypeError::New(env,
                           "Expected start(processId: number, includeProcessTree: boolean, onData: (chunk: Buffer) => void)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }
   if (m_capture)
   {
      Napi::Error::New(env, "Capture already started on this instance").ThrowAsJavaScriptException();
      return env.Undefined();
   }

   const pid_t processId = static_cast<pid_t>(info[0].As<Napi::Number>().Int32Value());
   const bool includeProcessTree = info[1].As<Napi::Boolean>().Value();

   Napi::Function callback = info[2].As<Napi::Function>();
   Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(
       env,
       callback,
       "LoopbackCaptureCallback",
       0,
       1);

   m_capture = std::make_unique<CLoopbackCapture>();
   std::string error;

   if (!m_capture->StartProcessCapture(processId, includeProcessTree, tsfn, error))
   {
      m_capture.reset();
      Napi::Error::New(env, error).ThrowAsJavaScriptException();
   }
   return env.Undefined();
}
Napi::Value LoopbackCaptureWrap::StartSystemAudio(const Napi::CallbackInfo &info)
{
   Napi::Env env = info.Env();
   if (info.Length() < 1 || !info[0].IsFunction())
   {
      Napi::TypeError::New(env, "Expected startSystemAudio(onData: (chunk: Buffer) => void)")
          .ThrowAsJavaScriptException();
      return env.Undefined();
   }
   if (m_capture)
   {
      Napi::Error::New(env, "Capture already started on this instance").ThrowAsJavaScriptException();
      return env.Undefined();
   }
   Napi::Function callback = info[0].As<Napi::Function>();
   Napi::ThreadSafeFunction tsfn = Napi::ThreadSafeFunction::New(
       env,
       callback,
       "LoopbackCaptureCallback",
       0,
       1);
   m_capture = std::make_unique<CLoopbackCapture>();
   std::string error;
   if (!m_capture->StartSystemCapture(tsfn, error))
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
