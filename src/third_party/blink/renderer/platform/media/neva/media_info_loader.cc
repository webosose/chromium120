// Copyright 2016-2018 LG Electronics, Inc.
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

#include "third_party/blink/renderer/platform/media/neva/media_info_loader.h"

#include "base/functional/callback_helpers.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/public/web/web_associated_url_loader.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader.h"

namespace blink {

static const int kHttpOK = 200;
static const int kHttpPartialContentOK = 206;

MediaInfoLoader::MediaInfoLoader(const GURL& url, const ReadyCB& ready_cb)
    : url_(url), ready_cb_(ready_cb) {}

MediaInfoLoader::~MediaInfoLoader() {}

void MediaInfoLoader::Start(WebLocalFrame* frame) {
  WebURLRequest request(url_);
  request.SetRequestContext(mojom::RequestContextType::VIDEO);
  frame->SetReferrerForRequest(request, WebURL());

  // To avoid downloading the data use two byte range
  request.AddHttpHeaderField("Range", "bytes=0-1");

  WebAssociatedURLLoaderOptions options;
  options.expose_all_response_headers = true;
  options.preflight_policy =
      network::mojom::CorsPreflightPolicy::kPreventPreflight;

  std::unique_ptr<WebAssociatedURLLoader> loader(
      frame->CreateAssociatedURLLoader(options));
  loader->LoadAsynchronously(request, this);
  active_loader_ = std::move(loader);
}

/////////////////////////////////////////////////////////////////////////////
// WebURLLoaderClient implementation.
bool MediaInfoLoader::WillFollowRedirect(
    const WebURL& new_url,
    const WebURLResponse& redirectResponse) {
  if (ready_cb_.is_null()) {
    return false;
  }

  url_ = new_url;
  return true;
}

void MediaInfoLoader::DidReceiveResponse(const WebURLResponse& response) {
  if (!url_.SchemeIs(url::kHttpScheme) && !url_.SchemeIs(url::kHttpsScheme)) {
    DidBecomeReady(true);
    return;
  }
  if (response.HttpStatusCode() == kHttpOK ||
      response.HttpStatusCode() == kHttpPartialContentOK) {
    DidBecomeReady(true);
    return;
  }
  DidBecomeReady(false);
}

void MediaInfoLoader::DidFinishLoading() {
  DidBecomeReady(true);
}

void MediaInfoLoader::DidFail(const WebURLError& error) {
  DidBecomeReady(false);
}

void MediaInfoLoader::DidBecomeReady(bool ok) {
  active_loader_.reset();
  if (!ready_cb_.is_null())
    std::move(ready_cb_).Run(ok, url_);
}

}  // namespace blink
