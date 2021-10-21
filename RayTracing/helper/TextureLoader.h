#pragma once

#include "core/D3DUtility.h"

using namespace Microsoft::WRL;
class TextureLoader
{
public:
	TextureLoader() = default;
	TextureLoader(const TextureLoader&) = delete;
	TextureLoader(const TextureLoader&&) = delete;

	void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
	//Load resource to your texture,you need make_shared first
	bool Load(std::string filename,std::shared_ptr<Texture> texture);

	ID3D12DescriptorHeap* GenerateHeap();
	std::vector<std::shared_ptr<Texture>>& GetTextureLoaded();

private:
	ID3D12Device* m_device;
	ID3D12GraphicsCommandList* m_cmdList;

	std::vector<std::shared_ptr<Texture>>	m_textureLoaded;
};

