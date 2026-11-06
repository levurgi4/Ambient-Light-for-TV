#pragma once

#include <QSize>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

namespace ambilight::capture {

// Capture-thread only. The UI owns the HWND and must stop capture before
// destroying/repositioning it. No CPU image readback, encoding or extra device.
class GpuMirrorPresenter final {
public:
    ~GpuMirrorPresenter() { reset(); }
    HRESULT initialize(ID3D11Device* device, HWND window);
    void reset() noexcept;
    HRESULT prepare(bool* ready);
    HRESULT present(ID3D11ShaderResourceView* source, QSize logicalSize,
        UINT rotation, bool* presented);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> parameters_;
    HWND window_ = nullptr;
    HANDLE available_ = nullptr;
    QSize size_;
    bool slotReady_ = false;
};
}
