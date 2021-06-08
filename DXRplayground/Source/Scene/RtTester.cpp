#include "Scene/RtTester.h"

#include <array>

#include "DXrenderer/Swapchain.h"

#include "DXrenderer/Model.h"
#include "DXrenderer/Shader.h"
#include "DXrenderer/PsoManager.h"
#include "DXrenderer/Textures/TextureManager.h"
#include "DXrenderer/Tonemapper.h"
#include "DXrenderer/LightManager.h"

#include "DXrenderer/Buffers/UploadBuffer.h"
#include "DXrenderer/Textures/EnvironmentMap.h"
#include "DXrenderer/RenderPipeline.h"

#include "External/IMGUI/imgui.h"

namespace DirectxPlayground
{

namespace
{
static constexpr UINT AccelStructSlot = 0;
static constexpr UINT DXROutputSlot = 1;
static constexpr UINT SceneCBSlot = 2;
}

using Microsoft::WRL::ComPtr;

RtTester::~RtTester()
{
    SafeDelete(m_cameraCb);
    SafeDelete(m_camera);
    SafeDelete(m_cameraController);
    SafeDelete(m_objectCb);
    SafeDelete(m_helmet);
    SafeDelete(m_tonemapper);
    SafeDelete(m_lightManager);
    SafeDelete(m_floor);
    SafeDelete(m_floorMaterialCb);
    SafeDelete(m_floorTransformCb);

    // dxr
    SafeDelete(m_tlas);
    SafeDelete(m_blas);
    SafeDelete(m_missShaderTable);
    SafeDelete(m_hitGroupShaderTable);
    SafeDelete(m_rayGenShaderTable);
    SafeDelete(m_instanceDescs);
    SafeDelete(m_scratchBuffer);
    SafeDelete(m_rtSceneDataCB);
}

void RtTester::InitResources(RenderContext& context)
{
    using Microsoft::WRL::ComPtr;

    m_camera = new Camera(1.0472f, 1.77864583f, 0.001f, 1000.0f);
    m_cameraCb = new UploadBuffer(*context.Device, sizeof(CameraShaderData), true, context.FramesCount);
    m_objectCb = new UploadBuffer(*context.Device, sizeof(XMFLOAT4X4), true, 1);

    XMFLOAT4X4 toWorld;
    XMStoreFloat4x4(&toWorld, XMMatrixTranspose(XMMatrixTranslation(0.0f, 0.0f, 3.0f)));
    m_objectCb->UploadData(0, toWorld);

    m_floorTransformCb = new UploadBuffer(*context.Device, sizeof(XMFLOAT4X4), true, context.FramesCount);
    XMStoreFloat4x4(&toWorld, XMMatrixTranspose(XMMatrixTranslation(0.0f, 0.0f, 0.0f)));
    for (UINT i = 0; i < context.FramesCount; ++i)
        m_floorTransformCb->UploadData(i, toWorld);

    m_floorMaterialCb = new UploadBuffer(*context.Device, sizeof(NonTexturedMaterial), true, context.FramesCount);
    m_floorMaterial.Albedo = { 0.8f, 0.8f, 0.8f, 1.0f };
    m_floorMaterial.AO = 1.0f;
    m_floorMaterial.Metallic = 0.0f;
    m_floorMaterial.Roughness = 1.0f;
    for (UINT i = 0; i < context.FramesCount; ++i)
        m_floorMaterialCb->UploadData(i, m_floorMaterial);

    m_cameraController = new CameraController(m_camera);
    m_lightManager = new LightManager(context);

    auto path = ASSETS_DIR + std::string("Textures//colorful_studio_4k.hdr");
    Light l = { { 300.0f, 300.0f, 300.0f, 1.0f}, { 0.0f, 5.70710678118f, 0.0f } };
    m_directionalLightInd = m_lightManager->AddLight(l);

    LoadGeometry(context);
    CreateRootSignature(context);

    m_tonemapper = new Tonemapper();
    m_tonemapper->InitResources(context, m_commonRootSig.Get());

    CreatePSOs(context);

    InitRaytracingPipeline(context);
}

void RtTester::Render(RenderContext& context)
{
    m_cameraController->Update();
    UpdateGui(context);

    UINT frameIndex = context.SwapChain->GetCurrentBackBufferIndex();

    m_cameraData.ViewProj = TransposeMatrix(m_camera->GetViewProjection());
    XMFLOAT4 camPos = m_camera->GetPosition();
    m_cameraData.Position = { camPos.x, camPos.y, camPos.z };
    m_cameraCb->UploadData(frameIndex, m_cameraData);
    m_helmet->UpdateMeshes(frameIndex);

    auto toRt = CD3DX12_RESOURCE_BARRIER::Transition(context.SwapChain->GetCurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    context.CommandList->ResourceBarrier(1, &toRt);

    D3D12_RECT scissorRect = { 0, 0, LONG(context.Width), LONG(context.Height) };
    D3D12_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(context.Width);
    viewport.Height = static_cast<float>(context.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    context.CommandList->RSSetScissorRects(1, &scissorRect);
    context.CommandList->RSSetViewports(1, &viewport);

    DepthPrepass(context);
    RaytraceShadows(context);
    RenderForwardObjects(context);

    m_tonemapper->Render(context);

    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(context.SwapChain->GetCurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    context.CommandList->ResourceBarrier(1, &toPresent);
}

void RtTester::DepthPrepass(RenderContext& context)
{
    UINT frameIndex = context.SwapChain->GetCurrentBackBufferIndex();
    auto rtCpuHandle = context.TexManager->GetRtHandle(context, m_tonemapper->GetRtIndex());

    context.CommandList->OMSetRenderTargets(0, nullptr, false, &context.SwapChain->GetDSCPUhandle());
    context.CommandList->ClearDepthStencilView(context.SwapChain->GetDSCPUhandle(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    context.CommandList->SetGraphicsRootSignature(m_commonRootSig.Get());
    context.CommandList->SetPipelineState(context.PsoManager->GetPso(m_depthPrepassPsoName));
    ID3D12DescriptorHeap* descHeap[] = { context.TexManager->GetDescriptorHeap() };
    context.CommandList->SetDescriptorHeaps(1, descHeap);
    context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(0), m_cameraCb->GetFrameDataGpuAddress(frameIndex));
    context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(1), m_objectCb->GetFrameDataGpuAddress(0));

    if (m_drawHelmet)
    {
        for (const auto mesh : m_helmet->GetMeshes())
        {
            context.CommandList->IASetVertexBuffers(0, 1, &mesh->GetVertexBufferView());
            context.CommandList->IASetIndexBuffer(&mesh->GetIndexBufferView());

            context.CommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context.CommandList->DrawIndexedInstanced(mesh->GetIndexCount(), 1, 0, 0, 0);
        }
    }

    if (m_drawFloor)
    {
        context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(1), m_floorTransformCb->GetFrameDataGpuAddress(frameIndex));
        context.CommandList->IASetVertexBuffers(0, 1, &m_floor->GetVertexBufferView());
        context.CommandList->IASetIndexBuffer(&m_floor->GetIndexBufferView());
        context.CommandList->DrawIndexedInstanced(m_floor->GetIndexCount(), 1, 0, 0, 0);
    }
}

void RtTester::RenderForwardObjects(RenderContext& context)
{
    UINT frameIndex = context.SwapChain->GetCurrentBackBufferIndex();
    auto rtCpuHandle = context.TexManager->GetRtHandle(context, m_tonemapper->GetRtIndex());

    context.CommandList->OMSetRenderTargets(1, &rtCpuHandle, false, &context.SwapChain->GetDSCPUhandle());
    context.CommandList->ClearRenderTargetView(rtCpuHandle, m_tonemapper->GetClearColor(), 0, nullptr);

    context.CommandList->SetGraphicsRootSignature(m_commonRootSig.Get());
    context.CommandList->SetPipelineState(context.PsoManager->GetPso(m_psoName));
    ID3D12DescriptorHeap* descHeap[] = { context.TexManager->GetDescriptorHeap() };
    context.CommandList->SetDescriptorHeaps(1, descHeap);
    context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(0), m_cameraCb->GetFrameDataGpuAddress(frameIndex));
    context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(1), m_objectCb->GetFrameDataGpuAddress(0));
    context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(3), m_lightManager->GetLightsBufferGpuAddress(frameIndex));
    context.CommandList->SetGraphicsRootDescriptorTable(TextureTableIndex, context.TexManager->GetDescriptorHeap()->GetGPUDescriptorHandleForHeapStart());

