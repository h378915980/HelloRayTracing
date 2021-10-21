#include "stdafx.h"
#include "TextureLoader.h"
#include "DXSampleHelper.h"
#include "WICTextureLoader12.h"

void TextureLoader::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
{
	m_device = device;
	m_cmdList = cmdList;
	ThrowIfFailed(CoInitializeEx(nullptr, COINITBASE_MULTITHREADED));
}

bool TextureLoader::Load(std::string filename,std::shared_ptr<Texture> texture)
{
	ComPtr<ID3D12Resource>		loadedTexture;
	std::unique_ptr<uint8_t[]>	decodedData;
	D3D12_SUBRESOURCE_DATA		subresource;

	//std::shared_ptr<Texture> texture = std::make_shared<Texture>();

	texture->FileName = filename;
	std::wstring wstrname = std::wstring(filename.begin(), filename.end());
	TextureInfo info;
	ThrowIfFailed(LoadWICTextureFromFile(m_device, wstrname.c_str(),
		loadedTexture.ReleaseAndGetAddressOf(), decodedData, subresource, info));


	const UINT64 texBufferSize = GetRequiredIntermediateSize(loadedTexture.Get(), 0, 1);
	texture->UploadResource = helper::CreateBuffer(m_device, texBufferSize, 
		D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, helper::kUploadHeapProps);

	UpdateSubresources(m_cmdList, loadedTexture.Get(), texture->UploadResource.Get(),0, 0, 1, &subresource);
	CD3DX12_RESOURCE_BARRIER barr = CD3DX12_RESOURCE_BARRIER::Transition(loadedTexture.Get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	m_cmdList->ResourceBarrier(1, &barr);

	texture->Resource = loadedTexture;

	m_textureLoaded.push_back(texture);

	return true;
}

ID3D12DescriptorHeap* TextureLoader::GenerateHeap()
{
	if (m_textureLoaded.empty()) return nullptr;

	ID3D12DescriptorHeap*  m_srvTexHeap = helper::CreateDescriptorHeap(m_device, m_textureLoaded.size(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

	UINT cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_srvTexHeap->GetCPUDescriptorHandleForHeapStart());
	int srvheapIndex = 0;

	for (auto& text : m_textureLoaded) {
		text->SrvHeapIndex = srvheapIndex++;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = text->Resource->GetDesc().Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;//2D贴图
		srvDesc.Texture2D.MostDetailedMip = 0;//细节最详尽的mipmap层级为0
		srvDesc.Texture2D.MipLevels = text->Resource->GetDesc().MipLevels;//mipmap层级数量
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;//可访问的mipmap最小层级数为0
		m_device->CreateShaderResourceView(text->Resource.Get(), &srvDesc, handle);

		handle.Offset(1, cbvSrvUavDescriptorSize);
	}

	return m_srvTexHeap;
}

std::vector<std::shared_ptr<Texture>>& TextureLoader::GetTextureLoaded()
{
	return m_textureLoaded;
}

