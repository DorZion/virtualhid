/**
 * @file tests/fixtures/windows_broker_service_test_hooks.cpp
 * @brief Windows broker service test hook definitions.
 */

// local includes
#include "fixtures/windows_broker_service_test_hooks.hpp"

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif

// platform includes
#include <Windows.h>

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#define main libvirtualhid_broker_test_main
#include "../../src/platform/windows/broker/libvirtualhid_broker.cpp"
#undef main

namespace lvh::detail::test {

  BrokerLicenseStatusResult broker_license_status(std::uint64_t active_devices) {
    LvhWindowsBrokerLicenseStatus license {};
    lvh::detail::windows_broker_service::fill_unrestricted_license_status(
      license,
      static_cast<std::size_t>(active_devices)
    );
    return {
      .version = license.version,
      .size = license.size,
      .state = license.state,
      .active_devices = license.active_devices,
      .free_active_device_limit = license.free_active_device_limit,
      .activation_limit = license.activation_limit,
      .activation_usage = license.activation_usage,
      .plan_name = license.plan_name.data(),
      .customer_email = license.customer_email.data(),
      .message = license.message.data(),
    };
  }

  BrokerCleanupRetryResult broker_cleanup_retry_policy() {
    using namespace lvh::detail::windows_broker_service;
    std::vector<std::uint64_t> tracked_devices {1U, 2U};
    std::array<LvhWindowsDestroyDeviceRequest, 2> requests {
      make_destroy_device_request(1U, {}),
      make_destroy_device_request(2U, {}),
    };
    auto attempts = std::uint32_t {};

    destroy_cleanup_candidates(
      requests,
      [&attempts](const auto &request) {
        ++attempts;
        return request.driver_device_id == 2U;
      },
      [&tracked_devices](const auto &request) {
        std::erase(tracked_devices, request.driver_device_id);
      }
    );

    const auto failed_destruction_is_retained = std::ranges::contains(tracked_devices, 1U);
    destroy_cleanup_candidates(
      std::span {requests}.first(1U),
      [&attempts](const auto &) {
        ++attempts;
        return true;
      },
      [&tracked_devices](const auto &request) {
        std::erase(tracked_devices, request.driver_device_id);
      }
    );

    return {
      .failed_destruction_is_retained = failed_destruction_is_retained,
      .failed_destruction_is_retried = !std::ranges::contains(tracked_devices, 1U),
      .successful_destruction_is_forgotten = !std::ranges::contains(tracked_devices, 2U),
      .destruction_attempts = attempts,
    };
  }

}  // namespace lvh::detail::test
