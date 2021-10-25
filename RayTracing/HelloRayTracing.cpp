#include "stdafx.h"
#include "HelloRayTracing.h"

#include "helper/BottomLevelASGenerator.h"
#include "helper/RaytracingPipelineGenerator.h"
#include "helper/RootSignatureGenerator.h"
#include "glm/gtc/type_ptr.hpp"
#include "helper/manipulator.h"
#include "helper/ModelLoader.h"
#include <windowsx.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>
#include <iostream>
#include "helper/TextureLoader.h"
#include "core/D3DUtility.h"

using namespace DirectX;

HelloRayTracing::HelloRayTracing(UINT width, UINT height, std::wstring name) 
    : DXSample(width, height, name), m_frameIndex(0),
    m_viewport(0.0f, 0.0f, static_cast<float>(width),static_cast<float>(height)),
    m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height)),
    m_rtvDescriptorSize(0) {}

void HelloRayTracing::OnInit()
{
    //init camera
    nv_helpers_dx12::CameraManip.setWindowSize(GetWidth(), GetHeight());
    nv_helpers_dx12::CameraManip.setLookat(glm::vec3(1.5f, 1.5f, 1.5f), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));

    Initialize();
    CheckRaytracingSupport();

    CreateDescriptorHeap();
    LoadAssets();
    LoadRasterPipeline();
    CreateCameraBuffer();

    CreateAccelerationStructures();
    CreateRaytracingPipeline();
    CreateRayTracingResource();
    CreateShaderBindingTable();


    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    WaitForPreviousFrame();

}

void HelloRayTracing::OnUpdate()
{
    UpdateCameraBuffer();
}

void HelloRayTracing::OnRender()
{
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_rasterPiplineState.Get()));

    // Set necessary state.
    m_commandList->SetGraphicsRootSignature(m_rasterRootSignature.Get());
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_commandList->ResourceBarrier(1, &transition);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle( m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_frameIndex, m_rtvDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    if (m_raster) {
        const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        std::vector< ID3D12DescriptorHeap* > heaps = { m_constHeap.Get()};
        m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
        m_commandList->SetGraphicsRootDescriptorTable(0, m_constHeap->GetGPUDescriptorHandleForHeapStart());

        m_commandList->SetGraphicsRootConstantBufferView(1, m_rasterObjectCB->GetGPUVirtualAddress());

        std::vector< ID3D12DescriptorHeap* > heaps2 = { m_srvTexHeap.Get() };
        m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps2.size()), heaps2.data());


        //D3D12_VERTEX_BUFFER_VIEW vertexBufferView = m_meshes["tet"]->VertexBufferView();
        //m_commandList->IASetVertexBuffers(0, 1,&vertexBufferView);
        //D3D12_INDEX_BUFFER_VIEW indexBufferView = m_meshes["tet"]->IndexBufferView();
        //m_commandList->IASetIndexBuffer(&indexBufferView);
        //m_commandList->DrawIndexedInstanced(m_meshes["tet"]->IndexCount, 1, 0, 0, 0);

        for (auto& mesh : m_sceneModel.Meshes) {
            if (!mesh.second.empty()) {
                CD3DX12_GPU_DESCRIPTOR_HANDLE srvTexHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_srvTexHeap->GetGPUDescriptorHandleForHeapStart());
                srvTexHandle.Offset(m_sceneModel.Textures[mesh.second[0]]->SrvHeapIndex, m_cbvSrvUavDescriptorSize);
                m_commandList->SetGraphicsRootDescriptorTable(2, srvTexHandle);
            }

            D3D12_VERTEX_BUFFER_VIEW vertexBufferView = mesh.first->VertexBufferView();
            m_commandList->IASetVertexBuffers(0, 1,&vertexBufferView);
            D3D12_INDEX_BUFFER_VIEW indexBufferView = mesh.first->IndexBufferView();
            m_commandList->IASetIndexBuffer(&indexBufferView);
            m_commandList->DrawIndexedInstanced(mesh.first->IndexCount, 1, 0, 0, 0);
        }

    }
    else { //DXR continue
        std::vector<ID3D12DescriptorHeap*> heaps = { m_rtSrvUavHeap.Get() };
        m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()),heaps.data());

        CD3DX12_RESOURCE_BARRIER transition = CD3DX12_RESOURCE_BARRIER::Transition(
            m_outputResource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_commandList->ResourceBarrier(1, &transition);

        // Setup the raytracing task
        D3D12_DISPATCH_RAYS_DESC desc = {};
        // The ray generation shaders are always at the beginning of the SBT.
        uint32_t rayGenerationSectionSizeInBytes = m_sbtHelper.GetRayGenSectionSize();
        desc.RayGenerationShaderRecord.StartAddress = m_sbtStorage->GetGPUVirtualAddress();
        desc.RayGenerationShaderRecord.SizeInBytes = rayGenerationSectionSizeInBytes;
        // The miss shaders are in the second SBT section
        uint32_t missSectionSizeInBytes = m_sbtHelper.GetMissSectionSize();
        desc.MissShaderTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() + rayGenerationSectionSizeInBytes;
        desc.MissShaderTable.SizeInBytes = missSectionSizeInBytes;
        desc.MissShaderTable.StrideInBytes = m_sbtHelper.GetMissEntrySize();
        // The hit groups section start after the miss shaders.
        uint32_t hitGroupsSectionSize = m_sbtHelper.GetHitGroupSectionSize();
        desc.HitGroupTable.StartAddress = m_sbtStorage->GetGPUVirtualAddress() +rayGenerationSectionSizeInBytes +missSectionSizeInBytes;
        desc.HitGroupTable.SizeInBytes = hitGroupsSectionSize;
        desc.HitGroupTable.StrideInBytes = m_sbtHelper.GetHitGroupEntrySize();
        // Dimensions of the image to render, identical to a kernel launch dimension
        desc.Width = GetWidth();
        desc.Height = GetHeight();
        desc.Depth = 1;


        // Bind the raytracing pipeline
        m_commandList->SetPipelineState1(m_rtStateObject.Get());
        m_commandList->SetComputeRootSignature(m_rtGlobalSignature.Get());
        m_commandList->DispatchRays(&desc);

        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_outputResource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_commandList->ResourceBarrier(1, &transition);
        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        m_commandList->ResourceBarrier(1, &transition);

        m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(),m_outputResource.Get());

        transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_commandList->ResourceBarrier(1, &transition);
    }

    transition = CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &transition);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Present the frame.
    ThrowIfFailed(m_swapChain->Present(1, 0));
    WaitForPreviousFrame();
}

