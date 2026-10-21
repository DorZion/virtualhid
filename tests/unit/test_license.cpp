/**
 * @file tests/unit/test_license.cpp
 * @brief Tests for the provider-neutral license API.
 */

// test includes
#include <gtest/gtest.h>

// lib includes
#include <libvirtualhid/license.hpp>

// standard includes
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
  // local includes
  #include "platform/windows/windows_broker_client.hpp"
#endif

TEST(LicenseStatusTest, LicensedReflectsCurrentState) {
  lvh::LicenseStatus status;
  EXPECT_FALSE(status.licensed());

  status.state = lvh::LicenseState::licensed;
  EXPECT_TRUE(status.licensed());

  status.state = lvh::LicenseState::expired;
  EXPECT_FALSE(status.licensed());
}

namespace {

  void expect_activated(const lvh::LicenseResult &result) {
    EXPECT_TRUE(result);
    EXPECT_TRUE(result.status.ok());
    EXPECT_EQ(result.status.code(), lvh::ErrorCode::ok);
    EXPECT_TRUE(result.status.message().empty());
    EXPECT_TRUE(result.license.service_available);
    EXPECT_EQ(result.license.state, lvh::LicenseState::licensed);
    EXPECT_TRUE(result.license.licensed());
    EXPECT_EQ(result.license.active_devices, 0U);
    EXPECT_EQ(result.license.activation_limit, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(result.license.activation_usage, 1U);
    EXPECT_EQ(result.license.plan_name, "Unrestricted");
    EXPECT_TRUE(result.license.customer_email.empty());
    EXPECT_EQ(result.license.message, "Activated");
    EXPECT_TRUE(result.license.purchase_url.empty());
    EXPECT_TRUE(result.license.manage_account_url.empty());
  }

}  // namespace

TEST(LicenseStatusTest, DefaultConstructionIsUnchanged) {
  const lvh::LicenseResult result;
  EXPECT_FALSE(result.license.service_available);
  EXPECT_EQ(result.license.state, lvh::LicenseState::unavailable);
  EXPECT_EQ(result.license.activation_limit, 0U);
  EXPECT_EQ(result.license.activation_usage, 0U);
  EXPECT_TRUE(result.license.message.empty());
}

TEST(LicenseStatusTest, PreservesEnumValues) {
  EXPECT_EQ(static_cast<int>(lvh::LicenseState::unavailable), 0);
  EXPECT_EQ(static_cast<int>(lvh::LicenseState::unlicensed), 1);
  EXPECT_EQ(static_cast<int>(lvh::LicenseState::licensed), 2);
  EXPECT_EQ(static_cast<int>(lvh::LicenseState::expired), 3);
  EXPECT_EQ(static_cast<int>(lvh::LicenseState::disabled), 4);
  EXPECT_EQ(static_cast<int>(lvh::LicenseState::invalid), 5);
}

TEST(LicenseApiTest, EveryOperationReportsActivatedWithoutABroker) {
  expect_activated(lvh::get_license_status());
  expect_activated(lvh::activate_license("test-key"));
  expect_activated(lvh::activate_license("test-key", "machine"));
  expect_activated(lvh::validate_license());
  expect_activated(lvh::deactivate_license());
}

TEST(LicenseApiTest, AcceptsAnyLicenseKeyAndInstanceName) {
  using namespace std::string_view_literals;
  const std::string oversized(1U << 20U, 'k');
  const std::string arbitrary_bytes {'\xff', '\x00', '\x80', '\x01'};
  for (const auto key : {""sv, "   \t\n"sv, "in\0valid"sv, std::string_view {oversized}, std::string_view {arbitrary_bytes}}) {
    expect_activated(lvh::activate_license(key));
    expect_activated(lvh::activate_license(key, key));
  }
}

TEST(LicenseApiTest, DeactivationLeavesTheEffectiveStateActivated) {
  for (int i = 0; i < 3; ++i) {
    expect_activated(lvh::activate_license("key"));
    expect_activated(lvh::deactivate_license());
    expect_activated(lvh::get_license_status());
    expect_activated(lvh::validate_license());
  }
}

TEST(LicenseApiTest, ConcurrentCallersAllReportActivated) {
  std::vector<std::thread> threads;
  std::atomic<int> failures {0};
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&failures] {
      for (int j = 0; j < 100; ++j) {
        const auto results = {lvh::get_license_status(), lvh::activate_license("key"), lvh::validate_license(), lvh::deactivate_license()};
        for (const auto &result : results) {
          if (!result || !result.license.licensed() || result.license.message != "Activated") {
            ++failures;
          }
        }
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(failures.load(), 0);
}

#if defined(_WIN32)
TEST(WindowsBrokerClientTest, BuildsVersionedRequestHeader) {
  const auto header = lvh::detail::windows_broker::make_request_header(
    LvhWindowsBrokerRequestType::validate_license,
    sizeof(LvhWindowsBrokerLicenseRequest)
  );

  EXPECT_EQ(header.version, LVH_WINDOWS_BROKER_PROTOCOL_VERSION);
  EXPECT_EQ(header.size, sizeof(LvhWindowsBrokerLicenseRequest));
  EXPECT_EQ(header.type, static_cast<std::uint32_t>(LvhWindowsBrokerRequestType::validate_license));
  EXPECT_EQ(header.reserved0, 0U);
  EXPECT_EQ(LVH_WINDOWS_BROKER_PROTOCOL_VERSION, 5U);
}

TEST(WindowsBrokerClientTest, PreservesFixedWireLayout) {
  EXPECT_EQ(sizeof(LvhWindowsBrokerRequestHeader), 16U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerLicenseStatus), 796U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerStatusRequest), 16U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerStatusResponse), 1324U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerCreateDeviceRequest), 2528U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerCreateDeviceResponse), 1648U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerDestroyDeviceRequest), 64U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerDestroyDeviceResponse), 1324U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerLicenseRequest), 272U);
  EXPECT_EQ(sizeof(LvhWindowsBrokerLicenseResponse), 1324U);
}

TEST(WindowsBrokerClientTest, MapsLicenseAndTransportStatuses) {
  using lvh::detail::windows_broker::response_status;

  EXPECT_TRUE(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::success), "").ok());
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::invalid_argument), "bad").code(), lvh::ErrorCode::invalid_argument);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::unsupported_profile), "bad").code(), lvh::ErrorCode::unsupported_profile);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::device_not_found), "bad").code(), lvh::ErrorCode::device_closed);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::backend_unavailable), "bad").code(), lvh::ErrorCode::backend_unavailable);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::license_required), "bad").code(), lvh::ErrorCode::license_required);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::license_invalid), "bad").code(), lvh::ErrorCode::license_invalid);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::activation_limit_reached), "bad").code(), lvh::ErrorCode::activation_limit_reached);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::network_unavailable), "bad").code(), lvh::ErrorCode::network_unavailable);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::backend_failure), "bad").code(), lvh::ErrorCode::backend_failure);
  EXPECT_EQ(response_status(999U, "bad").code(), lvh::ErrorCode::backend_failure);
  EXPECT_EQ(response_status(static_cast<std::uint32_t>(LvhWindowsBrokerStatusCode::backend_failure), "").message(), "Windows broker request failed");
}
#endif
