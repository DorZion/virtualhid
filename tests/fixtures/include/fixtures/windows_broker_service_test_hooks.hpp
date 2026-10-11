/**
 * @file tests/fixtures/include/fixtures/windows_broker_service_test_hooks.hpp
 * @brief Private Windows broker service test hooks.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>

namespace lvh::detail::test {

  /**
   * @brief License status fields reported by the broker's compatibility path.
   */
  struct BrokerLicenseStatusResult {
    std::uint32_t version = 0;  ///< Protocol version written into the status.
    std::uint32_t size = 0;  ///< Structure size written into the status.
    std::uint32_t state = 0;  ///< Raw license state value.
    std::uint32_t active_devices = 0;  ///< Reported active device count.
    std::uint32_t free_active_device_limit = 0;  ///< Reported free-device limit.
    std::uint32_t activation_limit = 0;  ///< Reported activation limit.
    std::uint32_t activation_usage = 0;  ///< Reported activation usage.
    std::string plan_name;  ///< Reported plan name.
    std::string customer_email;  ///< Reported customer email.
    std::string message;  ///< Reported status message.
  };

  /**
   * @brief Fill a broker license status the way the broker does.
   *
   * @param active_devices Device count the broker would report.
   * @return The fields written by the broker.
   */
  BrokerLicenseStatusResult broker_license_status(std::uint64_t active_devices);

  /**
   * @brief Observations from the broker's cleanup retry helper.
   */
  struct BrokerCleanupRetryResult {
    bool failed_destruction_is_retained = false;  ///< Failed destruction keeps the device tracked.
    bool failed_destruction_is_retried = false;  ///< A later pass retries the failed device.
    bool successful_destruction_is_forgotten = false;  ///< Successful destruction forgets the device.
    std::uint32_t destruction_attempts = 0;  ///< Total destruction attempts.
  };

  /**
   * @brief Exercise the broker's cleanup retry helper.
   *
   * @return Observed cleanup behavior.
   */
  BrokerCleanupRetryResult broker_cleanup_retry_policy();

}  // namespace lvh::detail::test
