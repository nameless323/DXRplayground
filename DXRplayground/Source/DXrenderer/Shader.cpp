#include "Shader.h"

#include "Utils/Logger.h"

namespace DirectxPlayground
{

bool Shader::CompileFromFile(const std::wstring& path, const std::string& entry, const std::string& shaderModel, Shader& outShader)
{
#ifdef _DEBUG
    static constexpr UINT shaderFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    static constexpr UINT shaderFlags = 0;
#endif
    HRESULT hr = CompileFromFile(outShader, path.c_str(), entry.c_str(), shaderModel.c_str(), shaderFlags);
    return SUCCEEDED(hr);
}

HRESULT Shader::CompileFromFile(Shader& shader, LPCWSTR fileName, const D3D_SHADER_MACRO* defines, ID3DInclude* includes, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2)
{
    shader.m_compiled = false;
    HRESULT hr = D3DCompileFromFile(fileName, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags1, flags2, &shader.m_shaderBlob, &shader.m_error);
    if (SUCCEEDED(hr))
    {
        shader.m_bytecode = CD3DX12_SHADER_BYTECODE(shader.m_shaderBlob.Get());
        shader.m_compiled = true;
    }
    else if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
    {
        LOG_W("ERROR_SHARING_VIOLATION occurred while compiling ", fileName, " please retry \n");
    }
    else
    {
        LOG("Shader compilation error ", reinterpret_cast<char*>(shader.m_error->GetBufferPointer()));
    }
    return hr;
}

HRESULT Shader::CompileFromFile(Shader& shader, LPCWSTR fileName, LPCSTR entry, LPCSTR target, UINT flags)
{
    return CompileFromFile(shader, fileName, nullptr, nullptr, entry, target, flags, 0);
}

}