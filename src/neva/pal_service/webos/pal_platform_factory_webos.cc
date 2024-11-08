// Copyright 2019 LG Electronics, Inc.
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

#include "neva/pal_service/pal_platform_factory.h"

#include <memory>

#include "neva/pal_service/os_crypt_delegate.h"
#include "neva/pal_service/webos/application_registrator_delegate_webos.h"
#include "neva/pal_service/webos/external_protocol_handler_delegate_webos.h"
#include "neva/pal_service/webos/language_tracker_delegate_webos.h"
#include "neva/pal_service/webos/memorymanager_delegate_webos.h"
#include "neva/pal_service/webos/network_error_page_controller_delegate_webos.h"
#include "neva/pal_service/webos/notification_manager_delegate_webos.h"
#include "neva/pal_service/webos/platform_system_delegate_webos.h"
#include "neva/pal_service/webos/proxy_setting_delegate_webos.h"
#include "neva/pal_service/webos/system_servicebridge_delegate_webos.h"
#if defined(ENABLE_PWA_MANAGER_WEBAPI)
#include "neva/pal_service/webos/webapp_browsernavigation_delegate_webos.h"
#include "neva/pal_service/webos/webapp_installable_delegate_webos.h"
#endif  // ENABLE_PWA_MANAGER_WEBAPI
namespace pal {

std::unique_ptr<ApplicationRegistratorDelegate>
PlatformFactory::CreateApplicationRegistratorDelegate(
    const std::string& application_id,
    const std::string& application_name,
    ApplicationRegistratorDelegate::RepeatingResponse callback) {
  return std::make_unique<webos::ApplicationRegistratorDelegateWebOS>(
      application_id, application_name, std::move(callback));
}

std::unique_ptr<LanguageTrackerDelegate>
PlatformFactory::CreateLanguageTrackerDelegate(
    const std::string& application_name,
    LanguageTrackerDelegate::RepeatingResponse callback) {
  return std::make_unique<webos::LanguageTrackerDelegateWebOS>(
      application_name, std::move(callback));
}

scoped_refptr<ProxySettingDelegate>
PlatformFactory::CreateProxySettingDelegate() {
  return base::MakeRefCounted<webos::ProxySettingDelegateWebos>();
}

std::unique_ptr<MemoryManagerDelegate>
PlatformFactory::CreateMemoryManagerDelegate() {
  return std::unique_ptr<MemoryManagerDelegate>(
      webos::MemoryManagerDelegateWebOS::Create());
}

std::unique_ptr<OSCryptDelegate> PlatformFactory::CreateOSCryptDelegate() {
  return std::unique_ptr<OSCryptDelegate>();
}

std::unique_ptr<SystemServiceBridgeDelegate>
PlatformFactory::CreateSystemServiceBridgeDelegate(
    SystemServiceBridgeDelegate::CreationParams params,
    SystemServiceBridgeDelegate::Response callback) {
  return std::make_unique<webos::SystemServiceBridgeDelegateWebOS>(
      std::move(params), std::move(callback));
}

std::unique_ptr<PlatformSystemDelegate>
PlatformFactory::CreatePlatformSystemDelegate() {
  return std::make_unique<webos::PlatformSystemDelegateWebOS>();
}

std::unique_ptr<NetworkErrorPageControllerDelegate>
PlatformFactory::CreateNetworkErrorPageControllerDelegate() {
  return std::make_unique<webos::NetworkErrorPageControllerDelegateWebOS>();
}

std::unique_ptr<NotificationManagerDelegate>
PlatformFactory::CreateNotificationManagerDelegate() {
  return std::make_unique<webos::NotificationManagerDelegateWebOS>();
}

std::unique_ptr<ExternalProtocolHandlerDelegate>
PlatformFactory::CreateExternalProtocolHandlerDelegate() {
  return std::make_unique<webos::ExternalProtocolHandlerDelegateWebOS>();
}

#if defined(ENABLE_PWA_MANAGER_WEBAPI)
std::unique_ptr<WebAppInstallableDelegate>
PlatformFactory::CreateWebAppInstallableDelegate() {
  return std::make_unique<webos::WebAppInstallableDelegateWebOS>();
}

std::unique_ptr<WebAppBrowserNavigationDelegate>
PlatformFactory::CreateWebAppBrowserNavigationDelegate() {
  return std::make_unique<webos::WebAppBrowserNavigationDelegateWebOS>();
}
#endif  // defined(ENABLE_PWA_MANAGER_WEBAPI)

}  // namespace pal
