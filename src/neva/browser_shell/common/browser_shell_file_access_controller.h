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

#ifndef NEVA_BROWSER_SHELL_COMMON_BROWSER_SHELL_FILE_ACCESS_CONTROLLER_H_
#define NEVA_BROWSER_SHELL_COMMON_BROWSER_SHELL_FILE_ACCESS_CONTROLLER_H_

#include "neva/app_runtime/common/app_runtime_file_access_controller.h"
#include "neva/browser_shell/common/browser_shell_export.h"

namespace browser_shell {

class BROWSER_SHELL_EXPORT BrowserShellFileAccessController
    : public neva_app_runtime::AppRuntimeFileAccessController {
 public:
  bool IsAccessAllowed(const base::FilePath& path,
                       const neva_app_runtime::WebViewInfo&) const override;
};

}  // namespace browser_shell

#endif  // NEVA_BROWSER_SHELL_COMMON_BROWSER_SHELL_FILE_ACCESS_CONTROLLER_H_
