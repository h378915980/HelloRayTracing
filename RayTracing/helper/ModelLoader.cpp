#include "stdafx.h"
#include "core/D3DUtility.h"
#include "helper/DXSampleHelper.h"
#include <iostream>
#include "ModelLoader.h"
#include "TextureLoader.h"

ModelLoader::ModelLoader(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,TextureLoader* textureLoader)
	: m_device ( device),m_cmdList (cmdList),m_textureLoader(textureLoader)
{
}

bool ModelLoader::Load(std::string filename, Model& model, unsigned int loadFlag)
{
	Assimp::Importer m_importer;
	const aiScene* pScene = m_importer.ReadFile(filename, loadFlag);
	
	if (!pScene || pScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !pScene->mRootNode) {
		std::cout << "ERROR::ASSIMP:: " << m_importer.GetErrorString() << std::endl;
		return false;
	}


	model.Directory = filename.substr(0, filename.find_last_of('/'));
	m_modelDic = model.Directory;

	m_indexInTextureLoader = 0;

	bool result;
	result = ProcessNode(pScene->mRootNode, pScene, model);

	return result;
}



bool ModelLoader::ProcessNode(aiNode* ai_node, const aiScene* ai_scene, Model& model)
{
	for (UINT i = 0; i < ai_node->mNumMeshes; i++){
		aiMesh* ai_mesh = ai_scene->mMeshes[ai_node->mMeshes[i]];
		ProcessMesh(ai_mesh, ai_scene, model);
	}

	for (UINT i = 0; i < ai_node->mNumChildren; i++){
		ProcessNode(ai_node->mChildren[i], ai_scene, model);
	}

	return true;
}

bool ModelLoader::ProcessMesh(aiMesh* ai_mesh, const aiScene* ai_scene, Model& model)
{
	// Data to fill
	std::vector<Vertex_Model> vertices;
	std::vector<UINT> indices;
	//std::vector<Texture> textures;

	// Walk through each of the mesh's vertices
	for (UINT i = 0; i < ai_mesh->mNumVertices; i++)
	{
		Vertex_Model vertex;

		vertex.Position.x = ai_mesh->mVertices[i].x ;
		vertex.Position.y = ai_mesh->mVertices[i].y ;
		vertex.Position.z = ai_mesh->mVertices[i].z ;

		if (ai_mesh->HasNormals()){
			vertex.Normal.x = ai_mesh->mNormals[i].x;
			vertex.Normal.y = ai_mesh->mNormals[i].y;
			vertex.Normal.z = ai_mesh->mNormals[i].z;
		}

		if (ai_mesh->HasTextureCoords(0))
		{
			vertex.TexCoord.x = (float)ai_mesh->mTextureCoords[0][i].x;
			vertex.TexCoord.y = (float)ai_mesh->mTextureCoords[0][i].y;
		}

		vertices.push_back(vertex);
	}

	//index
	for (UINT i = 0; i < ai_mesh->mNumFaces; i++){
		aiFace ai_face = ai_mesh->mFaces[i];

		for (UINT j = 0; j < ai_face.mNumIndices; j++)
			indices.push_back(ai_face.mIndices[j]);
	}


	std::unique_ptr<Mesh> mesh = std::make_unique<Mesh>();;
	const UINT vertexBufferSize = sizeof(Vertex_Model) * vertices.size();
	mesh->VertexBufferByteSize = vertexBufferSize;
	mesh->VertexByteStride = sizeof(Vertex_Model);
	mesh->VertexCount = vertices.size();
	mesh->VertexBufferGPU = helper::CreateDefaultBuffer(m_device, m_cmdList, vertices.data(), vertexBufferSize, mesh->VertexBufferUploader);

	const UINT indexBufferSize = static_cast<UINT>(indices.size()) * sizeof(UINT32);
	mesh->IndexBufferByteSize = indexBufferSize;
	mesh->IndexCount = indices.size();
	mesh->IndexBufferGPU = helper::CreateDefaultBuffer(m_device, m_cmdList, indices.data(), indexBufferSize, mesh->IndexBufferUploader);

	std::vector<UINT> indexInModelTextures{};

	if (ai_mesh->mMaterialIndex >= 0)
	{
		aiMaterial* ai_material = ai_scene->mMaterials[ai_mesh->mMaterialIndex];

		//if (m_textureType.empty())
		//	m_textureType = DetermineTextureType(ai_scene, ai_material);

		std::vector<std::shared_ptr<Texture>> diffuseMaps;
		LoadMaterialTextures(ai_material,aiTextureType_DIFFUSE, "texture_diffuse", ai_scene, diffuseMaps);
		model.Textures.insert(model.Textures.end(),diffuseMaps.begin(),diffuseMaps.end());
		for (int i = 0; i < diffuseMaps.size(); i++) {
			indexInModelTextures.push_back(m_indexInTextureLoader++);
		}

		std::vector<std::shared_ptr<Texture>> specMaps;
		LoadMaterialTextures(ai_material, aiTextureType_SPECULAR, "texture_specular", ai_scene, specMaps);
		model.Textures.insert(model.Textures.end(), specMaps.begin(), specMaps.end());
		for (int i = 0; i < specMaps.size(); i++) {
			indexInModelTextures.push_back(m_indexInTextureLoader++);
		}

	}
	//model.Meshes[std::move(mesh)] = indexInModelTextures;
	model.Meshes.push_back({ std::move(mesh),indexInModelTextures });

	return true;
}

