# MIT License
#
# Copyright(c) 2021-2022 Matthieu Bucchianeri
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this softwareand associated documentation files(the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions :
#
# The above copyright noticeand this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import os
import re
import sys

# Import dependencies from the OpenXR SDK.
cur_dir = os.path.abspath(os.path.dirname(__file__))
base_dir = os.path.abspath(os.path.join(cur_dir, '..', '..'))
sdk_dir = os.path.join(base_dir, 'external', 'OpenXR-SDK-Source')
sys.path.append(os.path.join(sdk_dir, 'specification', 'scripts'))
sys.path.append(os.path.join(sdk_dir, 'src', 'scripts'))

from automatic_source_generator import AutomaticSourceOutputGenerator, AutomaticSourceGeneratorOptions
from reg import Registry
from generator import write
from xrconventions import OpenXRConventions

# Import configuration.
import layer_apis

# Sanity checks on the configuration file
if 'xrCreateInstance' in layer_apis.override_functions:
    raise Exception("xrCreateInstance() is implicitly overriden and shall not be specified in override_functions. Use the xrCreateInstance() virtual method.")
if 'xrCreateInstance' in layer_apis.requested_functions:
    raise Exception("xrCreateInstance() cannot be specified in requested_functions")

if 'xrDestroyInstance' in layer_apis.override_functions:
    raise Exception("xrDestroyInstance() is implicitly overriden and shall not be specified in override_functions. Use the OpenXrApi destructor instead.")
if 'xrDestroyInstance' in layer_apis.requested_functions:
    raise Exception("xrDestroyInstance() cannot be specified in requested_functions")

if 'xrGetInstanceProcAddr' in layer_apis.override_functions:
    raise Exception("xrGetInstanceProcAddr() is implicitly overriden and shall not be specified in override_functions. Use the xrGetInstanceProcAddr() virtual method.")
if 'xrGetInstanceProcAddr' in layer_apis.requested_functions:
    raise Exception("xrGetInstanceProcAddr() cannot be specified in requested_functions. Use the m_xrGetInstanceProcAddr() class member.")


class DispatchGenOutputGenerator(AutomaticSourceOutputGenerator):
    '''Common generator utilities and formatting.'''
    def outputGeneratedHeaderWarning(self):
        warning = '''// *********** THIS FILE IS GENERATED - DO NOT EDIT ***********'''
        write(warning, file=self.outFile)

    def outputCopywriteHeader(self):
        copyright = '''// MIT License
//
// Copyright(c) 2021-2022 Matthieu Bucchianeri
// Copyright(c) 2026      Tsevopolus
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
'''
        write(copyright, file=self.outFile)

    def outputGeneratedAuthorNote(self):
        pass

    def makeParametersList(self, cmd):
        parameters_list = ""
        for param in cmd.params:
            if parameters_list:
                parameters_list += ', '
            parameters_list += param.cdecl.strip()

        return parameters_list

    def makeArgumentsList(self, cmd):
        arguments_list = ""
        for param in cmd.params:
            if arguments_list:
                arguments_list += ', '
            arguments_list += param.name

        return arguments_list

class DispatchGenCppOutputGenerator(DispatchGenOutputGenerator):
    '''Generator for dispatch.gen.cpp.'''
    def beginFile(self, genOpts):
        DispatchGenOutputGenerator.beginFile(self, genOpts)
        preamble = '''#include "pch.h"

#include <layer.h>

#include "dispatch.h"
#include "log.h"

#ifndef LAYER_NAMESPACE
#error Must define LAYER_NAMESPACE
#endif

using namespace LAYER_NAMESPACE::log;

namespace LAYER_NAMESPACE
{'''
        write(preamble, file=self.outFile)

    def endFile(self):
        generated_wrappers = self.genWrappers()
        generated_get_instance_proc_addr = self.genGetInstanceProcAddr()
        generated_create_instance = self.genCreateInstance()

        postamble = '''} // namespace LAYER_NAMESPACE
'''

        contents = f'''
	// Auto-generated wrappers for the requested APIs.
{generated_wrappers}

	// Auto-generated dispatcher handler.
{generated_get_instance_proc_addr}

	// Auto-generated create instance handler.
{generated_create_instance}

{postamble}'''

        write(contents, file=self.outFile)
        DispatchGenOutputGenerator.endFile(self)

    def genWrappers(self):
        generated = ''

        for cur_cmd in self.core_commands + self.ext_commands:
            if cur_cmd.name in layer_apis.override_functions:
                parameters_list = self.makeParametersList(cur_cmd)
                arguments_list = self.makeArgumentsList(cur_cmd)

                if cur_cmd.return_type is not None:
                    generated += f'''
	XrResult {cur_cmd.name}({parameters_list})
	{{
		TraceLocalActivity(local);
		TraceLoggingWriteStart(local, "{cur_cmd.name}");

		XrResult result;
		try
		{{
			result = LAYER_NAMESPACE::GetInstance()->{cur_cmd.name}({arguments_list});
		}}
		catch (std::exception& exc)
		{{
			TraceLoggingWriteTagged(local, "{cur_cmd.name}_Error", TLArg(exc.what(), "Error"));
			Log("{cur_cmd.name}: %s\\n", exc.what());
			result = XR_ERROR_RUNTIME_FAILURE;
		}}

		TraceLoggingWriteStop(local, "{cur_cmd.name}", TLArg(xr::ToCString(result), "Result"));

		return result;
	}}
'''
                else:
                    # Dead in practice today - every current OpenXR command returns XrResult, so
                    # this branch is never emitted - but kept for forward compatibility and made
                    # consistent with the XrResult branch above (std::exception, not the narrower
                    # std::runtime_error) so a future non-XrResult command doesn't silently let a
                    # different exception type escape through this C ABI boundary.
                    generated += f'''
	void {cur_cmd.name}({parameters_list})
	{{
		TraceLocalActivity(local);
		TraceLoggingWriteStart(local, "{cur_cmd.name}");

		try
		{{
			LAYER_NAMESPACE::GetInstance()->{cur_cmd.name}({arguments_list});
		}}
		catch (std::exception& exc)
		{{
			TraceLoggingWriteTagged(local, "{cur_cmd.name}_Error", TLArg(exc.what(), "Error"));
			Log("{cur_cmd.name}: %s\\n", exc.what());
		}}

		TraceLoggingWriteStop(local, "{cur_cmd.name}");
	}}
'''

        return generated

    def genCreateInstance(self):
        generated = '''	XrResult OpenXrApi::xrCreateInstance(const XrInstanceCreateInfo* createInfo)
    {
'''

        for cur_cmd in self.core_commands:
            if cur_cmd.name in layer_apis.requested_functions:
                generated += f'''		if (XR_FAILED(m_xrGetInstanceProcAddr(m_instance, "{cur_cmd.name}", reinterpret_cast<PFN_xrVoidFunction*>(&m_{cur_cmd.name}))))
		{{
			throw std::runtime_error("Failed to resolve {cur_cmd.name}");
		}}
		TraceLoggingWrite(g_traceProvider, "ProcAddr", TLArg("{cur_cmd.name}", "Name"), TLPArg(m_{cur_cmd.name}, "Ptr"));
'''

        # Functions from extensions are allowed to be null.
        for cur_cmd in self.ext_commands:
            if cur_cmd.name in layer_apis.requested_functions:
                generated += f'''		m_xrGetInstanceProcAddr(m_instance, "{cur_cmd.name}", reinterpret_cast<PFN_xrVoidFunction*>(&m_{cur_cmd.name}));
		TraceLoggingWrite(g_traceProvider, "OptionalProcAddr", TLArg("{cur_cmd.name}", "Name"), TLPArg(m_{cur_cmd.name}, "Ptr"));
'''

        generated += '''		m_applicationName = createInfo->applicationInfo.applicationName;
		return XR_SUCCESS;
	}'''

        return generated;

    def genGetInstanceProcAddr(self):
        generated = '''	XrResult OpenXrApi::xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function)
	{
		// m_xrGetInstanceProcAddr is only set once SetGetInstanceProcAddr() runs, at the end of
		// xrCreateApiLayerInstance(). It is reachable null here: the free xrGetInstanceProcAddr()
		// in dispatch.cpp forwards into this singleton (lazily default-constructed by
		// GetInstance() on first use) for *any* call, including one the loader or another layer
		// makes with XR_NULL_HANDLE to query global functions before our instance exists yet -
		// a legitimate call per spec. Calling through a null function pointer would otherwise
		// crash.
		if (!m_xrGetInstanceProcAddr)
		{
			return XR_ERROR_HANDLE_INVALID;
		}

		XrResult result = m_xrGetInstanceProcAddr(instance, name, function);

		if (XR_SUCCEEDED(result))
		{
			// This runs on every function an application resolves - often repeatedly - so avoid
			// the heap allocation a std::string would do here on every single call; string_view
			// comparison against the literals below is allocation-free.
			const std::string_view apiName(name);

			if (apiName == "xrDestroyInstance")
			{
				m_xrDestroyInstance = reinterpret_cast<PFN_xrDestroyInstance>(*function);
				*function = reinterpret_cast<PFN_xrVoidFunction>(LAYER_NAMESPACE::xrDestroyInstance);
			}
'''

        for cur_cmd in self.core_commands + self.ext_commands:
            if cur_cmd.name in layer_apis.override_functions:
                generated += f'''			else if (apiName == "{cur_cmd.name}")
			{{
				m_{cur_cmd.name} = reinterpret_cast<PFN_{cur_cmd.name}>(*function);
				*function = reinterpret_cast<PFN_xrVoidFunction>(LAYER_NAMESPACE::{cur_cmd.name});
			}}
'''

        generated += '''
		}

		return result;
	}'''

        return generated


class DispatchGenHOutputGenerator(DispatchGenOutputGenerator):
    '''Generator for dispatch.gen.h.'''
    def beginFile(self, genOpts):
        DispatchGenOutputGenerator.beginFile(self, genOpts)
        preamble = '''#pragma once

#ifndef LAYER_NAMESPACE
#error Must define LAYER_NAMESPACE
#endif

namespace LAYER_NAMESPACE
{

	class OpenXrApi
	{
	private:
		XrInstance m_instance{ XR_NULL_HANDLE };
		std::string m_applicationName;
		std::vector<std::string> m_upstreamLayers;

	protected:
		OpenXrApi() = default;

		PFN_xrGetInstanceProcAddr m_xrGetInstanceProcAddr{ nullptr };

	public:
		virtual ~OpenXrApi() = default;

		XrInstance GetXrInstance() const
		{
			return m_instance;
		}

		const std::string& GetApplicationName() const
		{
			return m_applicationName;
		}

		void SetGetInstanceProcAddr(PFN_xrGetInstanceProcAddr pfn_xrGetInstanceProcAddr, XrInstance instance)
		{
			m_xrGetInstanceProcAddr = pfn_xrGetInstanceProcAddr;
			m_instance = instance;
		}

		void SetUpstreamLayers(std::vector<std::string> upstreamLayers)
		{
			m_upstreamLayers = std::move(upstreamLayers);
		}

		const std::vector<std::string>& GetUpstreamLayers() const
		{
			return m_upstreamLayers;
		}

		// Specially-handled by the auto-generated code.
		virtual XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
		virtual XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo);
'''
        write(preamble, file=self.outFile)

    def endFile(self):
        generated_virtual_methods = self.genVirtualMethods()

        postamble = '''
	};

} // namespace LAYER_NAMESPACE
'''

        contents = f'''
		// Auto-generated entries for the requested APIs.
{generated_virtual_methods}

{postamble}'''

        write(contents, file=self.outFile)

        DispatchGenOutputGenerator.endFile(self)

    def genVirtualMethods(self):
        generated = ''

        # Dedup while preserving order (dict.fromkeys, not set()): a set's iteration order depends
        # on string hashing, which can vary between interpreter runs (unless PYTHONHASHSEED is
        # pinned), so generating from a set reordered dispatch.gen.h's virtual methods on every
        # regeneration - noisy diffs and non-reproducible builds for no reason.
        commands_to_include = list(dict.fromkeys(
            layer_apis.override_functions + layer_apis.requested_functions + ['xrDestroyInstance']))
        # genCreateInstance() resolves core_commands with CHECK_XRCMD (so m_<cmd> is guaranteed
        # non-null once xrCreateInstance succeeds) but explicitly allows ext_commands to resolve
        # to null ("Functions from extensions are allowed to be null" - e.g. a runtime can report
        # an extension as supported without exporting every one of its entry points). The wrapper
        # for an ext_command therefore needs a null guard that a core command's wrapper doesn't.
        ext_command_names = set(cmd.name for cmd in self.ext_commands)
        for cur_cmd in self.core_commands + self.ext_commands:
            if cur_cmd.name in commands_to_include:
                parameters_list = self.makeParametersList(cur_cmd)
                arguments_list = self.makeArgumentsList(cur_cmd)
                is_optional = cur_cmd.name in ext_command_names

                generated += '''
	public:'''

                if cur_cmd.return_type is not None:
                    if is_optional:
                        generated += f'''
		virtual XrResult {cur_cmd.name}({parameters_list})
		{{
			if (!m_{cur_cmd.name})
			{{
				return XR_ERROR_FUNCTION_UNSUPPORTED;
			}}
			return m_{cur_cmd.name}({arguments_list});
		}}
'''
                    else:
                        generated += f'''
		virtual XrResult {cur_cmd.name}({parameters_list})
		{{
			return m_{cur_cmd.name}({arguments_list});
		}}
'''
                else:
                    # Dead in practice today (see the matching note in genWrappers): no current
                    # OpenXR command has a void return, so there's no way to signal "unsupported"
                    # here even for an optional ext_command - left as-is.
                    generated += f'''
		virtual void {cur_cmd.name}({parameters_list})
		{{
			m_{cur_cmd.name}({arguments_list});
		}}
'''

                generated += f'''	private:
		PFN_{cur_cmd.name} m_{cur_cmd.name}{{ nullptr }};
'''
                
        return generated

def makeREstring(strings, default=None):
    """Turn a list of strings into a regexp string matching exactly those strings.

    Note the `strings` empty / `default=None` case: this falls through to building the pattern
    from an empty list, i.e. '^()$', which matches only the empty string - not "match nothing",
    which is probably what a caller relying on `default=None` here would expect. This currently
    only ever gets called with a non-empty `extensions_to_search`, so it doesn't bite in
    practice, but it's a footgun for any future caller that passes an empty list with no default.
    """
    if strings or default is None:
        return '^(' + '|'.join((re.escape(s) for s in strings)) + ')$'
    return default

if __name__ == '__main__':
    registry = Registry()
    registry.loadFile(os.path.join(sdk_dir, 'specification', 'registry', 'xr.xml'))

    conventions = OpenXRConventions()
    featuresPat = '.*'
    extensionsPat = makeREstring(layer_apis.extensions_to_search)

    registry.setGenerator(DispatchGenCppOutputGenerator(diagFile=None))
    registry.apiGen(AutomaticSourceGeneratorOptions(
            conventions       = conventions,
            filename          = 'dispatch.gen.cpp',
            directory         = cur_dir,
            apiname           = 'openxr',
            profile           = None,
            versions          = featuresPat,
            emitversions      = featuresPat,
            defaultExtensions = 'openxr',
            addExtensions     = None,
            removeExtensions  = None,
            emitExtensions    = extensionsPat))

    registry.setGenerator(DispatchGenHOutputGenerator(diagFile=None))
    registry.apiGen(AutomaticSourceGeneratorOptions(
            conventions       = conventions,
            filename          = 'dispatch.gen.h',
            directory         = cur_dir,
            apiname           = 'openxr',
            profile           = None,
            versions          = featuresPat,
            emitversions      = featuresPat,
            defaultExtensions = 'openxr',
            addExtensions     = None,
            removeExtensions  = None,
            emitExtensions    = extensionsPat))