    if (m_drawHelmet)
    {
        for (const auto mesh : m_helmet->GetMeshes())
        {
            context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(2), mesh->GetMaterialBufferGpuAddress(frameIndex));

            context.CommandList->IASetVertexBuffers(0, 1, &mesh->GetVertexBufferView());
            context.CommandList->IASetIndexBuffer(&mesh->GetIndexBufferView());

            context.CommandList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context.CommandList->DrawIndexedInstanced(mesh->GetIndexCount(), 1, 0, 0, 0);
        }
    }

    if (m_drawFloor)
    {
        context.CommandList->SetPipelineState(context.PsoManager->GetPso(m_floorPsoName));
        context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(1), m_floorTransformCb->GetFrameDataGpuAddress(frameIndex));
        context.CommandList->SetGraphicsRootConstantBufferView(GetCBRootParamIndex(2), m_floorMaterialCb->GetFrameDataGpuAddress(frameIndex));
        context.CommandList->IASetVertexBuffers(0, 1, &m_floor->GetVertexBufferView());
        context.CommandList->IASetIndexBuffer(&m_floor->GetIndexBufferView());
        context.CommandList->DrawIndexedInstanced(m_floor->GetIndexCount(), 1, 0, 0, 0);
    }
}

void RtTester::LoadGeometry(RenderContext& context)
{
    auto path = ASSETS_DIR + std::string("Models//FlightHelmet//glTF//FlightHelmet.gltf");
    m_helmet = new Model(context, path);

    std::vector<Vertex> verts;
    verts.resize(4);
    verts[0].Pos = { -100.0f, 0.0f, 100.0f };
    verts[1].Pos = { 100.0f, 0.0f, 100.0f };
    verts[2].Pos = { 100.0f, 0.0f, -100.0f };
    verts[3].Pos = { -100.0f, 0.0f, -100.0f };
    verts[0].Norm = { 0.0f, 1.0f, 0.0f };
    verts[1].Norm = { 0.0f, 1.0f, 0.0f };
    verts[2].Norm = { 0.0f, 1.0f, 0.0f };
    verts[3].Norm = { 0.0f, 1.0f, 0.0f };
    std::vector<UINT> ind = { 0, 1, 2, 0, 2, 3 };
    m_floor = new Model(context, verts, ind);
}

