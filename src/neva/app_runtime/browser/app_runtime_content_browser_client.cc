// Copyright 2016 LG Electronics, Inc.
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

#include "neva/app_runtime/browser/app_runtime_content_browser_client.h"

#include <limits>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/neva/base_switches.h"
#include "base/rand_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/system/sys_info.h"
#include "cc/base/switches_neva.h"
#include "components/custom_handlers/protocol_handler_registry.h"
#include "components/custom_handlers/protocol_handler_throttle.h"
#include "components/network_session_configurator/common/network_switches.h"
#include "components/os_crypt/sync/key_storage_config_linux.h"
#include "components/os_crypt/sync/os_crypt.h"
#include "components/viz/common/switches.h"
#include "content/browser/loader/file_url_loader_factory.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/client_certificate_delegate.h"
#include "content/public/browser/devtools_manager_delegate.h"
#include "content/public/browser/login_delegate.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/network_service_util.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/shared_cors_origin_access_list.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents_view_delegate.h"
#include "content/public/common/content_neva_switches.h"
#include "content/public/common/content_switches.h"
#include "crypto/crypto_buildflags.h"
#include "extensions/browser/extension_navigation_ui_data.h"
#include "extensions/browser/service_worker/service_worker_host.h"
#include "net/base/filename_util.h"
#include "net/ssl/client_cert_identity.h"
#include "net/ssl/client_cert_store.h"
#include "net/ssl/ssl_private_key.h"
#include "neva/app_runtime/app/app_runtime_main_delegate.h"
#include "neva/app_runtime/app/app_runtime_page_contents.h"
#include "neva/app_runtime/browser/app_runtime_browser_context.h"
#include "neva/app_runtime/browser/app_runtime_browser_main_parts.h"
#include "neva/app_runtime/browser/app_runtime_browser_switches.h"
#include "neva/app_runtime/browser/app_runtime_devtools_manager_delegate.h"
#include "neva/app_runtime/browser/app_runtime_web_contents_delegate.h"
#include "neva/app_runtime/browser/app_runtime_web_contents_view_delegate_creator.h"
#include "neva/app_runtime/browser/app_runtime_webview_controller_impl.h"
#include "neva/app_runtime/browser/app_runtime_webview_host_impl.h"
#include "neva/app_runtime/browser/custom_handlers/app_runtime_protocol_handler_registry_factory.h"
#include "neva/app_runtime/common/app_runtime_file_access_controller.h"
#include "neva/app_runtime/public/app_runtime_switches.h"
#include "neva/app_runtime/public/proxy_settings.h"
#include "neva/app_runtime/webview.h"
#include "neva/browser_service/browser/sitefilter_navigation_throttle.h"
#include "neva/pal_service/pal_platform_factory.h"
#include "neva/pal_service/public/external_protocol_handler_delegate.h"
#include "neva/pal_service/public/notification_manager_delegate.h"
#include "neva/user_agent/common/user_agent.h"
#include "sandbox/policy/switches.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/common/switches.h"
#include "ui/base/ui_base_neva_switches.h"

#if BUILDFLAG(USE_NSS_CERTS)
#include "net/ssl/client_cert_store_nss.h"
#endif

#if defined(USE_NEVA_CHROME_EXTENSIONS)
#include "content/public/common/content_switches.h"
#include "extensions/browser/api/messaging/messaging_api_message_filter.h"
#include "extensions/browser/api/web_request/web_request_api.h"
#include "extensions/browser/browser_context_keyed_api_factory.h"
#include "extensions/browser/extension_message_filter.h"
#include "extensions/browser/extension_navigation_throttle.h"
#include "extensions/browser/extension_protocols.h"
#include "extensions/browser/extension_service_worker_message_filter.h"
#include "extensions/browser/process_map.h"
#include "extensions/browser/url_loader_factory_manager.h"
#include "extensions/common/constants.h"
#include "extensions/common/switches.h"
#include "neva/extensions/browser/neva_extensions_service_impl.h"
#include "neva/extensions/browser/neva_extensions_services_manager_impl.h"
#include "neva/extensions/browser/web_contents_map.h"
#endif  // defined(USE_NEVA_CHROME_EXTENSIONS)

#if defined(ENABLE_PWA_MANAGER_WEBAPI)
#include "base/check.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "neva/pal_service/pal_platform_factory.h"
#include "third_party/re2/src/re2/re2.h"
#endif  // ENABLE_PWA_MANAGER_WEBAPI

