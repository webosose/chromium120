// Copyright 2022 LG Electronics, Inc.
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

#ifndef NEVA_APP_RUNTIME_APP_APP_RUNTIME_JS_DIALOG_MANAGER_DELEGATE_H_
#define NEVA_APP_RUNTIME_APP_APP_RUNTIME_JS_DIALOG_MANAGER_DELEGATE_H_

#include "content/public/browser/javascript_dialog_manager.h"

namespace neva_app_runtime {

class JSDialogManagerDelegate {
 public:
  virtual bool RunJSDialog(const std::string& type,
                           const std::string& message,
                           const std::string& default_prompt_text) = 0;
};

}  // namespace neva_app_runtime

#endif  // NEVA_APP_RUNTIME_APP_APP_RUNTIME_JS_DIALOG_MANAGER_DELEGATE_H_
