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

#ifndef NEVA_PAL_SERVICE_LUNA_LUNA_NAMES_H_
#define NEVA_PAL_SERVICE_LUNA_LUNA_NAMES_H_

#include <string>

namespace pal {
namespace luna {

namespace service_uri {

extern const char kAudio[];
extern const char kSettings[];
extern const char kMemoryManager[];
extern const char kPalmBus[];
extern const char kApplicationManager[];
extern const char kServiceMemoryManager[];
extern const char kNotification[];
extern const char kServiceBus[];
#if defined(ENABLE_PWA_MANAGER_WEBAPI)
extern const char kAppInstallService[];
#endif  // ENABLE_PWA_MANAGER_WEBAPI

}  // namespace service_uri

namespace service_name {

extern const char kApplicationManagerClient[];
#if defined(ENABLE_PWA_MANAGER_WEBAPI)
extern const char kChromiumInstallableManager[];
extern const char kChromiumPwa[];
#endif  // ENABLE_PWA_MANAGER_WEBAPI
extern const char kChromiumMedia[];
extern const char kChromiumMemory[];
extern const char kChromiumPlatformSystem[];
extern const char kNotificationClient[];
extern const char kSettingsClient[];

}  // namespace service_name

std::string GetServiceURI(const std::string& uri, const std::string& action);

std::string GetServiceName(const std::string& name,
                           int suffix,
                           const std::string& delimiter = ".");

std::string GetServiceNameWithRandSuffix(const std::string& name,
                                         const std::string& delimiter = ".");

std::string GetServiceNameWithPID(const std::string& name,
                                  const std::string& delimiter = ".");

}  // namespace luna
}  // namespace pal

#endif // NEVA_PAL_SERVICE_LUNA_LUNA_NAMES_H_
