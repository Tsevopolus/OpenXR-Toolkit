// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri
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

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Text;
using System.Windows.Forms;
using System.IO;
using Silk.NET.Core;
using Silk.NET.Core.Native;
using Silk.NET.OpenXR;
using System.Reflection;
using System.Diagnostics;
using System.Windows.Input;
using System.Runtime.InteropServices;
using System.Threading;
using System.Net;
using System.Runtime.Serialization.Json;
using System.Xml.Linq;
using System.Xml.XPath;

namespace companion
{
    public partial class Form1 : Form
    {
        [DllImport("XR_APILAYER_MBUCCHIA_toolkit.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.StdCall)]
        public static extern IntPtr getVersionString();

        // Must match config.cpp.
        public const string RegPrefix = "SOFTWARE\\OpenXR_Toolkit";

        private List<Tuple<string, int>> VirtualKeys;

        private bool loading = true;
        private bool tracing = false;

        private int keyMenuGen = 1;

        // appList entry: keeps the actual registry subkey name (RegistryName) separate from
        // the text shown in the list (ToString()), so looking up which app was (un)checked
        // never has to re-derive the name by parsing the display text.
        private class AppListEntry
        {
            public AppListEntry(string registryName, string display)
            {
                RegistryName = registryName;
                Display = display;
            }

            public string RegistryName { get; }
            public string Display { get; }

            public override string ToString() => Display;
        }

        public Form1()
        {
            InitializeComponent();

            InitializeKeyList(leftKey);
            InitializeKeyList(nextKey);
            InitializeKeyList(previousKey);
            InitializeKeyList(rightKey);
            InitializeKeyList(screenshotKey);

            InitXrVersionString();
            InitXrLayersAsync();
            timer1_Tick(null, null);

            SuspendLayout();
            Microsoft.Win32.RegistryKey key = null;
            try
            {
                key = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(RegPrefix);

                // Must match the defaults in the layer!
                keyMenuGen = (int)key.GetValue("key_menu_gen", 1);
                safemodeCheckbox.Checked = (int)key.GetValue("safe_mode", 0) == 1 ? true : false;
                screenshotCheckbox.Checked = (int)key.GetValue("enable_screenshot", 0) == 1 ? true : false;
                // Clamp registry-sourced indices to the actual item range: a value left over
                // from an older build with fewer choices (or a corrupted/hand-edited registry
                // value) would otherwise throw ArgumentOutOfRangeException here and abort the
                // rest of the constructor.
                screenshotFormat.SelectedIndex = ClampIndex((int)key.GetValue("screenshot_fileformat", 1), screenshotFormat.Items.Count);
                // Both share the same enable condition; set together so screenshotEye isn't
                // left at the designer default (enabled) until the async layer probe below
                // gets around to calling ApplyXrLayerProbeResult.
                screenshotFormat.Enabled = screenshotEye.Enabled = screenshotCheckbox.Enabled && screenshotCheckbox.Checked;
                screenshotEye.SelectedIndex = ClampIndex((int)key.GetValue("screenshot_eye", 0), screenshotEye.Items.Count);
                menuVisibility.SelectedIndex = ClampIndex((int)key.GetValue("menu_eye", 0), menuVisibility.Items.Count);
                ctrlModifierCheckbox.Checked = (int)key.GetValue("ctrl_modifier", 1) == 1 ? true : false;
                altModifierCheckbox.Checked = (int)key.GetValue("alt_modifier", 0) == 1 ? true : false;
                SelectKey(leftKey, (int)key.GetValue("key_left", KeyInterop.VirtualKeyFromKey(Key.F1)));
                SelectKey(nextKey, (int)key.GetValue("key_menu", KeyInterop.VirtualKeyFromKey(Key.F2)));
                SelectKey(previousKey, (int)key.GetValue("key_up", 0));
                SelectKey(rightKey, (int)key.GetValue("key_right", KeyInterop.VirtualKeyFromKey(Key.F3)));
                SelectKey(screenshotKey, (int)key.GetValue("key_screenshot", KeyInterop.VirtualKeyFromKey(Key.F12)));
                key.Close();

                // Bypass apps.
                key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RegPrefix);
                foreach (var subKey in key.GetSubKeyNames())
                {
                    var app = key.OpenSubKey(subKey);
                    var modulePath = (string)app.GetValue("module", null);
                    var displayName = subKey;
                    if (modulePath != null)
                    {
                        displayName += " (" + Path.GetFileName(modulePath) + ")";
                    }

                    // Keep the actual registry subkey name attached to the list entry
                    // (AppListEntry.ToString() is what the list box displays) instead of
                    // re-deriving it later by splitting the display text on '(' - which
                    // breaks for any app name that itself contains a parenthesis.
                    appList.Items.Add(new AppListEntry(subKey, displayName));
                    appList.SetItemChecked(appList.Items.Count - 1, (int)app.GetValue("bypass", 0) == 0);
                }
            }
            catch (Exception)
            {
                // This block both reads existing settings and (via CreateSubKey) opens the
                // key for write, so a failure here isn't necessarily a write failure -
                // keep the message generic rather than specifically blaming "write".
                MessageBox.Show(this, "Failed to load settings from the registry. Please make sure the app is running elevated.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                if (key != null)
                {
                    key.Close();
                }
            }
            ResumeLayout();

            CheckForUpdates();

            loading = false;
        }

        // Clamps a registry-sourced ComboBox index into the valid [0, itemCount-1] range
        // (or -1/no selection if the box has no items at all).
        private static int ClampIndex(int value, int itemCount)
        {
            if (itemCount <= 0)
            {
                return -1;
            }
            return Math.Min(Math.Max(value, 0), itemCount - 1);
        }

        private void InitializeKeyList(ComboBox box)
        {
            if (VirtualKeys == null)
            {
                VirtualKeys = new();
                Key[] allowed = new[] {
                    Key.Escape, Key.F1, Key.F2, Key.F3, Key.F4, Key.F5, Key.F6, Key.F7, Key.F8, Key.F9, Key.F10, Key.F11, Key.F12, Key.PrintScreen, Key.Scroll, Key.Pause,
                    Key.OemTilde, Key.D1, Key.D2, Key.D3, Key.D4, Key.D5, Key.D6, Key.D7, Key.D8, Key.D9, Key.D0, Key.OemMinus, Key.OemPlus, Key.Back, Key.Insert, Key.Home, Key.PageUp,
                    Key.Tab, Key.Q, Key.W, Key.E, Key.R, Key.T, Key.Y, Key.U, Key.I, Key.O, Key.P, Key.OemOpenBrackets, Key.OemCloseBrackets, Key.OemPipe, Key.Delete, Key.End, Key.PageDown,
                    Key.A, Key.S, Key.D, Key.F, Key.G, Key.H, Key.J, Key.K, Key.L, Key.OemSemicolon, Key.OemQuotes, Key.Enter,
                    Key.Z, Key.X, Key.C, Key.V, Key.B, Key.N, Key.M, Key.OemComma, Key.OemPeriod, Key.Separator,
                    Key.Space, Key.Left, Key.Up, Key.Down, Key.Right,
                    Key.NumPad0, Key.NumPad1, Key.NumPad2, Key.NumPad3, Key.NumPad4, Key.NumPad5, Key.NumPad6, Key.NumPad7, Key.NumPad8, Key.NumPad9,
                    Key.Divide, Key.Multiply, Key.Subtract, Key.Add
                };

                foreach (var key in allowed)
                {
                    var text = key switch
                    {
                        Key.Add => "NumPad+",
                        Key.Back => "Backspace",
                        Key.D0 => "0",
                        Key.D1 => "1",
                        Key.D2 => "2",
                        Key.D3 => "3",
                        Key.D4 => "4",
                        Key.D5 => "5",
                        Key.D6 => "6",
                        Key.D7 => "7",
                        Key.D8 => "8",
                        Key.D9 => "9",
                        Key.Divide => "NumPad/",
                        Key.Multiply => "NumPad*",
                        // Key.OemBackslash, Key.OemQuestion and Key.Snapshot are not in
                        // "allowed" above, so there are deliberately no cases for them here -
                        // add the key to "allowed" first if one of these should become
                        // selectable.
                        Key.OemCloseBrackets => "]",
                        Key.OemComma => ",",
                        Key.OemMinus => "-",
                        Key.OemOpenBrackets => "[",
                        Key.OemPeriod => ".",
                        Key.OemPipe => "|",
                        Key.OemPlus => "+",
                        Key.OemQuotes => "\"",
                        Key.OemSemicolon => ";",
                        Key.OemTilde => "~",
                        Key.Scroll => "ScrLk",
                        Key.Separator => "/",
                        Key.Subtract => "NumPad-",
                        _ => key.ToString()
                    };
                    VirtualKeys.Add(new(text, KeyInterop.VirtualKeyFromKey(key)));
                }
            }

            box.Items.Add("");
            foreach (var entry in VirtualKeys)
            {
                box.Items.Add(entry.Item1);
            }
        }

        private void SelectKey(ComboBox box, int virtualKey)
        {
            if (virtualKey == 0)
            {
                box.SelectedIndex = 0;
            }

            string keyText = "";
            foreach (var key in VirtualKeys)
            {
                if (key.Item2 == virtualKey)
                {
                    keyText = key.Item1;
                    break;
                }
            }

            foreach (var item in box.Items)
            {
                if ((string)item == keyText)
                {
                    box.SelectedItem = item;
                    break;
                }
            }
        }

        // Set by ApplyXrLayerProbeResult(), to whichever of the two layer names
        // EnumerateApiLayerProperties() actually reported as loaded.
        string activeLayerName = null;

        // This companion's own AssemblyFileVersion (see AssemblyInfo.cs), kept in lockstep
        // with version.info by the release process. Used both for the "active layer" label
        // and the update check below - the NewKitOnTheBlock fork doesn't export a
        // getVersionString() of its own (that DllImport is fixed to the original 1.3.2
        // DLL), so this is the only reliable source for "which fork version is this".
        private static string OwnVersionString()
        {
            var v = Assembly.GetExecutingAssembly().GetName().Version;
            return v.Major + "." + v.Minor + "." + v.Build;
        }

        private void SetActiveString()
        {
            string friendlyName;
            if (activeLayerName == "XR_APILAYER_NEWKITONTHEBLOCK_toolkit")
            {
                friendlyName = "OpenXR Toolkit " + OwnVersionString() + " (NewKitOnTheBlock)";
            }
            else if (activeLayerName == "XR_APILAYER_MBUCCHIA_toolkit")
            {
                friendlyName = "OpenXR Toolkit 1.3.2 (original)";
            }
            else if (versionString != null)
            {
                friendlyName = versionString;
            }
            else
            {
                friendlyName = "OpenXR Toolkit layer";
            }

            layerActive.Text = friendlyName + " is active";

            if (appString != "")
            {
                layerActive.Text += " (" + appString + ")";
            }
        }

        string versionString = null;
        string updateAvailable = null;

        private void InitXrVersionString()
        {
            try
            {
                IntPtr pStr = getVersionString();
                versionString = Marshal.PtrToStringAnsi(pStr);
            }
            catch (Exception)
            {
                // Not fatal: this only queries the original build's version string
                // specifically (it's a fixed DllImport target). SetActiveString() uses
                // OwnVersionString() for the fork and only falls back to this value when
                // neither known layer name matched (some third, unexpected layer).
                versionString = null;
            }
        }

        // Result of probing the OpenXR loader for installed/active API layers. Carries no
        // UI state so it can be produced off the UI thread (see InitXrLayersAsync below).
        private struct XrLayerProbeResult
        {
            public bool querySucceeded;
            public bool layerFound;
            public string activeLayerName;
            public string layersList;
        }

        // Creates a throwaway AppDomain, loads a fresh copy of the OpenXR loader into it (so
        // the registry's implicit-API-layer list is re-read every time instead of once per
        // process), enumerates the installed layers, then tears the domain down again. This
        // is the slow part: AppDomain creation/teardown under .NET Framework commonly takes
        // several hundred milliseconds, which is why it must not run on the UI thread during
        // startup (see InitXrLayersAsync). Touches no UI controls - safe to call from any thread.
        private unsafe XrLayerProbeResult ProbeXrLayers()
        {
            var result = new XrLayerProbeResult();

            AppDomain dom = AppDomain.CreateDomain("temporaryXr");
            try
            {
                // Load the OpenXR package into a temporary app domain. This is so make sure that the registry is read everytime when looking for implicit API layer.
                AssemblyName assemblyName = new AssemblyName();
                assemblyName.CodeBase = typeof(XR).Assembly.Location;
                Assembly assembly = dom.Load(assemblyName);
                Type localXR = assembly.GetType("Silk.NET.OpenXR.XR");

                XR xr = (XR)localXR.GetMethod("GetApi").Invoke(null, null);

                // Make sure our layer is installed.
                uint layerCount = 0;
                xr.EnumerateApiLayerProperties(ref layerCount, null);
                var layers = new ApiLayerProperties[layerCount];
                for (int i = 0; i < layers.Length; i++)
                {
                    layers[i].Type = StructureType.TypeApiLayerProperties;
                }
                var layersSpan = new Span<ApiLayerProperties>(layers);
                if (xr.EnumerateApiLayerProperties(ref layerCount, layersSpan) == Result.Success)
                {
                    result.querySucceeded = true;

                    string layersList = "";
                    for (int i = 0; i < layers.Length; i++)
                    {
                        fixed (void* nptr = layers[i].LayerName)
                        {
                            string layerName = SilkMarshal.PtrToString(new System.IntPtr(nptr));
                            layersList += layerName + "\n";
                            if (layerName == "XR_APILAYER_MBUCCHIA_toolkit" || layerName == "XR_APILAYER_NEWKITONTHEBLOCK_toolkit")
                            {
                                result.layerFound = true;
                                result.activeLayerName = layerName;
                            }
                        }
                    }
                    result.layersList = layersList;
                }
            }
            finally
            {
                AppDomain.Unload(dom);
            }

            return result;
        }

        // Applies a ProbeXrLayers() result to the UI. Must run on the UI thread.
        private void ApplyXrLayerProbeResult(XrLayerProbeResult result)
        {
            if (!result.querySucceeded)
            {
                MessageBox.Show(this, "Failed to query API layers", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            tooltip.SetToolTip(layerActive, result.layersList);

            bool wasLoading = loading;
            if (!result.layerFound)
            {
                activeLayerName = null;
                layerActive.Text = "OpenXR Toolkit layer is NOT active";
                layerActive.ForeColor = Color.Red;
                loading = true;
                disableCheckbox.Checked = true;
                loading = wasLoading;
            }
            else
            {
                activeLayerName = result.activeLayerName;
                SetActiveString();
                layerActive.ForeColor = Color.Green;
                loading = true;
                disableCheckbox.Checked = false;
                // Reflect which of the two layers is actually loaded, independent of
                // the disable/enable checkbox above (that one just toggles "any
                // toolkit layer at all" on or off).
                layerSelector.SelectedIndex = (activeLayerName == "XR_APILAYER_NEWKITONTHEBLOCK_toolkit") ? 1 : 0;
                loading = wasLoading;
            }
            safemodeCheckbox.Enabled = screenshotCheckbox.Enabled = screenshotFormat.Enabled = screenshotEye.Enabled =
                menuVisibility.Enabled = leftKey.Enabled = nextKey.Enabled = previousKey.Enabled = rightKey.Enabled = screenshotKey.Enabled =
                ctrlModifierCheckbox.Enabled = altModifierCheckbox.Enabled = layerSelector.Enabled = !disableCheckbox.Checked;
            screenshotFormat.Enabled = screenshotEye.Enabled = screenshotCheckbox.Enabled && screenshotCheckbox.Checked;
        }

        // Runs ProbeXrLayers() on a background thread (it creates/tears down a throwaway
        // AppDomain, which is slow - see ProbeXrLayers' own comment) and marshals the result
        // back onto the UI thread via BeginInvoke, where onComplete runs. onComplete always
        // runs on the UI thread, with a null result if the probe itself threw.
        private void RunXrProbeAsync(Action<XrLayerProbeResult?> onComplete)
        {
            new Thread(() =>
            {
                Thread.CurrentThread.IsBackground = true;

                XrLayerProbeResult? result = null;
                try
                {
                    result = ProbeXrLayers();
                }
                catch (Exception)
                {
                }

                try
                {
                    BeginInvoke(new System.Action(() => onComplete(result)));
                }
                catch (Exception)
                {
                    // Window may have been closed already while the probe was running.
                }
            }).Start();
        }

        // Fire-and-forget probe-and-apply: applies the result (or shows the same error
        // MessageBox the old synchronous InitXr() used to) once the background probe
        // completes. Never blocks the UI thread - use this instead of the old synchronous
        // InitXr() at every call site.
        private void InitXrAsync()
        {
            RunXrProbeAsync(result =>
            {
                if (result.HasValue)
                {
                    ApplyXrLayerProbeResult(result.Value);
                }
                else
                {
                    MessageBox.Show(this, "Failed to initialize OpenXR", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }

                // Try to reclaim memory.
                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();
            });
        }

        // Startup path: defers to InitXrAsync() like every other call site. The Handle touch
        // isn't load-bearing (InitializeComponent()/Show() already force it, and BeginInvoke
        // queues until the message pump starts regardless) - kept as a cheap, explicit
        // guarantee rather than relying on that ordering.
        private void InitXrLayersAsync()
        {
            var forceHandle = Handle;
            InitXrAsync();
        }

        private void CheckForUpdates()
        {
            new Thread(() =>
            {
                Thread.CurrentThread.IsBackground = true;

                string url = "https://api.github.com/repos/Tsevopolus/OpenXR-Toolkit/releases/latest";

                // https://stackoverflow.com/questions/9620278/how-do-i-make-calls-to-a-rest-api-using-c
                HttpWebRequest request = (HttpWebRequest)WebRequest.Create(url);
                request.Method = "GET";
                request.UserAgent = "PimaxXR/Updated";
                request.Timeout = 5000;
                try
                {
                    WebResponse webResponse = request.GetResponse();
                    using (Stream webStream = webResponse.GetResponseStream() ?? Stream.Null)
                    using (StreamReader responseReader = new StreamReader(webStream))
                    {
                        string response = responseReader.ReadToEnd();
                        var jsonReader = JsonReaderWriterFactory.CreateJsonReader(Encoding.UTF8.GetBytes(response), new System.Xml.XmlDictionaryReaderQuotas());
                        var root = XElement.Load(jsonReader);
                        string tagName = root.XPathSelectElement("//tag_name")?.Value;
                        if (string.IsNullOrEmpty(tagName))
                        {
                            return;
                        }

                        // Tags are plain "1.4.3" or prefixed "v1.4.3" - strip the prefix if present.
                        string tagVersionText = tagName.StartsWith("v", StringComparison.OrdinalIgnoreCase)
                            ? tagName.Substring(1)
                            : tagName;

                        // Compare against this companion's own version (kept in lockstep with
                        // version.info), not getVersionString() - that DllImport always reads
                        // the bundled original 1.3.2 DLL regardless of which layer is active,
                        // so it never reflects the fork's own version.
                        var ownVersion = Assembly.GetExecutingAssembly().GetName().Version;
                        if (Version.TryParse(tagVersionText, out var githubVersion) &&
                            githubVersion > new Version(ownVersion.Major, ownVersion.Minor, ownVersion.Build))
                        {
                            updateAvailable = tagName;
                        }
                    }
                }
                catch (Exception e)
                {
                    // Silently skipping the update check on any failure (network down, GitHub
                    // rate limit, etc.) is the right call for a release build - it's not worth
                    // bothering the user about. In a debug build, at least leave a trace.
#if DEBUG
                    Debug.WriteLine("CheckForUpdates failed: " + e);
#endif
                }

            }).Start();
        }

        private void WriteSetting(string name, int value)
        {
            Microsoft.Win32.RegistryKey key = null;
            try
            {
                key = Microsoft.Win32.Registry.LocalMachine.CreateSubKey(RegPrefix);
                key.SetValue(name, value, Microsoft.Win32.RegistryValueKind.DWord);
            }
            catch (Exception)
            {
                MessageBox.Show(this, "Failed to write to registry. Please make sure the app is running elevated.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                if (key != null)
                {
                    key.Close();
                }
            }
        }

        // Finds the actual registered value name (full manifest path, as the loader itself
        // sees it) for one of our two layers, by scanning the existing registry entries -
        // rather than guessing a path from where companion.exe happens to be running from
        // right now. Falls back to a path built from our own location only if nothing is
        // registered yet at all (e.g. a brand new install that hasn't run the MSI's own
        // registration step for some reason).
        private string FindRegisteredLayerPath(Microsoft.Win32.RegistryKey key, string jsonFileName)
        {
            if (key != null)
            {
                foreach (var valueName in key.GetValueNames())
                {
                    if (valueName.EndsWith("\\" + jsonFileName, StringComparison.OrdinalIgnoreCase))
                    {
                        return valueName;
                    }
                }
            }

            var assembly = Assembly.GetAssembly(GetType());
            var installPath = Path.GetDirectoryName(assembly.Location);
            return installPath + "\\" + jsonFileName;
        }

        private void layerSelector_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }

            Microsoft.Win32.RegistryKey key = null;
            try
            {
                key = Microsoft.Win32.Registry.LocalMachine.CreateSubKey("SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit");

                var newKitJsonPath = FindRegisteredLayerPath(key, "XR_APILAYER_NEWKITONTHEBLOCK_toolkit.json");
                var originalJsonPath = FindRegisteredLayerPath(key, "XR_APILAYER_MBUCCHIA_toolkit.json");

                // Registry convention here (matches the OpenXR loader spec and the existing
                // disableCheckbox above): value 0 = enabled, non-zero = disabled. Exactly one
                // of the two is enabled at a time - the dropdown makes that pairing explicit,
                // unlike two independent checkboxes which could both end up checked at once.
                if (layerSelector.SelectedIndex == 1)
                {
                    key.SetValue(newKitJsonPath, 0);
                    key.SetValue(originalJsonPath, 1);
                }
                else
                {
                    key.SetValue(newKitJsonPath, 1);
                    key.SetValue(originalJsonPath, 0);
                }
            }
            catch (Exception)
            {
                MessageBox.Show(this, "Failed to write to registry. Please make sure the app is running elevated.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                if (key != null)
                {
                    key.Close();
                }
            }

            // Re-run the same live check InitXrAsync() does, so layerActive updates without
            // needing a manual re-open of the app. A short delay first: reading the
            // registry back immediately after writing to HKEY_LOCAL_MACHINE can still
            // observe the pre-write value for a brief moment, so we give it a beat.
            // Reuse a single timer across calls instead of creating a new one on every
            // dropdown change - otherwise rapidly flipping the selector piles up multiple
            // independent timers all about to fire.
            if (layerRefreshTimer == null)
            {
                layerRefreshTimer = new System.Windows.Forms.Timer();
                layerRefreshTimer.Interval = 400;
                layerRefreshTimer.Tick += (s2, e2) =>
                {
                    layerRefreshTimer.Stop();
                    InitXrAsync();
                };
            }
            else
            {
                layerRefreshTimer.Stop();
            }
            layerRefreshTimer.Start();
        }

        private System.Windows.Forms.Timer layerRefreshTimer;

        private void reportIssuesLink_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            string githubIssues = "https://github.com/Tsevopolus/OpenXR-Toolkit/issues?q=is%3Aissue+is%3Aopen+label%3Abug";

            reportIssuesLink.LinkVisited = true;
            System.Diagnostics.Process.Start(githubIssues);
        }

        private void checkUpdatesLink_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            string homepage = "https://github.com/Tsevopolus/OpenXR-Toolkit";

            checkUpdatesLink.LinkVisited = true;
            System.Diagnostics.Process.Start(homepage);
        }

        private void disableCheckbox_CheckedChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }

            Microsoft.Win32.RegistryKey key = null;
            Microsoft.Win32.RegistryKey wmrKey = null;
            try
            {
                key = Microsoft.Win32.Registry.LocalMachine.CreateSubKey("SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit");

                // Disable whichever of the two layers is currently the selected one
                // in the dropdown, not always just the original.
                var targetJsonName = (layerSelector.SelectedIndex == 1)
                    ? "XR_APILAYER_NEWKITONTHEBLOCK_toolkit.json"
                    : "XR_APILAYER_MBUCCHIA_toolkit.json";
                var jsonPath = FindRegisteredLayerPath(key, targetJsonName);

                if (disableCheckbox.Checked)
                {
                    key.SetValue(jsonPath, 1);

                    // Always cleanup the global WMR options we might have set.
                    try
                    {
                        wmrKey = Microsoft.Win32.Registry.CurrentUser.CreateSubKey("SOFTWARE\\Microsoft\\OpenXR");
                        wmrKey.DeleteValue("MotionVectorEnabled");
                        wmrKey.DeleteValue("MinimumFrameInterval");
                        wmrKey.DeleteValue("MaximumFrameInterval");
                    }
                    catch (Exception)
                    {
                    }
                }
                else
                {
                    key.SetValue(jsonPath, 0);
                }
            }
            catch (Exception)
            {
                MessageBox.Show(this, "Failed to write to registry. Please make sure the app is running elevated.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                if (key != null)
                {
                    key.Close();
                }
                if (wmrKey != null)
                {
                    wmrKey.Close();
                }
            }

            var expectedValue = disableCheckbox.Checked;

            // The post-check below (did the requested state actually take?) used to run
            // right after a synchronous InitXr() call; now that the probe is async, it has
            // to move into the completion callback instead.
            RunXrProbeAsync(result =>
            {
                if (result.HasValue)
                {
                    ApplyXrLayerProbeResult(result.Value);
                }
                else
                {
                    MessageBox.Show(this, "Failed to initialize OpenXR", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }

                GC.Collect();
                GC.WaitForPendingFinalizers();
                GC.Collect();

                if (!expectedValue && disableCheckbox.Checked != expectedValue)
                {
                    MessageBox.Show(this, "Failed to activate OpenXR Toolkit. This can happen when incompatible software is installed or system dependencies are missing", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            });
        }

        private void safemodeCheckbox_CheckedChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }

            WriteSetting("safe_mode", safemodeCheckbox.Checked ? 1 : 0);
        }

        private void sceenshotCheckbox_CheckedChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }
            WriteSetting("enable_screenshot", screenshotCheckbox.Checked ? 1 : 0);
            screenshotFormat.Enabled = screenshotEye.Enabled = screenshotCheckbox.Checked;
        }

        private void menuVisibility_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }
            WriteSetting("menu_eye", menuVisibility.SelectedIndex);
        }

