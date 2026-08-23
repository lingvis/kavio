#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// initguid.h must be included before devpkey.h so DEVPKEY_Device_DriverInfPath
// gets a concrete definition in this translation unit with the Windows 8.1 SDK.
#include <initguid.h>
#include <devpkey.h>

#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>
#include <wincrypt.h>

#include <cwchar>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "VirtualDisplayControl.h"

#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "user32.lib")

namespace kavio_virtual_hdmi_control {
namespace {

HMODULE g_module = NULL;

const wchar_t kHardwareId[] = L"Root\\MttVDD";
const wchar_t kDeviceName[] = L"Virtual Display Driver";
const wchar_t kVddMonitorFriendlyName[] = L"VDD by MTT";
const wchar_t kRegistryPath[] = L"SOFTWARE\\Kavio\\VirtualHdmiVdd";
const wchar_t kStatusRegistryPath[] = L"SOFTWARE\\Kavio\\VirtualHdmiVddStatus";
const wchar_t kRegistryPublishedInf[] = L"PublishedInf";
const wchar_t kRegistryOwnedDevice[] = L"OwnedDevice";
const wchar_t kRegistryHadOriginalConfig[] = L"HadOriginalConfig";
const wchar_t kRegistryHadOriginalVddPath[] = L"HadOriginalVddPath";
const wchar_t kRegistryOriginalVddPath[] = L"OriginalVddPath";
const wchar_t kRegistryLastOperationCode[] = L"LastOperationCode";
const wchar_t kRegistryLastOperationDetail[] = L"LastOperationDetail";
const wchar_t kUpstreamRegistryPath[] = L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver";
const wchar_t kUpstreamVddPathValue[] = L"VDDPATH";
const wchar_t kVddConfigDirectory[] = L"C:\\VirtualDisplayDriver";
const wchar_t kVddConfigPath[] = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
const wchar_t kVddConfigBackupPath[] = L"C:\\VirtualDisplayDriver\\vdd_settings.kavio_backup.xml";
const wchar_t kVddPipeName[] = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";

class DeviceInfoSet {
public:
    explicit DeviceInfoSet(HDEVINFO handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~DeviceInfoSet() {
        if (handle_ != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(handle_);
    }
    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;
    HDEVINFO get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
private:
    HDEVINFO handle_;
};

std::wstring ModuleDirectory() {
    if (!g_module) return L".";

    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            g_module, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (length == 0) return L".";
        if (length < buffer.size() - 1) {
            std::wstring path(&buffer[0], length);
            const std::wstring::size_type slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? L"." : path.substr(0, slash);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring FormatWin32Error(DWORD error) {
    wchar_t* message = NULL;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, 0, reinterpret_cast<wchar_t*>(&message), 0, NULL);
    std::wstring result = length && message ? std::wstring(message, length)
                                            : L"unknown error";
    if (message) LocalFree(message);
    while (!result.empty() &&
           (result[result.size() - 1] == L'\r' || result[result.size() - 1] == L'\n')) {
        result.erase(result.size() - 1);
    }
    return result;
}

bool IsFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ContainsInsensitive(const wchar_t* value, const wchar_t* needle) {
    if (!value || !needle || !*needle) return false;
    const std::size_t value_length = std::wcslen(value);
    const std::size_t needle_length = std::wcslen(needle);
    if (needle_length > value_length) return false;
    for (std::size_t offset = 0; offset + needle_length <= value_length; ++offset) {
        if (_wcsnicmp(value + offset, needle, needle_length) == 0) return true;
    }
    return false;
}

bool MultiSzContains(const wchar_t* values, std::size_t character_count,
                     const wchar_t* expected) {
    if (!values || !expected) return false;
    const wchar_t* current = values;
    const wchar_t* end = values + character_count;
    while (current < end && *current) {
        if (_wcsicmp(current, expected) == 0) return true;
        current += std::wcslen(current) + 1;
    }
    return false;
}

bool GetHardwareIds(HDEVINFO set, SP_DEVINFO_DATA& data,
                    std::vector<wchar_t>& output) {
    DWORD type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(
        set, &data, SPDRP_HARDWAREID, &type, NULL, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) return false;
    output.resize((required + sizeof(wchar_t) - 1) / sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(
            set, &data, SPDRP_HARDWAREID, &type,
            reinterpret_cast<PBYTE>(&output[0]), required, NULL)) {
        return false;
    }
    return type == REG_MULTI_SZ || type == REG_SZ;
}

bool IsMatchingDevice(HDEVINFO set, SP_DEVINFO_DATA& data) {
    std::vector<wchar_t> ids;
    return GetHardwareIds(set, data, ids) &&
           MultiSzContains(&ids[0], ids.size(), kHardwareId);
}

bool GetPublishedInf(HDEVINFO set, SP_DEVINFO_DATA& data, std::wstring& output) {
    wchar_t buffer[MAX_PATH] = {};
    DEVPROPTYPE property_type = 0;
    DWORD required = 0;
    if (!SetupDiGetDevicePropertyW(
            set, &data, &DEVPKEY_Device_DriverInfPath, &property_type,
            reinterpret_cast<PBYTE>(buffer), sizeof(buffer), &required, 0)) {
        return false;
    }
    if (property_type != DEVPROP_TYPE_STRING || buffer[0] == L'\0') return false;
    output.assign(buffer);
    return true;
}

DWORD CountDevices(std::set<std::wstring>* published_infs) {
    DeviceInfoSet devices(SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES));
    if (!devices.valid()) return 0;

    DWORD count = 0;
    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices.get(), index, &data); ++index) {
        if (IsMatchingDevice(devices.get(), data)) {
            ++count;
            if (published_infs) {
                std::wstring published_inf;
                if (GetPublishedInf(devices.get(), data, published_inf)) {
                    published_infs->insert(published_inf);
                }
            }
        }
        data.cbSize = sizeof(data);
    }
    return count;
}

bool SetRegistryStringAt(const wchar_t* path, const wchar_t* name,
                         const std::wstring& value) {
    HKEY key = NULL;
    DWORD disposition = 0;
    const LONG create_result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE,
        NULL, &key, &disposition);
    if (create_result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(create_result));
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(
        key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) SetLastError(static_cast<DWORD>(result));
    return result == ERROR_SUCCESS;
}

bool GetRegistryStringAt(const wchar_t* path, const wchar_t* name,
                         std::wstring& value) {
    HKEY key = NULL;
    const LONG open_result =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key);
    if (open_result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(open_result));
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, name, NULL, &type, NULL, &bytes);
    if (result != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        SetLastError(static_cast<DWORD>(
            result != ERROR_SUCCESS ? result : ERROR_INVALID_DATA));
        return false;
    }

    std::vector<wchar_t> buffer((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(
        key, name, NULL, &type, reinterpret_cast<BYTE*>(&buffer[0]), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_SZ || buffer[0] == L'\0') {
        SetLastError(static_cast<DWORD>(
            result != ERROR_SUCCESS ? result : ERROR_INVALID_DATA));
        return false;
    }
    value.assign(&buffer[0]);
    return true;
}

bool DeleteRegistryValueAt(const wchar_t* path, const wchar_t* name) {
    HKEY key = NULL;
    const LONG open_result =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &key);
    if (open_result == ERROR_FILE_NOT_FOUND) return true;
    if (open_result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(open_result));
        return false;
    }
    const LONG result = RegDeleteValueW(key, name);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        SetLastError(static_cast<DWORD>(result));
        return false;
    }
    return true;
}