void HelloRayTracing::OnDestroy()
{
    WaitForPreviousFrame();

    CloseHandle(m_fenceEvent);
}

void HelloRayTracing::OnButtonDown(UINT32 lParam)
{
    nv_helpers_dx12::CameraManip.setMousePosition(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam));
}

void HelloRayTracing::OnMouseMove(UINT8 wParam, UINT32 lParam)
{
    using nv_helpers_dx12::Manipulator;
    Manipulator::Inputs inputs;
    inputs.lmb = wParam & MK_LBUTTON;
    inputs.mmb = wParam & MK_MBUTTON;
    inputs.rmb = wParam & MK_RBUTTON;
    if (!inputs.lmb && !inputs.rmb && !inputs.mmb)
        return; // no mouse button pressed

    inputs.ctrl = GetAsyncKeyState(VK_CONTROL);
    inputs.shift = GetAsyncKeyState(VK_SHIFT);
    inputs.alt = GetAsyncKeyState(VK_MENU);

    CameraManip.mouseMove(-GET_X_LPARAM(lParam), -GET_Y_LPARAM(lParam), inputs);
   
}

void HelloRayTracing::OnKeyUp(UINT8 key)
{
    if (key == VK_SPACE) {
        m_raster = !m_raster;
    }
}

void HelloRayTracing::Initialize()
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    // Enable the debug layer
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    //create device
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    if (m_useWarpDevice) {
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
    }
    else {
        ComPtr<IDXGIAdapter1> hardwareAdapter;
        GetHardwareAdapter(factory.Get(), &hardwareAdapter);
        ThrowIfFailed(D3D12CreateDevice(hardwareAdapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&m_device)));
    }

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Describe and create the command queue.
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
        ThrowIfFailed(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(m_commandList.GetAddressOf())));
        //m_commandList->Close();
    }

    // Describe and create the swap chain.
    {
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = FrameCount;
        swapChainDesc.Width = m_width;
        swapChainDesc.Height = m_height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain;
        ThrowIfFailed(factory->CreateSwapChainForHwnd(
            m_commandQueue.Get(),
            Win32Application::GetHwnd(), &swapChainDesc, nullptr, nullptr, &swapChain));
        // This sample does not support fullscreen transitions.
        ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
        ThrowIfFailed(swapChain.As(&m_swapChain));
    }
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Create rtv/dsv descriptor heaps .
    {
        //rtv
        m_rtvHeap = helper::CreateDescriptorHeap(m_device.Get(), FrameCount, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, false);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

        // Create a RTV for each frame.
        for (UINT n = 0; n < FrameCount; n++) {
            ThrowIfFailed(m_swapChain->GetBuffer(n, IID_PPV_ARGS(&m_renderTargets[n])));
            m_device->CreateRenderTargetView(m_renderTargets[n].Get(), nullptr,rtvHandle);
            rtvHandle.Offset(1, m_rtvDescriptorSize);
        }

        //dsv
        m_dsvHeap = helper::CreateDescriptorHeap(m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, false);
        D3D12_HEAP_PROPERTIES depthHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC depthResourceDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            DXGI_FORMAT_D32_FLOAT, m_width, m_height, 1, 1);
        depthResourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        CD3DX12_CLEAR_VALUE depthOptimizedClearValue(DXGI_FORMAT_D32_FLOAT, 1.0f, 0);

        // Allocate the buffer itself, with a state allowing depth writes
        ThrowIfFailed(m_device->CreateCommittedResource(
            &depthHeapProperties, D3D12_HEAP_FLAG_NONE, &depthResourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthOptimizedClearValue,
            IID_PPV_ARGS(&m_depthStencil)));

        // Write the depth buffer view into the depth buffer heap
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;

        m_device->CreateDepthStencilView(m_depthStencil.Get(), &dsvDesc,m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    //ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
        // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;

        // Create an event handle to use for frame synchronization.
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_fenceEvent == nullptr) {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }
}

