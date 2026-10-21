# Plan: remove licensing enforcement without breaking interfaces

## Objective

Remove the licensing gate by making every existing public licensing operation immediately return successful activated/licensed status. Preserve all existing public interfaces and broker wire compatibility. Relicensing will happen later, in a separate step.

Implementation has not started. Keep the execution checkboxes synchronized with actual progress; do not mark a step complete while its required checks fail or its implementation is partial.

## 1. Scope and non-breaking compatibility contract

### Preserve unchanged

- Public header paths, umbrella-header includes, function signatures, default arguments, and exported symbol names.
- Public structs: field order, field types, layout, and default construction.
- Existing enum members, underlying types, and numeric values, including obsolete licensing errors.
- Broker protocol versions, operation IDs, request/response sizes and layouts, field offsets, endpoints, and reserved fields.
- CMake consumer targets, library names, public build-option names, installed executable names and paths, and CLI command syntax.
- Device APIs, supported profiles, report formats, ownership, security, and normal lifecycle behavior.
- License files, SPDX identifiers, copyright notices, legal packaging, and third-party attribution.

No source, ABI, wire-protocol, CLI-command, installed-artifact, or package-consumer compatibility break is permitted. Compatibility must be verified against the pre-change baseline. If implementation reveals that a proposed change would violate this contract, revise that change rather than silently accepting a break.

This does not promise compatibility across unrelated compiler or C++ runtime ABIs. It preserves the existing compatibility boundary.

### Approved behavior changes

These changes are the goal, not interface removals:

- Any supplied license key succeeds, including empty or invalid keys.
- Validation never contacts a provider or reports expiration.
- Deactivation succeeds but leaves the effective state activated.
- Licensing status no longer indicates broker health or provider entitlement.
- Provider/customer metadata is no longer returned.
- The licensing CLI no longer prompts for a key.
- The UI no longer requires activation or offers purchasing controls.
- Licensing limits, evaluation timeouts, usage reporting, and license-driven device revocation disappear.

Existing licensing error categories remain defined for compatibility, even though the updated public licensing operations no longer emit them. Real device, transport, permission, and backend failures must not be converted into success.

### Excluded from this step

- Relicensing or replacing existing legal notices.
- Removing public licensing APIs, fields, enum members, or protocol operations.
- New features, dependencies, configuration switches, or replacement licensing systems.
- Automatic removal of old credentials or provider activations.
- External subscription cancellation, billing changes, refunds, or account migration.
- Platform signing, entitlement, installation, or permission changes.

## 2. Exact public API behavior

Retain these declarations in `src/include/libvirtualhid/license.hpp`, including the existing default argument:

```cpp
LicenseResult get_license_status();
LicenseResult activate_license(std::string_view license_key, std::string_view instance_name = {});
LicenseResult validate_license();
LicenseResult deactivate_license();
```

All four functions return the same compatibility result on every platform, including platforms previously reporting licensing as unavailable:

| Field                | Required value                                |
|----------------------|-----------------------------------------------|
| `status.code()`      | `ErrorCode::ok`                               |
| `status.ok()`        | `true`                                        |
| `status.message()`   | Empty                                         |
| Result boolean       | `true`                                        |
| `service_available`  | `true`                                        |
| `state`              | `LicenseState::licensed`                      |
| `licensed()`         | `true`                                        |
| `active_devices`     | `0`; not tracked by this local API            |
| `activation_limit`   | `UINT32_MAX`; unrestricted-status sentinel    |
| `activation_usage`   | `1`; effective activation, not provider usage |
| `plan_name`          | `"Unrestricted"`                              |
| `customer_email`     | Empty                                         |
| `message`            | `"Activated"`                                 |
| `purchase_url`       | Empty                                         |
| `manage_account_url` | Empty                                         |

These are compatibility values, not evidence of provider activation. Document that `service_available` means the local compatibility API is available, not that a broker is running. `active_devices = 0` means untracked here, not proof that no devices exist. The unrestricted sentinel is descriptive only; there is no licensing enforcement at that value or any other value.

Do not change the default initialization of caller-constructed `LicenseStatus` or `LicenseResult` objects. Override fields only in returned compatibility results.

