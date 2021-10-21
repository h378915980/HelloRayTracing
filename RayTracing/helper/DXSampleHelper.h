//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "core/Win32Application.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <dxcapi.h>
#include <d3dcompiler.h>


using Microsoft::WRL::ComPtr;

class HrException : public std::runtime_error
{
	inline std::string HrToString(HRESULT hr)
	{
		char s_str[64] = {};
		sprintf_s(s_str, "HRESULT of 0x%08X", static_cast<UINT>(hr));
		return std::string(s_str);
	}
public:
	HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), m_hr(hr) {}
	HRESULT Error() const { return m_hr; }
private:
	const HRESULT m_hr;
};

inline void ThrowIfFailed(HRESULT hr)
{
	if (FAILED(hr))
	{
		throw HrException(hr);
	}
}

inline void GetAssetsPath(_Out_writes_(pathSize) WCHAR* path, UINT pathSize)
{
	if (path == nullptr)
	{
		throw std::exception();
	}

	DWORD size = GetModuleFileName(nullptr, path, pathSize);
	if (size == 0 || size == pathSize)
	{
		// Method failed or path was truncated.
		throw std::exception();
	}

	WCHAR* lastSlash = wcsrchr(path, L'\\');
	if (lastSlash)
	{
		*(lastSlash + 1) = L'\0';
	}
}

inline HRESULT ReadDataFromFile(LPCWSTR filename, byte** data, UINT* size)
{
	using namespace Microsoft::WRL;

	CREATEFILE2_EXTENDED_PARAMETERS extendedParams = {};
	extendedParams.dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS);
	extendedParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	extendedParams.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
	extendedParams.dwSecurityQosFlags = SECURITY_ANONYMOUS;
	extendedParams.lpSecurityAttributes = nullptr;
	extendedParams.hTemplateFile = nullptr;

	Wrappers::FileHandle file(CreateFile2(filename, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &extendedParams));
	if (file.Get() == INVALID_HANDLE_VALUE)
	{
		throw std::exception();
	}

	FILE_STANDARD_INFO fileInfo = {};
	if (!GetFileInformationByHandleEx(file.Get(), FileStandardInfo, &fileInfo, sizeof(fileInfo)))
	{
		throw std::exception();
	}

	if (fileInfo.EndOfFile.HighPart != 0)
	{
		throw std::exception();
	}

	*data = reinterpret_cast<byte*>(malloc(fileInfo.EndOfFile.LowPart));
	*size = fileInfo.EndOfFile.LowPart;

	if (!ReadFile(file.Get(), *data, fileInfo.EndOfFile.LowPart, nullptr, nullptr))
	{
		throw std::exception();
	}

	return S_OK;
}

// Assign a name to the object to aid with debugging.
#if defined(_DEBUG)
inline void SetName(ID3D12Object* pObject, LPCWSTR name)
{
	pObject->SetName(name);
}
inline void SetNameIndexed(ID3D12Object* pObject, LPCWSTR name, UINT index)
{
	WCHAR fullName[50];
	if (swprintf_s(fullName, L"%s[%u]", name, index) > 0)
	{
		pObject->SetName(fullName);
	}
}
#else
inline void SetName(ID3D12Object*, LPCWSTR)
{
}
inline void SetNameIndexed(ID3D12Object*, LPCWSTR, UINT)
{
}
#endif

#define NAME_D3D12_OBJECT(x) SetName(x.Get(), L#x)
#define NAME_D3D12_OBJECT_INDEXED(x, n) SetNameIndexed(x[n].Get(), L#x, n)

#ifndef ROUND_UP
#define ROUND_UP(v, powerOf2Alignment) (((v) + (powerOf2Alignment)-1) & ~((powerOf2Alignment)-1))
#endif


