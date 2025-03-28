// Copyright 2021 LG Electronics, Inc.
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

#include "neva/app_runtime/app/app_runtime_shell.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/render_process_host.h"
#include "neva/app_runtime/app/app_runtime_page_contents.h"
#include "neva/app_runtime/app/app_runtime_page_view.h"
#include "neva/app_runtime/app/app_runtime_shell_observer.h"
#include "neva/app_runtime/app/app_runtime_shell_window.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/rect.h"

namespace neva_app_runtime {

namespace {
base::NoDestructor<base::OnceClosure> g_quit_main_message_loop;
const int kDefaultStartingWindowWidth = 800;
const int kDefaultStartingWindowHeight = 600;
}  // namespace

std::set<ShellWindow*> Shell::windows_;

Shell::CreateParams::CreateParams() = default;

Shell::CreateParams::CreateParams(const CreateParams&) = default;

Shell::CreateParams&
Shell::CreateParams::operator=(const CreateParams&) = default;

Shell::CreateParams::~CreateParams() = default;

Shell::Shell(const CreateParams& params)
    : app_id_(params.app_id),
      display_id_(params.display_id),
      launch_params_(params.launch_params),
      user_agent_(params.user_agent),
      enable_dev_tools_(params.enable_dev_tools) {}

Shell::~Shell() = default;

void Shell::AddObserver(ShellObserver* observer) {
  observers_.AddObserver(observer);
}

void Shell::RemoveObserver(ShellObserver* observer) {
  observers_.RemoveObserver(observer);
}

ShellWindow* Shell::CreateMainWindow(std::string url,
                                     const std::vector<std::string>& injections,
                                     bool fullscreen) {
  if (main_window_)
    return main_window_;

  PageContents::CreateParams page_contents_params;
  page_contents_params.app_id = app_id_;
  page_contents_params.injections.emplace("v8/browser_shell", "{}");
  page_contents_params.injections.emplace("v8/cookiemanager", "{}");
  page_contents_params.injections.emplace("v8/customuseragent", "{}");
  page_contents_params.injections.emplace("v8/userpermission", "{}");
  page_contents_params.injections.emplace("v8/mediacapture", "{}");
  page_contents_params.injections.emplace("v8/popupblocker", "{}");
  page_contents_params.injections.emplace("v8/webosservicebridge", "{}");
  page_contents_params.injections.emplace("v8/sitefilter", "{}");
  for (const auto& item : injections)
    page_contents_params.injections.emplace(item, "{}");
  page_contents_params.inspectable = enable_dev_tools_;
  page_contents_params.active = true;
  page_contents_params.allow_file_access_from_file_urls = true;
  page_contents_params.allow_universal_access_from_file_urls = true;
  page_contents_params.default_access_to_media =
      base::JSONReader::Read(GetLaunchParams())
          ->GetDict()
          .FindBool("media-access");
  page_contents_params.user_agent = user_agent_;
  // Main page contents shouldn't be able to pinch-to-zoom
  page_contents_params.pinch_to_zoom_enabled = false;
  page_contents_params.type = PageContents::Type::kMain;

  auto page_contents = std::make_unique<PageContents>(page_contents_params);
  page_contents->LoadURL(std::move(url));
  auto page_view = std::make_unique<PageView>();
  page_view->SetPageContents(std::move(page_contents));

  ShellWindow::CreateParams window_params;
  window_params.width = kDefaultStartingWindowWidth;
  window_params.height = kDefaultStartingWindowHeight;
  window_params.frameless = fullscreen;
  window_params.app_id = app_id_;

  if (!display_id_.empty()) {
    window_params.display_id = display_id_;
  }

  if (fullscreen) {
    gfx::Rect display_rect =
        display::Screen::GetScreen()->GetPrimaryDisplay().bounds();
    window_params.width = display_rect.width();
    window_params.height = display_rect.height();
  }

  // Shell only creates ShellWindow.
  // After creation ShellWindow pass ownership to Widget.
  main_window_ = new ShellWindow(window_params);
  main_window_->AddObserver(this);
  main_window_->SetPageView(std::move(page_view));

  windows_.insert(main_window_);
  return main_window_;
}

ShellWindow* Shell::GetMainWindow() {
  return main_window_;
}

PageContents::CreateParams Shell::GetDefaultContentsParams() {
  PageContents::CreateParams nested_params;
  nested_params.app_id = app_id_;
  nested_params.inspectable = enable_dev_tools_;
  nested_params.type = PageContents::Type::kTab;
  nested_params.accepted_languages = GetAcceptedLanguages();
  nested_params.user_agent = user_agent_;
  return nested_params;
}

const std::string& Shell::GetLaunchParams() const {
  return launch_params_;
}

void Shell::OnWindowClosing(ShellWindow* window) {
  if (window == main_window_) {
    for (ShellObserver& observer : observers_)
      observer.OnMainWindowClosing();
    Shutdown();
  }
}

// static
void Shell::SetQuitClosure(base::OnceClosure quit_main_message_loop) {
  *g_quit_main_message_loop = std::move(quit_main_message_loop);
}

// static
void Shell::Shutdown() {
  static bool shutdown_called = false;
  if (shutdown_called)
    return;
  else
    shutdown_called = true;

  content::DevToolsAgentHost::DetachAllClients();

  for (auto* window : windows_)
    window->CloseNow();

  for (auto it = content::RenderProcessHost::AllHostsIterator(); !it.IsAtEnd();
       it.Advance()) {
    CHECK(it.GetCurrentValue());
    it.GetCurrentValue()->DisableRefCounts();
  }
  if (*g_quit_main_message_loop)
    std::move(*g_quit_main_message_loop).Run();

  // Pump the message loop to allow window teardown tasks to run.
  base::RunLoop().RunUntilIdle();
}

std::string Shell::GetAcceptedLanguages() const {
  auto* page_view = main_window_->GetPageView();
  if (page_view) {
    auto* page_contents = page_view->GetPageContents();
    if (page_contents)
      return page_contents->GetAcceptedLanguages();
  }

  return std::string();
}

}  // namespace neva_app_runtime