namespace neva_app_runtime {

namespace {
const char kCacheStoreFile[] = "Cache";
const char kCookieStoreFile[] = "Cookies";
const int kDefaultDiskCacheSize = 16 * 1024 * 1024;  // default size is 16MB
}  // namespace

namespace {

bool GetConfiguredValueBySwitchName(const char switch_name[], double* value) {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (!command_line.HasSwitch(switch_name))
    return false;
  if (!base::StringToDouble(command_line.GetSwitchValueASCII(switch_name),
                            value))
    return false;
  return true;
}

// Skews |value| by +/- |percent|.
int64_t RandomizeByPercent(int64_t value, int percent) {
  double random_percent = (base::RandDouble() - 0.5) * percent * 2;
  return value + (value * (random_percent / 100.0));
}

absl::optional<storage::QuotaSettings> GetConfiguredQuotaSettings(
    const base::FilePath& partition_path) {
  int64_t total = base::SysInfo::AmountOfTotalDiskSpace(partition_path);
  const int kRandomizedPercentage = 10;
  const double kShouldRemainAvailableRatio = 0.1;  // 10%
  const double kMustRemainAvailableRatio = 0.01;   // 1%

  storage::QuotaSettings settings;
  double ratio;
  if (!GetConfiguredValueBySwitchName(kQuotaPoolSizeRatio, &ratio))
    return absl::optional<storage::QuotaSettings>();

  settings.pool_size =
      std::min(RandomizeByPercent(total, kRandomizedPercentage),
               static_cast<int64_t>(total * ratio));

  if (!GetConfiguredValueBySwitchName(kPerHostQuotaRatio, &ratio))
    return absl::optional<storage::QuotaSettings>();

  settings.per_storage_key_quota =
      std::min(RandomizeByPercent(total, kRandomizedPercentage),
               static_cast<int64_t>(settings.pool_size * ratio));
  settings.session_only_per_storage_key_quota = settings.per_storage_key_quota;
  settings.should_remain_available =
      static_cast<int64_t>(total * kShouldRemainAvailableRatio);
  settings.must_remain_available =
      static_cast<int64_t>(total * kMustRemainAvailableRatio);
  settings.refresh_interval = base::TimeDelta::Max();

  return absl::make_optional<storage::QuotaSettings>(std::move(settings));
}

}  // namespace

AppRuntimeContentBrowserClient::AppRuntimeContentBrowserClient()
#if defined(ENABLE_PWA_MANAGER_WEBAPI)
    : pal_browsernavigation_delegate_(
          pal::PlatformFactory::Get()->CreateWebAppBrowserNavigationDelegate())
#endif  // ENABLE_PWA_MANAGER_WEBAPI
{
}

AppRuntimeContentBrowserClient::~AppRuntimeContentBrowserClient() {}

void AppRuntimeContentBrowserClient::SetBrowserExtraParts(
    AppRuntimeBrowserMainExtraParts* browser_extra_parts) {
  browser_extra_parts_ = browser_extra_parts;
}

std::unique_ptr<content::BrowserMainParts>
AppRuntimeContentBrowserClient::CreateBrowserMainParts(
    bool /* is_integration_test */) {
  main_parts_ = new AppRuntimeBrowserMainParts();

  if (browser_extra_parts_)
    main_parts_->AddParts(browser_extra_parts_);

  return base::WrapUnique(main_parts_);
}

std::unique_ptr<content::WebContentsViewDelegate>
AppRuntimeContentBrowserClient::GetWebContentsViewDelegate(
    content::WebContents* web_contents) {
  return base::WrapUnique(
      CreateAppRuntimeWebContentsViewDelegate(web_contents));
}

void AppRuntimeContentBrowserClient::AllowCertificateError(
    content::WebContents* web_contents,
    int cert_error,
    const net::SSLInfo& ssl_info,
    const GURL& request_url,
    bool is_main_frame_request,
    bool strict_enforcement,
    base::OnceCallback<void(CertificateRequestResultType)> callback) {
  // HCAP requirements: For SSL Certificate error, follows the policy settings
  if (web_contents && web_contents->GetDelegate()) {
    AppRuntimeWebContentsDelegate* web_contents_delegate =
        static_cast<AppRuntimeWebContentsDelegate*>(web_contents->GetDelegate());

    switch (web_contents_delegate->GetSSLCertErrorPolicy()) {
      case SSL_CERT_ERROR_POLICY_IGNORE:
        std::move(callback).Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_CONTINUE);
        return;
      case SSL_CERT_ERROR_POLICY_DENY:
        std::move(callback).Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
        return;
      default:
        break;
    }
  }

  // A certificate error. The user doesn't really have a context for making the
  // right decision, so block the request hard, without adding info bar that
  // provides possibility to show the insecure content.
  std::move(callback).Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
}

bool AppRuntimeContentBrowserClient::ShouldEnableStrictSiteIsolation() {
  // TODO(neva): Temporarily disabled until we support site isolation.
  return false;
}

bool AppRuntimeContentBrowserClient::IsFileAccessAllowedFromNetwork() const {
  // If there is no delegate set up, keep original implementation (deny
  // access from network URI to local file resources). If there is a
  // delegate, then let it decide if file access is allowed for that
  // origin.
  return GetFileAccessController() != nullptr;
}

bool AppRuntimeContentBrowserClient::IsFileSchemeNavigationAllowed(
    const GURL& url,
    int render_frame_id,
    bool browser_initiated) {
  const AppRuntimeFileAccessController* file_access_controller =
      GetFileAccessController();

  // PC/webOS wam_demo case
  if (!file_access_controller) {
    if (browser_initiated) {
      // Check for the switch allowing browser process initiaited navigation
      return base::CommandLine::ForCurrentProcess()->HasSwitch(
          kAllowFileAccess);
    } else {
      // Proceed since it's covered by other permissions (e.g.
      // allow_universal_access_from_file_urls, allow_local_resource_load)
      return true;
    }
  }

  content::FrameTreeNode* frame_tree_node =
      content::FrameTreeNode::GloballyFindByID(render_frame_id);

  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(
          frame_tree_node->current_frame_host());

  // The following code appeared because of the necessity to somehow bypass
  // the problem of downcasting to the assumed type, which in case of
  // BrowserShell is not WebView. WebView implements WebContentsDelegate in
  // case of WAM or wam_demo. In case of BrowserShell PageContents is
  // WebContentsDelegate. If it is possible to get PageContents from
  // WebContents, it means that web_contents lives in PageContents and it
  // doesn't have WebView as delegate as soon as it doesn't know anything
  // about WebViewInfo yet.
  WebView* webview = PageContents::From(web_contents)
                   ? nullptr
                   : static_cast<WebView*>(web_contents->GetDelegate());

  // webOS WAM case (whitelisting)
  base::FilePath file_path;
  if (!net::FileURLToFilePath(url, &file_path))
    return false;

  neva_app_runtime::WebViewInfo web_view_info;
  if (webview)
    web_view_info = webview->GetWebViewDelegate()->GetWebViewInfo();

  return file_access_controller->IsAccessAllowed(file_path, web_view_info);
}

bool AppRuntimeContentBrowserClient::ShouldIsolateErrorPage(
    bool /* in_main_frame */) {
  // TODO(neva): Temporarily disabled until we support site isolation.
  return false;
}

void AppRuntimeContentBrowserClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line,
    int child_process_id) {
  command_line->AppendSwitchASCII(switches::kUseVizFMPWithTimeout, "0");
#if defined(OS_WEBOS)
  command_line->AppendSwitch(switches::kDisableQuic);
#endif

  // Append v8 snapshot path if exists
  auto iter = v8_snapshot_pathes_.find(child_process_id);
  if (iter != v8_snapshot_pathes_.end()) {
    command_line->AppendSwitchPath(switches::kV8SnapshotBlobPath,
                                   base::FilePath(iter->second));
    v8_snapshot_pathes_.erase(iter);
  }

  absl::optional<std::string> js_flags;
  const base::CommandLine& browser_command_line =
      *base::CommandLine::ForCurrentProcess();
  if (browser_command_line.HasSwitch(kWebOSJavaScriptFlags)) {
    js_flags = browser_command_line.GetSwitchValueASCII(kWebOSJavaScriptFlags);
  }
  // Append v8 extra flags if exists
  iter = v8_extra_flags_.find(child_process_id);
  if (iter != v8_extra_flags_.end()) {
    std::string extra_js_flags = iter->second;
    // If already has, append it also
    if (js_flags.has_value()) {
      (*js_flags).append(" ");
      (*js_flags).append(extra_js_flags);
    } else {
      js_flags = extra_js_flags;
    }
    v8_extra_flags_.erase(iter);
  }

  // Append native scroll related flags if native scroll is on by appinfo.json
  auto iter_ns = use_native_scroll_map_.find(child_process_id);
  if (iter_ns != use_native_scroll_map_.end()) {
    bool use_native_scroll = iter_ns->second;
    if (use_native_scroll) {
      // Enables EnableNativeScroll, which is only enabled when there is
      // 'useNativeScroll': true in appinfo.json. If this flag is enabled,
      if (!command_line->HasSwitch(cc::switches::kEnableWebOSNativeScroll))
        command_line->AppendSwitch(cc::switches::kEnableWebOSNativeScroll);

      // Enables SmoothScrolling, which is mandatory to enable
      // CSSOMSmoothScroll.
      if (!command_line->HasSwitch(switches::kEnableSmoothScrolling))
        command_line->AppendSwitch(switches::kEnableSmoothScrolling);

      // Enables PreferCompositingToLCDText. If this flag is enabled, Compositor
      // thread handles scrolling and disable LCD-text(AntiAliasing) in the
      // scroll area.
      // See PaintLayerScrollableArea.cpp::layerNeedsCompositingScrolling()
      if (!command_line->HasSwitch(
              blink::switches::kEnablePreferCompositingToLCDText))
        command_line->AppendSwitch(
            blink::switches::kEnablePreferCompositingToLCDText);

      // Sets kCustomMouseWheelGestureScrollDeltaOnWebOSNativeScroll.
      // If this value is provided from command line argument, then propagate
      // the value to render process. If not, initialize this flag as default
      // value.
      static const int kDefaultGestureScrollDistanceOnNativeScroll = 180;
      // We should find in browser's switch value.
      if (base::CommandLine::ForCurrentProcess()->HasSwitch(
              cc::switches::
                  kCustomMouseWheelGestureScrollDeltaOnWebOSNativeScroll)) {
        std::string propagated_value(
            base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
                cc::switches::
                    kCustomMouseWheelGestureScrollDeltaOnWebOSNativeScroll));
        command_line->AppendSwitchASCII(
            cc::switches::
                kCustomMouseWheelGestureScrollDeltaOnWebOSNativeScroll,
            propagated_value);
      } else {
        command_line->AppendSwitchASCII(
            cc::switches::
                kCustomMouseWheelGestureScrollDeltaOnWebOSNativeScroll,
            std::to_string(kDefaultGestureScrollDistanceOnNativeScroll));
      }
    }

    use_native_scroll_map_.erase(iter_ns);
  }

  if (js_flags.has_value()) {
    command_line->AppendSwitchASCII(blink::switches::kJavaScriptFlags,
                                    *js_flags);
  }

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kUseOzoneWaylandVkb))
    command_line->AppendSwitch(switches::kUseOzoneWaylandVkb);

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kOzoneWaylandUseXDGShell))
    command_line->AppendSwitch(switches::kOzoneWaylandUseXDGShell);

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableTimestampLogging))
    command_line->AppendSwitch(switches::kEnableTimestampLogging);

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableTickcountLogging))
    command_line->AppendSwitch(switches::kEnableTickcountLogging);

#if defined(USE_NEVA_CHROME_EXTENSIONS)
  std::string process_type =
      command_line->GetSwitchValueASCII(switches::kProcessType);
  if (process_type == switches::kRendererProcess) {
    content::RenderProcessHost* process =
        content::RenderProcessHost::FromID(child_process_id);
    AppRuntimeBrowserContext* browser_context =
        static_cast<AppRuntimeBrowserContext*>(process->GetBrowserContext());
    if (browser_context->ExtensionsAreAllowed() &&
        extensions::ProcessMap::Get(browser_context)
            ->Contains(process->GetID())) {
      command_line->AppendSwitch(extensions::switches::kExtensionProcess);
    }
  }
#endif  // defined(USE_NEVA_CHROME_EXTENSIONS)
}

void AppRuntimeContentBrowserClient::SetUseNativeScroll(
    int child_process_id,
    bool use_native_scroll) {
  use_native_scroll_map_.insert(
      std::pair<int, bool>(child_process_id, use_native_scroll));
}

void AppRuntimeContentBrowserClient::AppendExtraWebSocketHeader(
    const std::string& key,
    const std::string& value) {
  for (const auto& delegate : network_delegates_)
    delegate->SetWebSocketHeader(key, value);
}

void AppRuntimeContentBrowserClient::
    RegisterAssociatedInterfaceBindersForRenderFrameHost(
        content::RenderFrameHost& render_frame_host,
        blink::AssociatedInterfaceRegistry& associated_registry) {
  associated_registry.AddInterface<blink::mojom::AppRuntimeBlinkDelegate>(
      base::BindRepeating(
          [](content::RenderFrameHost* render_frame_host,
             mojo::PendingAssociatedReceiver<
                 blink::mojom::AppRuntimeBlinkDelegate> receiver) {
            neva_app_runtime::AppRuntimeWebViewHostImpl::
                BindAppRuntimeBlinkDelegate(std::move(receiver),
                                            render_frame_host);
          },
          &render_frame_host));
  associated_registry.AddInterface<
      mojom::AppRuntimeWebViewHost>(base::BindRepeating(
      [](content::RenderFrameHost* render_frame_host,
         mojo::PendingAssociatedReceiver<mojom::AppRuntimeWebViewHost>
             receiver) {
        neva_app_runtime::AppRuntimeWebViewHostImpl::BindAppRuntimeWebViewHost(
            std::move(receiver), render_frame_host);
      },
      &render_frame_host));
  associated_registry.AddInterface<mojom::AppRuntimeWebViewController>(
      base::BindRepeating(
          [](content::RenderFrameHost* render_frame_host,
             mojo::PendingAssociatedReceiver<mojom::AppRuntimeWebViewController>
                 receiver) {
            neva_app_runtime::AppRuntimeWebViewControllerImpl::
                BindAppRuntimeWebViewController(std::move(receiver),
                                                render_frame_host);
          },
          &render_frame_host));
#if defined(USE_NEVA_CHROME_EXTENSIONS)
  associated_registry.AddInterface<extensions::mojom::LocalFrameHost>(
      base::BindRepeating(
          [](content::RenderFrameHost* render_frame_host,
             mojo::PendingAssociatedReceiver<extensions::mojom::LocalFrameHost>
                 receiver) {
            extensions::ExtensionWebContentsObserver::BindLocalFrameHost(
                std::move(receiver), render_frame_host);
          },
          &render_frame_host));
#endif
}