void HelloRayTracing::CreateDescriptorHeap()
{
    m_constHeap = helper::CreateDescriptorHeap(m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

}

void HelloRayTracing::LoadAssets()
{
    {
        std::unique_ptr<Mesh> tetrahedron = std::make_unique<Mesh>();
        tetrahedron->Name = "tetrahedron";

        Vertex_Model triangleVertices[] = {
            {{std::sqrtf(8.f / 9.f), 0.f, -1.f / 3.f}, {1.f, 0.f, 0.f},{0.0,0.0}},
            {{-std::sqrtf(2.f / 9.f), std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 1.f, 0.f},{1.0,0.0}},
            {{-std::sqrtf(2.f / 9.f), -std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 0.f, 1.f},{0.0,1.0}},
            {{0.f, 0.f, 1.f}, {1.f, 0.f, 0.f},{1.0,1.0}}
        };
        const UINT vertexBufferSize = sizeof(triangleVertices);
        tetrahedron->VertexBufferByteSize = vertexBufferSize;
        tetrahedron->VertexByteStride = sizeof(Vertex_Model);
        tetrahedron->VertexCount = 4.;
        tetrahedron->VertexBufferGPU = helper::CreateDefaultBuffer(m_device.Get(), m_commandList.Get(),triangleVertices, vertexBufferSize,tetrahedron->VertexBufferUploader);

        std::vector<UINT32> indices = { 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2 };
        const UINT indexBufferSize = static_cast<UINT>(indices.size()) * sizeof(UINT32);
        tetrahedron->IndexBufferByteSize = indexBufferSize;
        tetrahedron->IndexCount = indices.size();
        tetrahedron->IndexBufferGPU = helper::CreateDefaultBuffer(m_device.Get(), m_commandList.Get(), indices.data(), indexBufferSize,tetrahedron->IndexBufferUploader);

        m_meshes["tet"] = std::move(tetrahedron);
    }

    m_textloader.Initialize(m_device.Get(), m_commandList.Get());

    {
        ModelLoader modelLoader(m_device.Get(),m_commandList.Get(), &m_textloader);
        modelLoader.Load("Resource/Model/sponza/sponza.obj", m_sceneModel);           
    }

    m_srvTexHeap = m_textloader.GenerateHeap();
}

void HelloRayTracing::WaitForPreviousFrame()
{
    //This is code implemented as such for simplicity. use FRAME RESOURCE can be more efficient
    const UINT64 fence = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fence));
    m_fenceValue++;

    // Wait until the previous frame is finished.
    if (m_fence->GetCompletedValue() < fence) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fence, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void HelloRayTracing::LoadRasterPipeline()
{
    // Create an empty root signature.
    {
        CD3DX12_ROOT_PARAMETER rootParameter[3];
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        rootParameter[0].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);

        rootParameter[1].InitAsConstantBufferView(1);

        CD3DX12_DESCRIPTOR_RANGE srvTable;
        srvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        rootParameter[2].InitAsDescriptorTable(1, &srvTable, D3D12_SHADER_VISIBILITY_PIXEL);

        auto staticSamplers = GetStaticSamplers();
        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(3, rootParameter, (UINT)staticSamplers.size(), staticSamplers.data(), D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&m_rasterRootSignature)));
    }

    // Create the pipeline state, which includes compiling and loading shaders.
    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(),
            nullptr, nullptr, "VSMain", "vs_5_0",
            compileFlags, 0, &vertexShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(),
            nullptr, nullptr, "PSMain", "ps_5_0",
            compileFlags, 0, &pixelShader, nullptr));

        // Define the vertex input layout.
        //D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        //    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        //     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        //    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
        //     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0} };

        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rasterRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;
        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_rasterPiplineState)));
    }
}