void RtTester::CreateRootSignature(RenderContext& context)
{
    CreateCommonRootSignature(context.Device, IID_PPV_ARGS(&m_commonRootSig));
    NAME_D3D12_OBJECT(m_commonRootSig);
}

void RtTester::CreatePSOs(RenderContext& context)
{
    auto& inputLayout = GetInputLayoutUV_N_T();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = GetDefaultOpaquePsoDescriptor(m_commonRootSig.Get(), 0);
    desc.InputLayout = { inputLayout.data(), static_cast<UINT>(inputLayout.size()) };

    desc.DSVFormat = context.SwapChain->GetDepthStencilFormat();
    auto shaderPath = ASSETS_DIR_W + std::wstring(L"Shaders//DepthPrepass.hlsl");
    context.PsoManager->CreatePso(context, m_depthPrepassPsoName, shaderPath, desc);

    desc.NumRenderTargets = 1;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.RTVFormats[0] = m_tonemapper->GetHDRTargetFormat();

    shaderPath = ASSETS_DIR_W + std::wstring(L"Shaders//PbrNonInstanced.hlsl");
    context.PsoManager->CreatePso(context, m_psoName, shaderPath, desc);

    shaderPath = ASSETS_DIR_W + std::wstring(L"Shaders//PbrNonTexturedNonInstanced.hlsl");
    context.PsoManager->CreatePso(context, m_floorPsoName, shaderPath, desc);

}

void RtTester::UpdateGui(RenderContext& context)
{
    Light& dirLight = m_lightManager->GetLightRef(m_directionalLightInd);
    ImGui::Begin("SceneControls");
    ImGui::Text("Lights");
    ImGui::InputFloat4("Color", reinterpret_cast<float*>(&dirLight.Color));
    ImGui::InputFloat3("Direction", reinterpret_cast<float*>(&dirLight.Direction));
    ImGui::Text("");
    ImGui::Text("Rasterizer");
    ImGui::Checkbox("Use Rasterizer", &m_useRasterizer);
    ImGui::Checkbox("Draw Helmet", &m_drawHelmet);
    ImGui::Checkbox("Draw Floor", &m_drawFloor);
    ImGui::End();

    m_lightManager->UpdateLights(context.SwapChain->GetCurrentBackBufferIndex());
}