bool SetRegistryString(const wchar_t* name, const std::wstring& value) {
    return SetRegistryStringAt(kRegistryPath, name, value);
}

bool GetRegistryString(const wchar_t* name, std::wstring& value) {
    return GetRegistryStringAt(kRegistryPath, name, value);
}

bool SetRegistryDwordAt(const wchar_t* path, const wchar_t* name, DWORD value) {
    HKEY key = NULL;
    DWORD disposition = 0;
    const LONG create_result = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE,
        NULL, &key, &disposition);
    if (create_result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(create_result));
        return false;
    }
    const LONG result = RegSetValueExW(
        key, name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) SetLastError(static_cast<DWORD>(result));
    return result == ERROR_SUCCESS;
}

bool GetRegistryDwordAt(const wchar_t* path, const wchar_t* name, DWORD& value) {
    HKEY key = NULL;
    const LONG open_result =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key);
    if (open_result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(open_result));
        return false;
    }
    DWORD type = 0;
    DWORD bytes = sizeof(value);
    const LONG result = RegQueryValueExW(
        key, name, NULL, &type, reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_DWORD || bytes != sizeof(value)) {
        SetLastError(static_cast<DWORD>(
            result != ERROR_SUCCESS ? result : ERROR_INVALID_DATA));
        return false;
    }
    return true;
}

bool SetRegistryDword(const wchar_t* name, DWORD value) {
    return SetRegistryDwordAt(kRegistryPath, name, value);
}

bool GetRegistryDword(const wchar_t* name, DWORD& value) {
    return GetRegistryDwordAt(kRegistryPath, name, value);
}

void DeleteIntegrationRegistry() {
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kRegistryPath);
}

bool CreateRootDevice(DWORD& error) {
    DeviceInfoSet devices(SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, NULL));
    if (!devices.valid()) {
        error = GetLastError();
        return false;
    }

    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);
    if (!SetupDiCreateDeviceInfoW(
            devices.get(), kDeviceName, &GUID_DEVCLASS_DISPLAY,
            NULL, NULL, DICD_GENERATE_ID, &data)) {
        error = GetLastError();
        return false;
    }

    const std::size_t id_length = std::wcslen(kHardwareId);
    std::vector<wchar_t> hardware_ids(id_length + 2, L'\0');
    std::wmemcpy(&hardware_ids[0], kHardwareId, id_length);
    if (!SetupDiSetDeviceRegistryPropertyW(
            devices.get(), &data, SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(&hardware_ids[0]),
            static_cast<DWORD>(hardware_ids.size() * sizeof(wchar_t)))) {
        error = GetLastError();
        return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices.get(), &data)) {
        error = GetLastError();
        return false;
    }
    return true;
}

bool ValidateVddPackage(std::wstring& missing_path) {
    const std::wstring vdd_dir = ModuleDirectory() + L"\\vdd\\";
    const wchar_t* names[] = {
        L"MttVDD.inf",
        L"MttVDD.dll",
        L"mttvdd.cat",
        L"vdd_settings.xml"
    };
    for (std::size_t index = 0; index < ARRAYSIZE(names); ++index) {
        const std::wstring path = vdd_dir + names[index];
        if (!IsFile(path)) {
            missing_path = path;
            return false;
        }
    }
    return true;
}


bool CertificateNameContains(PCCERT_CONTEXT certificate, const wchar_t* expected) {
    if (!certificate || !expected || !*expected) return false;
    wchar_t subject[512] = {};
    wchar_t issuer[512] = {};
    CertGetNameStringW(
        certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL,
        subject, ARRAYSIZE(subject));
    CertGetNameStringW(
        certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, NULL,
        issuer, ARRAYSIZE(issuer));
    return ContainsInsensitive(subject, expected) ||
           ContainsInsensitive(issuer, expected);
}

