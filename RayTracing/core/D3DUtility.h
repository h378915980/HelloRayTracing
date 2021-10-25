#pragma once

#include "stdafx.h"
#include <DirectXMath.h>

using namespace DirectX;

struct Vertex_Simple
{
    XMFLOAT3 Position;
    XMFLOAT4 Color;
};

struct Vertex_Model
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
};

struct Mesh
{
    // Give it a name so we can look it up by name.
    std::string Name;

    // System memory copies.  Use Blobs because the vertex/index format can be generic.
    // It is up to the client to cast appropriately.  
    //Microsoft::WRL::ComPtr<ID3DBlob> VertexBufferCPU = nullptr;
    //Microsoft::WRL::ComPtr<ID3DBlob> IndexBufferCPU = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferGPU = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBufferGPU = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferUploader = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBufferUploader = nullptr;

    // Data about the buffers.
    UINT VertexByteStride = 0;
    UINT VertexBufferByteSize = 0;
    UINT VertexCount = 0;
    DXGI_FORMAT IndexFormat = DXGI_FORMAT_R32_UINT;
    UINT IndexBufferByteSize = 0;
    UINT IndexCount = 0;

    D3D12_VERTEX_BUFFER_VIEW VertexBufferView()const
    {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
        vbv.StrideInBytes = VertexByteStride;
        vbv.SizeInBytes = VertexBufferByteSize;

        return vbv;
    }

    D3D12_INDEX_BUFFER_VIEW IndexBufferView()const
    {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
        ibv.Format = IndexFormat;
        ibv.SizeInBytes = IndexBufferByteSize;

        return ibv;
    }

    // We can free this memory after we finish upload to the GPU.
    void DisposeUploaders()
    {
        VertexBufferUploader = nullptr;
        IndexBufferUploader = nullptr;
    }
};

//
struct Texture
{
    std::string FileName;
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> UploadResource = nullptr;

    std::string Type;
    UINT width;
    UINT height;
    //index in Texture loader
    UINT SrvHeapIndex = 0;
};

struct Model
{
    std::string Name;
    std::string Directory;
    //mesh --- textures id :vector[0] - diffuse map, vector[1] - specular
    std::vector<std::pair<std::unique_ptr<Mesh>, std::vector<UINT>>> Meshes;
    std::vector<std::shared_ptr<Texture>> Textures;


};

struct Material
{
    std::string Name;

    // Material constant buffer data used for shading.
    DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 FresnelR0 = { 0.01f, 0.01f, 0.01f };
    float Roughness = .25f;

};




