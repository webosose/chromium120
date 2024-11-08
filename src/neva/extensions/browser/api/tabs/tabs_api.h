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

#ifndef NEVA_EXTENSIONS_BROWSER_API_TABS_TABS_API_H_
#define NEVA_EXTENSIONS_BROWSER_API_TABS_TABS_API_H_

#include "extensions/browser/api/execute_code_function.h"
#include "extensions/browser/extension_function.h"
#include "extensions/browser/script_executor.h"

namespace neva {

// Windows
class WindowsGetCurrentFunction : public ExtensionFunction {
  ~WindowsGetCurrentFunction() override {}
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.getCurrent", WINDOWS_GETCURRENT)
};
class WindowsGetLastFocusedFunction : public ExtensionFunction {
  ~WindowsGetLastFocusedFunction() override {}
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.getLastFocused", WINDOWS_GETLASTFOCUSED)
};
class WindowsGetAllFunction : public ExtensionFunction {
  ~WindowsGetAllFunction() override {}
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.getAll", WINDOWS_GETALL)
};
class WindowsCreateFunction : public ExtensionFunction {
  ~WindowsCreateFunction() override {}
  ResponseAction Run() override;
  void OnWindowsCreated(int window_id);
  DECLARE_EXTENSION_FUNCTION("windows.create", WINDOWS_CREATE)
};
class WindowsUpdateFunction : public ExtensionFunction {
  ~WindowsUpdateFunction() override {}
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.update", WINDOWS_UPDATE)
};
class WindowsRemoveFunction : public ExtensionFunction {
  ~WindowsRemoveFunction() override {}
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("windows.remove", WINDOWS_REMOVE)
};

// Tabs
class TabsGetFunction : public ExtensionFunction {
  ~TabsGetFunction() override {}
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("tabs.get", TABS_GET)
};
class TabsCreateFunction : public ExtensionFunction {
  ~TabsCreateFunction() override {}
  ResponseAction Run() override;
  void OnTabCreated(int tab_id);
  DECLARE_EXTENSION_FUNCTION("tabs.create", TABS_CREATE)
};
class TabsQueryFunction : public ExtensionFunction {
  ~TabsQueryFunction() override {}
  ResponseAction Run() override;
  DECLARE_EXTENSION_FUNCTION("tabs.query", TABS_QUERY)
};

// Implement API calls tabs.insertCSS.
class ExecuteCodeInTabFunction : public extensions::ExecuteCodeFunction {
 public:
  ExecuteCodeInTabFunction();

 protected:
  ~ExecuteCodeInTabFunction() override;

  // Initializes |execute_tab_id_| and |details_|.
  extensions::ExecuteCodeFunction::InitResult Init() override;
  bool ShouldInsertCSS() const override;
  bool ShouldRemoveCSS() const override;
  bool CanExecuteScriptOnPage(std::string* error) override;
  extensions::ScriptExecutor* GetScriptExecutor(std::string* error) override;
  bool IsWebView() const override;
  const GURL& GetWebViewSrc() const override;

 private:
  std::unique_ptr<extensions::ScriptExecutor> script_executor_;

  // Id of tab which executes code.
  int execute_tab_id_;
};

class TabsInsertCSSFunction : public ExecuteCodeInTabFunction {
 private:
  ~TabsInsertCSSFunction() override {}

  bool ShouldInsertCSS() const override;

  DECLARE_EXTENSION_FUNCTION("tabs.insertCSS", TABS_INSERTCSS)
};

}  // namespace neva

#endif  // NEVA_EXTENSIONS_BROWSER_API_TABS_TABS_API_H_