void RtTester::InitRaytracingPipeline(RenderContext& context)
{
    m_rtSceneDataCB = new UploadBuffer(*context.Device, sizeof(RtCb), true, context.FramesCount);

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    resDesc.Width = context.Width;
    resDesc.Height = context.Height;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    resDesc.DepthOrArraySize = 1;
    resDesc.SampleDesc.Count = 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    context.TexManager->CreateDxrOutput(context, resDesc);

    CreateRtRootSigs(context);
    BuildAccelerationStructures(context);
    CreateRtPSO(context);
    BuildShaderTables(context);
}

void RtTester::CreateRtRootSigs(RenderContext& context)
{
    std::vector<CD3DX12_ROOT_PARAMETER> params{};
    params.push_back({});
    params.back().InitAsShaderResourceView(0); // Acceleration structure

    CD3DX12_DESCRIPTOR_RANGE uavDesc;
    uavDesc.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);

    params.push_back({});
    params.back().InitAsDescriptorTable(1, &uavDesc); // Output rt

    params.push_back({});
    params.back().InitAsConstantBufferView(0); // Scene cb (camera etc)

    CD3DX12_ROOT_SIGNATURE_DESC globalRootSigDesc{ UINT(params.size()), params.data() };
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;

    ThrowIfFailed(D3D12SerializeRootSignature(&globalRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
    ThrowIfFailed(context.Device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rtGlobalRootSig)));

    // think about local root sig
}

