#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "VirtualDisplayCapture.h"

#include <algorithm>
#include <cwchar>
#include <cstring>
#include <sstream>

#include "VirtualHdmiProtocol.h"

namespace {

using Microsoft::WRL::ComPtr;
using namespace kavio_virtual_hdmi;

const wchar_t kVddMonitorFriendlyName[] = L"VDD by MTT";

std::uint64_t NowNanoseconds() {
    FILETIME value = {};
    GetSystemTimePreciseAsFileTime(&value);
    ULARGE_INTEGER ticks = {};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart * 100ull;
}

std::wstring HResultText(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
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

bool IsVddFriendlyName(const wchar_t* name) {
    return name && (_wcsicmp(name, kVddMonitorFriendlyName) == 0 ||
                    ContainsInsensitive(name, kVddMonitorFriendlyName));
}

bool FindVddViaDisplayConfig(std::wstring& gdi_name) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    LONG result = GetDisplayConfigBufferSizes(
        QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (result != ERROR_SUCCESS || path_count == 0) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    result = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &path_count,
        paths.data(),
        &mode_count,
        modes.data(),
        NULL);
    if (result != ERROR_SUCCESS) return false;
    paths.resize(path_count);

    for (std::size_t index = 0; index < paths.size(); ++index) {
        const DISPLAYCONFIG_PATH_INFO& path = paths[index];

        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS ||
            (!IsVddFriendlyName(target.monitorFriendlyDeviceName) &&
             !ContainsInsensitive(target.monitorDevicePath, L"MTT1337"))) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
            source.viewGdiDeviceName[0] != L'\0') {
            gdi_name.assign(source.viewGdiDeviceName);
            return true;
        }
    }
    return false;
}

bool FindVddViaGdi(std::wstring& gdi_name) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD adapter_index = 0;
         EnumDisplayDevicesW(NULL, adapter_index, &adapter, 0);
         ++adapter_index) {
        const bool active =
            (adapter.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0 &&
            (adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        if (active) {
            DISPLAY_DEVICEW monitor = {};
            monitor.cb = sizeof(monitor);
            for (DWORD monitor_index = 0;
                 EnumDisplayDevicesW(adapter.DeviceName, monitor_index, &monitor, 0);
                 ++monitor_index) {
                if (IsVddFriendlyName(monitor.DeviceString) ||
                    ContainsInsensitive(monitor.DeviceID, L"MTT1337")) {
                    gdi_name.assign(adapter.DeviceName);
                    return true;
                }
                monitor = DISPLAY_DEVICEW{};
                monitor.cb = sizeof(monitor);
            }

            if (ContainsInsensitive(adapter.DeviceString, L"Virtual Display Driver") ||
                ContainsInsensitive(adapter.DeviceString, L"IddSampleDriver Device HDR")) {
                gdi_name.assign(adapter.DeviceName);
                return true;
            }
        }
        adapter = DISPLAY_DEVICEW{};
        adapter.cb = sizeof(adapter);
    }
    return false;
}

bool FindVddDisplay(std::wstring& gdi_name) {
    gdi_name.clear();
    return FindVddViaDisplayConfig(gdi_name) || FindVddViaGdi(gdi_name);
}

} // namespace

VirtualDisplayCapture::VirtualDisplayCapture()
    : next_frame_id_(1) {
}

VirtualDisplayCapture::~VirtualDisplayCapture() {
    Close();
}

bool VirtualDisplayCapture::IsOpen() const {
    return duplication_.Get() != nullptr;
}

void VirtualDisplayCapture::Close() {
    staging_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    display_name_.clear();
}