bool AddCatalogSignerToTrustedPublisher(const std::wstring& catalog_path,
                                        DWORD& error,
                                        std::wstring& detail) {
    HCERTSTORE message_store = NULL;
    HCRYPTMSG message = NULL;
    DWORD encoding = 0;
    DWORD content_type = 0;
    DWORD format_type = 0;
    if (!CryptQueryObject(
            CERT_QUERY_OBJECT_FILE,
            catalog_path.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED |
                CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY,
            0,
            &encoding,
            &content_type,
            &format_type,
            &message_store,
            &message,
            NULL)) {
        error = GetLastError();
        detail = L"Failed to read the VDD catalog signature.";
        return false;
    }

    DWORD signer_size = 0;
    if (!CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, NULL, &signer_size) ||
        signer_size == 0) {
        error = GetLastError();
        CryptMsgClose(message);
        CertCloseStore(message_store, 0);
        detail = L"Failed to read the VDD catalog signer.";
        return false;
    }

    std::vector<BYTE> signer_buffer(signer_size);
    if (!CryptMsgGetParam(
            message, CMSG_SIGNER_INFO_PARAM, 0, &signer_buffer[0], &signer_size)) {
        error = GetLastError();
        CryptMsgClose(message);
        CertCloseStore(message_store, 0);
        detail = L"Failed to decode the VDD catalog signer.";
        return false;
    }

    CMSG_SIGNER_INFO* signer =
        reinterpret_cast<CMSG_SIGNER_INFO*>(&signer_buffer[0]);
    CERT_INFO signer_cert_info = {};
    signer_cert_info.Issuer = signer->Issuer;
    signer_cert_info.SerialNumber = signer->SerialNumber;
    PCCERT_CONTEXT signer_cert = CertFindCertificateInStore(
        message_store,
        X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
        0,
        CERT_FIND_SUBJECT_CERT,
        &signer_cert_info,
        NULL);
    if (!signer_cert) {
        error = GetLastError();
        CryptMsgClose(message);
        CertCloseStore(message_store, 0);
        detail = L"The signer certificate was not found in the VDD catalog.";
        return false;
    }

    // The public VirtualDrivers package is signed through SignPath Foundation.
    // Build only the catalog signer's own chain and refuse to trust an unrelated
    // catalog. This prevents a substituted vdd package from turning this helper
    // into a generic TrustedPublisher installer.
    CERT_CHAIN_PARA chain_parameters = {};
    chain_parameters.cbSize = sizeof(chain_parameters);
    PCCERT_CHAIN_CONTEXT chain_context = NULL;
    if (!CertGetCertificateChain(
            NULL,
            signer_cert,
            NULL,
            message_store,
            &chain_parameters,
            CERT_CHAIN_CACHE_END_CERT,
            NULL,
            &chain_context) ||
        !chain_context || chain_context->cChain == 0 ||
        !chain_context->rgpChain[0] ||
        chain_context->rgpChain[0]->cElement == 0) {
        error = GetLastError();
        if (chain_context) CertFreeCertificateChain(chain_context);
        CertFreeCertificateContext(signer_cert);
        CryptMsgClose(message);
        CertCloseStore(message_store, 0);
        detail = L"Failed to build the VDD catalog certificate chain.";
        return false;
    }

    PCERT_SIMPLE_CHAIN signer_chain = chain_context->rgpChain[0];
    bool signpath_chain = false;
    for (DWORD index = 0; index < signer_chain->cElement; ++index) {
        PCCERT_CONTEXT certificate = signer_chain->rgpElement[index]->pCertContext;
        if (CertificateNameContains(certificate, L"SignPath")) {
            signpath_chain = true;
            break;
        }
    }
    if (!signpath_chain) {
        error = ERROR_INVALID_DATA;
        CertFreeCertificateChain(chain_context);
        CertFreeCertificateContext(signer_cert);
        CryptMsgClose(message);
        CertCloseStore(message_store, 0);
        detail = L"Refusing to trust the VDD catalog because its signer chain is not SignPath-based.";
        return false;
    }

    HCERTSTORE trusted_publishers = CertOpenStore(
        CERT_STORE_PROV_SYSTEM_W,
        0,
        NULL,
        CERT_SYSTEM_STORE_LOCAL_MACHINE,
        L"TrustedPublisher");
    if (!trusted_publishers) {
        error = GetLastError();
        CertFreeCertificateChain(chain_context);
        CertFreeCertificateContext(signer_cert);
        CryptMsgClose(message);
        CertCloseStore(message_store, 0);
        detail = L"Failed to open the LocalMachine TrustedPublisher store.";
        return false;
    }

    // Match the current Virtual Driver Control installation flow: trust only
    // the catalog signer in LocalMachine\TrustedPublisher. Do not import the
    // whole certificate chain or modify the Trusted Root store.
    const BOOL added = CertAddCertificateContextToStore(
        trusted_publishers,
        signer_cert,
        CERT_STORE_ADD_REPLACE_EXISTING,
        NULL);
    if (!added) {
        error = GetLastError();
        detail = L"Failed to trust the VirtualDrivers VDD catalog signer.";
    }

    CertCloseStore(trusted_publishers, 0);
    CertFreeCertificateChain(chain_context);
    CertFreeCertificateContext(signer_cert);
    CryptMsgClose(message);
    CertCloseStore(message_store, 0);
    return added != FALSE;
}