bool ModelLoader::LoadMaterialTextures(aiMaterial* ai_mat, aiTextureType ai_texType, 
	std::string typeName, const aiScene* ai_scene, std::vector<std::shared_ptr<Texture>>& textures)
{
	bool result = false;
	std::vector<std::shared_ptr<Texture>>& textureLoaded = m_textureLoader->GetTextureLoaded();
	for (UINT i = 0; i < ai_mat->GetTextureCount(ai_texType); i++)
	{
		aiString str;
		ai_mat->GetTexture(ai_texType, i, &str);
		// Check if texture was loaded before and if so, continue to next iteration: skip loading a new texture
		bool skip = false;

		for (UINT j = 0; j < textureLoaded.size(); j++)
		{
			std::string tempstr = m_modelDic + "/" + std::string(str.C_Str());
			if (std::strcmp(textureLoaded[j]->FileName.c_str(), tempstr.c_str()) == 0)
			{
				textures.push_back(textureLoaded[j]);
				skip = true; // A texture with the same filepath has already been loaded, continue to next one. (optimization)
				break;
			}
		}

		if (!skip)
		{   // If texture hasn't been loaded already, load it
			std::shared_ptr<Texture> texture = std::make_shared<Texture>();
			texture->Type = typeName;

			std::string filename = std::string(str.C_Str());
			filename = m_modelDic + "/" + filename;

			result = m_textureLoader->Load(filename,texture);
			if (!result){
				return false;
			}
			textures.push_back(texture);
		}
	}
	return true;
}

std::string ModelLoader::DetermineTextureType(const aiScene* ai_scene, aiMaterial* ai_mat)
{
	aiString textypeStr;
	ai_mat->GetTexture(aiTextureType_DIFFUSE, 0, &textypeStr);
	std::string textypeteststr = textypeStr.C_Str();
	if (textypeteststr == "*0" || textypeteststr == "*1" || textypeteststr == "*2" || textypeteststr == "*3" || textypeteststr == "*4" || textypeteststr == "*5")
	{
		if (ai_scene->mTextures[0]->mHeight == 0)
		{
			return "embedded compressed texture";
		}
		else
		{
			return "embedded non-compressed texture";
		}
	}
	if (textypeteststr.find('.') != std::string::npos)
	{
		return "textures are on disk";
	}

	return "null";
}
