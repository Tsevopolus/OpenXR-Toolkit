# OpenXR Toolkit — NewKitOnTheBlock Fork

This is a personal, community-shared fork of [OpenXR Toolkit](https://github.com/mbucchia/OpenXR-Toolkit) by Matthieu Bucchianeri, distributed under the same [MIT License](LICENSE). It is **not** an official release of the upstream project.

It started as a fix for a real, reproducible crash and grew into a leaner, D3D12-only build tailored for a Varjo Aero + NVIDIA setup, primarily used with IL-2 Korea and DCS.

**If you're looking for the original, actively-referenced project, go to [mbucchia/OpenXR-Toolkit](https://github.com/mbucchia/OpenXR-Toolkit).** This fork removes support some users may still need (see below) — read this page before installing.

## What's different from upstream

### Removed / not supported in this fork
- **D3D11 is gone entirely** — this build only works with D3D12 titles. The original FW1FontWrapper + D3D11on12 text-rendering interop is replaced with a small, self-contained native D3D12 text renderer (see "Added" below).
- **NVAPI / VRS-over-D3D11 removed** — foveated rendering now relies solely on native D3D12 Tier-2 variable rate shading.
- **Third-party eye-tracker SDKs removed**: HP Omnicept, Pimax/aSeeVR, Meta/FB. Only the generic `XR_EXT_eye_gaze_interaction` path remains (works with Varjo and other headsets exposing that extension).
- **Hand tracking** code is still present but disabled by default via config; not actively maintained here.
- The standalone hand-to-controller mapping tool has been removed from the solution.

### Added
- **Native D3D12 text renderer** for the in-VR menu — bakes a GDI font atlas, builds its own pipeline state and vertex batching, replacing the old FW1FontWrapper/D3D11on12 interop path.
- **Bold text support** in the menu — a second, bold-weight glyph atlas so menu text can render in both weights, not just one.
- **Both this fork and an unmodified original 1.3.2 build ship side by side** in the installer. The `companion` app has a dropdown ("OpenXR API layer Version") to switch between them without reinstalling — useful for direct comparison, or as a fallback if something in this fork doesn't work for your setup.

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

None of the hardening work changes what the toolkit does for the user — it closes crash/leak/undefined-behavior paths that existed in the original error handling, without touching intended behavior.

## Should you use this fork?

Probably not, unless you specifically want the D3D12-only, Varjo-oriented, slimmed-down build described above. If you use D3D11 titles, hand tracking, or a headset relying on one of the removed eye-tracker SDKs, **use the upstream project instead**.

## Credits

All original design, engineering, and the vast majority of the code are the work of [Matthieu Bucchianeri](https://github.com/mbucchia) and contributors to the upstream project. See [THIRD_PARTY](XR_APILAYER_NEWKITONTHEBLOCK_toolkit/THIRD_PARTY) for third-party component licenses bundled with the toolkit.

## License

MIT — see [LICENSE](LICENSE). This fork carries the same license as upstream; the original copyright notices are preserved throughout.
