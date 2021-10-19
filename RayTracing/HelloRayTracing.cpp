#include "stdafx.h"
#include "HelloRayTracing.h"

#include "helper/BottomLevelASGenerator.h"
#include "helper/RaytracingPipelineGenerator.h"
#include "helper/RootSignatureGenerator.h"
#include "glm/gtc/type_ptr.hpp"
#include "helper/manipulator.h"
#include <windowsx.h>
#include <DirectXMath.h>
#include <d3dcompiler.h>

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
    LoadRasterPipeline();
    LoadAssets();
    CreateCameraBuffer();

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

        std::vector< ID3D12DescriptorHeap* > heaps = { m_constHeap.Get() };
        m_commandList->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
        m_commandList->SetGraphicsRootDescriptorTable(0, m_constHeap->GetGPUDescriptorHandleForHeapStart());

        D3D12_VERTEX_BUFFER_VIEW vertexBufferView = m_meshes["tet"]->VertexBufferView();
        m_commandList->IASetVertexBuffers(0, 1,&vertexBufferView);
        D3D12_INDEX_BUFFER_VIEW indexBufferView = m_meshes["tet"]->IndexBufferView();
        m_commandList->IASetIndexBuffer(&indexBufferView);
        m_commandList->DrawIndexedInstanced(m_meshes["tet"]->IndexCount, 1, 0, 0, 0);
    }
    else { //DXR continue

    }

    transition = CD3DX12_RESOURCE_BARRIER::Transition(
        m_renderTargets[m_frameIndex].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    m_commandList->ResourceBarrier(1, &transition);

    ThrowIfFailed(m_commandList->Close());

    // Execute the command list.
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

void HelloRayTracing::LoadAssets()
{
    {
        std::unique_ptr<Mesh> tetrahedron = std::make_unique<Mesh>();
        tetrahedron->Name = "tetrahedron";

        Vertex_Simple triangleVertices[] = {
            {{std::sqrtf(8.f / 9.f), 0.f, -1.f / 3.f}, {1.f, 0.f, 0.f, 1.f}},
            {{-std::sqrtf(2.f / 9.f), std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 1.f, 0.f, 1.f}},
            {{-std::sqrtf(2.f / 9.f), -std::sqrtf(2.f / 3.f), -1.f / 3.f}, {0.f, 0.f, 1.f, 1.f}},
            {{0.f, 0.f, 1.f}, {1, 0, 1, 1}}
        };
        const UINT vertexBufferSize = sizeof(triangleVertices);
        tetrahedron->VertexBufferByteSize = vertexBufferSize;
        tetrahedron->VertexByteStride = sizeof(Vertex_Simple);
        tetrahedron->VertexCount = 4.;
        tetrahedron->VertexBufferGPU = helper::CreateDefaultBuffer(m_device.Get(), m_commandList.Get(),triangleVertices, vertexBufferSize,tetrahedron->VertexBufferUploader);
        ThrowIfFailed(D3DCreateBlob(vertexBufferSize, &tetrahedron->VertexBufferCPU));
        CopyMemory(tetrahedron->VertexBufferCPU->GetBufferPointer(), triangleVertices, vertexBufferSize);

        std::vector<UINT16> indices = { 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2 };
        const UINT indexBufferSize = static_cast<UINT>(indices.size()) * sizeof(UINT16);
        tetrahedron->IndexBufferByteSize = indexBufferSize;
        tetrahedron->IndexCount = indices.size();
        tetrahedron->IndexBufferGPU = helper::CreateDefaultBuffer(m_device.Get(), m_commandList.Get(), indices.data(), indexBufferSize,tetrahedron->IndexBufferUploader);
        ThrowIfFailed(D3DCreateBlob(indexBufferSize, &tetrahedron->IndexBufferCPU));
        CopyMemory(tetrahedron->IndexBufferCPU->GetBufferPointer(), indices.data(), indexBufferSize);

        m_meshes["tet"] = std::move(tetrahedron);
    }
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
        CD3DX12_ROOT_PARAMETER constantParameter;
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        constantParameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
        CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init(1, &constantParameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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
        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0} };

        // Describe and create the graphics pipeline state object (PSO).
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rasterRootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
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

    m_constHeap = helper::CreateDescriptorHeap(m_device.Get(), 1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

    // Describe and create the constant buffer view.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_cameraBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = m_cameraBufferSize;

    // Get a handle to the heap memory on the CPU side, to be able to write the
    // descriptors directly
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =m_constHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateConstantBufferView(&cbvDesc, srvHandle);
}

void HelloRayTracing::UpdateCameraBuffer()
{
    std::vector<XMMATRIX> matrices(4);

    const glm::mat4& mat = nv_helpers_dx12::CameraManip.getMatrix();
    memcpy(&matrices[0].r->m128_f32[0], glm::value_ptr(mat), 16 * sizeof(float));

    float fovAngleY = 45.0f * XM_PI / 180.0f;
    matrices[1] = DirectX::XMMatrixPerspectiveFovRH(fovAngleY, m_aspectRatio, 0.1f, 1000.0f);

    XMVECTOR det;
    matrices[2] = DirectX::XMMatrixInverse(&det, matrices[0]);
    matrices[3] = DirectX::XMMatrixInverse(&det, matrices[1]);

    // Copy the matrix contents
    helper::CopyDataToUploadBuffer(m_cameraBuffer.Get(), matrices.data(), m_cameraBufferSize);
}