void HelloRayTracing::CreateCameraBuffer()
{
    uint32_t nbMatrix = 4; // view, perspective, viewInv, perspectiveInv
    m_cameraBufferSize = nbMatrix * sizeof(XMMATRIX);

    // Create the constant buffer for all matrices
    m_cameraBuffer = helper::CreateBuffer(m_device.Get(), m_cameraBufferSize, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ, helper::kUploadHeapProps);

    // Describe and create the constant buffer view.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_cameraBufferSize;

    // Get a handle to the heap memory on the CPU side, to be able to write the
    // descriptors directly
    D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle =m_constHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateConstantBufferView(&cbvDesc, cbvHandle);

    //--------------------------------------------------
    XMMATRIX world = DirectX::XMMatrixScaling(0.1, 0.1, 0.1);
    m_rasterObjectCB = helper::CreateBuffer(m_device.Get(), sizeof(XMMATRIX), D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ, helper::kUploadHeapProps);
    helper::CopyDataToUploadBuffer(m_rasterObjectCB.Get(), &world, sizeof(XMMATRIX));
    

}

void HelloRayTracing::UpdateCameraBuffer()
{
    std::vector<XMMATRIX> matrices(4);

    const glm::mat4& mat = nv_helpers_dx12::CameraManip.getMatrix();
    memcpy(&matrices[0].r->m128_f32[0], glm::value_ptr(mat), 16 * sizeof(float));

    float fovAngleY = 45.0f * XM_PI / 180.0f;
    matrices[1] = DirectX::XMMatrixPerspectiveFovRH(fovAngleY, m_aspectRatio, 0.1f, 10000.0f);

    XMVECTOR det;
    matrices[2] = DirectX::XMMatrixInverse(&det, matrices[0]);
    matrices[3] = DirectX::XMMatrixInverse(&det, matrices[1]);

    // Copy the matrix contents
    helper::CopyDataToUploadBuffer(m_cameraBuffer.Get(), matrices.data(), m_cameraBufferSize);


}

void HelloRayTracing::CheckRaytracingSupport()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    ThrowIfFailed(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
        &options5, sizeof(options5)));
    if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
        throw std::runtime_error("Raytracing not supported on device");
}

HelloRayTracing::AccelerationStructureBuffers
HelloRayTracing::CreateBottomLevelAS(
    std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vVertexBuffers, 
    std::vector<std::pair<ComPtr<ID3D12Resource>, uint32_t>> vIndexBuffers)
{
    nv_helpers_dx12::BottomLevelASGenerator bottomLevelAS;
    // Adding all vertex buffers and not transforming their position.
    for (size_t i = 0; i < vVertexBuffers.size(); i++) {
        if (i < vIndexBuffers.size() && vIndexBuffers[i].second > 0)
            bottomLevelAS.AddVertexBuffer(
                vVertexBuffers[i].first.Get(), 0,vVertexBuffers[i].second, sizeof(Vertex_Model),
                vIndexBuffers[i].first.Get(), 0,vIndexBuffers[i].second, nullptr, 0, true);
        else
            bottomLevelAS.AddVertexBuffer(vVertexBuffers[i].first.Get(), 0, vVertexBuffers[i].second, sizeof(Vertex_Model), 0, 0);
    }

    UINT64 scratchSizeInBytes = 0;
    UINT64 resultSizeInBytes = 0;

    bottomLevelAS.ComputeASBufferSizes(m_device.Get(), false, &scratchSizeInBytes, &resultSizeInBytes);

    AccelerationStructureBuffers buffers;
    buffers.pScratch = helper::CreateBuffer(m_device.Get(), scratchSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON,helper::kDefaultHeapProps);
    buffers.pResult = helper::CreateBuffer(m_device.Get(), resultSizeInBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,helper::kDefaultHeapProps);

    bottomLevelAS.Generate(m_commandList.Get(), buffers.pScratch.Get(), buffers.pResult.Get(), false, nullptr);

    return buffers;
}

