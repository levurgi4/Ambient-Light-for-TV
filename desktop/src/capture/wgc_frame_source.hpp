#pragma once

#include <memory>
#include <QString>
#include <d3d11.h>

namespace ambilight::capture {

// All methods, including destruction, run on the capture worker thread.
// The acquired frame stays leased until releaseFrame(), after GPU readback.
class WgcFrameSource final {
public:
    WgcFrameSource();
    ~WgcFrameSource();
    HRESULT initialize(ID3D11Device* device, HMONITOR monitor, int targetFps,
        QString* error, HWND sourceWindow = nullptr, DWORD sourceProcessId = 0);
    HRESULT acquire(ID3D11Texture2D** texture, int targetFps, QString* error);
    void releaseFrame() noexcept;
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ambilight::capture