bool PrepareVddConfiguration(DWORD& error, std::wstring& detail) {
    if (!CreateDirectoryW(kVddConfigDirectory, NULL)) {
        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            detail = L"Failed to create C:\\VirtualDisplayDriver.";
            return false;
        }
    }

    // VDD 25.7.23 resolves its settings directory through the upstream
    // VDDPATH registry value when present. Manage that value explicitly so the
    // driver reads the XML controlled by this App.
    DWORD path_tracked = 0;
    if (!GetRegistryDword(kRegistryHadOriginalVddPath, path_tracked)) {
        std::wstring original_path;
        const bool had_original_path = GetRegistryStringAt(
            kUpstreamRegistryPath, kUpstreamVddPathValue, original_path);
        if (!SetRegistryDword(kRegistryHadOriginalVddPath,
                              had_original_path ? 1u : 0u)) {
            error = GetLastError();
            detail = L"Failed to remember the previous VirtualDrivers VDDPATH setting.";
            return false;
        }
        if (had_original_path &&
            !SetRegistryString(kRegistryOriginalVddPath, original_path)) {
            error = GetLastError();
            detail = L"Failed to remember the previous VirtualDrivers VDDPATH value.";
            return false;
        }
    }

    if (!SetRegistryStringAt(
            kUpstreamRegistryPath, kUpstreamVddPathValue, kVddConfigDirectory)) {
        error = GetLastError();
        detail = L"Failed to point VirtualDrivers VDDPATH at C:\\VirtualDisplayDriver.";
        return false;
    }

    DWORD had_original = 0;
    if (!GetRegistryDword(kRegistryHadOriginalConfig, had_original)) {
        had_original = IsFile(kVddConfigPath) ? 1u : 0u;
        if (had_original != 0 && !IsFile(kVddConfigBackupPath)) {
            if (!CopyFileW(kVddConfigPath, kVddConfigBackupPath, TRUE)) {
                error = GetLastError();
                detail = L"Failed to back up the existing VDD configuration.";
                return false;
            }
        }
        SetRegistryDword(kRegistryHadOriginalConfig, had_original);
    }

    const std::wstring source = ModuleDirectory() + L"\\vdd\\vdd_settings.xml";
    if (!CopyFileW(source.c_str(), kVddConfigPath, FALSE)) {
        error = GetLastError();
        detail = L"Failed to install the Kavio VDD configuration.";
        return false;
    }
    return true;
}

bool RestoreVddConfiguration(DWORD& error, std::wstring& detail) {
    DWORD had_original = 0;
    const bool tracked = GetRegistryDword(kRegistryHadOriginalConfig, had_original);
    if (tracked) {
        if (had_original != 0) {
            if (!IsFile(kVddConfigBackupPath)) {
                error = ERROR_FILE_NOT_FOUND;
                detail = L"The previous VDD configuration backup is missing.";
                return false;
            }
            if (!CopyFileW(kVddConfigBackupPath, kVddConfigPath, FALSE)) {
                error = GetLastError();
                detail = L"Failed to restore the previous VDD configuration.";
                return false;
            }
            DeleteFileW(kVddConfigBackupPath);
        } else {
            DeleteFileW(kVddConfigPath);
            DeleteFileW(kVddConfigBackupPath);
        }
    }

    DWORD had_original_path = 0;
    if (GetRegistryDword(kRegistryHadOriginalVddPath, had_original_path)) {
        if (had_original_path != 0) {
            std::wstring original_path;
            if (!GetRegistryString(kRegistryOriginalVddPath, original_path) ||
                !SetRegistryStringAt(
                    kUpstreamRegistryPath, kUpstreamVddPathValue, original_path)) {
                error = GetLastError();
                detail = L"Failed to restore the previous VirtualDrivers VDDPATH value.";
                return false;
            }
        } else if (!DeleteRegistryValueAt(
                       kUpstreamRegistryPath, kUpstreamVddPathValue)) {
            error = GetLastError();
            detail = L"Failed to restore the default VirtualDrivers VDDPATH behavior.";
            return false;
        }
    }
    return true;
}

std::wstring CurrentVddConfigPath() {
    std::wstring directory;
    if (GetRegistryStringAt(
            kUpstreamRegistryPath, kUpstreamVddPathValue, directory) &&
        !directory.empty()) {
        while (!directory.empty() &&
               (directory[directory.size() - 1] == L'\\' ||
                directory[directory.size() - 1] == L'/')) {
            directory.erase(directory.size() - 1);
        }
        return directory + L"\\vdd_settings.xml";
    }
    return kVddConfigPath;
}

bool ReadVddConfiguredMonitorCount(DWORD& count) {
    count = 0;
    const std::wstring config_path = CurrentVddConfigPath();
    HANDLE file = CreateFileW(
        config_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER file_size = {};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
        file_size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    std::vector<char> bytes(static_cast<std::size_t>(file_size.QuadPart) + 1, '\0');
    DWORD read = 0;
    const BOOL read_ok = ReadFile(
        file, &bytes[0], static_cast<DWORD>(file_size.QuadPart), &read, NULL);
    CloseHandle(file);
    if (!read_ok || read == 0) return false;

    const std::string text(&bytes[0], read);
    const std::string open_tag = "<count>";
    const std::string close_tag = "</count>";
    const std::string::size_type begin_tag = text.find(open_tag);
    if (begin_tag == std::string::npos) return false;
    std::string::size_type pos = begin_tag + open_tag.size();
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' ||
            text[pos] == '\r' || text[pos] == '\n')) {
        ++pos;
    }

    DWORD parsed = 0;
    bool has_digit = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        has_digit = true;
        parsed = parsed * 10 + static_cast<DWORD>(text[pos] - '0');
        if (parsed > 16) return false;
        ++pos;
    }
    if (!has_digit || parsed == 0) return false;

    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' ||
            text[pos] == '\r' || text[pos] == '\n')) {
        ++pos;
    }
    if (text.compare(pos, close_tag.size(), close_tag) != 0) return false;

    count = parsed;
    return true;
}

