// MIT License
//
// Copyright(c) 2021 Matthieu Bucchianeri
// Copyright(c) 2026 Tsevopolus
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>

#include <layer.h>

#include "dispatch.h"
#include "factories.h"
#include "log.h"

#ifndef LAYER_NAMESPACE
#error Must define LAYER_NAMESPACE
#endif

using namespace LAYER_NAMESPACE::log;

namespace LAYER_NAMESPACE {

    // atomic rather than a plain global: xrGetInstanceProcAddr() (read, below) can be called from
    // another thread concurrently with xrCreateApiLayerInstance() (write, in this function) in
    // unusual setups - e.g. a background thread warming up function pointers while the main
    // thread creates an instance. Function pointers are lock-free atomic on every mainstream
    // x86/ARM target, so this costs nothing on the hot path. relaxed ordering throughout: this
    // value is used purely as itself (a function pointer to call, or not) - nothing else is
    // published through it that a reader would need synchronized, so the default seq_cst would
    // just be unnecessary cost without a correctness benefit here.
    std::atomic<PFN_xrGetInstanceProcAddr> g_bypass{nullptr};

    // Entry point for creating the layer.
    XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* const instanceCreateInfo,
                                                 const struct XrApiLayerCreateInfo* const apiLayerInfo,
                                                 XrInstance* const instance) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrCreateApiLayerInstance");

        if (!apiLayerInfo || apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
            apiLayerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
            apiLayerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !apiLayerInfo->nextInfo ||
            apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
            apiLayerInfo->nextInfo->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
            apiLayerInfo->nextInfo->structSize != sizeof(XrApiLayerNextInfo) ||
            apiLayerInfo->nextInfo->layerName != LayerName || !apiLayerInfo->nextInfo->nextGetInstanceProcAddr ||
            !apiLayerInfo->nextInfo->nextCreateApiLayerInstance) {
            Log("xrCreateApiLayerInstance validation failed\n");
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        // g_bypass is process-global and reflects only the most recent bypass decision below, so
        // re-evaluate it fresh on every xrCreateApiLayerInstance() call rather than leaving a
        // stale value from a previous (possibly already-destroyed) instance in place: if an
        // application creates one instance that gets bypassed (e.g. engine name "Chromium") and
        // later creates another, non-bypassed instance (same or different applicationName), a
        // leftover g_bypass would make the free xrGetInstanceProcAddr() below keep forwarding
        // straight to the runtime for the new instance too, silently disabling interception for
        // it entirely.
        g_bypass.store(nullptr, std::memory_order_relaxed);

        // Determine if we should entirely bypass the layer for this application.
        {
            std::string baseKey = RegPrefix + "\\" + instanceCreateInfo->applicationInfo.applicationName;

            // Always create a key to make each application name easy to find, and let the user add the bypass key
            // manually.
            {
                char path[_MAX_PATH];
                // GetModuleFileNameA truncates and returns the buffer's size (not 0) when the
                // path doesn't fit, with no terminating NUL guaranteed in that case - write
                // nothing rather than risk a non-terminated or silently-truncated path going into
                // the registry (truncation here just means a less helpful "module" value for the
                // user to look up, not a functional failure, so skipping the write is enough).
                const DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
                if (length > 0 && length < sizeof(path)) {
                    LAYER_NAMESPACE::utilities::RegSetString(
                        HKEY_CURRENT_USER, xr::utf8_to_wide(baseKey), L"module", path);
                }
            }

            const std::string_view engineName(instanceCreateInfo->applicationInfo.engineName);

            // Bypass the layer if it's either in the no-no list, or if the user requests it.
            const bool bypassLayer =
                engineName == "Chromium" ||
                (LAYER_NAMESPACE::utilities::RegGetDword(HKEY_CURRENT_USER, xr::utf8_to_wide(baseKey), L"bypass")
                     .value_or(0));
            if (bypassLayer) {
                Log("Bypassing OpenXR Toolkit for application '%s', engine '%s'\n",
                    instanceCreateInfo->applicationInfo.applicationName,
                    instanceCreateInfo->applicationInfo.engineName);

                // Bypass interception of xrGetInstanceProcAddr() calls. g_bypass is reset at the
                // top of this function on every call, so a later, non-bypassed instance-create
                // correctly clears this again (see the comment there). This still assumes at
                // most one XrInstance is active at a time, which holds for every known caller of
                // this layer; a process that legitimately holds two *simultaneous* instances with
                // different bypass decisions is not supported (g_bypass has no way to tell them
                // apart), but that scenario is not known to occur in practice.
                g_bypass.store(apiLayerInfo->nextInfo->nextGetInstanceProcAddr, std::memory_order_relaxed);

                // Call the chain to create the instance, and nothing else.
                XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
                chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;
                return apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                    instanceCreateInfo, &chainApiLayerInfo, instance);
            }
        }

        // Determine whether we are invoked from the OpenXR Developer Tools for Windows Mixed Reality.
        // If we are, we will skip dummy instance create to avoid he XR_LIMIT_REACHED error.
        // string_view here (not std::string) for the same reason as engineName above: avoid an
        // unnecessary heap allocation on every xrCreateApiLayerInstance() call just to compare
        // against a literal.
        const bool fastInitialization =
            std::string_view(instanceCreateInfo->applicationInfo.engineName) == "OpenXRDeveloperTools";

        // The extensions the application already requested itself. Used below so we never add a
        // duplicate of one of these to extensionsToRequest: the OpenXR spec doesn't define what a
        // runtime should do with a repeated extension name in enabledExtensionNames, and while
        // most runtimes tolerate it, there's no reason to rely on that.
        // (Looked up below with a std::string& key, so a transparent std::less<> comparator here
        // wouldn't actually save anything - left out rather than implying an optimization that
        // isn't happening.)
        const std::set<std::string> appEnabledExtensions(
            instanceCreateInfo->enabledExtensionNames,
            instanceCreateInfo->enabledExtensionNames + instanceCreateInfo->enabledExtensionCount);

        // Check that the extensions we need are supported by the runtime and/or an upstream API layer.
        //
        // Workaround: per specification, we should be able to retrive the pointer to
        // xrEnumerateInstanceExtensionProperties() without an XrInstance. However, some API layers (eg: Ultraleap) do
        // not seem to properly handle this case. So we create a dummy instance.
        //
        // This result only depends on the runtime and the current upstream API layer chain, not
        // on anything from this particular instanceCreateInfo, so we cache it process-wide after
        // the first successful probe and skip the dummy-instance dance entirely on any later
        // xrCreateApiLayerInstance() call in the same process whose chain matches.
        //
        // The chain is not actually process-wide-static: a process that swaps API layers between
        // calls (a plugin system, or a test harness) can see a different chain on a later call, so
        // the cached result is keyed on the chain and re-probed on a mismatch rather than trusted
        // unconditionally.
        //
        // Threading: every read and write of cachedDetectedExtensions/cachedLayerChain happens
        // under cacheMutex (the magic-statics initialization of the statics themselves is already
        // thread-safe, but the reads/assignments after that are not). The *probe itself* - dummy
        // instance creation plus xrGetSystem/xrGetSystemProperties - deliberately runs without
        // holding that lock (see the comment at the probe below for why); cacheMutex is only ever
        // held for the quick cache-check and the quick publish-result steps around it. On a cold
        // cache this means multiple threads can probe concurrently and redundantly - each creating
        // its own dummy instance - but every caller still converges on one published result per
        // chain (see the publish step below). On Vive specifically, this also means the mid-init-
        // destroy workaround's intentional leak is no longer bounded at exactly one dummy instance
        // per process under concurrent cold-cache calls, though it remains bounded by the number of
        // threads that raced to probe, not by every xrCreateInstance() call the application makes.
        static std::mutex cacheMutex;
        static std::optional<std::set<std::string>> cachedDetectedExtensions;
        static std::vector<std::string> cachedLayerChain;

        // The chain of upstream API layers for this call (skipping ourselves, the first entry),
        // used both as the cache validation key above and for the Ultraleap-detection workaround
        // below.
        std::vector<std::string> currentLayerChain;
        {
            auto info = apiLayerInfo->nextInfo;
            while (info && info->next) {
                currentLayerChain.emplace_back(info->next->layerName);
                info = info->next;
            }
        }

        std::set<std::string> detectedExtensions;
        if (!fastInitialization) {
            bool useCached = false;
            {
                std::lock_guard<std::mutex> cacheLock(cacheMutex);
                if (cachedDetectedExtensions && cachedLayerChain == currentLayerChain) {
                    detectedExtensions = *cachedDetectedExtensions;
                    useCached = true;
                }
            }

            if (!useCached) {
                // Deliberately probe without holding cacheMutex: this calls back into the
                // upstream API layer chain and the runtime (instance creation, xrGetSystem,
                // xrGetSystemProperties), which can take tens to hundreds of milliseconds and,
                // for an upstream layer that creates its own bootstrap instance, can re-enter
                // this very function on the same thread. Holding a non-recursive mutex across
                // that window would either deadlock on such re-entrancy or needlessly block
                // every other thread trying to create an instance for the full probe duration.
                // On a cold cache, two threads can therefore end up probing concurrently and
                // redundantly (each creating its own dummy instance); that is preferable to a
                // deadlock, and the publish step below ensures every caller still converges on
                // one consistent result per chain.
                // Declared outside the try below so the destroy/leak-logging step after it (and
                // the catch itself) can still see whatever got resolved before a throw.
                XrInstance dummyInstance = XR_NULL_HANDLE;
                PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
                PFN_xrGetSystem xrGetSystem = nullptr;
                PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
                PFN_xrDestroyInstance xrDestroyInstance = nullptr;
                bool probeSucceeded = false;

                try {
                    // CHECK_XRCMD below throws std::runtime_error on failure. This whole probe
                    // runs inside xrCreateApiLayerInstance(), a C-ABI entry point the loader calls
                    // directly - an uncaught exception here would unwind straight through that
                    // boundary (undefined behavior, std::terminate in practice on MSVC) instead of
                    // failing gracefully. The layer is still fully functional without the extra
                    // extensions this probe looks for, so a failure here is treated the same as
                    // "runtime doesn't support them" (probeSucceeded stays false) rather than
                    // aborting the whole instance creation.
                    // Try to speed things up by requesting no extensions.
                    XrInstanceCreateInfo dummyCreateInfo = *instanceCreateInfo;
                    dummyCreateInfo.enabledExtensionCount = dummyCreateInfo.enabledApiLayerCount = 0;

                    {
                        // Workaround: the Ultraleap API layer does not seem to properly enumerate the XR_EXT_hand_tracking
                        // extension when invoked from within another API layer. We assume the extension is present if we see
                        // the API layer.

                        for (const auto& layerName : currentLayerChain) {
                            TraceLoggingWriteTagged(
                                local, "xrCreateApiLayerInstance_UseLayer", TLArg(layerName.c_str(), "Layer"));
                            Log("Using layer: %s\n", layerName.c_str());

                            if (layerName == "XR_APILAYER_ULTRALEAP_hand_tracking") {
                                // Assume hand tracking extension is present.
                                detectedExtensions.insert("XR_EXT_hand_tracking");
                            }
                        }
                    }

                    // Call the chain to create the dummy instance.
                    XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
                    chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;

                    TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceCreate");
                    const XrResult result = apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                        &dummyCreateInfo, &chainApiLayerInfo, &dummyInstance);
                    if (result == XR_SUCCESS) {
                        TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceCreated");

                        CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                            dummyInstance,
                            "xrEnumerateInstanceExtensionProperties",
                            reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties)));
                        CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                            dummyInstance, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystem)));
                        CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                            dummyInstance,
                            "xrGetSystemProperties",
                            reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystemProperties)));
                        CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                            dummyInstance,
                            "xrDestroyInstance",
                            reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance)));

                        TraceLoggingWriteTagged(
                            local,
                            "xrCreateApiLayerInstance_DummyInstanceProcAddr",
                            TLPArg(xrEnumerateInstanceExtensionProperties, "xrEnumerateInstanceExtensionProperties"),
                            TLPArg(xrGetSystem, "xrGetSystem"),
                            TLPArg(xrGetSystemProperties, "xrGetSystemProperties"),
                            TLPArg(xrDestroyInstance, "xrDestroyInstance"));
                    } else {
                        TraceLoggingWriteTagged(
                            local, "xrCreateApiLayerInstance_Error_CreateInstance", TLArg((int)result, "Result"));
                        Log("Failed to create bootstrap instance: %d\n", result);
                    }

                    if (xrEnumerateInstanceExtensionProperties) {
                        uint32_t extensionsCount = 0;
                        CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionsCount, nullptr));
                        std::vector<XrExtensionProperties> extensions(extensionsCount, {XR_TYPE_EXTENSION_PROPERTIES});
                        CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(
                            nullptr, extensionsCount, &extensionsCount, extensions.data()));
                        for (auto extension : extensions) {
                            const std::string extensionName(extension.extensionName);

                            TraceLoggingWriteTagged(local,
                                                    "xrCreateApiLayerInstance_HasExtension",
                                                    TLArg(extension.extensionName, "Extension"));
                            Log("Runtime supports extension: %s\n", extension.extensionName);
                            if (extensionName == "XR_EXT_hand_tracking" ||
                                extensionName == "XR_EXT_eye_gaze_interaction" ||
                                extensionName == "XR_KHR_win32_convert_performance_counter_time" ||
                                extensionName == "XR_KHR_visibility_mask" ||
                                extensionName == "XR_FB_eye_tracking_social") {
                                detectedExtensions.insert(extensionName);
                            }
                        }
                        probeSucceeded = true;
                    } else {
                        Log("Failed to query extensions\n");
                    }

                    // Workaround: the Vive runtime does not seem to like our flow of destroying the instance
                    // mid-initialization. We skip destruction and we will just create a second instance.
                    if (xrGetSystem && xrGetSystemProperties) {
                        XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO};
                        getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                        XrSystemId systemId;
                        if (XR_SUCCEEDED(xrGetSystem(dummyInstance, &getInfo, &systemId))) {
                            XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                            CHECK_XRCMD(xrGetSystemProperties(dummyInstance, systemId, &systemProperties));
                            // The spec requires systemName to be NUL-terminated, but it's a
                            // fixed-size char[] filled in by the runtime - bound the read rather than
                            // trusting that unconditionally, so a non-conforming runtime can't make us
                            // read past the array. std::find over the known-size array (standard C++,
                            // not POSIX/C11 strnlen) stops at the first NUL or at the array's end,
                            // whichever comes first.
                            const char* const systemNameEnd = std::find(std::begin(systemProperties.systemName),
                                                                         std::end(systemProperties.systemName),
                                                                         '\0');
                            const std::string_view systemName(systemProperties.systemName,
                                                               systemNameEnd - systemProperties.systemName);
                            if (systemName.find("Vive Reality system") != std::string_view::npos) {
                                Log("Detected Vive runtime\n");
                                xrDestroyInstance = nullptr;
                            }
                        }
                    }
                } catch (const std::exception& exc) {
                    TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_ProbeError", TLArg(exc.what(), "Error"));
                    Log("Dummy instance probe failed, continuing without the extra extensions it looks for: %s\n",
                        exc.what());
                    // probeSucceeded intentionally left as-is here (not reset to false): it tracks
                    // only whether the extension list itself was fully enumerated, further up in
                    // this try block - not whether the unrelated Vive-detection step afterwards
                    // (which only gates destroy-vs-leak for the dummy instance, and doesn't touch
                    // detectedExtensions) also ran without throwing. A throw from that later step
                    // must not discard an extension list the probe already obtained successfully;
                    // it's already false if the throw happened before the list was obtained.
                }

                // Runs whether or not the try above threw, so a throw mid-probe doesn't silently
                // leak dummyInstance without even the diagnostic log below - it destroys (or logs
                // the leak of) whatever got resolved before the throw, same as the non-throwing path.
                if (xrDestroyInstance) {
                    TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceDestroy");
                    xrDestroyInstance(dummyInstance);
                    TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_DummyInstanceDestroyed");
                } else if (dummyInstance != XR_NULL_HANDLE) {
                    // We intentionally did not destroy dummyInstance above (Vive workaround, or we
                    // never got a valid xrDestroyInstance pointer to begin with). Log the leaked
                    // handle explicitly so support can correlate it against runtime-side resource
                    // reports; there is currently no way to release it later.
                    Log("Leaking dummy bootstrap instance %p for process lifetime\n",
                        reinterpret_cast<void*>(dummyInstance));
                }

                // Publish the result. A chain match here means another thread already published
                // for this chain while we were probing lock-free - adopt its result unconditionally
                // (even if our own probe failed) so every caller converges on one consistent answer
                // per chain, rather than some callers keeping a possibly-incomplete local result
                // just because they lost the race. Only write our own result when no one else beat
                // us to it, and only when our own probe actually succeeded - caching a failure (e.g.
                // a transient dummy-instance-creation error) would otherwise make that failure
                // permanent for the rest of the process, since a later call with the same chain
                // would just replay the cached miss.
                std::lock_guard<std::mutex> cacheLock(cacheMutex);
                if (cachedDetectedExtensions && cachedLayerChain == currentLayerChain) {
                    detectedExtensions = *cachedDetectedExtensions;
                } else if (probeSucceeded) {
                    cachedDetectedExtensions = detectedExtensions;
                    cachedLayerChain = currentLayerChain;
                }
            }
        }

        // Extensions to add to this particular instance's request: whatever we detected (from
        // cache or freshly probed above), minus anything the application already enabled itself.
        // Gated under !fastInitialization too: when fastInitialization is set, detectedExtensions
        // is always empty, so this would otherwise be dead work on every such call.
        std::set<std::string> extensionsToRequest;
        if (!fastInitialization) {
            for (const auto& extensionName : detectedExtensions) {
                if (!appEnabledExtensions.count(extensionName)) {
                    extensionsToRequest.insert(extensionName);
                }
            }
        }

        // Add the extra extensions to the list of requested extensions when available.
        XrInstanceCreateInfo chainInstanceCreateInfo = *instanceCreateInfo;
        std::vector<const char*> newEnabledExtensionNames;
        if (!fastInitialization) {
            if (!extensionsToRequest.empty()) {
                chainInstanceCreateInfo.enabledExtensionCount += (uint32_t)extensionsToRequest.size();

                newEnabledExtensionNames.resize(chainInstanceCreateInfo.enabledExtensionCount);
                chainInstanceCreateInfo.enabledExtensionNames = newEnabledExtensionNames.data();
                memcpy(newEnabledExtensionNames.data(),
                       instanceCreateInfo->enabledExtensionNames,
                       instanceCreateInfo->enabledExtensionCount * sizeof(const char*));
                uint32_t nextExtensionSlot = instanceCreateInfo->enabledExtensionCount;

                for (auto& extension : extensionsToRequest) {
                    newEnabledExtensionNames[nextExtensionSlot++] = extension.c_str();
                    Log("Requesting extra extension: %s\n", extension.c_str());
                }
            }
        }

        for (uint32_t i = 0; i < chainInstanceCreateInfo.enabledExtensionCount; i++) {
            TraceLoggingWriteTagged(local,
                                    "xrCreateApiLayerInstance_UseExtension",
                                    TLArg(chainInstanceCreateInfo.enabledExtensionNames[i], "Extension"));
        }

        // Call the chain to create the instance.
        XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
        chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;
        TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_RealInstanceCreate");
        XrResult result =
            apiLayerInfo->nextInfo->nextCreateApiLayerInstance(&chainInstanceCreateInfo, &chainApiLayerInfo, instance);
        if (result == XR_SUCCESS) {
            TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_RealInstanceCreated");

            // Create our layer.
            LAYER_NAMESPACE::GetInstance()->SetGetInstanceProcAddr(apiLayerInfo->nextInfo->nextGetInstanceProcAddr,
                                                                   *instance);

            // Record the other layers being used here. This is useful when evaluating features based not just on
            // XrInstanceCreateInfo. currentLayerChain (built above, skipping ourselves the same
            // way this used to) already holds exactly this list, so reuse it instead of walking
            // apiLayerInfo->nextInfo a second time with a second, slightly different-looking loop
            // - not used after this point, so move it rather than copy.
            LAYER_NAMESPACE::GetInstance()->SetUpstreamLayers(std::move(currentLayerChain));

            result = XR_ERROR_RUNTIME_FAILURE;

            // Forward the xrCreateInstance() call to the layer.
            try {
                result = LAYER_NAMESPACE::GetInstance()->xrCreateInstance(instanceCreateInfo);
            } catch (std::runtime_error& exc) {
                TraceLoggingWriteTagged(local, "xrCreateApiLayerInstance_Error", TLArg(exc.what(), "Error"));
            }

            // Cleanup attempt before returning an error.
            if (XR_FAILED(result)) {
                PFN_xrDestroyInstance xrDestroyInstance = nullptr;
                if (XR_SUCCEEDED(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                        *instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance)))) {
                    xrDestroyInstance(*instance);
                }
            }
        }

        TraceLoggingWriteStop(local, "xrCreateApiLayerInstance", TLArg((int)result, "Result"));

        return result;
    }

    // Handle cleanup of the layer's singleton.
    XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrDestroyInstance");

        XrResult result;
        try {
            result = LAYER_NAMESPACE::GetInstance()->xrDestroyInstance(instance);
            if (XR_SUCCEEDED(result)) {
                LAYER_NAMESPACE::ResetInstance();
            }
        } catch (std::runtime_error& exc) {
            TraceLoggingWriteTagged(local, "xrDestroyInstance_Error", TLArg(exc.what(), "Error"));
            result = XR_ERROR_RUNTIME_FAILURE;
        }

        TraceLoggingWriteStop(local, "xrDestroyInstance", TLArg((int)result, "Result"));

        return result;
    }

    // Forward the xrGetInstanceProcAddr() call to the dispatcher.
    XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
        TraceLoggingWrite(g_traceProvider,
                          "xrGetInstanceProcAddr",
                          TLArg(!!g_bypass.load(std::memory_order_relaxed), "Bypass"),
                          TLPArg(instance, "Instance"),
                          TLArg(name));

        // std::atomic<T*> has no operator() of its own - load the pointer once and call through
        // the local copy, both to get a callable value and to avoid loading it twice (the null
        // check and the call) if it changed in between.
        if (const PFN_xrGetInstanceProcAddr bypass = g_bypass.load(std::memory_order_relaxed)) {
            return bypass(instance, name, function);
        }

        try {
            return LAYER_NAMESPACE::GetInstance()->xrGetInstanceProcAddr(instance, name, function);
        } catch (std::runtime_error& exc) {
            TraceLoggingWrite(g_traceProvider, "xrGetInstanceProcAddr", TLArg(exc.what(), "Error"));
            Log("%s\n", exc.what());
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }

} // namespace LAYER_NAMESPACE