// Pretty-print a state object tree.
inline void PrintStateObjectDesc(const D3D12_STATE_OBJECT_DESC* desc)
{
    std::wstringstream wstr;
    wstr << L"\n";
    wstr << L"--------------------------------------------------------------------\n";
    wstr << L"| D3D12 State Object 0x" << static_cast<const void*>(desc) << L": ";
    if (desc->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION) wstr << L"Collection\n";
    if (desc->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) wstr << L"Raytracing Pipeline\n";

    auto ExportTree = [](UINT depth, UINT numExports, const D3D12_EXPORT_DESC* exports)
    {
        std::wostringstream woss;
        for (UINT i = 0; i < numExports; i++)
        {
            woss << L"|";
            if (depth > 0)
            {
                for (UINT j = 0; j < 2 * depth - 1; j++) woss << L" ";
            }
            woss << L" [" << i << L"]: ";
            if (exports[i].ExportToRename) woss << exports[i].ExportToRename << L" --> ";
            woss << exports[i].Name << L"\n";
        }
        return woss.str();
    };

    for (UINT i = 0; i < desc->NumSubobjects; i++)
    {
        wstr << L"| [" << i << L"]: ";
        switch (desc->pSubobjects[i].Type)
        {
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            wstr << L"Global Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
            wstr << L"Local Root Signature 0x" << desc->pSubobjects[i].pDesc << L"\n";
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
            wstr << L"Node Mask: 0x" << std::hex << std::setfill(L'0') << std::setw(8) << *static_cast<const UINT*>(desc->pSubobjects[i].pDesc) << std::setw(0) << std::dec << L"\n";
            break;
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
        {
            wstr << L"DXIL Library 0x";
            auto lib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(desc->pSubobjects[i].pDesc);
            wstr << lib->DXILLibrary.pShaderBytecode << L", " << lib->DXILLibrary.BytecodeLength << L" bytes\n";
            wstr << ExportTree(1, lib->NumExports, lib->pExports);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
        {
            wstr << L"Existing Library 0x";
            auto collection = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(desc->pSubobjects[i].pDesc);
            wstr << collection->pExistingCollection << L"\n";
            wstr << ExportTree(1, collection->NumExports, collection->pExports);
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            wstr << L"Subobject to Exports Association (Subobject [";
            auto association = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
            UINT index = static_cast<UINT>(association->pSubobjectToAssociate - desc->pSubobjects);
            wstr << index << L"])\n";
            for (UINT j = 0; j < association->NumExports; j++)
            {
                wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
            }
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
        {
            wstr << L"DXIL Subobjects to Exports Association (";
            auto association = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(desc->pSubobjects[i].pDesc);
            wstr << association->SubobjectToAssociate << L")\n";
            for (UINT j = 0; j < association->NumExports; j++)
            {
                wstr << L"|  [" << j << L"]: " << association->pExports[j] << L"\n";
            }
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
        {
            wstr << L"Raytracing Shader Config\n";
            auto config = static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(desc->pSubobjects[i].pDesc);
            wstr << L"|  [0]: Max Payload Size: " << config->MaxPayloadSizeInBytes << L" bytes\n";
            wstr << L"|  [1]: Max Attribute Size: " << config->MaxAttributeSizeInBytes << L" bytes\n";
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
        {
            wstr << L"Raytracing Pipeline Config\n";
            auto config = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(desc->pSubobjects[i].pDesc);
            wstr << L"|  [0]: Max Recursion Depth: " << config->MaxTraceRecursionDepth << L"\n";
            break;
        }
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
        {
            wstr << L"Hit Group (";
            auto hitGroup = static_cast<const D3D12_HIT_GROUP_DESC*>(desc->pSubobjects[i].pDesc);
            wstr << (hitGroup->HitGroupExport ? hitGroup->HitGroupExport : L"[none]") << L")\n";
            wstr << L"|  [0]: Any Hit Import: " << (hitGroup->AnyHitShaderImport ? hitGroup->AnyHitShaderImport : L"[none]") << L"\n";
            wstr << L"|  [1]: Closest Hit Import: " << (hitGroup->ClosestHitShaderImport ? hitGroup->ClosestHitShaderImport : L"[none]") << L"\n";
            wstr << L"|  [2]: Intersection Import: " << (hitGroup->IntersectionShaderImport ? hitGroup->IntersectionShaderImport : L"[none]") << L"\n";
            break;
        }
        }
        wstr << L"|--------------------------------------------------------------------\n";
    }
    wstr << L"\n";
    OutputDebugStringW(wstr.str().c_str());
}

void RtTester::CreateRtPSO(RenderContext& context)
{
    CD3DX12_STATE_OBJECT_DESC rtPipeline{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

    CD3DX12_DXIL_LIBRARY_SUBOBJECT* lib = rtPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();

    auto shaderPath = ASSETS_DIR_W + std::wstring(L"Shaders//Raytracing.hlsl");

    bool success = Shader::CompileFromFile(shaderPath, L"", L"lib_6_3", m_rayGenShader);
    assert(success);

    lib->SetDXILLibrary(&m_rayGenShader.GetBytecode());

    lib->DefineExport(L"Raygen");
    lib->DefineExport(L"ClosestHit");
    lib->DefineExport(L"Miss");

    CD3DX12_HIT_GROUP_SUBOBJECT* hitGroup = rtPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetClosestHitShaderImport(L"ClosestHit");
    hitGroup->SetHitGroupExport(L"TriangleHitGroup");
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);

    CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT* shaderConfig = rtPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    UINT payloadSize = sizeof(XMFLOAT4); // float4 color
    UINT attribSize = sizeof(XMFLOAT2); //float2 barycentrics. default for trigs
    shaderConfig->Config(payloadSize, attribSize);

    CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* globalRootSig = rtPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSig->SetRootSignature(m_rtGlobalRootSig.Get());

    CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT* pipelineConfig = rtPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    UINT maxRecursionDepth = 1;
    pipelineConfig->Config(maxRecursionDepth);

    PrintStateObjectDesc(rtPipeline);

    ThrowIfFailed(context.Device->CreateStateObject(rtPipeline, IID_PPV_ARGS(&m_dxrStateObject)), L"Couldn't create raytracing state object.\n");
}

void RtTester::BuildAccelerationStructures(RenderContext& context)
{
    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc{};
    geomDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geomDesc.Triangles.Transform3x4 = 0;
    geomDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs;

    std::vector<CD3DX12_RESOURCE_BARRIER> toNonPixelTranitions;
    std::vector<CD3DX12_RESOURCE_BARRIER> toIndexVertexTransitions;

    for (const auto mesh : m_helmet->GetMeshes())
    {
        toNonPixelTranitions.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mesh->GetIndexBufferResource(), D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        toNonPixelTranitions.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mesh->GetVertexBufferResource(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));

        toIndexVertexTransitions.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mesh->GetIndexBufferResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_INDEX_BUFFER));
        toIndexVertexTransitions.push_back(CD3DX12_RESOURCE_BARRIER::Transition(mesh->GetVertexBufferResource(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

        geomDesc.Triangles.IndexBuffer = mesh->GetIndexBufferGpuAddress();
        geomDesc.Triangles.IndexCount = mesh->GetIndexCount();
        geomDesc.Triangles.VertexCount = mesh->GetVertexCount();
        geomDesc.Triangles.VertexBuffer.StartAddress = mesh->GetVertexBufferGpuAddress();
        geomDescs.push_back(geomDesc);
    }
    context.CommandList->ResourceBarrier(UINT(toNonPixelTranitions.size()), toNonPixelTranitions.data());

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomLevelBuildDesc = {};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& bottomLevelInputs = bottomLevelBuildDesc.Inputs;
    bottomLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bottomLevelInputs.Flags = buildFlags;
    bottomLevelInputs.NumDescs = UINT(geomDescs.size());
    bottomLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottomLevelInputs.pGeometryDescs = geomDescs.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomLevelPrebuildInfo = {};
    context.Device->GetRaytracingAccelerationStructurePrebuildInfo(&bottomLevelInputs, &bottomLevelPrebuildInfo);
    assert(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = {};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& topLevelInputs = topLevelBuildDesc.Inputs;
    topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelInputs.Flags = buildFlags;
    topLevelInputs.NumDescs = 1; // How many blas?
    topLevelInputs.pGeometryDescs = nullptr;
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
    context.Device->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);
    assert(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);

    // during build
    m_scratchBuffer = new UnorderedAccessBuffer(context.CommandList, *context.Device, UINT(std::max(topLevelPrebuildInfo.ScratchDataSizeInBytes, bottomLevelPrebuildInfo.ScratchDataSizeInBytes)));

    D3D12_RESOURCE_STATES initialResourceState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

    m_blas = new UnorderedAccessBuffer(context.CommandList, *context.Device, UINT(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes), nullptr, false, true);
    m_blas->SetName(L"BottomLevelAccelerationStructure");

    m_tlas = new UnorderedAccessBuffer(context.CommandList, *context.Device, UINT(topLevelPrebuildInfo.ResultDataMaxSizeInBytes), nullptr, false, true);
    m_tlas->SetName(L"TopLevelAccelerationStructure");

    D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
    instanceDesc.Transform[0][0] = instanceDesc.Transform[1][1] = instanceDesc.Transform[2][2] = 1; // todo check here for instancing
    instanceDesc.Transform[2][3] = 3;
    instanceDesc.InstanceMask = 1;
    instanceDesc.AccelerationStructure = m_blas->GetGpuAddress();
    //MsHelper::AllocateUploadBuffer(context.Device, &instanceDesc, sizeof(instanceDesc), &instanceDescs, L"InstanceDescs");

    m_instanceDescs = new UploadBuffer{ *context.Device, sizeof(instanceDesc), false, 1 };
    m_instanceDescs->SetName(L"InstanceDescs");
    m_instanceDescs->UploadData(0, instanceDesc);

    bottomLevelBuildDesc.Inputs = bottomLevelInputs; // useless
    bottomLevelBuildDesc.ScratchAccelerationStructureData = m_scratchBuffer->GetGpuAddress();
    bottomLevelBuildDesc.DestAccelerationStructureData = m_blas->GetGpuAddress();

    topLevelBuildDesc.Inputs = topLevelInputs; // useless
    topLevelBuildDesc.ScratchAccelerationStructureData = m_scratchBuffer->GetGpuAddress();
    topLevelBuildDesc.DestAccelerationStructureData = m_tlas->GetGpuAddress();
    topLevelBuildDesc.Inputs.InstanceDescs = m_instanceDescs->GetFrameDataGpuAddress(0);

    context.CommandList->BuildRaytracingAccelerationStructure(&bottomLevelBuildDesc, 0, nullptr);
    context.CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_blas->GetResource()));
    context.CommandList->BuildRaytracingAccelerationStructure(&topLevelBuildDesc, 0, nullptr);

    //context.Pipeline->ExecuteCommandList(context.CommandList);
    //context.Pipeline->Flush();
    //context.Pipeline->ResetCommandList(context.CommandList);

    context.CommandList->ResourceBarrier(UINT(toIndexVertexTransitions.size()), toIndexVertexTransitions.data());
}

void RtTester::BuildShaderTables(RenderContext& context)
{
    ComPtr<ID3D12StateObjectProperties> stateObjectProps;
    ThrowIfFailed(m_dxrStateObject.As(&stateObjectProps));
    void* rayGenShaderIdentifier = stateObjectProps->GetShaderIdentifier(L"Raygen");
    void* missShaderIdentifier = stateObjectProps->GetShaderIdentifier(L"Miss");
    void* hitGroupShaderIdentifier = stateObjectProps->GetShaderIdentifier(L"TriangleHitGroup");

    UINT shaderIdentifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // + localRootSigSize

    UINT numShaderRecords = 1;

    // Local root sig for each table + copy data there. should be alighed by max root sig + BYTE_ALIGNMENT
    UINT shaderRecordSize = Align(shaderIdentifierSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

    m_rayGenShaderTable = new UploadBuffer(*context.Device, shaderRecordSize, false, 1, true);
    m_rayGenShaderTable->UploadData(0, reinterpret_cast<byte*>(rayGenShaderIdentifier));
    m_rayGenShaderTable->SetName(L"RayGenShaderTable");
    
    m_missShaderTable = new UploadBuffer(*context.Device, shaderRecordSize, false, 1, true);
    m_missShaderTable->UploadData(0, reinterpret_cast<byte*>(missShaderIdentifier));
    m_missShaderTable->SetName(L"MissShaderTable");

    m_hitGroupShaderTable = new UploadBuffer(*context.Device, shaderRecordSize, false, 1, true);
    m_hitGroupShaderTable->UploadData(0, reinterpret_cast<byte*>(hitGroupShaderIdentifier));
    m_hitGroupShaderTable->SetName(L"HitGroupShaderTable");
}

void RtTester::RaytraceShadows(RenderContext& context)
{
    XMMATRIX viewProj = XMLoadFloat4x4(&m_camera->GetViewProjection());
    XMVECTOR det = XMMatrixDeterminant(viewProj);
    XMMATRIX invViewProjSimd = XMMatrixInverse(&det, viewProj);
    XMFLOAT4X4 invViewProj;
    XMStoreFloat4x4(&invViewProj, XMMatrixTranspose(invViewProjSimd));
    RtCb rtSceneData{};
    rtSceneData.InvViewProj = invViewProj;
    rtSceneData.CamPosition = m_camera->GetPosition();
    m_rtSceneDataCB->UploadData(context.SwapChain->GetCurrentBackBufferIndex(), rtSceneData);

    auto cmdList = context.CommandList;

    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(context.TexManager->GetDXRResource(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    context.CommandList->ResourceBarrier(1, &transition);

    ID3D12DescriptorHeap* descHeaps[] = { context.TexManager->GetDXRUavHeap() };
    cmdList->SetComputeRootSignature(m_rtGlobalRootSig.Get());
    cmdList->SetDescriptorHeaps(1, descHeaps);
    cmdList->SetComputeRootConstantBufferView(SceneCBSlot, m_rtSceneDataCB->GetFrameDataGpuAddress(context.SwapChain->GetCurrentBackBufferIndex()));
    cmdList->SetComputeRootShaderResourceView(AccelStructSlot, m_tlas->GetGpuAddress());
    cmdList->SetComputeRootDescriptorTable(DXROutputSlot, context.TexManager->GetDXRUavHeap()->GetGPUDescriptorHandleForHeapStart());

    D3D12_DISPATCH_RAYS_DESC desc = {};
    desc.Width = context.Width;
    desc.Height = context.Height;
    desc.Depth = 1;

    desc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetFrameDataGpuAddress(0);
    desc.HitGroupTable.SizeInBytes = m_hitGroupShaderTable->GetBufferSize();
    desc.HitGroupTable.StrideInBytes = desc.HitGroupTable.SizeInBytes;

    desc.MissShaderTable.StartAddress = m_missShaderTable->GetFrameDataGpuAddress(0);
    desc.MissShaderTable.SizeInBytes = m_missShaderTable->GetBufferSize();
    desc.MissShaderTable.StrideInBytes = desc.MissShaderTable.SizeInBytes;

    desc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetFrameDataGpuAddress(0);
    desc.RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderTable->GetBufferSize();

    cmdList->SetPipelineState1(m_dxrStateObject.Get());
    cmdList->DispatchRays(&desc);

    transition = CD3DX12_RESOURCE_BARRIER::Transition(context.TexManager->GetDXRResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    context.CommandList->ResourceBarrier(1, &transition);
}

}