bool SendVddPipeCommand(const std::wstring& command, std::wstring& response) {
    response.clear();
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (!WaitNamedPipeW(kVddPipeName, 250)) {
            Sleep(100);
            continue;
        }

        HANDLE pipe = CreateFileW(
            kVddPipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(command.size() * sizeof(wchar_t));
        const BOOL write_ok = WriteFile(pipe, command.data(), bytes, &written, NULL);
        if (!write_ok || written != bytes) {
            CloseHandle(pipe);
            Sleep(100);
            continue;
        }

        wchar_t buffer[1024] = {};
        DWORD read = 0;
        if (ReadFile(pipe, buffer, sizeof(buffer) - sizeof(wchar_t), &read, NULL) && read > 0) {
            response.assign(buffer, read / sizeof(wchar_t));
        }
        CloseHandle(pipe);
        return true;
    }
    return false;
}

bool PipeResponseRejectsCommand(const std::wstring& response) {
    return ContainsInsensitive(response.c_str(), L"unknown") ||
           ContainsInsensitive(response.c_str(), L"invalid command") ||
           ContainsInsensitive(response.c_str(), L"unrecognized") ||
           ContainsInsensitive(response.c_str(), L"not supported");
}

bool SetVddDisplayCount(DWORD count, std::wstring& response) {
    // The packaged 25.7.23 driver accepts SETCOUNT. SETDISPLAYCOUNT is also
    // accepted as an alternate command for compatible VirtualDrivers packages.
    std::wostringstream primary_command;
    primary_command << L"SETCOUNT " << count;
    if (SendVddPipeCommand(primary_command.str(), response) &&
        !PipeResponseRejectsCommand(response)) {
        return true;
    }

    std::wostringstream alternate_command;
    alternate_command << L"SETDISPLAYCOUNT " << count;
    return SendVddPipeCommand(alternate_command.str(), response) &&
           !PipeResponseRejectsCommand(response);
}

bool IsVddControlPipeReady() {
    std::wstring response;
    if (!SendVddPipeCommand(L"PING", response)) return false;
    // A successful pipe transaction is enough to prove that the installed VDD
    // UMDF service is alive. When a response is present, reject explicit
    // protocol errors.
    return !PipeResponseRejectsCommand(response);
}

bool VddModeIsAvailable(const wchar_t* device_name) {
    DEVMODEW candidate = {};
    candidate.dmSize = sizeof(candidate);
    for (DWORD index = 0;
         EnumDisplaySettingsExW(device_name, index, &candidate, 0);
         ++index) {
        if (candidate.dmPelsWidth == 720 &&
            candidate.dmPelsHeight == 720 &&
            (candidate.dmDisplayFrequency == 0 || candidate.dmDisplayFrequency == 60)) {
            return true;
        }
        candidate = DEVMODEW{};
        candidate.dmSize = sizeof(candidate);
    }
    return false;
}

