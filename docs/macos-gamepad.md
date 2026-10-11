# macOS virtual gamepads

macOS gamepads use an installed broker with Apple's virtual HID entitlement.
The C++ library uses the broker for virtual gamepads and CoreGraphics for
keyboard and mouse input. No license, account, or internet connection is
needed to create a gamepad.
All built-in gamepad profiles are accepted, though individual games may map
them differently.

## Install

Install the signed, notarized DMG from the
[libvirtualhid releases](https://github.com/LizardByte/libvirtualhid/releases).
Mount it and open **Install libvirtualhid.command**. The installer requests
administrator authorization, installs **Virtual HID Broker** and
**Virtual HID Control** in Applications, and starts the broker service.

In **System Settings > Privacy & Security > Device Control and Data Access**
(**Accessibility** on older macOS versions), add
`/Applications/VirtualHIDBroker.app`. The broker runs as a system service and
cannot display the permission prompt while creating a gamepad. Review the
signed application before granting this broad device-control permission.

Open **Virtual HID Control** from Applications to create and inspect a test
controller. The host application runs in the normal user session; the broker
runs as a LaunchDaemon.

The `/usr/local/bin/libvirtualhid-license` command remains for scripts. Its
`status`, `activate [machine-name]`, `validate`, and `deactivate` commands no
longer prompt for a key, always report `Activated` with plan `Unrestricted`,
and exit with status 0. Upgrade the installed broker as well as applications:
an older broker still enforces its previous license checks.

## Troubleshoot

If creation reports `license_required`, an older broker is still installed;
install the current DMG.
For `backend_unavailable`, check the installed broker service:

```sh
sudo launchctl print system/dev.lizardbyte.app.libvirtualhid
```

For `backend_failure`, check the macOS permission above. If it persists,
verify the broker signature and embedded provisioning profile. A local
unsigned build can test library and broker communication but cannot prove
virtual HID creation. Check the controller in the intended macOS game or
streaming client; enumeration alone does not establish compatibility.

## Build and package

A contributor build requires Xcode and CMake:

```sh
cmake -S . -B cmake-build-macos-universal \
  -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-macos-universal
./cmake-build-macos-universal/tests/test_libvirtualhid
```

For a signed local installation, copy `.env.example` to an untracked `.env`
and provide the Apple Developer ID certificate, an approved provisioning
profile for `dev.lizardbyte.app.libvirtualhid` with
`com.apple.developer.hid.virtual.device`, and notarization credentials. Then
run `bash scripts/macos/build-and-install.sh`. Use `--package-only` to produce
a signed DMG without installing it. Keep signing credentials local. The
broker's restricted entitlement is required even for debug builds.

The DMG contains the MIT library and the LB-SAL broker; releases must retain
both license texts.
