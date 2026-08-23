#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class VirtualDisplayCapture {
public:
    VirtualDisplayCapture();
    ~VirtualDisplayCapture();

    VirtualDisplayCapture(const VirtualDisplayCapture&) = delete;
    VirtualDisplayCapture& operator=(const VirtualDisplayCapture&) = delete;

    bool IsOpen() const;
    void Close();

    // Opens the active VirtualDrivers VDD monitor (friendly name "VDD by MTT")
    // and creates a DXGI Desktop Duplication session for that output.
    bool TryOpen(std::wstring& detail);

    // Returns true only when a new 720x720 BGRA frame was captured.
    // Timeout is a normal no-frame condition. Fatal duplication errors close the
    // session so the caller can retry TryOpen().
    bool WaitAndRead(std::vector<std::uint8_t>& output,
                     std::uint64_t& frame_id,
                     std::uint64_t& timestamp_ns,
                     DWORD timeout_ms,
                     std::wstring& detail);

    const std::wstring& DisplayName() const { return display_name_; }

private:
    bool CreateStagingTexture(ID3D11Texture2D* source, std::wstring& detail);

    std::wstring display_name_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    std::uint64_t next_frame_id_;
};
