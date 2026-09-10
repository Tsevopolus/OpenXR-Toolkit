using System;
using System.Collections;
using System.Collections.Generic;
using System.ComponentModel;
using System.Configuration;
using System.IO;
using System.Windows.Forms;

// Reference: https://www.c-sharpcorner.com/article/how-to-perform-custom-actions-and-upgrade-using-visual-studio-installer/
namespace SetupCustomActions
{
    [RunInstaller(true)]
    public partial class CustomActions : System.Configuration.Install.Installer
    {
        public CustomActions()
        {
        }

        protected override void OnAfterInstall(IDictionary savedState)
        {
            var installPath = Path.GetDirectoryName(base.Context.Parameters["AssemblyPath"]);

            // Two layers ship side by side now: our fork (NewKitOnTheBlock) and the
            // original 1.3.2 build, kept for direct comparison. Both get registered;
            // which one actually runs is controlled at runtime via each manifest's
            // own "disable_environment" variable, toggled from the companion app.
            var jsonName = "XR_APILAYER_NEWKITONTHEBLOCK_toolkit.json";
            var jsonPath = installPath + "\\" + jsonName;
            var originalJsonName = "XR_APILAYER_MBUCCHIA_toolkit.json";
            var originalJsonPath = installPath + "\\" + originalJsonName;

            // We want to add our layer at the very beginning, so that any other layer like the Ultraleap layer is following us.
            // We delete all entries, create our own, and recreate all entries.

            Microsoft.Win32.RegistryKey key;
            key = Microsoft.Win32.Registry.LocalMachine.CreateSubKey("SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit");
            var existingValues = key.GetValueNames();
            var entriesValues = new Dictionary<string, object>();
            foreach (var value in existingValues)
            {
                // Leave the layers that we want to be upstream of us.
                if (value.EndsWith("\\XR_APILAYER_MBUCCHIA_vulkan_d3d12_interop.json") ||
                    value.EndsWith("\\XR_APILAYER_NOVENDOR_vulkan_d3d12_interop.json") ||
                    value.EndsWith("OpenXR-Meta-Foveated\\openxr-api-layer.json") ||
                    value.EndsWith("OpenXR-Quad-Views-Foveated\\openxr-api-layer.json"))
                {
                    continue;
                }

                var keyValue = key.GetValue(value);
                var valueKind = key.GetValueKind(value);
                key.DeleteValue(value);

                // Some installers might have created bogus keys: https://github.com/KhronosGroup/OpenXR-SDK-Source/issues/335.
                // Make sure we don't re-create them.
                if (valueKind != Microsoft.Win32.RegistryValueKind.DWord)
                {
                    continue;
                }

                entriesValues.Add(value, keyValue);
            }

            bool detectedOldSoftware = false;
            // Register both, but only the fork enabled by default (0 = enabled,
            // 1 = disabled) - the two are not meant to run simultaneously. Switching
            // to the original afterwards is done from the companion app's checkbox.
            key.SetValue(jsonPath, 0);
            key.SetValue(originalJsonPath, 1);
            foreach (var value in existingValues)
            {
                // Do not re-create keys for previous versions of our layer.
                if (value.EndsWith("\\XR_APILAYER_NOVENDOR_nis_scaler.json") ||
                    value.EndsWith("\\XR_APILAYER_NOVENDOR_hand_to_controller.json"))
                {
                    detectedOldSoftware = true;
                    continue;
                }

                // Do not re-create our own keys. We did it before this loop.
                if (value.EndsWith("\\" + jsonName) ||
                    value.EndsWith("\\" + originalJsonName) ||
                    value.EndsWith("\\XR_APILAYER_NOVENDOR_toolkit.json"))
                {
                    continue;
                }

                // Skip keys that we did not delete.
                if (!entriesValues.ContainsKey(value))
                {
                    continue;
                }

                key.SetValue(value, entriesValues[value]);
            }

            key.Close();

            try
            {
                key = Microsoft.Win32.Registry.LocalMachine.CreateSubKey("SOFTWARE\\OpenXR_Toolkit");

                // Force showing the splash again after re-install.
                var gen = key.GetValue("key_menu_gen", null);
                if (gen != null)
                {
                    key.SetValue("key_menu_gen", (int)gen + 1);
                }
            }
            catch (Exception)
            {
            }
            finally
            {
                key.Close();
            }

            if (detectedOldSoftware)
            {
                MessageBox.Show("An older version of this software was detected (OpenXR-NIS-Scaler or OpenXR-Hand-To-Controller). " +
                    "It was deactivated, however please uninstall it through 'Add or remove programs' to free up disk space.",
                    "Warning", MessageBoxButtons.OK, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button1, MessageBoxOptions.DefaultDesktopOnly);
            }

            base.OnAfterInstall(savedState);
        }
    }
}
