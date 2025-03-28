// Copyright 2018-2019 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "neva/injection/renderer/memorymanager/memorymanager_injection.h"

#include <tuple>

#include "base/functional/bind.h"
#include "gin/arguments.h"
#include "gin/function_template.h"
#include "gin/handle.h"
#include "neva/pal_service/public/mojom/constants.mojom.h"
#include "third_party/blink/public/common/thread_safe_browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"

namespace injections {

namespace {

// Returns true if |maybe| is both a value, and that value is true.
inline bool IsTrue(v8::Maybe<bool> maybe) {
  return maybe.IsJust() && maybe.FromJust();
}

}  // anonymous namespace

gin::WrapperInfo MemoryManagerInjection::kWrapperInfo = {
    gin::kEmbedderNativeGin};

const char MemoryManagerInjection::kGetMemoryStatusMethodName[] =
    "getMemoryStatus";
const char MemoryManagerInjection::kOnLevelChangedCallbackName[] =
    "onlevelchanged";

MemoryManagerInjection::MemoryManagerInjection() : listener_receiver_(this) {
  blink::Platform::Current()->GetBrowserInterfaceBroker()->GetInterface(
      remote_memorymanager_.BindNewPipeAndPassReceiver());
  SubscribeToLevelChanged();
}

MemoryManagerInjection::~MemoryManagerInjection() = default;

void MemoryManagerInjection::GetMemoryStatus(gin::Arguments* args) {
  v8::Local<v8::Function> local_func;
  if (!args->GetNext(&local_func)) {
    LOG(ERROR) << __func__ << "(), wrong arguments list";
    return;
  }

  auto callback_ptr = std::make_unique<v8::Persistent<v8::Function>>(
      args->isolate(), local_func);

  remote_memorymanager_->GetMemoryStatus(base::BindOnce(
      &MemoryManagerInjection::OnGetMemoryStatusRespond, base::Unretained(this),
      std::move(callback_ptr)));
}

gin::ObjectTemplateBuilder MemoryManagerInjection::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin::Wrappable<MemoryManagerInjection>::GetObjectTemplateBuilder(
             isolate)
      .SetMethod(kGetMemoryStatusMethodName,
                 &MemoryManagerInjection::GetMemoryStatus);
}

void MemoryManagerInjection::LevelChanged(const std::string& current) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Object> wrapper;
  if (!GetWrapper(isolate).ToLocal(&wrapper))
    return;

  v8::Local<v8::Context> context;
  if (!wrapper->GetCreationContext().ToLocal(&context))
    return;
  v8::Context::Scope context_scope(context);
  v8::Local<v8::String> callback_key(
      gin::StringToV8(isolate, kOnLevelChangedCallbackName));

  if (!IsTrue(wrapper->Has(context, callback_key))) {
    LOG(ERROR) << __func__ << "(): no appropriate callback";
    return;
  }

  v8::Local<v8::Value> func_value =
      wrapper->Get(context, callback_key).ToLocalChecked();
  if (!func_value->IsFunction()) {
    LOG(ERROR) << __func__ << "(): callback is not a function";
    return;
  }

  v8::Local<v8::Function> func = v8::Local<v8::Function>::Cast(func_value);

  const int argc = 1;
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate, current)};
  std::ignore = func->Call(context, wrapper, argc, argv);
}

void MemoryManagerInjection::SubscribeToLevelChanged() {
  remote_memorymanager_->Subscribe(base::BindOnce(
      &MemoryManagerInjection::OnSubscribeRespond, base::Unretained(this)));
}

void MemoryManagerInjection::OnSubscribeRespond(
    mojo::PendingAssociatedReceiver<pal::mojom::MemoryManagerListener>
        receiver) {
  listener_receiver_.Bind(std::move(receiver));
}

void MemoryManagerInjection::OnGetMemoryStatusRespond(
    std::unique_ptr<v8::Persistent<v8::Function>> callback,
    const std::string& status) {
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Object> wrapper;
  if (!GetWrapper(isolate).ToLocal(&wrapper)) {
    LOG(ERROR) << __func__ << "(): can not get wrapper";
    return;
  }

  v8::Local<v8::Context> context;
  if (!wrapper->GetCreationContext().ToLocal(&context)) {
    LOG(ERROR) << __func__ << "(): can not get context";
    return;
  }
  v8::Context::Scope context_scope(context);
  v8::Local<v8::Function> local_callback = callback->Get(isolate);

  const int argc = 1;
  v8::Local<v8::Value> argv[] = {gin::StringToV8(isolate, status)};
  std::ignore = local_callback->Call(context, wrapper, argc, argv);
}

// static
void MemoryManagerInjection::Install(blink::WebLocalFrame* frame) {
  v8::Isolate* isolate = blink::MainThreadIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = frame->MainWorldScriptContext();
  if (context.IsEmpty())
    return;

  v8::Local<v8::Object> global = context->Global();
  v8::Context::Scope context_scope(context);

  v8::Local<v8::String> navigator_name = gin::StringToV8(isolate, "navigator");
  v8::Local<v8::Object> navigator;
  if (!gin::Converter<v8::Local<v8::Object>>::FromV8(
          isolate, global->Get(context, navigator_name).ToLocalChecked(),
          &navigator))
    return;

  if (IsTrue(
          navigator->Has(context, gin::StringToV8(isolate, "memorymanager"))))
    return;

  v8::Local<v8::Object> memorymanager;
  if (CreateObject(isolate, navigator).ToLocal(&memorymanager)) {
    memorymanager
        ->Set(context,
              gin::StringToV8(
                  isolate, MemoryManagerInjection::kOnLevelChangedCallbackName),
              v8::Object::New(isolate))
        .Check();
  }
}

void MemoryManagerInjection::Uninstall(blink::WebLocalFrame* frame) {
  v8::Isolate* isolate = blink::MainThreadIsolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = frame->MainWorldScriptContext();
  if (context.IsEmpty())
    return;

  v8::Local<v8::Object> global = context->Global();
  v8::Context::Scope context_scope(context);

  v8::Local<v8::String> navigator_name = gin::StringToV8(isolate, "navigator");
  v8::Local<v8::Object> navigator;
  if (gin::Converter<v8::Local<v8::Object>>::FromV8(
          isolate, global->Get(context, navigator_name).ToLocalChecked(),
          &navigator)) {
    v8::Local<v8::String> memorymanager_name =
        gin::StringToV8(isolate, "memorymanager");
    if (IsTrue(navigator->Has(context, memorymanager_name)))
      std::ignore = navigator->Delete(context, memorymanager_name);
  }
}

// static
v8::MaybeLocal<v8::Object> MemoryManagerInjection::CreateObject(
    v8::Isolate* isolate,
    v8::Local<v8::Object> parent) {
  gin::Handle<MemoryManagerInjection> memorymanager =
      gin::CreateHandle(isolate, new MemoryManagerInjection());
  parent
      ->Set(isolate->GetCurrentContext(),
            gin::StringToV8(isolate, "memorymanager"), memorymanager.ToV8())
      .Check();
  return memorymanager->GetWrapper(isolate);
}

}  // namespace injections