std::unique_ptr<content::DevToolsManagerDelegate>
AppRuntimeContentBrowserClient::CreateDevToolsManagerDelegate() {
  return std::make_unique<AppRuntimeDevToolsManagerDelegate>();
}

void AppRuntimeContentBrowserClient::OverrideWebkitPrefs(
    content::WebContents* web_contents,
    blink::web_pref::WebPreferences* prefs) {
  content::WebContentsDelegate* delegate = web_contents->GetDelegate();
  if (delegate)
    delegate->OverrideWebkitPrefs(prefs);
}

bool AppRuntimeContentBrowserClient::HasQuotaSettings() const {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  return command_line.HasSwitch(kQuotaPoolSizeRatio) &&
         command_line.HasSwitch(kPerHostQuotaRatio);
}

void AppRuntimeContentBrowserClient::GetQuotaSettings(
    content::BrowserContext* context,
    content::StoragePartition* partition,
    storage::OptionalQuotaSettingsCallback callback) const {
  absl::optional<storage::QuotaSettings> quota_settings;
  if ((quota_settings = GetConfiguredQuotaSettings(partition->GetPath())) &&
      quota_settings.has_value()) {
    const int64_t kMBytes = 1024 * 1024;
    LOG(INFO) << "QuotaSettings pool_size: "
              << quota_settings->pool_size / kMBytes << "MB"
              << ", shoud_remain_available: "
              << quota_settings->should_remain_available / kMBytes << "MB"
              << ", must_remain_available: "
              << quota_settings->must_remain_available / kMBytes << "MB"
              << ", per_storage_key_quota: "
              << quota_settings->per_storage_key_quota / kMBytes << "MB"
              << ", session_only_per_storage_key_quota: "
              << quota_settings->session_only_per_storage_key_quota / kMBytes
              << "MB";

    std::move(callback).Run(*quota_settings);
    return;
  }

  LOG(ERROR) << __func__
             << "(), usage of default quota settings instead of configured one";
  storage::GetNominalDynamicSettings(
      partition->GetPath(), context->IsOffTheRecord(),
      storage::GetDefaultDeviceInfoHelper(), std::move(callback));
}

content::GeneratedCodeCacheSettings
AppRuntimeContentBrowserClient::GetGeneratedCodeCacheSettings(
    content::BrowserContext* context) {
  return content::GeneratedCodeCacheSettings(true, 0, context->GetPath());
}

void AppRuntimeContentBrowserClient::
    RegisterNonNetworkServiceWorkerUpdateURLLoaderFactories(
        BrowserContext* browser_context,
        NonNetworkURLLoaderFactoryMap* factories) {
  if (browser_context) {
    factories->emplace(url::kFileScheme,
                       content::FileURLLoaderFactory::Create(
                           browser_context->GetPath(),
                           browser_context->GetSharedCorsOriginAccessList(),
                           base::TaskPriority::USER_VISIBLE));
  }

#if defined(USE_NEVA_CHROME_EXTENSIONS)
  factories->emplace(
      extensions::kExtensionScheme,
      extensions::CreateExtensionServiceWorkerScriptURLLoaderFactory(
          browser_context));
#endif
}

void AppRuntimeContentBrowserClient::
    RegisterNonNetworkSubresourceURLLoaderFactories(
        int render_process_id,
        int render_frame_id,
        const absl::optional<url::Origin>& request_initiator_origin,
        NonNetworkURLLoaderFactoryMap* factories) {
  content::RenderProcessHost* process =
      RenderProcessHost::FromID(render_process_id);
  if (process) {
    content::BrowserContext* browser_context = process->GetBrowserContext();
    if (browser_context) {
      factories->emplace(url::kFileScheme,
                         content::FileURLLoaderFactory::Create(
                             browser_context->GetPath(),
                             browser_context->GetSharedCorsOriginAccessList(),
                             base::TaskPriority::USER_VISIBLE));
    }
  }

#if defined(USE_NEVA_CHROME_EXTENSIONS)
  factories->emplace(extensions::kExtensionScheme,
                     extensions::CreateExtensionURLLoaderFactory(
                         render_process_id, render_frame_id));
#endif
}

#if defined(USE_NEVA_CHROME_EXTENSIONS)
// TODO(pikulik): I think it makes sense to take into account that we can
// have more than one default BrowserContext.

void AppRuntimeContentBrowserClient::RenderProcessWillLaunch(
    content::RenderProcessHost* host) {
  int render_process_id = host->GetID();
  BrowserContext* browser_context = host->GetBrowserContext();
  host->AddFilter(new extensions::ExtensionMessageFilter(render_process_id,
                                                         browser_context));
  host->AddFilter(new extensions::MessagingAPIMessageFilter(render_process_id,
                                                            browser_context));
  host->AddFilter(new extensions::ExtensionServiceWorkerMessageFilter(
      render_process_id, browser_context,
      host->GetStoragePartition()->GetServiceWorkerContext()));
}

void AppRuntimeContentBrowserClient::SiteInstanceGotProcess(
    content::SiteInstance* site_instance) {
  BrowserContext* browser_context = site_instance->GetBrowserContext();
  extensions::ExtensionRegistry* registry =
      extensions::ExtensionRegistry::Get(browser_context);
  const extensions::Extension* extension =
      registry->enabled_extensions().GetExtensionOrAppByURL(
          site_instance->GetSiteURL());

  // If this isn't an extension renderer there's nothing to do.
  if (!extension)
    return;

  extensions::ProcessMap::Get(browser_context)
      ->Insert(extension->id(), site_instance->GetProcess()->GetID());
}

void AppRuntimeContentBrowserClient::OnWebContentsCreated(
    content::WebContents* web_contents) {
  neva::WebContentsMap::GetInstance()->OnWebContentsCreated(web_contents);
}

void AppRuntimeContentBrowserClient::ExposeInterfacesToRenderer(
    service_manager::BinderRegistry* registry,
    blink::AssociatedInterfaceRegistry* associated_registry,
    content::RenderProcessHost* render_process_host) {
  registry->AddInterface<neva::mojom::NevaExtensionsServicesManager>(
      base::BindRepeating(
          &neva::NevaExtensionsServicesManagerImpl::BindForRenderer,
          render_process_host->GetID()),
      content::GetUIThreadTaskRunner({}));

  associated_registry->AddInterface<extensions::mojom::EventRouter>(
      base::BindRepeating(&extensions::EventRouter::BindForRenderer,
                          render_process_host->GetID()));
  associated_registry->AddInterface<extensions::mojom::ServiceWorkerHost>(
      base::BindRepeating(&extensions::ServiceWorkerHost::BindReceiver,
                          render_process_host->GetID()));
}