bool ConfigureVddMode(std::wstring& detail) {
    bool saw_vdd = false;
    bool saw_expected_mode = false;
    std::wstring last_device_name;
    DWORD last_width = 0;
    DWORD last_height = 0;
    DWORD last_frequency = 0;

    // SETCOUNT causes the driver to rebuild its monitor mode list
    // asynchronously.  Wait for 720x720 to actually appear before asking
    // Windows to switch to it; otherwise ChangeDisplaySettingsEx reports
    // BADMODE and leaves the monitor at the existing/default mode.
    for (int retry = 0; retry < 80; ++retry) {
        DISPLAY_DEVICEW adapter = {};
        adapter.cb = sizeof(adapter);
        for (DWORD adapter_index = 0;
             EnumDisplayDevicesW(NULL, adapter_index, &adapter, 0);
             ++adapter_index) {
            DISPLAY_DEVICEW monitor = {};
            monitor.cb = sizeof(monitor);
            bool is_vdd = false;
            for (DWORD monitor_index = 0;
                 EnumDisplayDevicesW(adapter.DeviceName, monitor_index, &monitor, 0);
                 ++monitor_index) {
                if (_wcsicmp(monitor.DeviceString, kVddMonitorFriendlyName) == 0 ||
                    ContainsInsensitive(monitor.DeviceString, kVddMonitorFriendlyName) ||
                    ContainsInsensitive(monitor.DeviceID, L"MTT1337")) {
                    is_vdd = true;
                    break;
                }
                monitor = DISPLAY_DEVICEW{};
                monitor.cb = sizeof(monitor);
            }
            if (!is_vdd) {
                is_vdd = ContainsInsensitive(adapter.DeviceString, L"Virtual Display Driver") ||
                         ContainsInsensitive(adapter.DeviceString, L"IddSampleDriver Device HDR");
            }
            if (!is_vdd) {
                adapter = DISPLAY_DEVICEW{};
                adapter.cb = sizeof(adapter);
                continue;
            }

            saw_vdd = true;
            last_device_name = adapter.DeviceName;

            DEVMODEW current = {};
            current.dmSize = sizeof(current);
            const bool has_current_mode =
                EnumDisplaySettingsExW(
                    adapter.DeviceName, ENUM_CURRENT_SETTINGS, &current, 0) != FALSE;
            if (has_current_mode) {
                last_width = current.dmPelsWidth;
                last_height = current.dmPelsHeight;
                last_frequency = current.dmDisplayFrequency;
                if (current.dmPelsWidth == 720 &&
                    current.dmPelsHeight == 720 &&
                    (current.dmDisplayFrequency == 0 ||
                     current.dmDisplayFrequency == 60)) {
                    return true;
                }
            }

            if (!VddModeIsAvailable(adapter.DeviceName)) {
                adapter = DISPLAY_DEVICEW{};
                adapter.cb = sizeof(adapter);
                continue;
            }
            saw_expected_mode = true;

            DEVMODEW mode = has_current_mode ? current : DEVMODEW{};
            mode.dmSize = sizeof(mode);
            const bool attached =
                (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
            if (!attached || !has_current_mode) {
                mode.dmPosition.x =
                    GetSystemMetrics(SM_XVIRTUALSCREEN) +
                    GetSystemMetrics(SM_CXVIRTUALSCREEN);
                mode.dmPosition.y = GetSystemMetrics(SM_YVIRTUALSCREEN);
            }
            mode.dmPelsWidth = 720;
            mode.dmPelsHeight = 720;
            mode.dmBitsPerPel = 32;
            mode.dmDisplayFrequency = 60;
            mode.dmFields |= DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT |
                             DM_BITSPERPEL | DM_DISPLAYFREQUENCY;

            const LONG set_result = ChangeDisplaySettingsExW(
                adapter.DeviceName, &mode, NULL,
                CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
            if (set_result != DISP_CHANGE_SUCCESSFUL &&
                set_result != DISP_CHANGE_RESTART) {
                std::wostringstream stream;
                stream << L"VDD was found as " << adapter.DeviceName
                       << L" and 720x720@60 is advertised, but Windows rejected the mode, result="
                       << set_result << L".";
                detail = stream.str();
                return false;
            }

            const LONG apply_result =
                ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
            if (apply_result != DISP_CHANGE_SUCCESSFUL &&
                apply_result != DISP_CHANGE_RESTART) {
                std::wostringstream stream;
                stream << L"VDD mode was staged but Windows could not apply the display topology, result="
                       << apply_result << L".";
                detail = stream.str();
                return false;
            }

            // Do not report success until Windows exposes the requested mode
            // as the current mode.  This also gives DWM/DXGI time to rebuild
            // the output before the normal Host capture thread retries.
            for (int verify = 0; verify < 30; ++verify) {
                DEVMODEW active = {};
                active.dmSize = sizeof(active);
                if (EnumDisplaySettingsExW(
                        adapter.DeviceName, ENUM_CURRENT_SETTINGS, &active, 0) &&
                    active.dmPelsWidth == 720 && active.dmPelsHeight == 720 &&
                    (active.dmDisplayFrequency == 0 || active.dmDisplayFrequency == 60)) {
                    return true;
                }
                Sleep(100);
            }

            detail = L"Windows accepted the VDD mode change, but 720x720 did not become active.";
            return false;
        }
        Sleep(100);
    }

    std::wostringstream stream;
    if (!saw_vdd) {
        stream << L"VDD installed, but its MTT1337/VDD by MTT monitor endpoint did not appear.";
    } else if (!saw_expected_mode) {
        stream << L"VDD monitor " << last_device_name
               << L" appeared, but 720x720@60 never entered its mode list after reloading vdd_settings.xml";
        if (last_width != 0 && last_height != 0) {
            stream << L" (current " << last_width << L"x" << last_height;
            if (last_frequency != 0) stream << L"@" << last_frequency;
            stream << L")";
        }
        stream << L".";
    } else {
        stream << L"VDD 720x720 mode was discovered but could not be activated.";
    }
    detail = stream.str();
    return false;
}

bool InstallVdd(bool& reboot_required, DWORD& error, std::wstring& detail) {
    std::wstring missing_path;
    if (!ValidateVddPackage(missing_path)) {
        error = ERROR_FILE_NOT_FOUND;
        detail = L"Missing staged VirtualDrivers VDD file: " + missing_path;
        return false;
    }

    if (!PrepareVddConfiguration(error, detail)) return false;

    DWORD owned_value = 0;
    const bool ownership_known = GetRegistryDword(kRegistryOwnedDevice, owned_value);
    const DWORD existing_count = CountDevices(NULL);
    bool owned_device = ownership_known ? owned_value != 0 : existing_count == 0;
    if (!ownership_known) SetRegistryDword(kRegistryOwnedDevice, owned_device ? 1u : 0u);

    if (existing_count == 0) {
        if (!CreateRootDevice(error)) return false;
    }

    // Do not force-install the same display driver on every Configure click.
    // If an existing Root\MttVDD device has a live control pipe, the driver
    // is already running and only its XML/live monitor state needs updating.
    // This avoids an unnecessary adapter restart (and an extra screen flash).
    const bool existing_driver_alive =
        existing_count != 0 && IsVddControlPipeReady();
    if (existing_count != 0 && !owned_device && !existing_driver_alive) {
        error = ERROR_NOT_READY;
        detail = L"An existing non-Kavio Root\\MttVDD device is present but its control pipe is not responding; Kavio will not force-update a shared VDD installation.";
        return false;
    }
    if (existing_count == 0 || (owned_device && !existing_driver_alive)) {
        const std::wstring catalog_path = ModuleDirectory() + L"\\vdd\\mttvdd.cat";
        if (!AddCatalogSignerToTrustedPublisher(catalog_path, error, detail)) {
            return false;
        }

        const std::wstring inf_path = ModuleDirectory() + L"\\vdd\\MttVDD.inf";
        BOOL reboot = FALSE;
        if (!UpdateDriverForPlugAndPlayDevicesW(
                NULL, kHardwareId, inf_path.c_str(), INSTALLFLAG_FORCE, &reboot)) {
            error = GetLastError();
            detail = L"Failed to bind/update the staged VirtualDrivers VDD package.";
            return false;
        }
        reboot_required = reboot != FALSE;

        std::set<std::wstring> published_infs;
        CountDevices(&published_infs);
        if (!published_infs.empty()) {
            SetRegistryString(kRegistryPublishedInf, *published_infs.begin());
        }
    }

    std::wstring pipe_response;
    if (!SetVddDisplayCount(1, pipe_response)) {
        error = ERROR_NOT_READY;
        detail = L"VirtualDrivers VDD was installed, but its control pipe could not reload the Kavio configuration.";
        if (!pipe_response.empty()) detail += L" Pipe response: " + pipe_response;
        return false;
    }

    std::wstring mode_detail;
    if (!ConfigureVddMode(mode_detail)) {
        detail = mode_detail;
        if (!reboot_required) {
            error = ERROR_NOT_READY;
            return false;
        }
    }
    return true;
}

bool RemoveMatchingDevices(bool& reboot_required, DWORD& error,
                           DWORD& removed_count,
                           std::set<std::wstring>& published_infs) {
    DeviceInfoSet devices(SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES));
    if (!devices.valid()) {
        error = GetLastError();
        return false;
    }

    removed_count = 0;
    reboot_required = false;
    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices.get(), index, &data); ++index) {
        if (!IsMatchingDevice(devices.get(), data)) {
            data.cbSize = sizeof(data);
            continue;
        }

        std::wstring published_inf;
        if (GetPublishedInf(devices.get(), data, published_inf)) {
            published_infs.insert(published_inf);
        }

        SP_REMOVEDEVICE_PARAMS remove = {};
        remove.ClassInstallHeader.cbSize = sizeof(remove.ClassInstallHeader);
        remove.ClassInstallHeader.InstallFunction = DIF_REMOVE;
        remove.Scope = DI_REMOVEDEVICE_GLOBAL;
        remove.HwProfile = 0;
        if (!SetupDiSetClassInstallParamsW(
                devices.get(), &data,
                &remove.ClassInstallHeader, sizeof(remove)) ||
            !SetupDiCallClassInstaller(DIF_REMOVE, devices.get(), &data)) {
            error = GetLastError();
            return false;
        }

        SP_DEVINSTALL_PARAMS_W install_params = {};
        install_params.cbSize = sizeof(install_params);
        if (SetupDiGetDeviceInstallParamsW(devices.get(), &data, &install_params)) {
            reboot_required = reboot_required ||
                (install_params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
        }
        ++removed_count;
        data.cbSize = sizeof(data);
    }
    return true;
}

