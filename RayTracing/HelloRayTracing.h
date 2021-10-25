#pragma once
#include "core/DXSample.h"
#include <dxcapi.h>
#include <vector>
#include "core/D3DUtility.h"
#include "helper/TextureLoader.h"
#include "helper/TopLevelASGenerator.h"
#include "helper/ShaderBindingTableGenerator.h"

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
	virtual void OnKeyUp(UINT8);

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
	void CheckRaytracingSupport();

	//DXR  objects
	struct AccelerationStructureBuffers {
		ComPtr<ID3D12Resource> pScratch;      // Scratch memory for AS builder
		ComPtr<ID3D12Resource> pResult;       // Where the AS is
		ComPtr<ID3D12Resource> pInstanceDesc; // Hold the matrices of the instances
	};

	ComPtr<ID3D12Resource>					m_bottomLevelAS; 
	nv_helpers_dx12::TopLevelASGenerator	m_topLevelASGenerator;
	AccelerationStructureBuffers			m_topLevelASBuffers;
	std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>> m_instances;

	AccelerationStructureBuffers CreateBottomLevelAS(
		std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vVertexBuffers,
		std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vIndexBuffers = {});
	void CreateTopLevelAS(const std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>>& instances);
	void CreateAccelerationStructures();


	//dxr signature
	ComPtr<ID3D12RootSignature> m_rtGlobalSignature;
	struct RayTracingShaderLibrary {
		std::string					name; //user define
		ComPtr<IDxcBlob>			library;
		std::vector<std::wstring>	exportSymbols;
		ComPtr<ID3D12RootSignature> signature;   //local signature for each raytracing shader
	};
	std::vector<RayTracingShaderLibrary> m_rtShaderLibrary;
	RayTracingShaderLibrary CreateRayTracingShaderLibrary(std::string name, LPCWSTR shadername,
		std::vector<std::wstring> exportSymbols, ComPtr<ID3D12RootSignature> signature);

	// Ray tracing pipeline state
	ComPtr<ID3D12StateObject>			m_rtStateObject;
	ComPtr<ID3D12StateObjectProperties> m_rtStateObjectProps;
	void CreateRaytracingPipeline();

	//SBT
	ComPtr<ID3D12Resource>				m_outputResource;
	ComPtr<ID3D12DescriptorHeap>		m_rtSrvUavHeap;
	nv_helpers_dx12::ShaderBindingTableGenerator	m_sbtHelper;
	ComPtr<ID3D12Resource>							m_sbtStorage;
	void CreateRayTracingResource();
	void CreateShaderBindingTable();


private:
	std::array<CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

};