void AppRuntimeContentBrowserClient::OverrideURLLoaderFactoryParams(
    content::BrowserContext* browser_context,
    const url::Origin& origin,
    bool is_for_isolated_world,
    network::mojom::URLLoaderFactoryParams* factory_params) {
  extensions::URLLoaderFactoryManager::OverrideURLLoaderFactoryParams(
      browser_context, origin, is_for_isolated_world, factory_params);
}

void AppRuntimeContentBrowserClient::
    RegisterNonNetworkNavigationURLLoaderFactories(
        int frame_tree_node_id,
        ukm::SourceIdObj ukm_source_id,
        NonNetworkURLLoaderFactoryMap* factories) {
  DCHECK(factories);

  content::WebContents* web_contents =
      content::WebContents::FromFrameTreeNodeId(frame_tree_node_id);
  factories->emplace(
      extensions::kExtensionScheme,
      extensions::CreateExtensionNavigationURLLoaderFactory(
          web_contents->GetBrowserContext(), ukm_source_id, false));
}

void AppRuntimeContentBrowserClient::
    RegisterNonNetworkWorkerMainResourceURLLoaderFactories(
        content::BrowserContext* browser_context,
        NonNetworkURLLoaderFactoryMap* factories) {
  DCHECK(browser_context);
  DCHECK(factories);

  factories->emplace(
      extensions::kExtensionScheme,
      extensions::CreateExtensionWorkerMainResourceURLLoaderFactory(
          browser_context));
}

bool AppRuntimeContentBrowserClient::ShouldSendOutermostOriginToRenderer(
    const url::Origin& outermost_origin) {
  // From ChromeContentBrowserClient::ShouldSendOutermostOriginToRenderer():
  //
  // We only want to send the outermost origin if it is an extension scheme.
  // We do not send the outermost origin to every renderer to avoid leaking
  // additional information into the renderer about the embedder. For
  // extensions though this is required for the way content injection API
  // works. We do not want one extension injecting content into the context
  // of another extension.
  return outermost_origin.scheme() == extensions::kExtensionScheme;
}

bool AppRuntimeContentBrowserClient::DoesSiteRequireDedicatedProcess(
    content::BrowserContext* browser_context,
    const GURL& effective_site_url) {
  const extensions::Extension* extension =
      extensions::ExtensionRegistry::Get(browser_context)
          ->enabled_extensions()
          .GetExtensionOrAppByURL(effective_site_url);
  // Isolate all extensions.
  return extension != nullptr;
}

bool AppRuntimeContentBrowserClient::ShouldSwapBrowsingInstancesForNavigation(
    content::SiteInstance* site_instance,
    const GURL& current_effective_url,
    const GURL& destination_effective_url) {
  // This logic is to cover the case for loading the extension from
  // the initial empty document.
  // See http://clm.lge.com/issue/browse/NEVA-8058 for details.
  if (current_effective_url.is_empty() &&
      destination_effective_url.SchemeIs("chrome-extension")) {
    LOG(INFO) << __func__
              << "(), The extension is loaded from an empty document.";
    return true;
  }
  return false;
}
#endif  // defined(USE_NEVA_CHROME_EXTENSIONS)

bool AppRuntimeContentBrowserClient::WillCreateURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    int render_process_id,
    URLLoaderFactoryType type,
    const url::Origin& request_initiator,
    absl::optional<int64_t> navigation_id,
    ukm::SourceIdObj ukm_source_id,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory>* factory_receiver,
    mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
        header_client,
    bool* bypass_redirect_checks,
    bool* disable_secure_dns,
    network::mojom::URLLoaderFactoryOverridePtr* factory_override,
    scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner) {
#if defined(USE_NEVA_CHROME_EXTENSIONS)
  auto* web_request_api =
      extensions::BrowserContextKeyedAPIFactory<extensions::WebRequestAPI>::Get(
          browser_context);
  bool use_proxy = web_request_api->MaybeProxyURLLoaderFactory(
      browser_context, frame, render_process_id, type, navigation_id,
      ukm_source_id, factory_receiver, header_client,
      std::move(navigation_response_task_runner));
  if (bypass_redirect_checks)
    *bypass_redirect_checks = use_proxy;
  if (use_proxy)
    return use_proxy;
#endif  // defined(USE_NEVA_CHROME_EXTENSIONS)

  // Create ProxyURL factory
  auto proxied_receiver = std::move(*factory_receiver);
  mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory_remote;
  *factory_receiver = target_factory_remote.InitWithNewPipeAndPassReceiver();

  // To implement the proxying factory we rely on the
  // WebRequestProxyingURLLoaderFactory implementation in extensions and
  // ProxyingURLLoaderFactory in ElectronJS. Both of them use extension:: code.
  // We don't have in AppRuntime any navigation data like tabid or windowid.
  // But extensions::ExtensionNavigationUIData are required by
  // WebRequestInfoInitParams. In our case it's used just like a stub.
  std::unique_ptr<extensions::ExtensionNavigationUIData> navigation_ui_data;
  if (navigation_id.has_value()) {
    navigation_ui_data =
        std::make_unique<extensions::ExtensionNavigationUIData>();
  }

  mojo::PendingReceiver<network::mojom::TrustedURLLoaderHeaderClient>
      header_client_receiver;
  if (header_client)
    header_client_receiver = header_client->InitWithNewPipeAndPassReceiver();

  new AppRuntimeProxyingURLLoaderFactory(
      neva_app_runtime::AppRuntimeWebRequestHandler::From(browser_context),
      render_process_id, frame ? frame->GetRoutingID() : MSG_ROUTING_NONE,
      &url_factory_next_id_, std::move(navigation_ui_data), std::move(navigation_id),
      std::move(proxied_receiver), std::move(target_factory_remote),
      std::move(header_client_receiver), type);

  return true;
}

void AppRuntimeContentBrowserClient::GetAdditionalAllowedSchemesForFileSystem(
    std::vector<std::string>* additional_schemes) {
  ContentBrowserClient::GetAdditionalAllowedSchemesForFileSystem(
      additional_schemes);
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableFileAPIDirectoriesAndSystem))
    additional_schemes->push_back(url::kFileScheme);

#if defined(USE_NEVA_CHROME_EXTENSIONS)
  additional_schemes->push_back(extensions::kExtensionScheme);
#endif  // defined(USE_NEVA_CHROME_EXTENSIONS)
}

