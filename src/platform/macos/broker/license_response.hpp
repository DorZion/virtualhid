// SPDX-FileCopyrightText: 2026 LIZARDBYTE LLC
// SPDX-License-Identifier: LicenseRef-LizardByte-SAL-1.0

/**
 * @file src/platform/macos/broker/license_response.hpp
 * @brief Fixed macOS broker reply for retained licensing messages.
 */

#pragma once

#include "protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <libvirtualhid/license.hpp>
#include <limits>
#include <string_view>

namespace lvh::detail::macos_broker {

  /**
   * @brief Build the unrestricted reply for a status, activate, validate, or deactivate message.
   *
   * @param active_devices Number of devices the broker currently serves.
   * @return Successful licensing response for existing clients.
   */
  inline Message license_response(std::uint32_t active_devices) {
    constexpr std::string_view plan_name = "Unrestricted";
    constexpr std::string_view message = "Activated";
    Message response;
    response.type = MessageType::response;
    response.active_devices = active_devices;
    response.activation_limit = std::numeric_limits<std::uint32_t>::max();
    response.activation_usage = 1;
    response.license_state = static_cast<std::uint32_t>(lvh::LicenseState::licensed);
    std::ranges::copy(plan_name, response.plan_name.begin());
    std::ranges::copy(message, response.message.begin());
    return response;
  }

}  // namespace lvh::detail::macos_broker
