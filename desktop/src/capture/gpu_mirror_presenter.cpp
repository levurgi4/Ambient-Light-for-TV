#include "gpu_mirror_presenter.hpp"
#include "mirror_geometry.hpp"
#include <d3dcompiler.h>
#include <cstring>

namespace ambilight::capture {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char shader[] = R"(
Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);
cbuffer Parameters : register(b0) { uint Rotation; uint3 Padding; };
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs(uint id : SV_VertexID) {
    Vertex o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(Vertex i) : SV_Target {
    float2 uv = i.uv;
    if (Rotation == 1) uv = float2(uv.y, 1 - uv.x);
    else if (Rotation == 2) uv = 1 - uv;
    else if (Rotation == 3) uv = float2(1 - uv.y, uv.x);
    return float4(Source.Sample(LinearClamp, uv).rgb, 1);
}
)";
constexpr UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
}

HRESULT GpuMirrorPresenter::initialize(ID3D11Device* device, HWND window)
{
    reset();
    device_ = device;
    window_ = window;
    device_->GetImmediateContext(context_.GetAddressOf());
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    HRESULT hr = device_.As(&dxgi);
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(adapter.GetAddressOf());
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = flags;
    ComPtr<IDXGISwapChain1> chain;
    hr = factory->CreateSwapChainForHwnd(device, window, &description,
        nullptr, nullptr, chain.GetAddressOf());
    if (SUCCEEDED(hr)) hr = chain.As(&chain_);
    if (SUCCEEDED(hr)) hr = factory->MakeWindowAssociation(window,
        DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    if (SUCCEEDED(hr)) hr = chain_->SetMaximumFrameLatency(1);
    if (FAILED(hr)) return hr;
    available_ = chain_->GetFrameLatencyWaitableObject();
    if (!available_) return E_FAIL;
    ComPtr<ID3DBlob> code;
    hr = D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr,
        "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), nullptr);
    if (SUCCEEDED(hr)) hr = device_->CreateVertexShader(code->GetBufferPointer(),
        code->GetBufferSize(), nullptr, vertex_.GetAddressOf());
    if (FAILED(hr)) return hr;
    code.Reset();
    hr = D3DCompile(shader, std::strlen(shader), nullptr, nullptr, nullptr,
        "ps", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), nullptr);
    if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(code->GetBufferPointer(),
        code->GetBufferSize(), nullptr, pixel_.GetAddressOf());
    if (FAILED(hr)) return hr;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler, sampler_.GetAddressOf());
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&buffer, nullptr, parameters_.GetAddressOf());
    return hr;
}

void GpuMirrorPresenter::reset() noexcept
{
    if (context_) context_->OMSetRenderTargets(0, nullptr, nullptr);
    target_.Reset();
    vertex_.Reset();
    pixel_.Reset();
    sampler_.Reset();
    parameters_.Reset();
    if (available_) CloseHandle(available_);
    available_ = nullptr;
    chain_.Reset();
    // A flip swap chain must actually be destroyed before another one can be
    // created for the same HWND after a backend switch.
    if (context_) { context_->ClearState(); context_->Flush(); }
    context_.Reset();
    device_.Reset();
    window_ = nullptr;
    size_ = {};
    slotReady_ = false;
}

HRESULT GpuMirrorPresenter::prepare(bool* ready)
{
    *ready = false;
    if (!chain_ || !IsWindow(window_)) return E_HANDLE;
    RECT bounds{};
    if (!GetClientRect(window_, &bounds)) return HRESULT_FROM_WIN32(GetLastError());
    const QSize size(bounds.right, bounds.bottom);
    if (size.isEmpty() || !IsWindowVisible(window_) || IsIconic(window_)) return S_OK;
    if (size != size_) {
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        target_.Reset();
        HRESULT hr = chain_->ResizeBuffers(0, size.width(), size.height(),
            DXGI_FORMAT_UNKNOWN, flags);
        ComPtr<ID3D11Texture2D> backBuffer;
        if (SUCCEEDED(hr)) hr = chain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(backBuffer.Get(),
            nullptr, target_.GetAddressOf());
        if (FAILED(hr)) return hr;
        size_ = size;
    }
    const DWORD result = slotReady_ ? WAIT_OBJECT_0 : WaitForSingleObject(available_, 0);
    if (result == WAIT_FAILED) return HRESULT_FROM_WIN32(GetLastError());
    slotReady_ = *ready = result == WAIT_OBJECT_0;
    return S_OK;
}

HRESULT GpuMirrorPresenter::present(ID3D11ShaderResourceView* source,
    const QSize logicalSize, const UINT rotation, bool* presented)
{
    *presented = false;
    const QRectF rectangle = mirrorContentRect(logicalSize, size_);
    if (rectangle.isEmpty() || !target_) return E_INVALIDARG;
    constexpr float black[4] = {0, 0, 0, 1};
    context_->ClearRenderTargetView(target_.Get(), black);
    ID3D11RenderTargetView* target = target_.Get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    const D3D11_VIEWPORT viewport{static_cast<float>(rectangle.x()),
        static_cast<float>(rectangle.y()), static_cast<float>(rectangle.width()),
        static_cast<float>(rectangle.height()), 0, 1};
    context_->RSSetViewports(1, &viewport);
    const UINT parameters[4] = {rotation, 0, 0, 0};
    context_->UpdateSubresource(parameters_.Get(), 0, nullptr, parameters, 0, 0);
    ID3D11Buffer* buffer = parameters_.Get();
    ID3D11SamplerState* sampler = sampler_.Get();
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, &source);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->PSSetConstantBuffers(0, 1, &buffer);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    const HRESULT hr = chain_->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING || hr == DXGI_STATUS_OCCLUDED) return S_OK;
    *presented = SUCCEEDED(hr);
    if (*presented) slotReady_ = false;
    return hr;
}
}