std::unique_ptr<content::LoginDelegate>
AppRuntimeContentBrowserClient::CreateLoginDelegate(
    const net::AuthChallengeInfo& auth_info,
    content::WebContents* web_contents,
    const content::GlobalRequestID& request_id,
    bool is_request_for_main_frame,
    const GURL& url,
    scoped_refptr<net::HttpResponseHeaders> response_headers,
    bool first_auth_attempt,
    LoginAuthRequiredCallback auth_required_callback) {
  if (!auth_required_callback.is_null() && !credentials_.Empty()) {
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(auth_required_callback), credentials_));
    return std::make_unique<content::LoginDelegate>();
  }
  return nullptr;
}

bool AppRuntimeContentBrowserClient::HandleExternalProtocol(
    const GURL& url,
    content::WebContents::Getter web_contents_getter,
    int frame_tree_node_id,
    content::NavigationUIData* navigation_data,
    bool is_primary_main_frame,
    bool is_in_fenced_frame_tree,
    network::mojom::WebSandboxFlags sandbox_flags,
    ui::PageTransition page_transition,
    bool has_user_gesture,
    const absl::optional<url::Origin>& initiating_origin,
    content::RenderFrameHost* initiator_document,
    mojo::PendingRemote<network::mojom::URLLoaderFactory>* out_factory) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableExternalProtocolsHandling)) {
    content::WebContents* web_contents = web_contents_getter.Run();

    if (web_contents && HasCustomSchemeHandler(
                            web_contents->GetBrowserContext(), url.scheme())) {
      auto* protocol_handler_registry =
          AppRuntimeProtocolHandlerRegistryFactory::GetForBrowserContext(
              web_contents->GetBrowserContext());

      if (!external_protocol_handler_delegate_) {
        external_protocol_handler_delegate_ =
            pal::PlatformFactory::Get()
                ->CreateExternalProtocolHandlerDelegate();
      }
      if (protocol_handler_registry && external_protocol_handler_delegate_) {
        GURL translated_url = protocol_handler_registry->Translate(url);
        if (!translated_url.is_empty()) {
          external_protocol_handler_delegate_->HandleExternalProtocol(
              web_contents->GetMutableRendererPrefs()->application_id,
              translated_url.spec());
          return true;
        }
      }
    }
  }

  if (url.SchemeIs(url::kMailToScheme)) {
    if (base::CommandLine::ForCurrentProcess()->HasSwitch(
            switches::kEnableNotificationForUnsupportedFeatures)) {
      if (!notification_manager_delegate_) {
        notification_manager_delegate_ =
            pal::PlatformFactory::Get()->CreateNotificationManagerDelegate();
      }

      if (notification_manager_delegate_) {
        content::WebContents* web_contents = web_contents_getter.Run();
        if (web_contents) {
          notification_manager_delegate_->CreateToast(
              web_contents->GetMutableRendererPrefs()->application_id,
              "The mailto protocol is not supported.");
        }
      }
    }
  }
  return true;
}

