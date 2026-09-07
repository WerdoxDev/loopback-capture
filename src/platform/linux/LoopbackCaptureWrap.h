#pragma once

#include <memory>

#include <napi.h>

#include "LoopbackCapture.h"

class LoopbackCaptureWrap : public Napi::ObjectWrap<LoopbackCaptureWrap>
{
public:
   static Napi::Object Init(Napi::Env env, Napi::Object exports);

   explicit LoopbackCaptureWrap(const Napi::CallbackInfo &info);
   ~LoopbackCaptureWrap();

private:
   Napi::Value Start(const Napi::CallbackInfo &info);
   Napi::Value StartSystemAudio(const Napi::CallbackInfo &info);
   Napi::Value Stop(const Napi::CallbackInfo &info);

   std::unique_ptr<CLoopbackCapture> m_capture;
};
