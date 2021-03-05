#include "TextureManager.h"

#include <vector>
#include <sstream>

#include "External/Dx12Helpers/d3dx12.h"
#include "External/lodepng/lodepng.h"
#include "DXrenderer/RenderContext.h"

#include "DXhelpers.h"

namespace DirectxPlayground
{

TextureManager::TextureManager(RenderContext& ctx)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = RenderContext::MaxTextures;
    ThrowIfFailed(ctx.Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)));
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    viewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    viewDesc.Texture2D.MostDetailedMip = 0;
    viewDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    for (UINT i = 0; i < RenderContext::MaxTextures; ++i)
    {
        ctx.Device->CreateShaderResourceView(nullptr, &viewDesc, handle);
        handle.Offset(ctx.CbvSrvUavDescriptorSize);
    }
}

UINT TextureManager::CreateTexture(RenderContext& ctx, const std::string& filename)
{
    std::vector<unsigned char> bufferInMemory;
    std::vector<unsigned char> buffer;

    UINT w = 0, h = 0;
    lodepng::load_file(bufferInMemory, filename);
    UINT error = lodepng::decode(buffer, w, h, bufferInMemory);
    if (error)
    {
        std::stringstream ss;
        ss << "PNG decoding error " << error << " : " << lodepng_error_text(error) << std::endl;
        LOG(ss.str().c_str());
    }

    m_resources.emplace_back();
    auto resource = m_resources.back().Get();
    m_uploadResources.emplace_back();
    auto uploadResource = m_uploadResources.back().Get();

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    texDesc.DepthOrArraySize = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    ThrowIfFailed(ctx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&resource)));

#if defined(_DEBUG)
    std::wstring s{ filename.begin(), filename.end() };
    SetDXobjectName(resource, s.c_str());
#endif

    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(resource, 0, 1);

    size_t texSize = buffer.size() * sizeof(unsigned char);
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    ThrowIfFailed(ctx.Device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadResource)));

    D3D12_SUBRESOURCE_DATA texData = {};
    texData.pData = buffer.data();
    texData.RowPitch = w * 4;
    texData.SlicePitch = texData.RowPitch * h;

    UpdateSubresources(ctx.CommandList, resource, uploadResource, 0, 0, 1, &texData);

    CD3DX12_RESOURCE_BARRIER toDest = CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.CommandList->ResourceBarrier(1, &toDest);

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    viewDesc.Format = resource->GetDesc().Format;
    viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = resource->GetDesc().MipLevels;
    viewDesc.Texture2D.MostDetailedMip = 0;
    viewDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    handle.Offset(m_currentTexCount * ctx.CbvSrvUavDescriptorSize);
    ctx.Device->CreateShaderResourceView(resource, &viewDesc, handle);

    return m_currentTexCount++;
}

}