base::OnceClosure AppRuntimeContentBrowserClient::SelectClientCertificate(
    content::BrowserContext* browser_context,
    content::WebContents* web_contents,
    net::SSLCertRequestInfo* cert_request_info,
    net::ClientCertIdentityList client_certs,
    std::unique_ptr<content::ClientCertificateDelegate> delegate) {
  for (size_t i = 0; i < client_certs.size(); ++i) {
#if defined(OS_WEBOS)
    std::string issuer =
        client_certs[i]->certificate()->issuer().GetDisplayName();
    if (client_certs.size() == 1 ||
        issuer.find("webOS") != std::string::npos) {
#else
    if (client_certs.size() == 1) {
#endif
      // The callback will own |auto_selected_identity| and |delegate|, keeping
      // them alive until after ContinueWithCertificate is called.
      scoped_refptr<net::X509Certificate> cert =
          client_certs[i]->certificate();
      net::ClientCertIdentity::SelfOwningAcquirePrivateKey(
          std::move(client_certs[i]),
          base::BindOnce(
              &content::ClientCertificateDelegate::ContinueWithCertificate,
              std::move(delegate), std::move(cert)));
      return base::OnceClosure();
    }
  }
  return base::OnceClosure();
}

std::unique_ptr<net::ClientCertStore>
AppRuntimeContentBrowserClient::CreateClientCertStore(
    content::BrowserContext* resource_context) {
#if BUILDFLAG(USE_NSS_CERTS)
  return std::unique_ptr<net::ClientCertStore>(new net::ClientCertStoreNSS(
      net::ClientCertStoreNSS::PasswordDelegateFactory()));
#else
  return nullptr;
#endif
}

void AppRuntimeContentBrowserClient::SetV8SnapshotPath(
    int child_process_id,
    const std::string& path) {
  v8_snapshot_pathes_.insert(
      std::make_pair(child_process_id, path));
}

void AppRuntimeContentBrowserClient::SetV8ExtraFlags(int child_process_id,
                                                     const std::string& flags) {
  v8_extra_flags_.insert(std::make_pair(child_process_id, flags));
}

std::string AppRuntimeContentBrowserClient::GetUserAgent() {
  return neva_user_agent::GetDefaultUserAgent();
}

blink::UserAgentMetadata
AppRuntimeContentBrowserClient::GetUserAgentMetadata() {
  return neva_user_agent::GetDefaultUserAgentMetadata();
}

bool AppRuntimeContentBrowserClient::IsNevaDynamicProxyEnabled() {
  return false;
}

void AppRuntimeContentBrowserClient::OnNetworkServiceCreated(
    network::mojom::NetworkService* network_service) {
  if (IsNevaDynamicProxyEnabled() && !proxy_setting_delegate_) {
    proxy_setting_delegate_ =
        pal::PlatformFactory::Get()->CreateProxySettingDelegate();
    if (proxy_setting_delegate_)
      proxy_setting_delegate_->ObserveSystemProxySetting(this);
  }

#if defined(OS_WEBOS)
  network_service->DisableQuic();
#endif
  // The OSCrypt keys are process bound, so if network service is out of
  // process, send it the required key.
  if (content::IsOutOfProcessNetworkService()) {
    network_service->SetEncryptionKey(OSCrypt::GetRawEncryptionKey());
  }
}

void AppRuntimeContentBrowserClient::ConfigureNetworkContextParams(
    content::BrowserContext* context,
    bool in_memory,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* network_context_params,
    cert_verifier::mojom::CertVerifierCreationParams*
        cert_verifier_creation_params) {
  network_context_params->user_agent = GetUserAgent();
  network_context_params->accept_language = "en-us,en";
  network_context_params->file_paths =
      ::network::mojom::NetworkContextFilePaths::New();
  network_context_params->file_paths->data_directory = context->GetPath();
  network_context_params->file_paths->cookie_database_name =
      base::FilePath(kCookieStoreFile);
  network_context_params->enable_encrypted_cookies = true;

  {
    mojo::Remote<network::mojom::CustomProxyConfigClient>
        custom_proxy_config_client;
    network_context_params->custom_proxy_config_client_receiver =
        custom_proxy_config_client.BindNewPipeAndPassReceiver();
    custom_proxy_config_clients_.Add(std::move(custom_proxy_config_client));
    if (IsNevaDynamicProxyEnabled() && proxy_setting_delegate_)
      SetProxyServer(proxy_setting_delegate_->GetProxySetting());
  }

  {
    mojo::Remote<network::mojom::ExtraHeaderNetworkDelegate> network_delegate;
    network_context_params->network_delegate_receiver =
        network_delegate.BindNewPipeAndPassReceiver();
    network_delegates_.Add(std::move(network_delegate));
  }

  int disk_cache_size = kDefaultDiskCacheSize;
  base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (cmd_line->HasSwitch(kDiskCacheSize)) {
    if (!base::StringToInt(cmd_line->GetSwitchValueASCII(kDiskCacheSize),
                           &disk_cache_size) ||
        disk_cache_size < 0) {
      LOG(ERROR) << __func__ << " invalid value("
                 << cmd_line->GetSwitchValueASCII(kDiskCacheSize)
                 << ") for the command-line switch of --" << kDiskCacheSize;
      disk_cache_size = kDefaultDiskCacheSize;
    }
  }

  network_context_params->http_cache_max_size = disk_cache_size;
  network_context_params->file_paths->http_cache_directory =
      context->GetPath().Append(kCacheStoreFile);

  if (cmd_line->HasSwitch(kDisableModernCookieSameSite)) {
    if (!network_context_params->cookie_manager_params) {
      network_context_params->cookie_manager_params =
          network::mojom::CookieManagerParams::New();
    }
    network_context_params->cookie_manager_params->cookie_access_delegate_type =
        network::mojom::CookieAccessDelegateType::ALWAYS_LEGACY;
  }
}

void AppRuntimeContentBrowserClient::SetProxyServer(
    const content::ProxySettings& proxy_settings) {
  if (!custom_proxy_config_clients_.empty()) {
    if (proxy_settings.enabled) {
      credentials_ =
          net::AuthCredentials(base::UTF8ToUTF16(proxy_settings.username),
                               base::UTF8ToUTF16(proxy_settings.password));
      std::string proxy_string = proxy_settings.ip + ":" + proxy_settings.port;
      if (!proxy_settings.scheme.empty())
        proxy_string = proxy_settings.scheme + "://" + proxy_string;
      net::ProxyConfig::ProxyRules proxy_rules;
      proxy_rules.ParseFromString(proxy_string);

      std::string proxy_bypass_list = proxy_settings.bypass_list;
      // Merge given settings bypass list with one from command line.
      if (base::CommandLine::ForCurrentProcess()->HasSwitch(kProxyBypassList)) {
        std::string cmd_line_proxy_bypass_list =
            base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
                kProxyBypassList);
        if (!proxy_bypass_list.empty())
          proxy_bypass_list += ',';
        proxy_bypass_list += cmd_line_proxy_bypass_list;
      }

      if (!proxy_bypass_list.empty())
        proxy_rules.bypass_rules.ParseFromString(proxy_bypass_list);

      for (const auto& config_client : custom_proxy_config_clients_) {
        network::mojom::CustomProxyConfigPtr proxy_config =
            network::mojom::CustomProxyConfig::New();
        proxy_config->rules = proxy_rules;
        config_client->OnCustomProxyConfigUpdated(
            std::move(proxy_config), base::DoNothing());
      }
    } else {
      credentials_ = net::AuthCredentials();
    }
  }
}

// Implements a stub BadgeService. This implementation does nothing, but is
// required because inbound Mojo messages which do not have a registered
// handler are considered an error, and the render process is terminated.
// See https://crbug.com/1090429
class AppRuntimeContentBrowserClient::StubBadgeService
    : public blink::mojom::BadgeService {
 public:
  StubBadgeService() = default;
  StubBadgeService(const StubBadgeService&) = delete;
  StubBadgeService& operator=(const StubBadgeService&) = delete;
  ~StubBadgeService() override = default;

  void Bind(mojo::PendingReceiver<blink::mojom::BadgeService> receiver) {
    receivers_.Add(this, std::move(receiver));
  }

  // blink::mojom::BadgeService:
  void SetBadge(blink::mojom::BadgeValuePtr value) override {}
  void ClearBadge() override {}

 private:
  mojo::ReceiverSet<blink::mojom::BadgeService> receivers_;
};

void AppRuntimeContentBrowserClient::RegisterBrowserInterfaceBindersForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  map->Add<blink::mojom::BadgeService>(base::BindRepeating(
      &AppRuntimeContentBrowserClient::BindBadgeServiceForFrame,
      base::Unretained(this)));
}

void AppRuntimeContentBrowserClient::BindBadgeServiceForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<blink::mojom::BadgeService> receiver) {
  if (!stub_badge_service_)
    stub_badge_service_ = std::make_unique<StubBadgeService>();

  stub_badge_service_->Bind(std::move(receiver));
}

void AppRuntimeContentBrowserClient::SetCorsCorbDisabled(int process_id,
                                                         bool disabled) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!GetNetworkService()) {
    return;
  }

  if (disabled) {
    GetNetworkService()->AddCorsCorbExceptionForProcess(process_id);
  } else {
    GetNetworkService()->RemoveCorsCorbExceptionForProcess(process_id);
  }
}

void AppRuntimeContentBrowserClient::SetCorsCorbDisabledForURL(const GURL& url,
                                                               bool disabled) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (!GetNetworkService()) {
    return;
  }

  if (disabled) {
    GetNetworkService()->AddCorsCorbExceptionForURL(url);
  } else {
    GetNetworkService()->RemoveCorsCorbExceptionForURL(url);
  }
}

bool AppRuntimeContentBrowserClient::HasCustomSchemeHandler(
    content::BrowserContext* browser_context,
    const std::string& scheme) {
  if (custom_handlers::ProtocolHandlerRegistry* protocol_handler_registry =
          AppRuntimeProtocolHandlerRegistryFactory::GetForBrowserContext(
              browser_context)) {
    return protocol_handler_registry->IsHandledProtocol(scheme);
  }
  return false;
}

