/**
 * @file src/platform/license_unavailable.cpp
 * @brief Unconditional compatibility definitions for the public license API.
 */

// local includes
#include <libvirtualhid/license.hpp>

// standard includes
#include <cstdint>
#include <limits>

namespace lvh {
  namespace {

    /**
     * @brief Build the fixed compatibility result returned by every license operation.
     *
     * No broker, provider, filesystem, or credential access takes place.
     *
     * @return Successful result reporting an activated, unrestricted license.
     */
    LicenseResult activated_result() {
      LicenseResult result;
      result.license.service_available = true;
      result.license.state = LicenseState::licensed;
      result.license.active_devices = 0;
      result.license.activation_limit = std::numeric_limits<std::uint32_t>::max();
      result.license.activation_usage = 1;
      result.license.plan_name = "Unrestricted";
      result.license.message = "Activated";
      return result;
    }

  }  // namespace

  LicenseResult get_license_status() {
    return activated_result();
  }

  LicenseResult activate_license(std::string_view /*license_key*/, std::string_view /*instance_name*/) {
    return activated_result();
  }

  LicenseResult validate_license() {
    return activated_result();
  }

  LicenseResult deactivate_license() {
    return activated_result();
  }

}  // namespace lvh
