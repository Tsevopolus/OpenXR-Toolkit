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

#pragma once

#ifndef LAYER_NAMESPACE
#error Must define LAYER_NAMESPACE
#endif

namespace LAYER_NAMESPACE {

    // These three are loader/API-layer ABI entry points: their addresses are handed to the
    // loader (via apiLayerRequest->getInstanceProcAddr/createApiLayerInstance in entry.cpp) and
    // called back through PFN_xrGetInstanceProcAddr/PFN_xrCreateApiLayerInstance function-pointer
    // types, which - per the OpenXR headers - carry the XRAPI_CALL calling-convention macro. On
    // Win64 XRAPI_CALL is a no-op so this currently makes no ABI difference, but on Win32 it
    // expands to __stdcall; without it here, entry.cpp's reinterpret_cast to the XRAPI_CALL
    // function-pointer types would silently paper over a calling-convention mismatch (cdecl vs
    // stdcall) that corrupts the stack on every call.
    XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance,
                                              const char* name,
                                              PFN_xrVoidFunction* function);
    XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance);
    XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* instanceCreateInfo,
                                                 const struct XrApiLayerCreateInfo* apiLayerInfo,
                                                 XrInstance* instance);

} // namespace LAYER_NAMESPACE
