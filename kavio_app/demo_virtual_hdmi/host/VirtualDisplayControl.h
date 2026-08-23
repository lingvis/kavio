#pragma once

#include <string>

namespace kavio_virtual_hdmi_control {

// Set by host.dll DllMain so the integrated controller can locate the vdd/
// directory even when host.dll is loaded by an elevated rundll32 process.
void SetModuleHandle(void* module_handle);

// Returns true when at least one Root\\MttVDD device is present.
bool IsInstalled();

// Returns the current Windows desktop mode for the MTT virtual monitor.
bool GetCurrentDisplayMode(std::wstring& mode_text);

// Verifies that the signed public VirtualDrivers package staged beside
// host.dll contains all files required by the integrated installer.
bool ValidatePackage(std::wstring& missing_path);

// Executes an administrator-only VDD action in the current process.
// Return codes intentionally match the former VirtualDisplayCtl.exe:
//   0 = success, 1 = failure, 2 = invalid action, 3 = success/reboot required.
int RunAdminAction(const wchar_t* action, std::wstring* detail_out);

// Reads the detailed result written by the last elevated administrator action.
// This lets the normal Panel show the real configuration failure instead of
// only a rundll32 process exit code.
bool GetLastOperationDetail(std::wstring& detail, unsigned long* code);

} // namespace kavio_virtual_hdmi_control