std::vector<std::unique_ptr<blink::URLLoaderThrottle>>
AppRuntimeContentBrowserClient::CreateURLLoaderThrottles(
    const network::ResourceRequest& request,
    content::BrowserContext* browser_context,
    const base::RepeatingCallback<content::WebContents*()>& wc_getter,
    content::NavigationUIData* navigation_ui_data,
    int frame_tree_node_id) {
  std::vector<std::unique_ptr<blink::URLLoaderThrottle>> result;
  return result;
}

#if defined(ENABLE_PWA_MANAGER_WEBAPI)
class PwaNavigationThrottle : public NavigationThrottle {
 public:
  explicit PwaNavigationThrottle(NavigationHandle* navigation_handle,
                                 const GURL& url,
                                 AppRuntimeContentBrowserClient* browser_client)
      : NavigationThrottle(navigation_handle),
        pwa_url_(url),
        browser_client_(browser_client) {
    CHECK(browser_client_);
  }
  ~PwaNavigationThrottle() override = default;

  ThrottleCheckResult WillStartRequest() override {
    int pid = navigation_handle()
                  ->GetWebContents()
                  ->GetPrimaryMainFrame()
                  ->GetProcess()
                  ->GetID();

    std::string permit_list;
    browser_client_->GetPwaExternalLinkPermitList(pid, permit_list);

    if (url::IsSameOriginWith(navigation_handle()->GetURL(), pwa_url_))
      return PROCEED;

    RenderFrameHost* render_frame_host =
        navigation_handle()->GetParentFrameOrOuterDocument();

    if (render_frame_host)
      return PROCEED;

    std::string navigation_url = navigation_handle()->GetURL().spec();

    if (!permit_list.empty()) {
      if (RE2::PartialMatch(navigation_url, permit_list))
        return PROCEED;
    }

    browser_client_->OpenUrlInBrowser(navigation_url);
    return CANCEL_AND_IGNORE;
  }

  const char* GetNameForLogging() override { return "PwaNavigationThrottle"; }

  static std::unique_ptr<NavigationThrottle> CreateThrottleForNavigation(
      NavigationHandle* navigation_handle,
      const GURL& url,
      AppRuntimeContentBrowserClient* browser_client) {
    return std::make_unique<PwaNavigationThrottle>(navigation_handle, url,
                                                   browser_client);
  }

  PwaNavigationThrottle(const PwaNavigationThrottle&) = delete;
  PwaNavigationThrottle& operator=(const PwaNavigationThrottle&) = delete;

 private:
  GURL pwa_url_;
  AppRuntimeContentBrowserClient* browser_client_;
};

void AppRuntimeContentBrowserClient::SetPwaAppOrigin(int child_process_id,
                                                     const GURL& url) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  pwa_origins_.emplace(child_process_id, url);
}

void AppRuntimeContentBrowserClient::RemovePwaAppOrigin(int child_process_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  pwa_origins_.erase(child_process_id);
}

void AppRuntimeContentBrowserClient::OpenUrlInBrowser(const std::string& url) {
  if (pal_browsernavigation_delegate_)
    pal_browsernavigation_delegate_->OpenUrlInBrowser(url);
}

void AppRuntimeContentBrowserClient::SetPwaExternalLinkPermitList(
    int child_process_id,
    const net::HttpResponseHeaders* headers) {
  if (pwa_externalLink_permit_list_.count(child_process_id))
    return;

  std::string permit_list_url;
  size_t iter = 0;
  std::string name, value;
  while (headers->EnumerateHeaderLines(&iter, &name, &value)) {
    if (name == "content-security-policy")
      break;
    value = "";
  }

  if (value.empty())
    return;

  for (const auto& directive : base::SplitStringPiece(
           value, ";", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    size_t pos = directive.find_first_of(base::kWhitespaceASCII);
    if (pos != std::string::npos) {
      auto name_field = directive.substr(0, pos);
      if (RE2::PartialMatch(name_field.begin(),
                            "frame-src|default-src|script-src")) {
        for (auto& ss : base::SplitString(directive.substr(pos + 1), " ",
                                          base::TRIM_WHITESPACE,
                                          base::SPLIT_WANT_NONEMPTY)) {
          if (!RE2::PartialMatch(ss, "blob|'|data")) {
            pos = ss.find("*");
            if (pos != std::string::npos)
              ss.insert(pos, ".");
            if (!permit_list_url.empty())
              permit_list_url += "|";
            permit_list_url += ss;
          }
        }
      }
    }
  }
  pwa_externalLink_permit_list_.emplace(child_process_id, permit_list_url);
}

void AppRuntimeContentBrowserClient::GetPwaExternalLinkPermitList(
    int child_process_id,
    std::string& list) const {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  list = "";
  if (pwa_externalLink_permit_list_.count(child_process_id))
    list = pwa_externalLink_permit_list_.at(child_process_id);
}

void AppRuntimeContentBrowserClient::RemoveExternalLinkPermitList(
    int child_process_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  pwa_externalLink_permit_list_.erase(child_process_id);
}
#endif  // ENABLE_PWA_MANAGER_WEBAPI

///@name USE_NEVA_CHROME_EXTENSIONS | ENABLE_PWA_MANAGER_WEBAPI
///@{
std::vector<std::unique_ptr<content::NavigationThrottle>>
AppRuntimeContentBrowserClient::CreateThrottlesForNavigation(
    content::NavigationHandle* navigation_handle) {
  auto throttles =
      ContentBrowserClient::CreateThrottlesForNavigation(navigation_handle);

#if defined(ENABLE_PWA_MANAGER_WEBAPI)
  int pid = navigation_handle->GetWebContents()
                ->GetPrimaryMainFrame()
                ->GetProcess()
                ->GetID();
  if (pwa_origins_.count(pid)) {
    throttles.push_back(PwaNavigationThrottle::CreateThrottleForNavigation(
        navigation_handle, pwa_origins_[pid], this));
    return throttles;
  }
#endif  // ENABLE_PWA_MANAGER_WEBAPI

#if defined(USE_NEVA_CHROME_EXTENSIONS)
  throttles.push_back(std::make_unique<extensions::ExtensionNavigationThrottle>(
      navigation_handle));
#endif  // defined(USE_NEVA_CHROME_EXTENSIONS)

  throttles.push_back(std::make_unique<neva::SiteFilterNavigationThrottle>(
      navigation_handle));

  return throttles;
}
///@}

size_t AppRuntimeContentBrowserClient::GetMaxRendererProcessCountOverride() {
  return std::numeric_limits<size_t>::max();
}

}  // namespace neva_app_runtime
