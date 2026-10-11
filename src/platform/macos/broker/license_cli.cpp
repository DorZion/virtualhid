// SPDX-FileCopyrightText: 2026 LIZARDBYTE LLC
// SPDX-License-Identifier: LicenseRef-LizardByte-SAL-1.0

/**
 * @file src/platform/macos/broker/license_cli.cpp
 * @brief macOS command-line interface kept for license-command compatibility.
 */

#include <iostream>
#include <libvirtualhid/license.hpp>
#include <string_view>

namespace {

  void print_status(const lvh::LicenseResult &result) {
    const auto &license = result.license;
    std::cout << license.message << '\n';
    if (!license.plan_name.empty()) {
      std::cout << "Plan: " << license.plan_name << '\n';
    }
    std::cout << "Active gamepads: " << license.active_devices << '\n';
  }

}  // namespace

int main(int argc, char **argv) {
  if (argc > 3) {
    std::cerr << "Usage: libvirtualhid-license [status|activate [machine-name]|validate|deactivate]\n";
    return 2;
  }
  const auto action = argc > 1 ? std::string_view {argv[1]} : std::string_view {"status"};
  lvh::LicenseResult result;
  if (action == "status" && argc <= 2) {
    result = lvh::get_license_status();
  } else if (action == "activate") {
    // No key is needed; the command stays for scripts that still call it.
    result = lvh::activate_license("", argc == 3 ? argv[2] : "");
  } else if (action == "validate" && argc == 2) {
    result = lvh::validate_license();
  } else if (action == "deactivate" && argc == 2) {
    result = lvh::deactivate_license();
  } else {
    std::cerr << "Usage: libvirtualhid-license [status|activate [machine-name]|validate|deactivate]\n";
    return 2;
  }
  print_status(result);
  return 0;
}