void HelloRayTracing::CreateTopLevelAS(const std::vector<std::pair<ComPtr<ID3D12Resource>, DirectX::XMMATRIX>>& instances)
{
    // Gather all the instances into the builder helper
    for (size_t i = 0; i < instances.size(); i++) {
        m_topLevelASGenerator.AddInstance(instances[i].first.Get(),
            instances[i].second, static_cast<UINT>(i),static_cast<UINT>(i));
    }

    UINT64 scratchSize, resultSize, instanceDescsSize;
    m_topLevelASGenerator.ComputeASBufferSizes(m_device.Get(), true, &scratchSize, &resultSize, &instanceDescsSize);

    m_topLevelASBuffers.pScratch = helper::CreateBuffer(m_device.Get(), scratchSize, 
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,helper::kDefaultHeapProps);
    m_topLevelASBuffers.pResult = helper::CreateBuffer(m_device.Get(), resultSize, 
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,helper::kDefaultHeapProps);
    m_topLevelASBuffers.pInstanceDesc = helper::CreateBuffer(m_device.Get(), instanceDescsSize, 
        D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ, helper::kUploadHeapProps);

    m_topLevelASGenerator.Generate(m_commandList.Get(),m_topLevelASBuffers.pScratch.Get(),
        m_topLevelASBuffers.pResult.Get(), m_topLevelASBuffers.pInstanceDesc.Get());
}

void HelloRayTracing::CreateAccelerationStructures()
{
    // Build the bottom AS from the Triangle vertex buffer
    UINT meshCount = m_sceneModel.Meshes.size();
    std::vector<AccelerationStructureBuffers> bottomLevelBuffers(meshCount);
    for (int i = 0; i < meshCount;++i) {
        AccelerationStructureBuffers bottomLevelBuffers = CreateBottomLevelAS(
            { {m_sceneModel.Meshes[i].first->VertexBufferGPU.Get(),m_sceneModel.Meshes[i].first->VertexCount} },
            { {m_sceneModel.Meshes[i].first->IndexBufferGPU.Get(),m_sceneModel.Meshes[i].first->IndexCount} }
           );
        m_instances.push_back({ bottomLevelBuffers.pResult,DirectX::XMMatrixScaling(0.1, 0.1, 0.1) });
    }

    CreateTopLevelAS(m_instances);

    // Store the AS buffers. The rest of the buffers will be released once we exit the function
    //m_bottomLevelAS = bottomLevelBuffers.pResult;

}

HelloRayTracing::RayTracingShaderLibrary 
HelloRayTracing::CreateRayTracingShaderLibrary(std::string name, LPCWSTR shadername, 
    std::vector<std::wstring> exportSymbols, ComPtr<ID3D12RootSignature> signature)
{
    HelloRayTracing::RayTracingShaderLibrary rtsl;
    rtsl.name = name;
    rtsl.library = helper::CompileShaderLibrary(shadername);
    rtsl.exportSymbols = exportSymbols;
    rtsl.signature = signature;
    return rtsl;
}

