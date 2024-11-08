// Copyright 2024 LG Electronics, Inc.
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

#ifndef NEVA_APP_RUNTIME_BROWSER_MEDIA_MEDIA_CAPTURE_OBSERVER_H_
#define NEVA_APP_RUNTIME_BROWSER_MEDIA_MEDIA_CAPTURE_OBSERVER_H_

#include "neva/app_runtime/browser/media/webrtc/media_stream_capture_indicator.h"
#include "neva/browser_service/browser/mediacapture_service_impl.h"

// Provides events related to Media capture to UI.
class MediaStreamCaptureIndicator;

class MediaCaptureObserver
    : public neva_app_runtime::MediaStreamCaptureIndicator::Observer {
 public:
  enum CaptureType { kAudio = 0, kVideo, kWindow, kDisplay };

  MediaCaptureObserver();
  ~MediaCaptureObserver() override;

  // MediaStreamCaptureIndicator::Observer
  void OnIsCapturingAudioChanged(content::WebContents* web_contents,
                                 bool is_capturing_audio) override;
  void OnIsCapturingVideoChanged(content::WebContents* web_contents,
                                 bool is_capturing_video) override;
  void OnIsCapturingWindowChanged(content::WebContents* web_contents,
                                  bool is_capturing_window) override;
  void OnIsCapturingDisplayChanged(content::WebContents* web_contents,
                                   bool is_capturing_display) override;

 private:
  MediaCaptureObserver(const MediaCaptureObserver&) = delete;
  MediaCaptureObserver& operator=(const MediaCaptureObserver&) = delete;
};

#endif  // NEVA_APP_RUNTIME_BROWSER_MEDIA_MEDIA_CAPTURE_OBSERVER_H_
