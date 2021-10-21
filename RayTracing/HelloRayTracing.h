#pragma once
#include "core/DXSample.h"
#include <dxcapi.h>
#include <vector>
#include "core/D3DUtility.h"
#include "helper/TextureLoader.h"
using Microsoft::WRL::ComPtr;

class HelloRayTracing : public DXSample
{
public:
	HelloRayTracing(UINT width, UINT height, std::wstring name);

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

	virtual void OnButtonDown(UINT32 lParam);
	virtual void OnMouseMove(UINT8 wParam, UINT32 lParam);
	virtual void OnKeyDown(UINT8) {}
	virtual void OnKeyUp(UINT8) {}

private:
	//common base
	static const UINT					FrameCount = 2;
	CD3DX12_VIEWPORT					m_viewport;
	CD3DX12_RECT						m_scissorRect;
	ComPtr<IDXGISwapChain3>				m_swapChain;
	ComPtr<ID3D12Device5>				m_device;
	ComPtr<ID3D12Resource>				m_renderTargets[FrameCount];

	ComPtr<ID3D12CommandAllocator>		m_commandAllocator;
	ComPtr<ID3D12CommandQueue>			m_commandQueue;
	ComPtr<ID3D12GraphicsCommandList4>	m_commandList;

	ComPtr<ID3D12DescriptorHeap>		m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap>		m_dsvHeap;
	ComPtr<ID3D12Resource>				m_depthStencil;

	UINT m_rtvDescriptorSize;
	UINT m_dsvDescriptorSize;
	UINT m_cbvSrvUavDescriptorSize;

	void Initialize();
	void CreateDescriptorHeap();
	void LoadAssets();
	void WaitForPreviousFrame();

	std::unordered_map<std::string, std::unique_ptr<Mesh>> m_meshes;
	Model m_sceneModel;

	//raster Pipeline objects.
	ComPtr<ID3D12RootSignature> m_rasterRootSignature;
	ComPtr<ID3D12PipelineState> m_rasterPiplineState;
	void LoadRasterPipeline();
	ComPtr<ID3D12Resource>		m_rasterObjectCB;

	// Synchronization objects.
	UINT				m_frameIndex;
	HANDLE				m_fenceEvent;
	ComPtr<ID3D12Fence>	m_fence;
	UINT64				m_fenceValue;

	// Perspective Camera
	ComPtr<ID3D12Resource>			m_cameraBuffer;
	ComPtr<ID3D12DescriptorHeap>	m_constHeap;
	uint32_t						m_cameraBufferSize = 0;
	void CreateCameraBuffer();
	void UpdateCameraBuffer();

	//texture 
	TextureLoader m_textloader;
	ComPtr<ID3D12DescriptorHeap>	m_srvTexHeap;

	//
	bool m_raster = true;

	//DXR Pipeline objects



	std::array<CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();
};

