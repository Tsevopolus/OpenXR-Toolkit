# OpenXR Toolkit — NewKitOnTheBlock Fork

This is a personal, community-shared fork of [OpenXR Toolkit](https://github.com/mbucchia/OpenXR-Toolkit) by Matthieu Bucchianeri, distributed under the same [MIT License](LICENSE). It is **not** an official release of the upstream project.

It started as a fix for a real, reproducible crash and grew into a leaner, D3D12-only build tailored for a Varjo Aero + NVIDIA setup, primarily used with IL-2 Korea and DCS.

**If you're looking for the original, actively-referenced project, go to [mbucchia/OpenXR-Toolkit](https://github.com/mbucchia/OpenXR-Toolkit).** This fork removes support some users may still need (see below) — read this page before installing.

## What's different from upstream

### Removed / not supported in this fork
- **D3D11 is gone entirely** — this build only works with D3D12 titles. The original FW1FontWrapper + D3D11on12 text-rendering interop is replaced with a small, self-contained native D3D12 text renderer (see "Added" below).
- **NVAPI / VRS-over-D3D11 removed** — foveated rendering now relies solely on native D3D12 Tier-2 variable rate shading.
- **HP Omnicept SDK removed.** Pimax/aSeeVR and Meta/FB eye tracking, originally dropped along with it, have since been restored (see "Added" below) — the generic `XR_EXT_eye_gaze_interaction` path (Varjo and other headsets exposing that extension) still works alongside them.
- **Hand tracking** code is still present but disabled by default via config; not actively maintained here.
- The standalone hand-to-controller mapping tool has been removed from the solution.

### Added
- **Native D3D12 text renderer** for the in-VR menu — bakes a GDI font atlas, builds its own pipeline state and vertex batching, replacing the old FW1FontWrapper/D3D11on12 interop path.
- **Bold text support** in the menu — a second, bold-weight glyph atlas so menu text can render in both weights, not just one.
- **Both this fork and an unmodified original 1.3.2 build ship side by side** in the installer. The `companion` app has a dropdown ("OpenXR API layer Version") to switch between them without reinstalling — useful for direct comparison, or as a fallback if something in this fork doesn't work for your setup.
- **Pimax/aSeeVR and Meta/FB eye tracking restored.** Both were ported back in and independently re-verified against third-party SDK usage and documentation rather than trusted as-is; one real bug (a missing `void*` cast on callback function pointers passed to `aSeeVR_register_callback`) was found and fixed in the process. The `PimaxEyeTracker` wrapper was also reworked to drop a back-pointer into shared state that outlived its safe lifetime.

### Updated
- `companion` app dependencies refreshed (Newtonsoft.Json, System.* BCL packages) to close a known high-severity advisory in the bundled JSON library.

### Bugfixes

**Crash fix (the original motivation for this fork):**
- Fixed a crash on mission reload (swapchain re-creation): the Varjo runtime occasionally hands back an `ID3D12Resource*` with an invalid vtable. The root cause was a race condition in the `D3D12Texture` constructor — validity checks and the real `AddRef()` happened at different times. Fixed by performing the `AddRef()` itself under SEH (`__try`/`__except`), closing the window entirely. Verified via PDB+dump analysis and confirmed stable across multiple mission reloads.

**In-VR menu:**
- Fixed a legacy-menu-mode layout/positioning bug, including a layout-reset setting that wasn't being respected correctly.
- Removed dead code (an unused helper and an unused local variable) left over from earlier menu layout logic.

**Robustness / exception-safety hardening pass:**
A multi-round, independently-verified code review (every finding checked against the actual source before anything was changed) turned up and fixed a number of latent crash/leak risks, mostly around error handling at the boundaries where this layer talks to the OpenXR runtime and to Direct3D:

- `xrCreateSwapchain()` could leak a live OpenXR swapchain handle on the runtime side, and let a C++ exception unwind straight across the OpenXR loader's dispatch table (undefined behavior), if any step of wrapping the runtime's swapchain images failed. Same class of issue existed in the menu-swapchain creation path, screenshot capture, and the text renderer's lazy pipeline setup — all now fail safely (clean error return, menu disabled for the session, or the current frame's screenshot/text skipped) instead of crashing, leaking, or corrupting state.
- The GPU timer allocator could silently read/write out of bounds once its fixed-size buffer was exhausted — now throws a catchable error instead.
- An internal event-dispatch macro could permanently disable all further event handling (menu, frame analyzer, variable rate shading) for the rest of the session if a single event handler threw once — fixed so cleanup always runs.
- Several D3D12 resource lookups had a check-then-use race where the game could destroy a resource in the gap between validating it and using it — closed with SEH-guarded safe accessors.
- `D3D12Device::beginText()` could null-dereference if called before any render target was bound.
- A texture-upload helper reused a stale buffer size across calls, risking an over-read from the caller's data.
- Various unchecked Win32/GDI calls in the font-atlas baking code (`CreateFontW`, `CreateCompatibleDC`, `CreateDIBSection`, `SelectObject`) could crash or silently produce garbled menu text on failure — now checked, with cleanup and a loud failure instead.
- A handful of smaller correctness fixes (an off-by-one in a lookup index, a stale enum default) and documentation-only comments clarifying subtle-but-not-actually-buggy logic (e.g. a registry-watcher race in the config manager, a shader blend-mode interaction in the variable-rate-shading pass).

