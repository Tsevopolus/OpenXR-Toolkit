# OpenXR Toolkit — NewKitOnTheBlock Fork

This is a personal, community-shared fork of [OpenXR Toolkit](https://github.com/mbucchia/OpenXR-Toolkit) by Matthieu Bucchianeri, distributed under the same [MIT License](LICENSE). It is **not** an official release of the upstream project.

It started as a fix for a real, reproducible crash and grew into a leaner, D3D12-only build tailored for a Varjo Aero + NVIDIA setup, primarily used with IL-2 Korea and DCS.

**If you're looking for the original, actively-referenced project, go to [mbucchia/OpenXR-Toolkit](https://github.com/mbucchia/OpenXR-Toolkit).** This fork removes support some users may still need (see below) — read this page before installing.

## What's different from upstream

### Bugfix
- Fixed a crash on mission reload (swapchain re-creation): the Varjo runtime occasionally hands back an `ID3D12Resource*` with an invalid vtable. The root cause was a race condition in the `D3D12Texture` constructor — validity checks and the real `AddRef()` happened at different times. Fixed by performing the `AddRef()` itself under SEH (`__try`/`__except`), closing the window entirely. Verified via PDB+dump analysis and confirmed stable across multiple mission reloads.

### Removed / not supported in this fork
- **D3D11 is gone entirely** — this build only works with D3D12 titles. The original FW1FontWrapper + D3D11on12 text-rendering interop is replaced with a small, self-contained native D3D12 text renderer.
- **NVAPI / VRS-over-D3D11 removed** — foveated rendering now relies solely on native D3D12 Tier-2 variable rate shading.
- **Third-party eye-tracker SDKs removed**: HP Omnicept, Pimax/aSeeVR, Meta/FB. Only the generic `XR_EXT_eye_gaze_interaction` path remains (works with Varjo and other headsets exposing that extension).
- **Hand tracking** code is still present but disabled by default via config; not actively maintained here.
- The standalone hand-to-controller mapping tool has been removed from the solution.

### Updated
- `companion` app dependencies refreshed (Newtonsoft.Json, System.* BCL packages) to close a known high-severity advisory in the bundled JSON library.

### Added
- **Both this fork and an unmodified original 1.3.2 build ship side by side** in the installer. The `companion` app has a dropdown ("OpenXR API layer Version") to switch between them without reinstalling — useful for direct comparison, or as a fallback if something in this fork doesn't work for your setup.

## Should you use this fork?

Probably not, unless you specifically want the D3D12-only, Varjo-oriented, slimmed-down build described above. If you use D3D11 titles, hand tracking, or a headset relying on one of the removed eye-tracker SDKs, **use the upstream project instead**.

## Credits

All original design, engineering, and the vast majority of the code are the work of [Matthieu Bucchianeri](https://github.com/mbucchia) and contributors to the upstream project. See [THIRD_PARTY](XR_APILAYER_NEWKITONTHEBLOCK_toolkit/THIRD_PARTY) for third-party component licenses bundled with the toolkit.

## License

MIT — see [LICENSE](LICENSE). This fork carries the same license as upstream; the original copyright notices are preserved throughout.