void HelloRayTracing::CreateRaytracingPipeline()
{
    nv_helpers_dx12::RayTracingPipelineGenerator pipeline(m_device.Get());

    //create global signature
    auto staticSamplers = GetStaticSamplers();
    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(0,nullptr, (UINT)staticSamplers.size(), staticSamplers.data(), D3D12_ROOT_SIGNATURE_FLAG_NONE);
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),IID_PPV_ARGS(&m_rtGlobalSignature)));
    pipeline.AddGlobalRootSignature(m_rtGlobalSignature.Get());

    //create loacl signature
    nv_helpers_dx12::RootSignatureGenerator raygenRSG;
    raygenRSG.AddHeapRangesParameter({
            {0,1,0,D3D12_DESCRIPTOR_RANGE_TYPE_UAV,0 },
            {0,1,0,D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1 },
            {0,1,0,D3D12_DESCRIPTOR_RANGE_TYPE_CBV,2 }
        });

    m_rtShaderLibrary.push_back(CreateRayTracingShaderLibrary(
        "MyFirstRayGen",L"shaders/RayGen.hlsl", { L"RayGen" }, raygenRSG.Generate(m_device.Get(),true)));

    nv_helpers_dx12::RootSignatureGenerator missRSG;
    m_rtShaderLibrary.push_back(CreateRayTracingShaderLibrary(
        "MyFirstMiss", L"shaders/Miss.hlsl", { L"Miss" }, missRSG.Generate(m_device.Get(), true)));

    nv_helpers_dx12::RootSignatureGenerator hitRSG;
    hitRSG.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 0); // vertex
    hitRSG.AddRootParameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 1); // indices
    hitRSG.AddHeapRangesParameter({                            //texture
            {2,1,0,D3D12_DESCRIPTOR_RANGE_TYPE_SRV,0 }
        });

    m_rtShaderLibrary.push_back(CreateRayTracingShaderLibrary(
        "MyFirstHit", L"shaders/Hit.hlsl", { L"ClosestHit" }, hitRSG.Generate(m_device.Get(), true)));

    for (auto& lib : m_rtShaderLibrary) {
        pipeline.AddLibrary(lib.library.Get(), lib.exportSymbols);
    }

    pipeline.AddHitGroup(L"HitGroup", L"ClosestHit");
    pipeline.AddRootSignatureAssociation(m_rtShaderLibrary[0].signature.Get(), { L"RayGen" });
    pipeline.AddRootSignatureAssociation(m_rtShaderLibrary[1].signature.Get(), { L"Miss" });
    pipeline.AddRootSignatureAssociation(m_rtShaderLibrary[2].signature.Get(), { L"HitGroup" });


    pipeline.SetMaxPayloadSize(4 * sizeof(float)); // RGB + distance
    pipeline.SetMaxAttributeSize(2 * sizeof(float)); // barycentric coordinates
    pipeline.SetMaxRecursionDepth(1);
    m_rtStateObject = pipeline.Generate();
    ThrowIfFailed(m_rtStateObject->QueryInterface(IID_PPV_ARGS(&m_rtStateObjectProps)));
}

void HelloRayTracing::CreateRayTracingResource()
{
    //create output buffer
    {
        D3D12_RESOURCE_DESC resDesc = {};
        resDesc.DepthOrArraySize = 1;
        resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        resDesc.Width = GetWidth();
        resDesc.Height = GetHeight();
        resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resDesc.MipLevels = 1;
        resDesc.SampleDesc.Count = 1;
        ThrowIfFailed(m_device->CreateCommittedResource(&helper::kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&m_outputResource)));
    }

    //create shader resource heap
    {
        m_rtSrvUavHeap = helper::CreateDescriptorHeap(m_device.Get(), 3, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =m_rtSrvUavHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(m_outputResource.Get(), nullptr, &uavDesc,srvHandle);

        srvHandle.ptr += m_cbvSrvUavDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.RaytracingAccelerationStructure.Location = m_topLevelASBuffers.pResult->GetGPUVirtualAddress();
        m_device->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);

        srvHandle.ptr += m_cbvSrvUavDescriptorSize;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = m_cameraBufferSize;
        m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
    }
}