bool RemoveVddIntegration(bool& reboot_required, DWORD& error,
                          DWORD& removed_count, DWORD& removed_package_count,
                          bool& shared_vdd_kept, std::wstring& detail) {
    DWORD owned_value = 0;
    const bool owned_device =
        GetRegistryDword(kRegistryOwnedDevice, owned_value) && owned_value != 0;

    removed_count = 0;
    removed_package_count = 0;
    shared_vdd_kept = !owned_device && CountDevices(NULL) > 0;

    if (owned_device) {
        std::set<std::wstring> published_infs;
        std::wstring saved_inf;
        if (GetRegistryString(kRegistryPublishedInf, saved_inf)) {
            published_infs.insert(saved_inf);
        }

        if (!RemoveMatchingDevices(
                reboot_required, error, removed_count, published_infs)) {
            return false;
        }

        for (std::set<std::wstring>::const_iterator it = published_infs.begin();
             it != published_infs.end(); ++it) {
            if (SetupUninstallOEMInfW(it->c_str(), SUOI_FORCEDELETE, NULL)) {
                ++removed_package_count;
                continue;
            }
            const DWORD remove_error = GetLastError();
            if (remove_error == ERROR_FILE_NOT_FOUND || remove_error == ERROR_NOT_FOUND) {
                continue;
            }
            error = remove_error;
            detail = L"Failed to remove driver package " + *it;
            return false;
        }
    }

    if (!RestoreVddConfiguration(error, detail)) return false;

    // When the VDD is shared with another application, restore both its XML
    // configuration and its live monitor count. SetVddDisplayCount() selects a
    // command accepted by the available VirtualDrivers control pipe.
    if (shared_vdd_kept && IsFile(CurrentVddConfigPath())) {
        DWORD restored_count = 0;
        if (ReadVddConfiguredMonitorCount(restored_count)) {
            std::wstring pipe_response;
            if (!SetVddDisplayCount(restored_count, pipe_response)) {
                std::wostringstream warning;
                warning << L"The previous VDD configuration file was restored, but its live "
                        << L"display count (" << restored_count
                        << L") could not be re-applied through the VDD control pipe.";
                detail = warning.str();
            }
        } else {
            detail = L"The previous VDD configuration file was restored, but its monitor count could not be parsed for live reload.";
        }
    }

    DeleteIntegrationRegistry();
    return true;
}

 } // namespace

void SetModuleHandle(void* module_handle) {
    g_module = static_cast<HMODULE>(module_handle);
}

bool IsInstalled() {
    return CountDevices(NULL) > 0;
}

bool ValidatePackage(std::wstring& missing_path) {
    return ValidateVddPackage(missing_path);
}