**Loader/dispatch layer hardening:** a separate, multi-round review of the OpenXR loader entry points and the generated dispatch code (`dispatch.cpp`, `entry.cpp`, `dispatch.gen.cpp`/`.h`, and the code generator that produces them) found and fixed several issues, most significantly:
- A deadlock risk where a one-time runtime-capability probe held a mutex across a call back into the upstream API layer chain — a layer that creates its own bootstrap instance during that window would have deadlocked this layer's loader entry point on re-entry. The probe now runs lock-free with a double-checked publish step instead.
- An uncaught C++ exception from that same probe could unwind straight across the loader's C-ABI boundary (`xrCreateApiLayerInstance`), which is undefined behavior and would abort the whole process instead of just failing the probe gracefully; now caught and handled in place.
- A stale global "bypass this instance entirely" flag that wasn't reset between instance creations, and later wasn't synchronized against concurrent reads — now reset every call and backed by an atomic.
- A Vive-specific workaround that deliberately leaks a bootstrap `XrInstance` (the runtime doesn't like it being destroyed mid-init) used to leak one per `xrCreateInstance()` call; now cached and leaked at most once per process (per upstream API layer chain), with the leaked handle logged for diagnostics.
- Missing `XRAPI_CALL` calling-convention annotations on the loader entry points, which would have silently mismatched the calling convention (and corrupted the stack) on a Win32 build.

**Screenshot capture (`ScreenGrab12.cpp`):** the D3D12 MSAA screenshot path was missing the resource-state transitions `ResolveSubresource()` requires (source into `RESOLVE_SOURCE`, destination into `RESOLVE_DEST`), and transitioned the wrong resource before the subsequent copy — would have produced D3D12 validation errors and incorrect/corrupted screenshots on any MSAA render target. Fixed and verified against the upstream DirectXTK12 implementation; a handful of related correctness fixes (a use-after-const_cast, a 64-bit mask truncation, missing null checks) went in alongside it.

None of the hardening work changes what the toolkit does for the user — it closes crash/leak/undefined-behavior paths that existed in the original error handling, without touching intended behavior.

## Should you use this fork?

If you're on D3D12, this fork should now cover close to the same ground as upstream — eye tracking (Pimax/aSeeVR, Meta/FB, and the generic `XR_EXT_eye_gaze_interaction` path) is back, on top of the crash fixes, the native D3D12 text renderer, and the hardening work described above. The main thing still missing is D3D11 support, and hand tracking is present but unmaintained here.

You don't have to choose blind, either: the installer puts both this fork and an unmodified original 1.3.2 build side by side, and the `companion` app's "OpenXR API layer Version" dropdown switches between them without reinstalling — so you can run this fork day to day and flip back to stock upstream for a specific title or to compare, with no separate setup. If you rely on D3D11 titles as your primary use case, upstream (or the bundled 1.3.2 build) is still the simpler default.

## Credits

All original design, engineering, and the vast majority of the code are the work of [Matthieu Bucchianeri](https://github.com/mbucchia) and contributors to the upstream project. See [THIRD_PARTY](XR_APILAYER_NEWKITONTHEBLOCK_toolkit/THIRD_PARTY) for third-party component licenses bundled with the toolkit.

## License

MIT — see [LICENSE](LICENSE). This fork carries the same license as upstream; the original copyright notices are preserved throughout.
