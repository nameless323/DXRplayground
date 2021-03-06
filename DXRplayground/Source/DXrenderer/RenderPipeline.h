#pragma once

#include <array>
#include <cassert>
#include <exception>
#include <map>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include <wrl.h>

#include "DXrenderer/RenderContext.h"

#include "DXrenderer/Swapchain.h"

namespace DirectxPlayground
{
class Scene;

class ImguiTextureManager;

class IRenderPipeline
{
public:
    virtual ~IRenderPipeline() = default;

    virtual void Flush() = 0;
    virtual void ExecuteCommandList(ID3D12GraphicsCommandList* commandList) = 0;
    virtual void ResetCommandList(ID3D12GraphicsCommandList* commandList) = 0;
};

class RenderPipeline : public IRenderPipeline
{
public:
    ~RenderPipeline();

    void Init(HWND hwnd, int width, int height, Scene* scene);
    void Flush() override;
    void ExecuteCommandList(ID3D12GraphicsCommandList* commandList) override;
    void ResetCommandList(ID3D12GraphicsCommandList* commandList) override; // TODO: Nice way to mess things up, rethink
    void Resize(int width, int height);

    void Render(Scene* scene);

    void Shutdown();

private:
    void GetHardwareAdapter(IDXGIFactory7* factory, IDXGIAdapter1** adapter);

    void InitImGui();
    void RenderImGui();
    void ShutdownImGui();
    void IncrementAllocatorIndex();

    bool m_isTearingSupported = false;

    RenderContext m_context;

    Swapchain m_swapChain;
    TextureManager* m_textureManager = nullptr;
    PsoManager* m_psoManager = nullptr;

    ImguiTextureManager* m_imguiTextureManager = nullptr;

    Microsoft::WRL::ComPtr<IDXGIFactory7> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, RenderContext::AllocatorsCount> m_commandAllocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList5> m_commandList;
    UINT64 m_fenceValues[RenderContext::FramesCount]{};
    UINT64 m_currentFence = 0;
    UINT64 m_currentAllocatorIdx = 0; // TODO: recheck allocators
};

inline void RenderPipeline::IncrementAllocatorIndex()
{
    m_currentAllocatorIdx = (m_currentAllocatorIdx + 1) % RenderContext::AllocatorsCount;
}
}