### Meaning of immediately

- No IPC, network requests, filesystem or registry access, credential storage, sleeps, retries, or background tasks.
- No scanning, normalizing, validating, logging, or retaining either argument.
- Empty, oversized, whitespace-only, embedded-NUL, and arbitrary-byte inputs succeed without interpretation.
- Only ordinary in-process result construction occurs.
- Repeated activation, validation, and deactivation have no side effects.

### Implementation paths

- Reuse `src/platform/license_unavailable.cpp` for the common implementation and update its documentation.
- Compile it exactly once per production and test library through `src/CMakeLists.txt`.
- Remove duplicate platform-specific licensing definitions from `src/platform/windows/windows_license.cpp` and `src/platform/macos/macos_broker_client.cpp` without losing unrelated broker/device code.
- Keep `LicenseState`, `LicenseStatus`, `LicenseResult`, `ErrorCode`, and their existing helpers intact.
- Keep `src/include/libvirtualhid/libvirtualhid.hpp` including the licensing header.
- Update affected Doxygen blocks in `src/include/libvirtualhid/license.hpp`, `src/include/libvirtualhid/types.hpp`, and implementation declarations to explain the compatibility behavior.

## 3. Broker protocol compatibility

Keep all licensing operations and existing request/response structures:

- Windows protocol version **5** in `src/platform/windows/shared/lvh_windows_broker_protocol.h`.
- macOS protocol version **1** in `src/platform/macos/broker/protocol.hpp`.
- Existing endpoints, operation IDs, field types, sizes, offsets, reserved fields, and encodings.

### Licensing responses

For structurally valid status, activation, validation, and deactivation messages:

- Return success promptly without provider or persistent-state access. Broker transport itself still requires the broker to be running.
- Report licensed using each protocol's existing encoding: Windows `LvhWindowsBrokerLicenseState::licensed`; macOS the existing `LicenseState::licensed` encoding. These encodings are not interchangeable.
- Return `"Activated"` in licensing messages and the unrestricted metadata defined above where represented by the protocol.
- Retain actual broker-tracked device counts in wire responses, preserving the existing counting scope.
- Set Windows `free_active_device_limit` to `UINT32_MAX`; do not enforce it.
- Initialize reserved fields and string buffers correctly.
- Populate embedded licensing status in create/destroy responses consistently, without changing the actual operation result.

Preserve the Windows ordinary status response's `"Broker is running."` message. Keep device-operation errors and normal transport errors truthful.

### Validation boundary

- Continue rejecting wrong versions, incorrect lengths, unsupported operation IDs, invalid reserved fields, and truncated messages.
- Treat licensing key/name payloads as unused bounded bytes. Empty or arbitrary contents must not trigger credential rejection.
- Do not interpret ignored credential payloads as strings.
- Preserve unrelated device descriptor, report, ownership, handle, and session-token validation.
- Update `src/platform/windows/broker/broker_request_validation.hpp` only where credential checks are obsolete.
- Retain legacy status/error mappings so clients can still interpret older broker responses.

## 4. Windows enforcement removal

In `src/platform/windows/broker/libvirtualhid_broker.cpp`, remove:

- Polar HTTP calls, request construction, and response parsing.
- Provider activation, validation, deactivation, and usage reporting.
- License and evaluation persistence, trusted-time bookkeeping, and licensing-only boot markers.
- Validation threads, wakeups, retries, outage grace periods, and evaluation timers.
- Device-creation authorization based on licensing.
- License/evaluation flags attached to device records.
- Expiration, outage, and evaluation-driven destruction.

Replace retained licensing handlers with compatibility responses.

Preserve:

- Named-pipe security and rejection of remote clients.
- Client-process identification and handle validation.
- Secure session-token generation and matching.
- Normal destruction and process-exit cleanup.
- Driver reconciliation, service shutdown, and transport failure handling.
- Xbox-specific software-device behavior and all driver-required checks.

Remove obsolete private code only after checking all remaining callers:

- `src/platform/shared/lvh_broker_license_policy.hpp`
- `src/platform/windows/shared/lvh_windows_broker_config.hpp`
- `src/platform/windows/shared/lvh_windows_github_actions_evaluation.hpp`