bool GetCurrentDisplayMode(std::wstring& mode_text) {
    mode_text.clear();
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD adapter_index = 0;
         EnumDisplayDevicesW(NULL, adapter_index, &adapter, 0);
         ++adapter_index) {
        bool is_vdd = false;
        DISPLAY_DEVICEW monitor = {};
        monitor.cb = sizeof(monitor);
        for (DWORD monitor_index = 0;
             EnumDisplayDevicesW(adapter.DeviceName, monitor_index, &monitor, 0);
             ++monitor_index) {
            if (_wcsicmp(monitor.DeviceString, kVddMonitorFriendlyName) == 0 ||
                ContainsInsensitive(monitor.DeviceString, kVddMonitorFriendlyName) ||
                ContainsInsensitive(monitor.DeviceID, L"MTT1337")) {
                is_vdd = true;
                break;
            }
            monitor = DISPLAY_DEVICEW{};
            monitor.cb = sizeof(monitor);
        }
        if (!is_vdd) {
            is_vdd = ContainsInsensitive(adapter.DeviceString, L"Virtual Display Driver") ||
                     ContainsInsensitive(adapter.DeviceString, L"IddSampleDriver Device HDR");
        }
        if (!is_vdd) {
            adapter = DISPLAY_DEVICEW{};
            adapter.cb = sizeof(adapter);
            continue;
        }

        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(
                adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
            mode_text = std::wstring(adapter.DeviceName) + L" (not attached to desktop)";
            return true;
        }

        std::wostringstream stream;
        stream << adapter.DeviceName << L" "
               << mode.dmPelsWidth << L"x" << mode.dmPelsHeight;
        if (mode.dmDisplayFrequency != 0) {
            stream << L"@" << mode.dmDisplayFrequency;
        }
        mode_text = stream.str();
        return true;
    }
    return false;
}

int RunAdminAction(const wchar_t* action, std::wstring* detail_out) {
    const auto finish = [&](int code, const std::wstring& text) -> int {
        if (detail_out) *detail_out = text;
        SetRegistryDwordAt(kStatusRegistryPath, kRegistryLastOperationCode,
                           static_cast<DWORD>(code));
        SetRegistryStringAt(kStatusRegistryPath, kRegistryLastOperationDetail, text);
        return code;
    };

    if (!action || !*action) {
        return finish(2, L"Missing VDD administrator action.");
    }

    DWORD error = ERROR_SUCCESS;
    bool reboot_required = false;
    std::wstring detail;

    if (_wcsicmp(action, L"install") == 0) {
        if (!InstallVdd(reboot_required, error, detail)) {
            std::wostringstream stream;
            stream << L"VDD install/configure failed: " << error << L" ("
                   << FormatWin32Error(error) << L")";
            if (!detail.empty()) stream << L". " << detail;
            return finish(1, stream.str());
        }

        const std::wstring success = detail.empty()
            ? L"VirtualDrivers VDD is installed/configured for Kavio 720x720@60."
            : detail;
        return finish(reboot_required ? 3 : 0, success);
    }

    if (_wcsicmp(action, L"uninstall") == 0) {
        DWORD removed_count = 0;
        DWORD removed_package_count = 0;
        bool shared_vdd_kept = false;
        if (!RemoveVddIntegration(
                reboot_required, error, removed_count, removed_package_count,
                shared_vdd_kept, detail)) {
            std::wostringstream stream;
            stream << L"VDD removal failed: " << error << L" ("
                   << FormatWin32Error(error) << L")";
            if (!detail.empty()) stream << L". " << detail;
            return finish(1, stream.str());
        }

        std::wostringstream stream;
        if (shared_vdd_kept) {
            stream << L"Kavio VDD configuration removed; existing third-party VDD device was kept.";
        } else {
            stream << L"Removed " << removed_count << L" Kavio-created VDD device(s) and "
                   << removed_package_count << L" driver package(s).";
        }
        if (!detail.empty()) stream << L" " << detail;
        return finish(reboot_required ? 3 : 0, stream.str());
    }

    return finish(2, L"Unsupported VDD administrator action.");
}


bool GetLastOperationDetail(std::wstring& detail, DWORD* code) {
    DWORD stored_code = 0;
    const bool have_code = GetRegistryDwordAt(
        kStatusRegistryPath, kRegistryLastOperationCode, stored_code);
    const bool have_detail = GetRegistryStringAt(
        kStatusRegistryPath, kRegistryLastOperationDetail, detail);
    if (code) *code = have_code ? stored_code : 0;
    return have_detail;
}

} // namespace kavio_virtual_hdmi_control

extern "C" __declspec(dllexport) void CALLBACK
KavioVirtualDisplayAdmin(HWND, HINSTANCE, LPSTR command_line, int) {
    // rundll32 calls an undecorated entry point with the classic ANSI
    // callback signature.  Do not use LPWSTR here: doing so interprets the
    // ASCII bytes of "install"/"uninstall" as UTF-16 and makes every
    // valid action fall through to the invalid-action exit code (2).
    //
    // Terminate the helper process with the 0/1/2/3 operation result expected
    // by the panel, so no additional executable is required.
    std::string action = command_line ? command_line : "";
    const std::string::size_type first = action.find_first_not_of(" \t\r\n\"");
    const std::string::size_type last = action.find_last_not_of(" \t\r\n\"");
    if (first == std::string::npos) {
        ExitProcess(2);
    }
    action = action.substr(first, last - first + 1);

    const wchar_t* wide_action = NULL;
    if (_stricmp(action.c_str(), "install") == 0) {
        wide_action = L"install";
    } else if (_stricmp(action.c_str(), "uninstall") == 0) {
        wide_action = L"uninstall";
    } else {
        ExitProcess(2);
    }

    ExitProcess(static_cast<UINT>(
        kavio_virtual_hdmi_control::RunAdminAction(wide_action, NULL)));
}
