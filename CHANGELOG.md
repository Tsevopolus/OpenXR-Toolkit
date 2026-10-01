# Changelog

All notable changes to this fork are documented here. Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [v1.4.3] – 2026-10-01

### Added
- Pimax/aSeeVR and Meta/FB eye tracking restored (previously removed in v1.4.2). Both were ported back in and independently re-verified against third-party SDK usage and documentation rather than trusted as-is, alongside the generic `XR_EXT_eye_gaze_interaction` path.

### Fixed
- **aSeeVR callback cast**: a missing `void*` cast on callback function pointers passed to `aSeeVR_register_callback`, given the real SDK signature and this project's `/permissive-` setting.
- **PimaxEyeTracker lifetime & ordering**: dropped a `tracker` back-pointer out of `SharedState` that could outlive its safe lifetime; `aSeeVR_start` moved outside the lock; tracker construction switched to `make_unique` + `release()`; fixed a dismiss-ordering issue and a legacy-mode double-stamp of eye-gaze state; added a null-check in the state callback.
- **Loader/dispatch deadlock risk**: a one-time runtime-capability probe in `xrCreateApiLayerInstance()` held a mutex across a call back into the upstream API layer chain — an upstream layer that creates its own bootstrap instance during that window would have deadlocked this layer's loader entry point on re-entry. The probe now runs lock-free with a double-checked publish step.
- **Loader/dispatch exception safety**: an uncaught C++ exception from that same probe could unwind straight across the loader's C-ABI boundary (undefined behavior, process abort) instead of failing the probe gracefully; now caught and handled in place without discarding an already-successful result.
- **`g_bypass` data race**: the process-global flag that lets the layer step aside entirely for certain apps wasn't reset between instance creations and wasn't synchronized against concurrent reads; now reset on every call and backed by `std::atomic`.
- **Vive bootstrap-instance leak**: the Vive mid-init-destroy workaround deliberately leaks a bootstrap `XrInstance`; this used to leak one per `xrCreateInstance()` call, now cached and leaked at most once per process (per upstream API layer chain), with the leaked handle logged for diagnostics.
- **Missing `XRAPI_CALL` annotations**: the loader entry points were missing the calling-convention macro, which would have silently mismatched the calling convention (and corrupted the stack) on a Win32 build.
- **MSAA screenshot capture** (`ScreenGrab12.cpp`): the D3D12 MSAA screenshot path was missing the resource-state transitions `ResolveSubresource()` requires (source into `RESOLVE_SOURCE`, destination into `RESOLVE_DEST`), and transitioned the wrong resource before the subsequent copy — would have produced D3D12 validation errors and corrupted screenshots on any MSAA render target. Fixed and verified against the upstream DirectXTK12 implementation.
- A handful of related correctness fixes alongside the screenshot fix: a `const_cast`-induced undefined behavior, a 64-bit mask truncation, missing null checks on the public entry points.

### Changed
- `version.info` bumped to 1.4.3; installer version label and `companion` `AssemblyInfo` updated to match.

### Notes
Like v1.4.2, this release doesn't change intended toolkit behavior beyond restoring the Pimax/aSeeVR and Meta/FB eye trackers — the rest is a robustness/exception-safety pass, this time in the OpenXR loader/dispatch layer and the screenshot capture path.

## [v1.4.2] – 2026-09-19

### Removed
- D3D11 support (D3D12-only from here on).
- Third-party eye-tracker SDKs (HP Omnicept, Pimax/aSeeVR, Meta/FB) — only the generic XR_EXT_eye_gaze_interaction path remains.
- NVAPI / VRS-over-D3D11.
- Hand-to-controller mapping tool.

### Added
- Bold text support in the in-VR menu (new bold-weight glyph atlas in the native D3D12 text renderer)
- Separate install/uninstall scripts for the original (`*-MBUCCHIA.ps1`) and this fork (`*-NEWKITONTHEBLOCK.ps1`)

### Fixed
- **Swapchain leak & undefined behavior**: `xrCreateSwapchain()` could leak a live OpenXR swapchain handle on the runtime side, and let a C++ exception unwind across the OpenXR loader's dispatch table, if wrapping the runtime's swapchain images failed partway through. Same class of issue fixed in the menu-swapchain creation path, screenshot capture, and the text renderer's pipeline setup — all now fail safely instead of crashing, leaking, or corrupting state.
- **GPU timer buffer overflow**: the GPU timer allocator could silently read/write out of bounds once exhausted; now throws a catchable error instead.
- **Stuck event dispatch**: a single throwing event handler could permanently disable all further menu/frame-analyzer/VRS event handling for the rest of the session.
- **Resource lifetime races**: closed a check-then-use race in several D3D12 resource lookups where the game could destroy a resource between validation and use.
- **Null-deref guard**: `D3D12Device::beginText()` could crash if called before any render target was bound.
- **Font atlas hardening**: unchecked Win32/GDI calls (`CreateFontW`, `CreateCompatibleDC`, `CreateDIBSection`, `SelectObject`) in the menu font-atlas baking could crash or silently produce garbled text on failure.
- Legacy menu mode layout/positioning bug fixed.
- Fixed a texture-upload helper reusing a stale buffer size across calls.

### Changed
- `companion` app dependencies refreshed (Newtonsoft.Json, System.* BCL packages) to close a known high-severity advisory.
- Minor code cleanup: removed dead code in `menu.cpp`, off-by-one fix in a lookup table index.

### Notes
This release does not change intended toolkit behavior — it's primarily a robustness/exception-safety pass closing crash and leak paths in error handling around the OpenXR loader and Direct3D 12, plus the bold-text feature and script split.
