#pragma once

#include "DXrenderer/DXhelpers.h"
#include "External/Dx12Helpers/d3dx12.h"

#include "DXrenderer/Textures/TextureManager.h"

namespace DirectxPlayground
{
class UnorderedAccessBuffer;
class UploadBuffer;

class EnvironmentMap
{
public:
    EnvironmentMap(RenderContext& ctx, const std::string& path, UINT width, UINT height);
    ~EnvironmentMap();

    void ConvertToCubemap(RenderContext& ctx);
    bool IsConvertedToCubemap() const;

private:
    struct
    {
        UINT EnvMapIndex;
        UINT CubemapIndex;
    } m_texturesIndices;

    RtvSrvUavResourceIdx m_cubemapIndex;
    RtvSrvUavResourceIdx m_envMapIndex;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_commonRootSig;
    UploadBuffer* m_indicesBuffer = nullptr;
    const std::string m_psoName = "CubemapConvertor_PBR";
    bool m_converted = false;
    UINT m_cubemapHeight = 0;
    UINT m_cubemapWidth = 0;
};

}
