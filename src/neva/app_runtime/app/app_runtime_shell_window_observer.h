// Copyright 2023 LG Electronics, Inc.
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

#ifndef NEVA_APP_RUNTIME_APP_APP_RUNTIME_SHELL_WINDOW_OBSERVER_H_
#define NEVA_APP_RUNTIME_APP_APP_RUNTIME_SHELL_WINDOW_OBSERVER_H_

#include "base/observer_list.h"
#include "neva/app_runtime/public/app_runtime_export.h"

namespace neva_app_runtime {

class ShellWindow;

class APP_RUNTIME_EXPORT ShellWindowObserver : public base::CheckedObserver {
 public:
  ~ShellWindowObserver() override;
  virtual void OnWindowClosing(ShellWindow* window);
  virtual void OnDestroying(ShellWindow* window);
};

}  // namespace neva_app_runtime

#endif  // NEVA_APP_RUNTIME_APP_APP_RUNTIME_SHELL_WINDOW_OBSERVER_H_
