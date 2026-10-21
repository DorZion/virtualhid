/**
 * @file tests/unit/test_virtualhid_control_device_panel.cpp
 * @brief Headless UI tests showing device creation is never gated by licensing.
 */

// standard includes
#include <string_view>
#include <vector>

// third-party includes
#include <gtest/gtest.h>
#include <imgui_internal.h>
// GoogleTest owns the entry point, including on Windows.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#undef SDL_MAIN_HANDLED

// local includes
#include <virtualhid_control_model.hpp>

namespace {
  int license_calls = 0;

  /**
   * @brief Build the most restrictive legacy license answer.
   *
   * @return An unavailable, unlicensed result that would have disabled creation before.
   */
  lvh::LicenseResult restrictive_license_result() {
    ++license_calls;
    lvh::LicenseResult result;
    result.status = lvh::OperationStatus::failure(lvh::ErrorCode::network_unavailable, "License service unavailable.");
    result.license.service_available = false;
    result.license.state = lvh::LicenseState::unlicensed;
    return result;
  }
}  // namespace

namespace lvh {
  LicenseResult control_test_get_license_status() {
    return restrictive_license_result();
  }

  LicenseResult control_test_activate_license(std::string_view, std::string_view = {}) {
    return restrictive_license_result();
  }

  LicenseResult control_test_validate_license() {
    return restrictive_license_result();
  }

  LicenseResult control_test_deactivate_license() {
    return restrictive_license_result();
  }
}  // namespace lvh

// Load the UI with restrictive fake license calls so any remaining license
// gating would show up. Rename its entry point after loading SDL's main header.
#ifdef main
  #undef main
#endif
#define main virtualhid_control_test_main
#define get_license_status control_test_get_license_status
#define activate_license control_test_activate_license
#define validate_license control_test_validate_license
#define deactivate_license control_test_deactivate_license
#include "../../tools/virtualhid_control.cpp"
#undef deactivate_license
#undef validate_license
#undef activate_license
#undef get_license_status
#undef main

namespace {
  using lvh::tools::virtualhid_control::ui::DeviceListItem;
  using lvh::tools::virtualhid_control::ui::DevicePanel;
  using lvh::tools::virtualhid_control::ui::ErrorPanel;

  constexpr auto window_name = "Device panel test";

  class VirtualHidControlDevicePanelTest: public testing::Test {
  protected:
    void SetUp() override {
      license_calls = 0;
      ImGui::CreateContext();
      auto &io = ImGui::GetIO();
      io.IniFilename = nullptr;
      io.DisplaySize = {800.0F, 1000.0F};
      io.DeltaTime = 1.0F / 60.0F;
      io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
      unsigned char *pixels = nullptr;
      int width = 0;
      int height = 0;
      io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
      render();
    }

    void TearDown() override {
      ImGui::DestroyContext();
    }

    void render() {
      ImGui::NewFrame();
      ImGui::SetNextWindowPos({0.0F, 0.0F});
      ImGui::SetNextWindowSize({800.0F, 1000.0F});
      ImGui::Begin(window_name);
      const auto ignore = [] {
      };
      const auto create = [this] {
        ++create_calls_;
      };
      panel_.render(devices_, selected_id_, create, ignore, ignore, ignore);
      error_panel_.render();
      ImGui::End();
      ImGui::Render();
    }

    void activate_item(const char *window, const char *label) {
      auto *target = ImGui::FindWindowByName(window);
      ASSERT_NE(target, nullptr);
      ImGui::ActivateItemByID(target->GetID(label));
      render();
    }

    bool error_popup_open() const {
      const auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
      const auto *window = ImGui::FindWindowByName(window_name);
      return window != nullptr && popups.Size == 1 && popups[0].PopupId == window->GetID("Error");
    }

    DevicePanel panel_;
    ErrorPanel error_panel_;
    std::vector<DeviceListItem> devices_;
    lvh::DeviceId selected_id_ = 0;
    int create_calls_ = 0;
  };
}  // namespace

TEST_F(VirtualHidControlDevicePanelTest, CreateIsNeverGatedByLicensing) {
  activate_item(window_name, "Create");
  activate_item(window_name, "Create");

  EXPECT_EQ(create_calls_, 2);
  EXPECT_EQ(license_calls, 0);
}

TEST_F(VirtualHidControlDevicePanelTest, ShowsCreationFailuresUntilDismissed) {
  error_panel_.show("Virtual HID driver is not installed.");
  render();
  ASSERT_TRUE(error_popup_open());

  const auto *popup = ImGui::GetCurrentContext()->OpenPopupStack[0].Window;
  ASSERT_NE(popup, nullptr);
  activate_item(popup->Name, "OK");
  render();
  EXPECT_FALSE(error_popup_open());
  EXPECT_EQ(license_calls, 0);
}