        private void leftKey_SelectedIndexChanged(object sender, EventArgs e)
        {
            AssignKey(leftKey, "key_left", new ComboBox[] { previousKey, nextKey, rightKey, screenshotKey });
        }

        private void nextKey_SelectedIndexChanged(object sender, EventArgs e)
        {
            AssignKey(nextKey, "key_menu", new ComboBox[] { leftKey, previousKey, rightKey, screenshotKey });
        }

        private void previousKey_SelectedIndexChanged(object sender, EventArgs e)
        {
            AssignKey(previousKey, "key_up", new ComboBox[] { leftKey, nextKey, rightKey, screenshotKey });
        }

        private void rightKey_SelectedIndexChanged(object sender, EventArgs e)
        {
            AssignKey(rightKey, "key_right", new ComboBox[] { leftKey, previousKey, nextKey, screenshotKey });
        }

        private void screenshotKey_SelectedIndexChanged(object sender, EventArgs e)
        {
            AssignKey(screenshotKey, "key_screenshot", new ComboBox[] { leftKey, previousKey, nextKey, rightKey });
        }

        private void AssignKey(ComboBox key, string setting, ComboBox[] otherKeys)
        {
            if (loading)
            {
                return;
            }

            if (key.SelectedIndex > 0)
            {
                foreach (var other in otherKeys)
                {
                    if (key.SelectedItem == other.SelectedItem)
                    {
                        MessageBox.Show("Please make the key assignments unique.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);

                        // Revert to the last valid selection instead of leaving the
                        // rejected duplicate shown in the box.
                        var wasLoading = loading;
                        loading = true;
                        key.SelectedItem = key.Tag ?? key.Items[0];
                        loading = wasLoading;
                        return;
                    }

                }

                foreach (var k in VirtualKeys)
                {
                    if (k.Item1 == (string)key.SelectedItem)
                    {
                        WriteSetting(setting, k.Item2);

                        // Force the splash to appear again after changing the menu key.
                        if (setting == "key_menu")
                        {
                            WriteSetting("key_menu_gen", ++keyMenuGen);
                        }

                        break;
                    }
                }
            }
            else
            {
                WriteSetting(setting, 0);
            }

            // Remember this as the last valid (non-duplicate) selection, for the revert above.
            key.Tag = key.SelectedItem;
        }



        private void ctrlModifierCheckbox_CheckedChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }
            WriteSetting("ctrl_modifier", ctrlModifierCheckbox.Checked ? 1 : 0);
        }

        private void altModifierCheckbox_CheckedChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }
            WriteSetting("alt_modifier", altModifierCheckbox.Checked ? 1 : 0);
        }

        private void openLog_Click(object sender, EventArgs e)
        {
            var processInfo = new ProcessStartInfo();
            processInfo.Verb = "Open";
            processInfo.UseShellExecute = true;
            var logLayerName = activeLayerName ?? "XR_APILAYER_MBUCCHIA_toolkit";
            processInfo.FileName = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) + "\\OpenXR-Toolkit\\logs\\" + logLayerName + ".log";
            try
            {
                Process.Start(processInfo);
            }
            catch (Win32Exception)
            {
                MessageBox.Show("Failed to open the log file. Please check attempt to locate '" + processInfo.FileName + "' manually.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void openScreenshots_Click(object sender, EventArgs e)
        {
            var processInfo = new ProcessStartInfo();
            processInfo.Verb = "Open";
            processInfo.UseShellExecute = true;
            processInfo.FileName = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) + "\\OpenXR-Toolkit\\screenshots";
            try
            {
                Process.Start(processInfo);
            }
            catch (Win32Exception)
            {
            }
        }

        private void pictureBox1_Click(object sender, EventArgs e)
        {
            string homepage = "https://github.com/Tsevopolus/OpenXR-Toolkit";

            checkUpdatesLink.LinkVisited = true;
            System.Diagnostics.Process.Start(homepage);
        }

        private void licences_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            var assembly = Assembly.GetAssembly(GetType());
            var installPath = Path.GetDirectoryName(assembly.Location);

            var processInfo = new ProcessStartInfo();
            processInfo.Verb = "Open";
            processInfo.FileName = "notepad";
            processInfo.Arguments = installPath + "\\THIRD_PARTY";
            try
            {
                Process.Start(processInfo);
                licences.LinkVisited = true;
            }
            catch (Win32Exception)
            {
            }
        }

        private void screenshotFormat_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }
            WriteSetting("screenshot_fileformat", screenshotFormat.SelectedIndex);
        }

        private void screenshotEye_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (loading)
            {
                return;
            }
            WriteSetting("screenshot_eye", screenshotEye.SelectedIndex);
        }

        private void traceButton_Click(object sender, EventArgs e)
        {
            if (!tracing)
            {
                if (MessageBox.Show(this, "PRIVACY WARNING: The trace file generated by this tool may include the following personal information \"Name of the computer\", \"Windows account name\". By continuing, you consent to have this information collected and potentially exposed online (if you share the resulting file online)." +
                        "\n\nNOTE: Do not use this functionality unless instructed by the developers and you understand what you are doing.\n\nDo you wish to continue?", "Confirmation", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.No)
                {
                    return;
                }

                var captureForWMR = MessageBox.Show(this, "Do you want to capture additional Windows Mixed Reality traces?", "Windows Mixed Reality tracing", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;

                // Cancel any pending traces.
                var processInfo = new ProcessStartInfo();
                processInfo.Verb = "Open";
                processInfo.FileName = "wpr";
                processInfo.Arguments = "-cancel";
                processInfo.CreateNoWindow = true;
                try
                {
                    Process.Start(processInfo);
                }
                catch (Win32Exception)
                {
                }

                var assembly = Assembly.GetAssembly(GetType());
                var installPath = Path.GetDirectoryName(assembly.Location);

                // Start a new trace.
                processInfo = new ProcessStartInfo();
                processInfo.Verb = "Open";
                processInfo.FileName = "wpr";
                processInfo.Arguments = "-start \"" + installPath + (captureForWMR ? "\\OXRTK_WMR.wprp" : "\\OXRTK.wprp") + "\" -filemode";
                processInfo.CreateNoWindow = true;
                processInfo.RedirectStandardOutput = true;
                processInfo.RedirectStandardError = true;
                processInfo.UseShellExecute = false;
                try
                {
                    var process = Process.Start(processInfo);
                    process.WaitForExit();

                    var output = process.StandardOutput.ReadToEnd();
                    var error = process.StandardError.ReadToEnd();
                    if (output != "" || error != "")
                    {
                        MessageBox.Show(this, "Standard output:\n" + output + "\nStandard error:\n" + error, "Outcome", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                }
                catch (Win32Exception)
                {
                    MessageBox.Show(this, "Failed to start tracing. Please make sure the app is running elevated.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }

                traceButton.Text = "Stop capture";
                tracing = true;

                MessageBox.Show(this, "Do not close the Companion app until after you have stopped the capture.", "Confirmation", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else
            {
                var processInfo = new ProcessStartInfo();
                processInfo.Verb = "Open";
                processInfo.FileName = "wpr";
                processInfo.Arguments = "-stop \"" + Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) + "\\OpenXR-Toolkit\\logs\\OXRTK.etl\"";
                processInfo.CreateNoWindow = true;
                processInfo.RedirectStandardOutput = true;
                processInfo.RedirectStandardError = true;
                processInfo.UseShellExecute = false;
                try
                {
                    var process = Process.Start(processInfo);
                    process.WaitForExit();

                    var output = process.StandardOutput.ReadToEnd();
                    var error = process.StandardError.ReadToEnd();
                    if (output != "" || error != "")
                    {
                        MessageBox.Show(this, "Standard output:\n" + output + "\nStandard error:\n" + error, "Outcome", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                }
                catch (Win32Exception)
                {
                    MessageBox.Show(this, "Failed to save trace. Please make sure the app is running elevated.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }

                traceButton.Text = "Capture trace";
                tracing = false;

                // Open the output folder with our file.
                processInfo = new ProcessStartInfo();
                processInfo.Verb = "Open";
                processInfo.UseShellExecute = true;
                processInfo.FileName = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData) + "\\OpenXR-Toolkit\\logs";
                try
                {
                    Process.Start(processInfo);
                }
                catch (Win32Exception)
                {
                }
            }
        }

        private void appList_ItemCheck(object sender, ItemCheckEventArgs e)
        {
            if (loading)
            {
                return;
            }
            var app = ((AppListEntry)appList.Items[e.Index]).RegistryName;
            Microsoft.Win32.RegistryKey key = null;
            try
            {
                key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RegPrefix + "\\" + app);
                key.SetValue("bypass", e.NewValue == CheckState.Checked ? 0 : 1, Microsoft.Win32.RegistryValueKind.DWord);
            }
            catch (Exception)
            {
                MessageBox.Show(this, "Failed to write to registry.", "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            finally
            {
                if (key != null)
                {
                    key.Close();
                }
            }

        }

        string appString = null;

        private void timer1_Tick(object sender, EventArgs e)
        {
            if (disableCheckbox.Checked)
            {
                return;
            }

            Microsoft.Win32.RegistryKey key = null;
            try
            {
                key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RegPrefix);
                appString = (string)key.GetValue("running", "");
                SetActiveString();
            }
            catch (Exception)
            {
            }
            finally
            {
                if (key != null)
                {
                    key.Close();
                }
            }

            if (updateAvailable != null)
            {
                // Capture the version text before clearing the flag - clearing it first
                // (as before) meant the message below always showed an empty version.
                var newVersion = updateAvailable;
                updateAvailable = null;
                if (MessageBox.Show(this, "A new version of OpenXR Toolkit is available: " + newVersion + ".\n\nDo you wish to open the download page?", "New version is available", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
                {
                    checkUpdatesLink_LinkClicked(null, null);
                }
            }
        }
    }
}
