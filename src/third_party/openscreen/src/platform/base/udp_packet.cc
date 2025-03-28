// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/base/udp_packet.h"

#include <cassert>
#include <sstream>

namespace openscreen {

UdpPacket::UdpPacket() : std::vector<uint8_t>() {}

UdpPacket::UdpPacket(size_type size, uint8_t fill_value)
    : std::vector<uint8_t>(size, fill_value) {
  assert(size <= kUdpMaxPacketSize);
}

UdpPacket::UdpPacket(UdpPacket&& other) noexcept = default;

UdpPacket::UdpPacket(std::initializer_list<uint8_t> init)
    : std::vector<uint8_t>(init) {
  assert(size() <= kUdpMaxPacketSize);
}

UdpPacket::~UdpPacket() = default;

UdpPacket& UdpPacket::operator=(UdpPacket&& other) = default;

// static
const UdpPacket::size_type UdpPacket::kUdpMaxPacketSize = 1 << 16;

}  // namespace openscreen