Do not remove shared utilities still needed outside licensing. Do not add a custom kernel-mode driver or alter privileges.

## 5. macOS enforcement removal

In `src/platform/macos/broker/libvirtualhid_macos_broker.cpp`:

- Remove `LicenseManager` authorization and report-loop licensing checks.
- Remove evaluation deadlines, provider traffic, persistence, and usage reporting.
- Respond successfully to retained licensing messages.
- Preserve simple device-count bookkeeping needed by existing wire fields without retaining the licensing policy machinery.

Remove the obsolete private implementations when their callers have been replaced:

- `src/platform/macos/broker/license_manager.hpp`
- `src/platform/macos/broker/license_manager.mm`

Preserve:

- Root-owned socket-directory protection.
- IPC framing and creation/report validation.
- Disconnect cleanup, HID cancellation, and resource release.
- Existing input/output behavior.
- Apple entitlement, signing, provisioning, and Accessibility requirements.

Successful licensing responses must not conceal missing entitlements, permission errors, or failed HID creation.

## 6. CLI compatibility

Update `src/platform/macos/broker/license_cli.cpp`:

- Retain `status`, `activate [machine-name]`, `validate`, and `deactivate`.
- Retain the executable name, installed path, existing output labels, and argument-error conventions.
- Return exit code **0** for valid commands and keep exit code **2** for invalid command syntax.
- Remove key input, terminal manipulation, and waiting on stdin.
- Print the compatibility result, including `"Activated"`.
- Keep the existing active-gamepad output label; document that the local API's value is untracked.
- Optional customer/purchase/account output disappears because those fields are empty; do not introduce a new output format.

Keep the CLI target, packaging, signing, installation, and coverage checks in:

- `src/platform/macos/broker/CMakeLists.txt`
- `scripts/macos/build-and-install.sh`
- `scripts/macos/package-dmg.sh`
- `scripts/macos/install.command`
- `scripts/macos/check-package-coverage.sh`

Remove only instructions telling users to activate.

## 7. UI changes

Update:

- `tools/virtualhid_control.cpp`
- `tools/virtualhid_control_model.cpp`
- `tools/virtualhid_control_model.hpp`

Remove key entry, activation/deactivation controls, purchase links, licensing polling, and license-state conditions disabling device creation.

Do not use the now-unconditional public `service_available` value to infer broker availability. Preserve actual backend diagnostics and creation errors, using existing capability/operation paths rather than a new public API. Keep ordinary-user operation and existing device controls.

Private UI-only helpers may be removed when unused. Public licensing APIs remain.

## 8. Builds, dependencies, CI, and packaging

### Builds and dependencies

Update `src/CMakeLists.txt`, broker CMake files, and `tests/CMakeLists.txt`:

- Remove licensing-only `nlohmann_json`, WinHTTP, and DPAPI/Crypt32 dependencies after confirming no remaining use.
- Keep BCrypt for session security.
- Keep dependencies needed by service permissions, driver setup, HID operation, or other features.
- Preserve consumer targets, names, public build options, and package exports.
- Update misleading licensed-broker descriptions without renaming options.
- Modify generated dependency files only through their official tooling.
- Keep production/test library configurations consistent and avoid duplicate definitions.

### CI

Update:

- `.github/workflows/ci-build.yml`
- `.github/workflows/ci-macos-broker.yml`

Remove evaluation-window setup. Replace licensing smoke-test expectations with unconditional success and unrestricted device tests. Keep CLI build/sign/package coverage. Treat legacy evaluation environment settings as irrelevant to operation, not a new error condition.

### Packaging

- Keep existing license notices and license-file installation unchanged.
- Preserve service names, executable paths, package identities, components, and upgrade behavior.
- Update licensing-related installer/service descriptions, including `scripts/windows/install-driver.ps1`.
- Check Windows MSI and macOS DMG contents and package coverage scripts.
- Do not add a network-dependent migration step or remove installed compatibility tools.

## 9. Existing installations, stored data, and mixed versions

### Stored data and external accounts

Leave old credential/evaluation files untouched but completely unused:

