/**
 * @file src/include/libvirtualhid/license.hpp
 * @brief License compatibility API.
 *
 * Licensing is not enforced. These declarations remain for source and binary
 * compatibility, and every operation reports an activated, unrestricted license.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>

// local includes
#include <libvirtualhid/types.hpp>

namespace lvh {

  /**
   * @brief Compatibility license states. Current releases always report `licensed`.
   */
  enum class LicenseState {
    unavailable,  ///< Retained for compatibility; never reported by current releases.
    unlicensed,  ///< Retained for compatibility; never reported by current releases.
    licensed,  ///< Always reported by current releases.
    expired,  ///< Retained for compatibility; never reported by current releases.
    disabled,  ///< Retained for compatibility; never reported by current releases.
    invalid,  ///< Retained for compatibility; never reported by current releases.
  };

  /**
   * @brief Compatibility license details returned by the licensing API.
   */
  struct LicenseStatus {
    bool service_available = false;  ///< Always `true` in results from current releases; does not indicate a running broker.
    LicenseState state = LicenseState::unavailable;  ///< Always `LicenseState::licensed` in current results.
    std::uint32_t active_devices = 0;  ///< Always `0` from the public API; brokers may report their device count to older clients.
    std::uint32_t activation_limit = 0;  ///< Always `UINT32_MAX` in current results.
    std::uint32_t activation_usage = 0;  ///< Always `1` in current results.
    std::string plan_name;  ///< Always `Unrestricted` in current results.
    std::string customer_email;  ///< Always empty in current results.
    std::string message;  ///< Always `Activated` in current results.
    std::string purchase_url;  ///< Always empty in current results.
    std::string manage_account_url;  ///< Always empty in current results.

    /**
     * @brief Check whether the machine has an active license.
     *
     * @return `true` when the current state is licensed.
     */
    bool licensed() const {
      return state == LicenseState::licensed;
    }
  };

  /**
   * @brief Result returned by license service operations.
   */
  struct LicenseResult {
    OperationStatus status;  ///< Operation status, including transport and provider failures.
    LicenseStatus license;  ///< Latest license details returned by the service.

    /**
     * @brief Check whether the license operation succeeded.
     *
     * @return `true` when the operation completed successfully.
     */
    explicit operator bool() const {
      return status.ok();
    }
  };

  /**
   * @brief Report the compatibility license status.
   *
   * Always succeeds immediately on every platform without broker, network,
   * filesystem, or credential access. The result reports `LicenseState::licensed`,
   * `service_available = true` (the local compatibility API is available, not a
   * running broker), `active_devices = 0` (untracked here), an unrestricted
   * `activation_limit` of `UINT32_MAX`, `activation_usage = 1`, plan name
   * `"Unrestricted"`, and message `"Activated"`. Provider and customer fields are empty.
   *
   * @return Successful compatibility result.
   */
  LicenseResult get_license_status();

  /**
   * @brief Compatibility activation that always succeeds.
   *
   * Both arguments are ignored without being read, validated, logged, sent, or stored.
   * Any value, including an empty key, succeeds.
   *
   * @param license_key Ignored.
   * @param instance_name Ignored.
   * @return The same successful compatibility result as get_license_status().
   */
  LicenseResult activate_license(std::string_view license_key, std::string_view instance_name = {});

  /**
   * @brief Compatibility validation that always succeeds without contacting a provider.
   *
   * @return The same successful compatibility result as get_license_status().
   */
  LicenseResult validate_license();

  /**
   * @brief Compatibility deactivation that always succeeds.
   *
   * The effective state stays activated. No provider activation is released and
   * no stored data is changed.
   *
   * @return The same successful compatibility result as get_license_status().
   */
  LicenseResult deactivate_license();

}  // namespace lvh