namespace helper {
	inline ID3D12DescriptorHeap* CreateDescriptorHeap(ID3D12Device* device, uint32_t count, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible)
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.NumDescriptors = count;
		desc.Type = type;
		desc.Flags =
			shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		ID3D12DescriptorHeap* pHeap;
		ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&pHeap)));
		return pHeap;
	}

	//create GPU buffer without data
	inline ID3D12Resource* CreateBuffer(ID3D12Device* m_device, uint64_t size,
		D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState,
		const D3D12_HEAP_PROPERTIES& heapProps)
	{
		D3D12_RESOURCE_DESC bufDesc = {};
		bufDesc.Alignment = 0;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Flags = flags;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.Height = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.MipLevels = 1;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.SampleDesc.Quality = 0;
		bufDesc.Width = size;

		ID3D12Resource* pBuffer;
		ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
			initState, nullptr, IID_PPV_ARGS(&pBuffer)));
		return pBuffer;
	}

	// Specifies a heap used for uploading. This heap type has CPU access optimized for uploading to the GPU.
	static const D3D12_HEAP_PROPERTIES kUploadHeapProps = { D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };

	// Specifies the default heap. This heap type experiences the most bandwidth for the GPU, but cannot provide CPU access.
	static const D3D12_HEAP_PROPERTIES kDefaultHeapProps = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };

	inline IDxcBlob* CompileShaderLibrary(LPCWSTR fileName)
	{
		static IDxcCompiler* pCompiler = nullptr;
		static IDxcLibrary* pLibrary = nullptr;
		static IDxcIncludeHandler* dxcIncludeHandler;

		HRESULT hr;

		// Initialize the DXC compiler and compiler nv_helpers_dx12
		if (!pCompiler)
		{
			ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), (void**)&pCompiler));
			ThrowIfFailed(DxcCreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void**)&pLibrary));
			ThrowIfFailed(pLibrary->CreateIncludeHandler(&dxcIncludeHandler));
		}
		// Open and read the file
		std::ifstream shaderFile(fileName);
		if (shaderFile.good() == false)
		{
			throw std::logic_error("Cannot find shader file");
		}
		std::stringstream strStream;
		strStream << shaderFile.rdbuf();
		std::string sShader = strStream.str();

		// Create blob from the string
		IDxcBlobEncoding* pTextBlob;
		ThrowIfFailed(pLibrary->CreateBlobWithEncodingFromPinned(
			(LPBYTE)sShader.c_str(), (uint32_t)sShader.size(), 0, &pTextBlob));

		// Compile
		IDxcOperationResult* pResult;
		ThrowIfFailed(pCompiler->Compile(pTextBlob, fileName, L"", L"lib_6_3", nullptr, 0, nullptr, 0,
			dxcIncludeHandler, &pResult));

		// Verify the result
		HRESULT resultCode;
		ThrowIfFailed(pResult->GetStatus(&resultCode));
		if (FAILED(resultCode))
		{
			IDxcBlobEncoding* pError;
			hr = pResult->GetErrorBuffer(&pError);
			if (FAILED(hr))
			{
				throw std::logic_error("Failed to get shader compiler error");
			}

			// Convert error blob to a string
			std::vector<char> infoLog(pError->GetBufferSize() + 1);
			memcpy(infoLog.data(), pError->GetBufferPointer(), pError->GetBufferSize());
			infoLog[pError->GetBufferSize()] = 0;

			std::string errorMsg = "Shader Compiler Error:\n";
			errorMsg.append(infoLog.data());

			MessageBoxA(nullptr, errorMsg.c_str(), "Error!", MB_OK);
			throw std::logic_error("Failed compile shader");
		}

		IDxcBlob* pBlob;
		ThrowIfFailed(pResult->GetResult(&pBlob));
		return pBlob;
	}

	inline void CopyDataToUploadBuffer(ID3D12Resource* dst, const void* src, UINT size)
	{
		uint8_t* pData;
		ThrowIfFailed(dst->Map(0, nullptr, (void**)&pData));
		memcpy(pData, src, size);
		dst->Unmap(0, nullptr);
	}

	inline ID3D12Resource* CreateDefaultBuffer(
		ID3D12Device* device,ID3D12GraphicsCommandList* cmdList,
		const void* initData,UINT64 byteSize, ComPtr<ID3D12Resource>& uploadBuffer)
	{
		ComPtr<ID3D12Resource> defaultBuffer = CreateBuffer(device, byteSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, kDefaultHeapProps);
		uploadBuffer = CreateBuffer(device, byteSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);

		D3D12_SUBRESOURCE_DATA subResourceData = {};
		subResourceData.pData = initData;
		subResourceData.RowPitch = byteSize;
		subResourceData.SlicePitch = subResourceData.RowPitch;

		CD3DX12_RESOURCE_BARRIER commonToCopy = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(),
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		cmdList->ResourceBarrier(1, &commonToCopy);

		UpdateSubresources<1>(cmdList, defaultBuffer.Get(), uploadBuffer.Get(), 0, 0, 1, &subResourceData);
		//mCommandList->CopyBufferRegion(VertexBufferGPU.Get(), 0, VertexBufferUploader.Get(), 0, vertex_total_bytes);


		CD3DX12_RESOURCE_BARRIER copyToRead = CD3DX12_RESOURCE_BARRIER::Transition(defaultBuffer.Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
		cmdList->ResourceBarrier(1, &copyToRead);

		return defaultBuffer.Get();
	}
}