- No reads, writes, automatic deletion, or remote deactivation.
- No provider activation is released automatically.
- No subscription or billing changes occur.

This avoids destructive migration and preserves rollback options. Data cleanup and external account retirement are separate future tasks. Existing sensitive files retain their existing protection; do not weaken ACLs or permissions.

### Compatibility limits to document

- Old clients using the unchanged wire protocol should work with updated brokers; verify this against pre-change clients.
- Old applications may still display cached purchase links or perform their own key validation before sending a request. The update cannot change already-shipped client code.
- Updated local APIs cannot remove enforcement from an old broker. Unrestricted device operation requires an updated broker.
- The library is currently static: replacing its installed archive does not update already-linked applications. Rebuild/relink applications to obtain immediate local API behavior. No consumer source change is required.
- Existing compiler/runtime ABI restrictions remain unchanged.
- Rolling back to an older broker restores its enforcement.

Do not claim that an updated local licensing result proves an older broker will allow device creation. Document the required broker upgrade without making it a prerequisite for local licensing API success.

## 10. Tests and validation

### Public API

Update `tests/unit/test_license.cpp` to cover all four operations on every supported build:

- Exact compatibility result fields, unchanged enum values, and existing boolean helpers.
- Missing broker, offline operation, and absent credentials.
- Empty, invalid, oversized, whitespace-only, embedded-NUL, and arbitrary-byte arguments.
- Repeated calls, concurrent callers, and deactivation followed by status still reporting activated.
- No external I/O, credential persistence, provider access, or argument logging.

Avoid fragile millisecond assertions. Prove immediate behavior through the absence of external-I/O paths and tests that do not need a broker/provider.

### Protocols and brokers

Update:

- `tests/unit/test_windows_broker_validation.cpp`
- `tests/unit/test_windows_broker_service.cpp`
- `tests/unit/test_macos_broker_protocol.cpp`
- Related broker fixtures and fixture headers.

Verify:

- Existing protocol versions, operation IDs, sizes, offsets, and encodings.
- Successful retained licensing operations, including ignored empty/arbitrary credential payloads.
- Rejection of malformed envelopes and device requests.
- No state-file, evaluation-setting, or provider dependency.
- Multiple devices without licensing limits.
- No licensing/evaluation/outage-driven revocation.
- Preserved process-exit, disconnect, shutdown, and reconciliation cleanup.
- Real backend errors remain errors even when embedded licensing status is activated.

Keep Windows fixed-layout assertions. Add equivalent macOS layout/offset checks and public-interface compatibility coverage against the pre-change baseline.

### Connected callers and fixtures

Update affected tests and private fixtures:

- `tests/unit/test_windows_backend.cpp`
- `tests/fixtures/windows_backend_test_hooks.cpp`
- `tests/fixtures/include/fixtures/windows_backend_test_hooks.hpp`
- `tests/fixtures/windows_broker_service_test_hooks.cpp`
- `tests/fixtures/include/fixtures/windows_broker_service_test_hooks.hpp`
- `tests/unit/test_virtualhid_control_license_panel.cpp` (renamed to `tests/unit/test_virtualhid_control_device_panel.cpp`)
- `tests/unit/test_virtualhid_control_model.cpp`

Keep meaningful legacy error-mapping/fallback coverage. Remove tests and fixture scenarios whose only purpose was obsolete enforcement. Replace UI gating tests with coverage that device creation is not blocked by licensing and actual backend failures are still shown.

### Compatibility evidence

- Compile callers using pre-change public headers against the updated library using the same supported toolchain/runtime ABI.
- Compare public symbols, type layouts, enum values, and exported CMake package interfaces against the baseline.
- Exercise pre-change wire messages/clients against updated brokers.
- Check existing CLI command syntax, exit codes, output labels, executable names, and install paths.
- Verify all existing public build options still configure and package-consumer tests still compile/link.

### Build and installed-device matrix