void HelloRayTracing::CreateShaderBindingTable()
{
    m_sbtHelper.Reset();
    D3D12_GPU_DESCRIPTOR_HANDLE srvUavHeapHandle = m_rtSrvUavHeap->GetGPUDescriptorHandleForHeapStart();

    auto heapPointer = reinterpret_cast<UINT64*>(srvUavHeapHandle.ptr);
    m_sbtHelper.AddRayGenerationProgram(L"RayGen", {heapPointer});
    m_sbtHelper.AddMissProgram(L"Miss", {});
    m_sbtHelper.AddMissProgram(L"Miss", {});

    for (int i = 0; i < m_sceneModel.Meshes.size();++i) {
        if (!m_sceneModel.Meshes[i].second.empty()) {      
            CD3DX12_GPU_DESCRIPTOR_HANDLE srvTexHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_srvTexHeap->GetGPUDescriptorHandleForHeapStart());
            srvTexHandle.Offset(m_sceneModel.Textures[m_sceneModel.Meshes[i].second[0]]->SrvHeapIndex, m_cbvSrvUavDescriptorSize);
            auto texheapPointer = reinterpret_cast<UINT64*>(srvTexHandle.ptr);
            m_sbtHelper.AddHitGroup(L"HitGroup", {
                    (void*)(m_sceneModel.Meshes[i].first->VertexBufferGPU->GetGPUVirtualAddress()),
                    (void*)(m_sceneModel.Meshes[i].first->IndexBufferGPU->GetGPUVirtualAddress()),
                    texheapPointer
                });
        }
        else {
            m_sbtHelper.AddHitGroup(L"HitGroup", {
                    (void*)(m_sceneModel.Meshes[i].first->VertexBufferGPU->GetGPUVirtualAddress()),
                    (void*)(m_sceneModel.Meshes[i].first->IndexBufferGPU->GetGPUVirtualAddress())
                });
        }
    }

    uint32_t sbtSize = m_sbtHelper.ComputeSBTSize();
    m_sbtStorage = helper::CreateBuffer(m_device.Get(), sbtSize, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_GENERIC_READ, helper::kUploadHeapProps);

    if (!m_sbtStorage) {
        throw std::logic_error("Could not allocate the shader binding table");
    }

    m_sbtHelper.Generate(m_sbtStorage.Get(), m_rtStateObjectProps.Get());
}


std::array<CD3DX12_STATIC_SAMPLER_DESC, 6> HelloRayTracing::GetStaticSamplers()
{
    //过滤器POINT,寻址模式WRAP的静态采样器
    CD3DX12_STATIC_SAMPLER_DESC pointWarp(0,	//着色器寄存器
        D3D12_FILTER_MIN_MAG_MIP_POINT,		//过滤器类型为POINT(常量插值)
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//U方向上的寻址模式为WRAP（重复寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//V方向上的寻址模式为WRAP（重复寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);	//W方向上的寻址模式为WRAP（重复寻址模式）

    //过滤器POINT,寻址模式CLAMP的静态采样器
    CD3DX12_STATIC_SAMPLER_DESC pointClamp(1,	//着色器寄存器
        D3D12_FILTER_MIN_MAG_MIP_POINT,		//过滤器类型为POINT(常量插值)
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//U方向上的寻址模式为CLAMP（钳位寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//V方向上的寻址模式为CLAMP（钳位寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);	//W方向上的寻址模式为CLAMP（钳位寻址模式）

    //过滤器LINEAR,寻址模式WRAP的静态采样器
    CD3DX12_STATIC_SAMPLER_DESC linearWarp(2,	//着色器寄存器
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,		//过滤器类型为LINEAR(线性插值)
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//U方向上的寻址模式为WRAP（重复寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//V方向上的寻址模式为WRAP（重复寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);	//W方向上的寻址模式为WRAP（重复寻址模式）

    //过滤器LINEAR,寻址模式CLAMP的静态采样器
    CD3DX12_STATIC_SAMPLER_DESC linearClamp(3,	//着色器寄存器
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,		//过滤器类型为LINEAR(线性插值)
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//U方向上的寻址模式为CLAMP（钳位寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//V方向上的寻址模式为CLAMP（钳位寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);	//W方向上的寻址模式为CLAMP（钳位寻址模式）

    //过滤器ANISOTROPIC,寻址模式WRAP的静态采样器
    CD3DX12_STATIC_SAMPLER_DESC anisotropicWarp(4,	//着色器寄存器
        D3D12_FILTER_ANISOTROPIC,			//过滤器类型为ANISOTROPIC(各向异性)
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//U方向上的寻址模式为WRAP（重复寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,	//V方向上的寻址模式为WRAP（重复寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);	//W方向上的寻址模式为WRAP（重复寻址模式）

    //过滤器LINEAR,寻址模式CLAMP的静态采样器
    CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(5,	//着色器寄存器
        D3D12_FILTER_ANISOTROPIC,			//过滤器类型为ANISOTROPIC(各向异性)
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//U方向上的寻址模式为CLAMP（钳位寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,	//V方向上的寻址模式为CLAMP（钳位寻址模式）
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);	//W方向上的寻址模式为CLAMP（钳位寻址模式）

    return { pointWarp, pointClamp, linearWarp, linearClamp, anisotropicWarp, anisotropicClamp };
}