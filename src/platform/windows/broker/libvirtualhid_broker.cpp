// SPDX-FileCopyrightText: 2026 LIZARDBYTE LLC
// SPDX-License-Identifier: LicenseRef-LizardByte-SAL-1.0

/**
 * @file src/platform/windows/broker/libvirtualhid_broker.cpp
 * @brief Windows service boundary for libvirtualhid driver access.
 */

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif

// platform includes
// clang-format off
#include <Windows.h>
#include <bcrypt.h>
#include <sddl.h>
// clang-format on

// local includes
#include "broker_request_validation.hpp"
#include "lvh_windows_broker_protocol.h"
#include "xbox_360_software_device.hpp"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lvh::detail::windows_broker_service {

  using UniqueHandle = std::unique_ptr<void, decltype(&::CloseHandle)>;

  constexpr auto service_name = L"libvirtualhid_broker";
  constexpr auto broker_instance_name = "libvirtualhid Windows broker";
  constexpr auto pipe_buffer_size = 8192U;
  constexpr auto pipe_connect_timeout = 1000U;
  constexpr auto pipe_io_timeout = 5000U;

  // Message-mode clients need the complete GENERIC_READ mapping plus individual write rights.
  constexpr auto pipe_client_granted_access = FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES;
  constexpr auto pipe_security_descriptor = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x0012018B;;;AU)";

  static_assert(pipe_client_granted_access == 0x0012018BU);
  static_assert((pipe_client_granted_access & FILE_CREATE_PIPE_INSTANCE) == 0U);

  struct ServiceRuntime {
    SERVICE_STATUS_HANDLE status_handle = nullptr;
    SERVICE_STATUS status {
      .dwServiceType = SERVICE_WIN32_OWN_PROCESS,
      .dwCurrentState = SERVICE_STOPPED,
      .dwControlsAccepted = 0,
      .dwWin32ExitCode = NO_ERROR,
      .dwServiceSpecificExitCode = 0,
      .dwCheckPoint = 0,
      .dwWaitHint = 0,
    };
    HANDLE stop_event = nullptr;
  };

  ServiceRuntime &service_runtime() {
    static ServiceRuntime runtime;
    return runtime;
  }

  UniqueHandle make_unique_handle(HANDLE handle) {
    if (handle == INVALID_HANDLE_VALUE) {
      handle = nullptr;
    }
    return {handle, &::CloseHandle};
  }

  bool complete_pending_pipe_io(
    HANDLE pipe,
    OVERLAPPED &overlapped,
    HANDLE requested_stop_event,
    DWORD timeout,
    DWORD &bytes_transferred
  ) {
    std::array<HANDLE, 2> wait_handles {
      overlapped.hEvent,
      requested_stop_event,
    };
    const auto handle_count = requested_stop_event == nullptr ? 1U : 2U;
    if (const auto wait_result = ::WaitForMultipleObjects(handle_count, wait_handles.data(), FALSE, timeout); wait_result == WAIT_OBJECT_0) {
      return ::GetOverlappedResult(
               pipe,
               &overlapped,
               &bytes_transferred,
               FALSE
             ) != FALSE;
    }

    static_cast<void>(::CancelIoEx(pipe, &overlapped));
    DWORD ignored = 0;
    static_cast<void>(::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE));
    return false;
  }

  bool read_pipe_message(
    HANDLE pipe,
    std::span<std::byte> buffer,
    HANDLE requested_stop_event,
    DWORD &bytes_read
  ) {
    auto operation_event = make_unique_handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!operation_event) {
      return false;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = operation_event.get();
    bytes_read = 0;
    if (::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, &overlapped) != FALSE) {
      return true;
    }
    if (::GetLastError() != ERROR_IO_PENDING) {
      return false;
    }

    return complete_pending_pipe_io(
      pipe,
      overlapped,
      requested_stop_event,
      pipe_io_timeout,
      bytes_read
    );
  }

  bool write_pipe_message(
    HANDLE pipe,
    std::span<const std::byte> buffer,
    HANDLE requested_stop_event
  ) {
    auto operation_event = make_unique_handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!operation_event) {
      return false;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = operation_event.get();
    const auto buffer_size = static_cast<DWORD>(buffer.size());
    DWORD bytes_written = 0;
    if (::WriteFile(pipe, buffer.data(), buffer_size, &bytes_written, &overlapped) != FALSE) {
      return bytes_written == buffer_size;
    }
    if (::GetLastError() != ERROR_IO_PENDING) {
      return false;
    }

    return complete_pending_pipe_io(
             pipe,
             overlapped,
             requested_stop_event,
             pipe_io_timeout,
             bytes_written
           ) &&
           bytes_written == buffer_size;
  }

  template<std::size_t Size>
  void copy_c_string(std::array<char, Size> &target, std::string_view value) {
    std::ranges::fill(target, '\0');
    const auto count = std::min(value.size(), Size - 1U);
    std::memcpy(target.data(), value.data(), count);
  }

  std::string windows_error_message(DWORD error_code) {
    std::array<char, 1024> message_buffer {};
    const auto message_size = ::FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      error_code,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      message_buffer.data(),
      static_cast<DWORD>(message_buffer.size()),
      nullptr
    );

    if (message_size == 0U) {
      std::ostringstream fallback;
      fallback << "Windows error " << error_code;
      return fallback.str();
    }

    std::string message {message_buffer.data(), message_size};
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
      message.pop_back();
    }
    return message;
  }

  /**
   * @brief Fill a broker license status with the unrestricted compatibility values.
   *
   * @param license Status to fill.
   * @param active_devices Number of devices the broker currently owns.
   */
  void fill_unrestricted_license_status(
    LvhWindowsBrokerLicenseStatus &license,
    std::size_t active_devices
  ) {
    license.version = LVH_WINDOWS_BROKER_PROTOCOL_VERSION;
    license.size = sizeof(license);
    license.state = std::to_underlying(LvhWindowsBrokerLicenseState::licensed);
    license.active_devices = static_cast<std::uint32_t>(
      std::min<std::size_t>(active_devices, std::numeric_limits<std::uint32_t>::max())
    );
    license.free_active_device_limit = std::numeric_limits<std::uint32_t>::max();
    license.activation_limit = std::numeric_limits<std::uint32_t>::max();
    license.activation_usage = 1U;
    copy_c_string(license.plan_name, "Unrestricted");
    copy_c_string(license.customer_email, "");
    copy_c_string(license.message, "Activated");
  }

  bool session_token_matches(
    const LvhWindowsSessionToken &lhs,
    const LvhWindowsSessionToken &rhs
  ) {
    return std::ranges::equal(lhs.bytes, rhs.bytes);
  }

  bool generate_session_token(LvhWindowsSessionToken &session_token) {
    if (::BCryptGenRandom(nullptr, session_token.bytes.data(), static_cast<ULONG>(session_token.bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
      return false;
    }
    return !std::ranges::all_of(session_token.bytes, [](std::uint8_t value) {
      return value == 0U;
    });
  }

  LvhWindowsDestroyDeviceRequest make_destroy_device_request(
    std::uint64_t driver_device_id,
    const LvhWindowsSessionToken &session_token
  ) {
    auto request = LvhWindowsDestroyDeviceRequest {};
    request.version = LVH_WINDOWS_CONTROL_PROTOCOL_VERSION;
    request.size = sizeof(request);
    request.driver_device_id = driver_device_id;
    request.session_token = session_token;
    return request;
  }

  template<typename DestroyDevice, typename ForgetDevice>
  void destroy_cleanup_candidates(
    std::span<const LvhWindowsDestroyDeviceRequest> requests,
    DestroyDevice &&destroy_device,
    ForgetDevice &&forget_device
  ) {
    for (const auto &request : requests) {
      if (destroy_device(request)) {
        forget_device(request);
      }
    }
  }

  DWORD pipe_client_process_id(HANDLE pipe) {
    ULONG process_id = 0;
    if (::GetNamedPipeClientProcessId(pipe, &process_id) == FALSE) {
      return 0;
    }
    return process_id;
  }

  UniqueHandle open_client_process(DWORD process_id) {
    if (process_id == 0U) {
      return make_unique_handle(nullptr);
    }
    return make_unique_handle(::OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, process_id));
  }

  UniqueHandle duplicate_client_handle(
    HANDLE client_process,
    std::uint64_t client_handle_value
  ) {
    const auto native_handle_value = static_cast<std::uintptr_t>(client_handle_value);
    if (client_process == nullptr || static_cast<std::uint64_t>(native_handle_value) != client_handle_value || native_handle_value == 0U) {
      return make_unique_handle(nullptr);
    }

    auto duplicated_handle = HANDLE {};
    if (::DuplicateHandle(client_process, std::bit_cast<HANDLE>(native_handle_value), ::GetCurrentProcess(), &duplicated_handle, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
      return make_unique_handle(nullptr);
    }
    return make_unique_handle(duplicated_handle);
  }

  LvhWindowsBrokerStatusCode broker_status_from_protocol(std::uint32_t status) {
    using enum LvhWindowsBrokerStatusCode;

    switch (status) {
      case LVH_WINDOWS_STATUS_SUCCESS:
        return success;
      case LVH_WINDOWS_STATUS_INVALID_ARGUMENT:
        return invalid_argument;
      case LVH_WINDOWS_STATUS_UNSUPPORTED_PROFILE:
        return unsupported_profile;
      case LVH_WINDOWS_STATUS_DEVICE_NOT_FOUND:
        return device_not_found;
      case LVH_WINDOWS_STATUS_BACKEND_FAILURE:
      default:
        return backend_failure;
    }
  }

  class DriverChannel {
  public:
    DriverChannel() = default;

    DriverChannel(const DriverChannel &) = delete;
    DriverChannel &operator=(const DriverChannel &) = delete;

    bool open() {
      if (handle_) {
        return true;
      }

      for (const auto *path : {LVH_WINDOWS_CONTROL_DEVICE_PATH, LVH_WINDOWS_GLOBAL_CONTROL_DEVICE_PATH}) {
        const auto handle = ::CreateFileA(
          path,
          GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL,
          nullptr
        );
        if (handle != INVALID_HANDLE_VALUE) {
          path_ = path;
          handle_ = make_unique_handle(handle);
          return true;
        }
      }

      last_error_ = ::GetLastError();
      return false;
    }

    bool create_device(
      HANDLE control_handle,
      LvhWindowsCreateDeviceRequest request,
      LvhWindowsCreateDeviceResponse &response,
      LvhWindowsBrokerStatusCode &status,
      std::string &message
    ) const {
      if (control_handle == nullptr || control_handle == INVALID_HANDLE_VALUE) {
        status = LvhWindowsBrokerStatusCode::backend_unavailable;
        message = "The requesting client control handle is unavailable.";
        return false;
      }

      auto operation_event = make_unique_handle(::CreateEventA(nullptr, TRUE, FALSE, nullptr));
      if (!operation_event) {
        status = LvhWindowsBrokerStatusCode::backend_failure;
        message = "Unable to create a Windows driver request event: " + windows_error_message(::GetLastError());
        return false;
      }

      OVERLAPPED overlapped {};
      overlapped.hEvent = operation_event.get();
      DWORD bytes_returned = 0;
      if (::DeviceIoControl(control_handle, LVH_WINDOWS_IOCTL_CREATE_DEVICE, &request, sizeof(request), &response, sizeof(response), nullptr, &overlapped) == FALSE && ::GetLastError() != ERROR_IO_PENDING) {
        status = LvhWindowsBrokerStatusCode::backend_failure;
        message = "create Windows virtual HID device: " + windows_error_message(::GetLastError());
        return false;
      }
      if (::GetOverlappedResult(control_handle, &overlapped, &bytes_returned, TRUE) == FALSE) {
        status = LvhWindowsBrokerStatusCode::backend_failure;
        message = "create Windows virtual HID device: " + windows_error_message(::GetLastError());
        return false;
      }

      if (bytes_returned < sizeof(response)) {
        status = LvhWindowsBrokerStatusCode::backend_failure;
        message = "Windows driver returned a truncated device response";
        return false;
      }

      status = broker_status_from_protocol(response.status);
      if (status != LvhWindowsBrokerStatusCode::success) {
        message = "Windows driver rejected virtual HID device creation";
        return false;
      }

      if (response.device_path[0] == '\0') {
        copy_c_string(response.device_path, path_);
      }

      message.clear();
      return true;
    }

    bool destroy_device(
      LvhWindowsDestroyDeviceRequest request,
      LvhWindowsBrokerStatusCode &status,
      std::string &message
    ) {
      using enum LvhWindowsBrokerStatusCode;

      if (!open()) {
        status = backend_unavailable;
        message = "Windows UMDF control device is unavailable: " + windows_error_message(last_error_);
        return false;
      }

      if (DWORD bytes_returned = 0; ::DeviceIoControl(handle_.get(), LVH_WINDOWS_IOCTL_DESTROY_DEVICE, &request, sizeof(request), nullptr, 0, &bytes_returned, nullptr) == FALSE) {
        status = backend_failure;
        message = "destroy Windows virtual HID device: " + windows_error_message(::GetLastError());
        return false;
      }

      status = success;
      message.clear();
      return true;
    }

    bool reset_devices(
      LvhWindowsBrokerStatusCode &status,
      std::string &message
    ) {
      using enum LvhWindowsBrokerStatusCode;

      if (!open()) {
        status = backend_unavailable;
        message = "Windows UMDF control device is unavailable: " + windows_error_message(last_error_);
        return false;
      }

      LvhWindowsResetDevicesRequest request {};
      request.version = LVH_WINDOWS_CONTROL_PROTOCOL_VERSION;
      request.size = sizeof(request);
      if (DWORD bytes_returned = 0; ::DeviceIoControl(handle_.get(), LVH_WINDOWS_IOCTL_RESET_DEVICES, &request, sizeof(request), nullptr, 0, &bytes_returned, nullptr) == FALSE) {
        status = backend_failure;
        message = "reset Windows virtual HID devices: " + windows_error_message(::GetLastError());
        return false;
      }

      status = success;
      message.clear();
      return true;
    }

  private:
    UniqueHandle handle_ {nullptr, &::CloseHandle};
    std::string path_;
    DWORD last_error_ = ERROR_FILE_NOT_FOUND;
  };

  class BrokerState {
  public:
    struct DeviceRecord {
      LvhWindowsSessionToken session_token {};
      DWORD owner_process_id {};
      UniqueHandle owner_process {nullptr, &::CloseHandle};
      std::unique_ptr<lvh::windows::Xbox360SoftwareDevice> xbox360_device;
    };

    BrokerState() {
      auto ignored_status = LvhWindowsBrokerStatusCode::success;
      std::string ignored_message;
      static_cast<void>(reconcile_driver_state(ignored_status, ignored_message));
    }

    void fill_license_status(LvhWindowsBrokerLicenseStatus &license) const {
      std::lock_guard lock {mutex_};
      fill_license_status_locked(license);
    }

    LvhWindowsBrokerStatusResponse handle_status() const {
      LvhWindowsBrokerStatusResponse response {};
      response.version = LVH_WINDOWS_BROKER_PROTOCOL_VERSION;
      response.size = sizeof(response);
      response.status = std::to_underlying(LvhWindowsBrokerStatusCode::success);
      fill_license_status(response.license);
      copy_c_string(response.message, "Broker is running.");
      return response;
    }

    LvhWindowsBrokerCreateDeviceResponse handle_create(
      const LvhWindowsBrokerCreateDeviceRequest &request,
      DWORD client_process_id
    ) {
      LvhWindowsBrokerCreateDeviceResponse response {};
      response.version = LVH_WINDOWS_BROKER_PROTOCOL_VERSION;
      response.size = sizeof(response);
      response.device.version = LVH_WINDOWS_CONTROL_PROTOCOL_VERSION;
      response.device.size = sizeof(response.device);

      if (!lvh::windows::broker_validation::valid_request(request)) {
        response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
        copy_c_string(response.message, "Invalid broker create request.");
        fill_license_status(response.license);
        return response;
      }

      auto reconciliation_status = LvhWindowsBrokerStatusCode::success;
      if (std::string reconciliation_message; !reconcile_driver_state(reconciliation_status, reconciliation_message)) {
        response.status = std::to_underlying(reconciliation_status);
        copy_c_string(response.message, reconciliation_message);
        fill_license_status(response.license);
        return response;
      }

      auto owner_process = open_client_process(client_process_id);
      if (!owner_process) {
        response.status = std::to_underlying(LvhWindowsBrokerStatusCode::backend_failure);
        copy_c_string(response.message, "Unable to open the requesting client process.");
        fill_license_status(response.license);
        return response;
      }

      auto status = LvhWindowsBrokerStatusCode::success;
      std::unique_ptr<lvh::windows::Xbox360SoftwareDevice> xbox360_device;
      if (
        request.device.device_type == LVH_WINDOWS_DEVICE_GAMEPAD &&
        request.device.gamepad_kind == LVH_WINDOWS_GAMEPAD_XBOX_360
      ) {
        const auto driver_device_id = next_xbox360_driver_device_id_.fetch_add(1U);
        auto session_token = LvhWindowsSessionToken {};
        std::string message;
        if (!generate_session_token(session_token)) {
          response.status = std::to_underlying(LvhWindowsBrokerStatusCode::backend_failure);
          fill_license_status(response.license);
          copy_c_string(response.message, "Unable to generate an Xbox 360 device session token.");
          return response;
        }
        xbox360_device = lvh::windows::Xbox360SoftwareDevice::create(
          request.device,
          driver_device_id,
          session_token,
          owner_process.get(),
          response.device,
          status,
          message
        );
        if (!xbox360_device) {
          response.status = std::to_underlying(status);
          fill_license_status(response.license);
          copy_c_string(response.message, message);
          return response;
        }
      } else {
        auto client_control_handle = duplicate_client_handle(
          owner_process.get(),
          request.client_control_handle
        );
        if (!client_control_handle) {
          response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
          copy_c_string(response.message, "Unable to duplicate the requesting client control handle.");
          fill_license_status(response.license);
          return response;
        }

        std::string message;
        if (!driver_.create_device(
              client_control_handle.get(),
              request.device,
              response.device,
              status,
              message
            )) {
          response.status = std::to_underlying(status);
          fill_license_status(response.license);
          copy_c_string(response.message, message);
          return response;
        }
      }

      {
        std::lock_guard lock {mutex_};
        devices_.try_emplace(
          response.device.driver_device_id,
          DeviceRecord {
            .session_token = response.device.session_token,
            .owner_process_id = client_process_id,
            .owner_process = std::move(owner_process),
            .xbox360_device = std::move(xbox360_device),
          }
        );
        fill_license_status_locked(response.license);
      }

      response.status = std::to_underlying(LvhWindowsBrokerStatusCode::success);
      copy_c_string(response.message, "Created virtual HID device.");
      return response;
    }

    LvhWindowsBrokerDestroyDeviceResponse handle_destroy(
      const LvhWindowsBrokerDestroyDeviceRequest &request
    ) {
      LvhWindowsBrokerDestroyDeviceResponse response {};
      response.version = LVH_WINDOWS_BROKER_PROTOCOL_VERSION;
      response.size = sizeof(response);

      if (!lvh::windows::broker_validation::valid_request(request)) {
        response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
        copy_c_string(response.message, "Invalid broker destroy request.");
        fill_license_status(response.license);
        return response;
      }

      {
        std::lock_guard lock {mutex_};
        const auto iter = devices_.find(request.device.driver_device_id);
        if (iter == devices_.end()) {
          response.status = std::to_underlying(LvhWindowsBrokerStatusCode::device_not_found);
          fill_license_status_locked(response.license);
          copy_c_string(response.message, "Broker does not own this virtual device.");
          return response;
        }
        if (!session_token_matches(iter->second.session_token, request.device.session_token)) {
          response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
          fill_license_status_locked(response.license);
          copy_c_string(response.message, "Invalid virtual device session token.");
          return response;
        }
      }

      auto status = LvhWindowsBrokerStatusCode::success;
      if (std::string message; !destroy_device_transport(request.device, status, message)) {
        response.status = std::to_underlying(status);
        fill_license_status(response.license);
        copy_c_string(response.message, message);
        return response;
      }

      {
        std::lock_guard lock {mutex_};
        devices_.erase(request.device.driver_device_id);
        fill_license_status_locked(response.license);
      }

      response.status = std::to_underlying(LvhWindowsBrokerStatusCode::success);
      copy_c_string(response.message, "Destroyed virtual device.");
      return response;
    }

    void cleanup_devices() {
      auto reconciliation_status = LvhWindowsBrokerStatusCode::success;
      if (std::string reconciliation_message; !reconcile_driver_state(reconciliation_status, reconciliation_message)) {
        return;
      }

      std::vector<LvhWindowsDestroyDeviceRequest> cleanup_requests;
      {
        std::lock_guard lock {mutex_};
        for (const auto &[driver_device_id, device] : devices_) {
          const auto wait_result = ::WaitForSingleObject(device.owner_process.get(), 0);
          if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) {
            cleanup_requests.push_back(make_destroy_device_request(driver_device_id, device.session_token));
          }
        }
      }

      destroy_cleanup_candidates(
        cleanup_requests,
        [this](const auto &request) {
          auto status = LvhWindowsBrokerStatusCode::success;
          std::string message;
          return destroy_device_transport(request, status, message);
        },
        [this](const auto &request) {
          std::lock_guard lock {mutex_};
          const auto iter = devices_.find(request.driver_device_id);
          if (iter != devices_.end() && session_token_matches(iter->second.session_token, request.session_token)) {
            devices_.erase(iter);
          }
        }
      );
    }

    LvhWindowsBrokerLicenseResponse handle_activate_license(
      const LvhWindowsBrokerLicenseRequest &request
    ) {
      LvhWindowsBrokerLicenseResponse response {};
      response.version = LVH_WINDOWS_BROKER_PROTOCOL_VERSION;
      response.size = sizeof(response);

      if (!lvh::windows::broker_validation::valid_request(
            request,
            LvhWindowsBrokerRequestType::activate_license
          )) {
        response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
        copy_c_string(response.message, "Invalid broker activate request.");
        fill_license_status(response.license);
        return response;
      }

      return activated_response(response);
    }

    LvhWindowsBrokerLicenseResponse handle_validate_license(
      const LvhWindowsBrokerLicenseRequest &request
    ) {
      LvhWindowsBrokerLicenseResponse response {};
      response.version = LVH_WINDOWS_BROKER_PROTOCOL_VERSION;
      response.size = sizeof(response);

      if (!lvh::windows::broker_validation::valid_request(
            request,
            LvhWindowsBrokerRequestType::validate_license
          )) {
        response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
        copy_c_string(response.message, "Invalid broker validate request.");
        fill_license_status(response.license);
        return response;
      }

      return activated_response(response);
    }

    LvhWindowsBrokerLicenseResponse handle_deactivate_license(
      const LvhWindowsBrokerLicenseRequest &request
    ) {
      LvhWindowsBrokerLicenseResponse response {};
      response.version = LVH_WINDOWS_BROKER_PROTOCOL_VERSION;
      response.size = sizeof(response);

      if (!lvh::windows::broker_validation::valid_request(
            request,
            LvhWindowsBrokerRequestType::deactivate_license
          )) {
        response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
        copy_c_string(response.message, "Invalid broker deactivate request.");
        fill_license_status(response.license);
        return response;
      }

      return activated_response(response);
    }

  private:
    LvhWindowsBrokerLicenseResponse activated_response(LvhWindowsBrokerLicenseResponse &response) const {
      response.status = std::to_underlying(LvhWindowsBrokerStatusCode::success);
      fill_license_status(response.license);
      copy_c_string(response.message, "Activated");
      return response;
    }

    bool destroy_device_transport(
      const LvhWindowsDestroyDeviceRequest &request,
      LvhWindowsBrokerStatusCode &status,
      std::string &message
    ) {
      {
        std::lock_guard lock {mutex_};
        const auto device = devices_.find(request.driver_device_id);
        if (device != devices_.end() && device->second.xbox360_device) {
          status = LvhWindowsBrokerStatusCode::success;
          message.clear();
          return true;
        }
      }
      return driver_.destroy_device(request, status, message);
    }

    bool reconcile_driver_state(
      LvhWindowsBrokerStatusCode &status,
      std::string &message
    ) {
      {
        std::lock_guard lock {mutex_};
        if (driver_state_reconciled_) {
          status = LvhWindowsBrokerStatusCode::success;
          message.clear();
          return true;
        }
      }

      if (!driver_.reset_devices(status, message)) {
        return false;
      }

      std::lock_guard lock {mutex_};
      devices_.clear();
      driver_state_reconciled_ = true;
      return true;
    }

    void fill_license_status_locked(LvhWindowsBrokerLicenseStatus &license) const {
      fill_unrestricted_license_status(license, devices_.size());
    }

    mutable std::mutex mutex_;
    std::map<std::uint64_t, DeviceRecord> devices_;
    DriverChannel driver_;
    std::atomic<std::uint64_t> next_xbox360_driver_device_id_ {UINT64_C(0x8000000000000000)};
    bool driver_state_reconciled_ = false;
  };

  BrokerState &broker_state() {
    static BrokerState state;
    return state;
  }

  template<typename Response>
  bool write_response(
    HANDLE pipe,
    const Response &response,
    HANDLE requested_stop_event
  ) {
    if (!write_pipe_message(
          pipe,
          std::as_bytes(std::span<const Response> {&response, 1}),
          requested_stop_event
        )) {
      return false;
    }

    // DisconnectNamedPipe discards responses that the client has not read yet.
    // A well-behaved one-request client closes its handle after reading the
    // response, so wait for that close before disconnecting the server end. The
    // overlapped read retains the normal I/O timeout and stop-event handling,
    // preventing an uncooperative client from blocking the service indefinitely.
    std::array<std::byte, 1> extra_request {};
    DWORD bytes_read = 0;
    static_cast<void>(read_pipe_message(
      pipe,
      extra_request,
      requested_stop_event,
      bytes_read
    ));
    return true;
  }

  template<typename Request, std::size_t Size>
  Request request_from_buffer(const std::array<std::byte, Size> &buffer) {
    Request request {};
    std::memcpy(&request, buffer.data(), sizeof(request));
    return request;
  }

  void handle_pipe_client(HANDLE pipe, HANDLE requested_stop_event) {
    broker_state().cleanup_devices();

    const auto send_response = [pipe, requested_stop_event](const auto &response) {
      return write_response(pipe, response, requested_stop_event);
    };

    std::array<std::byte, pipe_buffer_size> request_buffer {};
    DWORD bytes_read = 0;
    if (!read_pipe_message(pipe, request_buffer, requested_stop_event, bytes_read) || bytes_read < sizeof(LvhWindowsBrokerRequestHeader)) {
      auto response = broker_state().handle_status();
      response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
      copy_c_string(response.message, "Broker request was empty or truncated.");
      static_cast<void>(send_response(response));
      return;
    }

    const auto header = request_from_buffer<LvhWindowsBrokerRequestHeader>(request_buffer);
    if (header.version != LVH_WINDOWS_BROKER_PROTOCOL_VERSION || header.size != bytes_read) {
      auto response = broker_state().handle_status();
      response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
      copy_c_string(response.message, "Broker request header is invalid.");
      static_cast<void>(send_response(response));
      return;
    }

    switch (static_cast<LvhWindowsBrokerRequestType>(header.type)) {
      case LvhWindowsBrokerRequestType::status:
        if (header.size == sizeof(LvhWindowsBrokerStatusRequest)) {
          const auto request = request_from_buffer<LvhWindowsBrokerStatusRequest>(request_buffer);
          if (lvh::windows::broker_validation::valid_request(request)) {
            static_cast<void>(send_response(broker_state().handle_status()));
            return;
          }
        }
        break;

      case LvhWindowsBrokerRequestType::create_device:
        if (header.size == sizeof(LvhWindowsBrokerCreateDeviceRequest)) {
          const auto request = request_from_buffer<LvhWindowsBrokerCreateDeviceRequest>(request_buffer);
          static_cast<void>(send_response(
            broker_state().handle_create(request, pipe_client_process_id(pipe))
          ));
          return;
        }
        break;

      case LvhWindowsBrokerRequestType::destroy_device:
        if (header.size == sizeof(LvhWindowsBrokerDestroyDeviceRequest)) {
          const auto request = request_from_buffer<LvhWindowsBrokerDestroyDeviceRequest>(request_buffer);
          static_cast<void>(send_response(broker_state().handle_destroy(request)));
          return;
        }
        break;

      case LvhWindowsBrokerRequestType::activate_license:
        if (header.size == sizeof(LvhWindowsBrokerLicenseRequest)) {
          const auto request = request_from_buffer<LvhWindowsBrokerLicenseRequest>(request_buffer);
          static_cast<void>(send_response(broker_state().handle_activate_license(request)));
          return;
        }
        break;

      case LvhWindowsBrokerRequestType::validate_license:
        if (header.size == sizeof(LvhWindowsBrokerLicenseRequest)) {
          const auto request = request_from_buffer<LvhWindowsBrokerLicenseRequest>(request_buffer);
          static_cast<void>(send_response(broker_state().handle_validate_license(request)));
          return;
        }
        break;

      case LvhWindowsBrokerRequestType::deactivate_license:
        if (header.size == sizeof(LvhWindowsBrokerLicenseRequest)) {
          const auto request = request_from_buffer<LvhWindowsBrokerLicenseRequest>(request_buffer);
          static_cast<void>(send_response(broker_state().handle_deactivate_license(request)));
          return;
        }
        break;
    }

    auto response = broker_state().handle_status();
    response.status = std::to_underlying(LvhWindowsBrokerStatusCode::invalid_argument);
    copy_c_string(response.message, "Broker request type or size is unsupported.");
    static_cast<void>(send_response(response));
  }

  std::optional<UniqueHandle> wait_for_pipe_client(HANDLE requested_stop_event) {
    PSECURITY_DESCRIPTOR raw_security_descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(pipe_security_descriptor, SDDL_REVISION_1, &raw_security_descriptor, nullptr) == FALSE) {
      return std::nullopt;
    }
    auto security_descriptor = std::unique_ptr<void, decltype(&::LocalFree)> {
      raw_security_descriptor,
      &::LocalFree,
    };
    SECURITY_ATTRIBUTES security_attributes {
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = security_descriptor.get(),
      .bInheritHandle = FALSE,
    };

    auto pipe = make_unique_handle(::CreateNamedPipeA(LVH_WINDOWS_BROKER_PIPE_PATH, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, pipe_buffer_size, pipe_buffer_size, pipe_connect_timeout, &security_attributes));
    if (!pipe) {
      return std::nullopt;
    }

    auto connected_event = make_unique_handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connected_event) {
      return std::nullopt;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = connected_event.get();
    if (::ConnectNamedPipe(pipe.get(), &overlapped) == FALSE) {
      const auto error = ::GetLastError();
      if (error == ERROR_PIPE_CONNECTED) {
        return pipe;
      }
      if (error != ERROR_IO_PENDING) {
        return std::nullopt;
      }

      DWORD ignored = 0;
      if (!complete_pending_pipe_io(
            pipe.get(),
            overlapped,
            requested_stop_event,
            pipe_connect_timeout,
            ignored
          )) {
        return std::nullopt;
      }
    }

    return pipe;
  }

  void report_service_status(DWORD current_state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0) {
    auto &runtime = service_runtime();
    if (runtime.status_handle == nullptr) {
      return;
    }

    runtime.status.dwCurrentState = current_state;
    runtime.status.dwWin32ExitCode = win32_exit_code;
    runtime.status.dwWaitHint = wait_hint;
    runtime.status.dwControlsAccepted = current_state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    if (current_state == SERVICE_RUNNING || current_state == SERVICE_STOPPED) {
      runtime.status.dwCheckPoint = 0;
    } else {
      ++runtime.status.dwCheckPoint;
    }

    static_cast<void>(SetServiceStatus(runtime.status_handle, &runtime.status));
  }

  DWORD run_broker_loop(HANDLE requested_stop_event) {
    if (requested_stop_event == nullptr) {
      return ERROR_INVALID_HANDLE;
    }

    while (WaitForSingleObject(requested_stop_event, 0) == WAIT_TIMEOUT) {
      broker_state().cleanup_devices();

      auto pipe = wait_for_pipe_client(requested_stop_event);
      if (!pipe) {
        continue;
      }

      handle_pipe_client(pipe->get(), requested_stop_event);
      static_cast<void>(::DisconnectNamedPipe(pipe->get()));
    }

    return ERROR_SUCCESS;
  }

  BOOL WINAPI console_control_handler(DWORD control_type) {
    switch (control_type) {
      case CTRL_C_EVENT:
      case CTRL_BREAK_EVENT:
      case CTRL_CLOSE_EVENT:
        if (const auto event = service_runtime().stop_event; event != nullptr) {
          static_cast<void>(::SetEvent(event));
          return TRUE;
        }
        return FALSE;

      default:
        return FALSE;
    }
  }

  void WINAPI service_control_handler(DWORD control_code) {
    switch (control_code) {
      case SERVICE_CONTROL_STOP:
      case SERVICE_CONTROL_SHUTDOWN:
        report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 1000);
        if (const auto event = service_runtime().stop_event; event != nullptr) {
          static_cast<void>(SetEvent(event));
        }
        return;

      default:
        return;
    }
  }

  void WINAPI service_main(DWORD argc, wchar_t **argv) {
    static_cast<void>(argc);
    static_cast<void>(argv);

    auto &runtime = service_runtime();
    runtime.status_handle = RegisterServiceCtrlHandlerW(service_name, service_control_handler);
    if (runtime.status_handle == nullptr) {
      return;
    }

    report_service_status(SERVICE_START_PENDING, NO_ERROR, 1000);
    runtime.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (runtime.stop_event == nullptr) {
      report_service_status(SERVICE_STOPPED, GetLastError());
      return;
    }

    report_service_status(SERVICE_RUNNING);
    const auto result = run_broker_loop(runtime.stop_event);
    static_cast<void>(CloseHandle(runtime.stop_event));
    runtime.stop_event = nullptr;
    report_service_status(SERVICE_STOPPED, result);
  }

  int run_console() {
    auto &runtime = service_runtime();
    runtime.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (runtime.stop_event == nullptr) {
      return static_cast<int>(GetLastError());
    }

    static_cast<void>(SetConsoleCtrlHandler(console_control_handler, TRUE));
    const auto result = run_broker_loop(runtime.stop_event);
    static_cast<void>(SetConsoleCtrlHandler(console_control_handler, FALSE));
    static_cast<void>(CloseHandle(runtime.stop_event));
    runtime.stop_event = nullptr;
    return static_cast<int>(result);
  }

}  // namespace lvh::detail::windows_broker_service

int main(int argc, char **argv) {
  if (argc > 1 && std::string_view {argv[1]} == "--console") {
    return lvh::detail::windows_broker_service::run_console();
  }
  if (argc > 1 && std::string_view {argv[1]} == "--service-name") {
    (void) lvh::detail::windows_broker_service::broker_instance_name;
    return 0;
  }

  std::wstring mutable_service_name {
    lvh::detail::windows_broker_service::service_name,
  };
  if (std::array<SERVICE_TABLE_ENTRYW, 2> dispatch_table {{
        {
          mutable_service_name.data(),
          lvh::detail::windows_broker_service::service_main,
        },
        {nullptr, nullptr},
      }};
      StartServiceCtrlDispatcherW(dispatch_table.data()) != FALSE) {
    return 0;
  }

  const auto error = GetLastError();
  if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
    return lvh::detail::windows_broker_service::run_console();
  }

  return static_cast<int>(error);
}