- Normal Windows library: MSVC and MSYS2/UCRT64.
- Windows driver package: separate WDK/MSVC build.
- macOS library, broker, CLI, and packaged installation.
- Linux and other configured platform builds.
- Package-consumer compilation/linking against retained headers and symbols.
- Existing lifecycle suite and `examples/gamepad_adapter.cpp`.
- Fresh and upgraded installations, offline and without credentials, including existing invalid/expired stored state.
- Installed-device operation beyond the old five-minute evaluation period.
- Real device creation, reports, feedback, cleanup, and ordinary-user permissions.

Use `cmake-build-*` directories. Run GoogleTest from `tests/test_libvirtualhid` inside the build directory, with `.exe` on Windows. Prefix normal Windows build/test commands with `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c` as required by repository guidance. Keep WDK/MSVC package validation separate.

Report unavailable signing, platform, or hardware checks explicitly. Do not mark implementation fully verified if a required check has not run.

## 11. Documentation and release notes

Update Doxygen and affected usage pages:

- `README.md`
- `docs/usage.md`
- `docs/windows-driver.md`
- `docs/macos-gamepad.md`
- `docs/platform-support.md`
- `docs/end-user-gamepad-guide.md`
- `docs/maintainer/store-review-validation.md`

Update activation/purchase messaging in:

- `gh-pages-template/index.html`
- `gh-pages-template/virtual-hid-driver.html`
- `gh-pages-template/_config.yml`

Review screenshots and installer messages that show activation requirements. Preserve legal/license sections unrelated to runtime activation.

Document:

- No runtime activation, payment, account, or internet requirement for device operation with updated brokers.
- Licensing APIs remain as unconditional compatibility operations.
- Deactivation no longer releases provider activations.
- Metadata, service-availability, and device-count semantics.
- Broker upgrade and static-library rebuild/relink requirements.
- Approved behavior changes, including CLI prompt removal.
- Unchanged signing, permissions, hardware limitations, and legal terms.

Do not describe this release as fully open-source: relicensing has not happened. External checkout/store/customer messaging is a separate operational follow-up; this implementation must not silently change billing or customer accounts.

## 12. Execution checklist

Perform these steps in order and update this file immediately when a step is completed:

- [x] Record the pre-change compatibility baseline and add/update interface and behavior tests. Baseline: commit `48e94b8`.
- [x] Implement the common immediate-success public licensing API on every platform.
- [x] Remove Windows enforcement while retaining wire handlers, security, and lifecycle behavior.
- [x] Remove macOS enforcement while retaining wire handlers, security, and lifecycle behavior.
- [x] Update CLI, UI, callers, fixtures, builds, dependencies, CI, and packaging without interface removals.
- [x] Update Doxygen, usage documentation, website messaging, and release notes without relicensing. No release-notes file exists in the repository; release notes are written at release time. `docs/images/screenshots/libvirtualhid-control-01.png` may still show the removed license panel and needs a fresh capture.
- [ ] Run source/ABI/wire/CLI/package compatibility checks and the supported build/test matrix. Outstanding: only the macOS library, unit tests (before the step 3–5 edits), and macOS broker targets were built locally; per the user, no further test runs were made. Windows broker, fixtures, and tests, the new macOS broker response test, and Linux/FreeBSD/MSVC/MinGW builds are uncompiled.
- [ ] Run fresh/upgrade/offline installed-device and lifecycle checks; record any unavailable validation. Outstanding: no installed-device, upgrade, offline, signing, or hardware checks were run.

## 13. Completion criteria

- Every retained public licensing operation immediately reports successful activated/licensed status without external I/O.
- Updated brokers never gate or revoke devices for licensing, evaluation expiry, or provider outages.
- No licensing-provider traffic, usage reporting, credential interpretation, or credential persistence remains.
- No public interfaces, type layouts, enum values, symbols, wire layouts, CLI commands, installed artifacts, or consumer build interfaces are broken.
- Security, request validation, real backend failures, and normal device lifecycle behavior remain intact.
- Existing legal terms and notices remain unchanged.
- Required compatibility and behavior checks pass; unavailable platform/hardware checks are explicitly recorded as outstanding validation, not claimed as passed.

## Planning status

Steps 1–6 are implemented. Steps 7 and 8 remain outstanding validation; see the checklist notes. `LICENSES/license-map.md` still names the deleted Windows headers and was left unchanged because legal notices are out of scope.