bool VirtualDisplayCapture::TryOpen(std::wstring& detail) {
    detail.clear();
    if (IsOpen()) return true;

    std::wstring target_display;
    if (!FindVddDisplay(target_display)) {
        detail = L"No active VirtualDrivers monitor named 'VDD by MTT' was found.";
        return false;
    }

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        detail = L"CreateDXGIFactory1 failed: " + HResultText(hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> matched_adapter;
    ComPtr<IDXGIOutput1> matched_output;
    for (UINT adapter_index = 0; ; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        for (UINT output_index = 0; ; ++output_index) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(output_index, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) continue;

            DXGI_OUTPUT_DESC output_desc = {};
            if (FAILED(output->GetDesc(&output_desc)) ||
                _wcsicmp(output_desc.DeviceName, target_display.c_str()) != 0) {
                continue;
            }

            ComPtr<IDXGIOutput1> output1;
            hr = output.As(&output1);
            if (FAILED(hr)) {
                detail = L"The VDD output does not expose IDXGIOutput1: " + HResultText(hr);
                return false;
            }
            matched_adapter = adapter;
            matched_output = output1;
            break;
        }
        if (matched_output.Get()) break;
    }

    if (!matched_adapter.Get() || !matched_output.Get()) {
        detail = L"The active VDD monitor was not found in the DXGI output list.";
        return false;
    }

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_9_1;
    hr = D3D11CreateDevice(
        matched_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL,
        0,
        D3D11_SDK_VERSION,
        &device_,
        &feature_level,
        &context_);
    if (FAILED(hr)) {
        detail = L"D3D11CreateDevice for the VDD adapter failed: " + HResultText(hr);
        Close();
        return false;
    }

    hr = matched_output->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        detail = L"IDXGIOutput1::DuplicateOutput failed: " + HResultText(hr);
        Close();
        return false;
    }

    DXGI_OUTDUPL_DESC duplication_desc = {};
    duplication_->GetDesc(&duplication_desc);
    if (duplication_desc.ModeDesc.Width != kFrameWidth ||
        duplication_desc.ModeDesc.Height != kFrameHeight) {
        std::wostringstream stream;
        stream << L"VDD is active at "
               << duplication_desc.ModeDesc.Width << L"x"
               << duplication_desc.ModeDesc.Height
               << L", but this Host Component requires 720x720. Reinstall/configure VDD with the bundled settings.";
        detail = stream.str();
        Close();
        return false;
    }

    display_name_ = target_display;
    next_frame_id_ = 1;
    return true;
}

bool VirtualDisplayCapture::CreateStagingTexture(
    ID3D11Texture2D* source, std::wstring& detail) {
    if (!source || !device_) return false;

    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);
    if (source_desc.Width != kavio_virtual_hdmi::kFrameWidth ||
        source_desc.Height != kavio_virtual_hdmi::kFrameHeight ||
        source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        std::wostringstream stream;
        stream << L"Unexpected desktop duplication texture: "
               << source_desc.Width << L"x" << source_desc.Height
               << L" format=" << static_cast<unsigned int>(source_desc.Format);
        detail = stream.str();
        return false;
    }

    D3D11_TEXTURE2D_DESC staging_desc = source_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.SampleDesc.Quality = 0;

    HRESULT hr = device_->CreateTexture2D(&staging_desc, NULL, &staging_);
    if (FAILED(hr)) {
        detail = L"CreateTexture2D(staging) failed: " + HResultText(hr);
        return false;
    }
    return true;
}

bool VirtualDisplayCapture::WaitAndRead(
    std::vector<std::uint8_t>& output,
    std::uint64_t& frame_id,
    std::uint64_t& timestamp_ns,
    DWORD timeout_ms,
    std::wstring& detail) {
    detail.clear();
    if (!IsOpen() && !TryOpen(detail)) return false;

    DXGI_OUTDUPL_FRAME_INFO frame_info = {};
    ComPtr<IDXGIResource> desktop_resource;
    HRESULT hr = duplication_->AcquireNextFrame(
        timeout_ms, &frame_info, &desktop_resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        detail = L"AcquireNextFrame failed: " + HResultText(hr);
        Close();
        return false;
    }

    ComPtr<ID3D11Texture2D> desktop_texture;
    hr = desktop_resource.As(&desktop_texture);
    if (FAILED(hr)) {
        detail = L"Desktop frame is not an ID3D11Texture2D: " + HResultText(hr);
        duplication_->ReleaseFrame();
        Close();
        return false;
    }

    if (!staging_ && !CreateStagingTexture(desktop_texture.Get(), detail)) {
        duplication_->ReleaseFrame();
        Close();
        return false;
    }

    context_->CopyResource(staging_.Get(), desktop_texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        detail = L"Map(staging) failed: " + HResultText(hr);
        duplication_->ReleaseFrame();
        Close();
        return false;
    }

    output.resize(kFrameBytes);
    const std::uint8_t* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t row = 0; row < kFrameHeight; ++row) {
        std::memcpy(
            output.data() + static_cast<std::size_t>(row) * kFrameStride,
            source + static_cast<std::size_t>(row) * mapped.RowPitch,
            kFrameStride);
    }
    context_->Unmap(staging_.Get(), 0);

    hr = duplication_->ReleaseFrame();
    if (FAILED(hr)) {
        detail = L"ReleaseFrame failed: " + HResultText(hr);
        Close();
        return false;
    }

    frame_id = next_frame_id_++;
    timestamp_ns = NowNanoseconds();
    return true;
}
