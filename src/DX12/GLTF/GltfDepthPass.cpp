// AMD AMDUtils code
// 
// Copyright(c) 2018 Advanced Micro Devices, Inc.All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "stdafx.h"
#include "GLTF/GltfHelpers.h"
#include "Base/ShaderCompilerHelper.h"
#include "Misc/ThreadPool.h"
#include "Misc/Misc.h"

#include "GLTF/GltfPbrMaterial.h"
#include "GltfDepthPass.h"

namespace CAULDRON_DX12
{
    //--------------------------------------------------------------------------------------
    //
    // OnCreate
    //
    //--------------------------------------------------------------------------------------
    void GltfDepthPass::OnCreate(
        Device *pDevice,
        UploadHeap *pUploadHeap,
        ResourceViewHeaps *pHeaps,
        DynamicBufferRing *pDynamicBufferRing,
        StaticBufferPool *pStaticBufferPool,
        GLTFTexturesAndBuffers *pGLTFTexturesAndBuffers,
        AsyncPool *pAsyncPool)
    {
        m_pDevice = pDevice;
        m_pResourceViewHeaps = pHeaps;
        m_pStaticBufferPool = pStaticBufferPool;
        m_pDynamicBufferRing = pDynamicBufferRing;
        m_pGLTFTexturesAndBuffers = pGLTFTexturesAndBuffers;

        const json &j3 = pGLTFTexturesAndBuffers->m_pGLTFCommon->j3;

        /////////////////////////////////////////////
        // Create default material

        m_defaultMaterial.m_textureCount = 0;
        m_defaultMaterial.m_doubleSided = false;

        // Create static sampler in case there is transparency
        //
        ZeroMemory(&m_samplerDesc, sizeof(m_samplerDesc));
        m_samplerDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        m_samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        m_samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        m_samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        m_samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        m_samplerDesc.MinLOD = 0.0f;
        m_samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        m_samplerDesc.MipLODBias = 0;
        m_samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        m_samplerDesc.MaxAnisotropy = 1;
        m_samplerDesc.ShaderRegister = 0;
        m_samplerDesc.RegisterSpace = 0;
        m_samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Create materials (in a depth pass materials are still needed to handle non opaque textures
        //
        if (j3.find("materials") != j3.end())
        {
            const json &materials = j3["materials"];

            m_materialsData.resize(materials.size());
            for (uint32_t i = 0; i < materials.size(); i++)
            {
                const json &material = materials[i];

                DepthMaterial *tfmat = &m_materialsData[i];

                // Load material constants. This is a depth pass and we are only interested in the mask texture
                //               
                tfmat->m_doubleSided = GetElementBoolean(material, "doubleSided", false);
                std::string alphaMode = GetElementString(material, "alphaMode", "OPAQUE");
                tfmat->m_defines["DEF_alphaMode_" + alphaMode] = std::to_string(1);

                // If transparent use the baseColorTexture for alpha
                //
                if (alphaMode == "MASK")
                {
                    tfmat->m_defines["DEF_alphaCutoff"] = std::to_string(GetElementFloat(material, "alphaCutoff", 0.5));

                    auto pbrMetallicRoughnessIt = material.find("pbrMetallicRoughness");
                    if (pbrMetallicRoughnessIt != material.end())
                    {
                        const json &pbrMetallicRoughness = pbrMetallicRoughnessIt.value();

                        int id = GetElementInt(pbrMetallicRoughness, "baseColorTexture/index", -1);
                        if (id >= 0)
                        {
                            tfmat->m_defines["MATERIAL_METALLICROUGHNESS"] = "1";

                            // allocate descriptor table for the texture
                            tfmat->m_textureCount = 1;
                            tfmat->m_pTransparency = new CBV_SRV_UAV();
                            pHeaps->AllocCBV_SRV_UAVDescriptor(tfmat->m_textureCount, tfmat->m_pTransparency);
                            Texture *pTexture = pGLTFTexturesAndBuffers->GetTextureViewByID(id);
                            pTexture->CreateSRV(0, tfmat->m_pTransparency);
                            tfmat->m_defines["ID_baseColorTexture"] = "0";
                            tfmat->m_defines["ID_baseTexCoord"] = std::to_string(GetElementInt(pbrMetallicRoughness, "baseColorTexture/texCoord", 0));
                        }
                    }
                }
            }
        }

        // Load Meshes
        //
        if (j3.find("meshes") != j3.end())
        {
            const json &meshes = j3["meshes"];
            m_meshes.resize(meshes.size());
            for (uint32_t i = 0; i < meshes.size(); i++)
            {
                DepthMesh *tfmesh = &m_meshes[i];

                const json &primitives = meshes[i]["primitives"];
                tfmesh->m_pPrimitives.resize(primitives.size());
                for (uint32_t p = 0; p < primitives.size(); p++)
                {
                    const json &primitive = primitives[p];
                    DepthPrimitives *pPrimitive = &tfmesh->m_pPrimitives[p];

                    ExecAsyncIfThereIsAPool(pAsyncPool, [this, i, &primitive, pPrimitive]()
                    {
                        // Set Material
                        //
                        auto mat = primitive.find("material");
                        pPrimitive->m_pMaterial = (mat != primitive.end()) ? &m_materialsData[mat.value()] : &m_defaultMaterial;

                        // make a list of all the attribute names our pass requires, in the case of a depth pass we only need the position and a few other things. 
                        //
                        std::vector<std::string > requiredAttributes;
                        for (auto const & it : primitive["attributes"].items())
                        {
                            const std::string semanticName = it.key();
                            if (
                                (semanticName == "POSITION") ||
                                (semanticName.substr(0, 7) == "WEIGHTS") || // for skinning
                                (semanticName.substr(0, 6) == "JOINTS") || // for skinning
                                (DoesMaterialUseSemantic(pPrimitive->m_pMaterial->m_defines, semanticName) == true) // if there is transparency this will make sure we use the texture coordinates of that texture
                                )
                            {
                                requiredAttributes.push_back(semanticName);
                            }
                        }

                        // holds all the #defines from materials, geometry and texture IDs, the VS & PS shaders need this to get the bindings and code paths
                        //
                        DefineList defines = pPrimitive->m_pMaterial->m_defines;

                        // create an input layout from the required attributes
                        // shader's can tell the slots from the #defines
                        //
                        std::vector<std::string> semanticNames;
                        std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
                        m_pGLTFTexturesAndBuffers->CreateGeometry(primitive, requiredAttributes, semanticNames, layout, defines, &pPrimitive->m_geometry);

                        // Create Pipeline
                        //
                        bool bUsingSkinning = m_pGLTFTexturesAndBuffers->m_pGLTFCommon->FindMeshSkinId(i) != -1;
                        CreatePipeline(bUsingSkinning, layout, defines, pPrimitive);
                    });
                }
            }
        }
    }

    //--------------------------------------------------------------------------------------
    //
    // OnDestroy
    //
    //--------------------------------------------------------------------------------------
    void GltfDepthPass::OnDestroy()
    {
        for (uint32_t m = 0; m < m_meshes.size(); m++)
        {
            DepthMesh *pMesh = &m_meshes[m];
            for (uint32_t p = 0; p < pMesh->m_pPrimitives.size(); p++)
            {
                DepthPrimitives *pPrimitive = &pMesh->m_pPrimitives[p];
                pPrimitive->m_pipelineRender->Release();
                pPrimitive->m_rootSignature->Release();
            }
        }
    }

    //--------------------------------------------------------------------------------------
    //
    // CreatePipeline
    //
    //--------------------------------------------------------------------------------------
    void GltfDepthPass::CreatePipeline(bool bUsingSkinning, std::vector<D3D12_INPUT_ELEMENT_DESC> layout, DefineList &defines, DepthPrimitives *pPrimitive)
    {
        bool bUsingTransparency = (pPrimitive->m_pMaterial->m_pTransparency != NULL);
      
        /////////////////////////////////////////////
        // Create descriptors 

        {
            CD3DX12_DESCRIPTOR_RANGE DescRange[1];
            CD3DX12_ROOT_PARAMETER RTSlot[5];
            CD3DX12_ROOT_SIGNATURE_DESC descRootSignature = CD3DX12_ROOT_SIGNATURE_DESC();

            if (bUsingTransparency)
            {
                DescRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

                RTSlot[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);                 // b0 <- per frame
                RTSlot[1].InitAsDescriptorTable(1, &DescRange[0], D3D12_SHADER_VISIBILITY_PIXEL);      // t0 <- per material
                RTSlot[2].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);                 // b1 <- per material parameters
                if (bUsingSkinning)
                {
                    RTSlot[3].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
                    defines["ID_SKINNING_MATRICES"] = "2";
                }

                // the root signature contains 3 slots to be used        
                descRootSignature.NumParameters = bUsingSkinning ? 4 : 3;
                descRootSignature.pParameters = RTSlot;
                descRootSignature.NumStaticSamplers = 1;
                descRootSignature.pStaticSamplers = &m_samplerDesc;
            }
            else
            {
                RTSlot[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);  // b0 <- per frame
                RTSlot[1].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);  // b1 <- per material parameters
                if (bUsingSkinning)
                {
                    RTSlot[2].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
                    defines["ID_SKINNING_MATRICES"] = "2";
                }

                // the root signature contains 3 slots to be used        
                descRootSignature.NumParameters = bUsingSkinning ? 3 : 2;
                descRootSignature.pParameters = RTSlot;
                descRootSignature.NumStaticSamplers = 0;
                descRootSignature.pStaticSamplers = NULL;
            }


            // deny uneccessary access to certain pipeline stages   
            descRootSignature.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE
                | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                //| D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
            //| D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

            ID3DBlob *pOutBlob, *pErrorBlob = NULL;
            D3D12SerializeRootSignature(&descRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, &pOutBlob, &pErrorBlob);

            if (pErrorBlob != NULL)
            {
                char *msg = (char *)pErrorBlob->GetBufferPointer();
                MessageBoxA(0, msg, "", 0);
            }

            ThrowIfFailed(
                m_pDevice->GetDevice()->CreateRootSignature(
                    0,
                    pOutBlob->GetBufferPointer(),
                    pOutBlob->GetBufferSize(),
                    IID_PPV_ARGS(&pPrimitive->m_rootSignature))
            );
            SetName(pPrimitive->m_rootSignature, "GltfDepthPass::m_RootSignature");

            pOutBlob->Release();
            if (pErrorBlob)
                pErrorBlob->Release();
        }

        /////////////////////////////////////////////
        // Compile and create shaders

        D3D12_SHADER_BYTECODE shaderVert, shaderPixel;
        CompileShaderFromFile("GLTFDepthPass.hlsl", &defines, "mainVS", "-T vs_6_0", &shaderVert);
        CompileShaderFromFile("GLTFDepthPass.hlsl", &defines, "mainPS", "-T ps_6_0", &shaderPixel);

        /////////////////////////////////////////////
        // Create a Pipeline 

        D3D12_GRAPHICS_PIPELINE_STATE_DESC descPso = {};
        descPso.InputLayout = { layout.data(), (UINT)layout.size() };
        descPso.pRootSignature = pPrimitive->m_rootSignature;
        descPso.VS = shaderVert;
        if (bUsingTransparency)
            descPso.PS = shaderPixel;
        descPso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        descPso.RasterizerState.CullMode = (pPrimitive->m_pMaterial->m_doubleSided) ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_FRONT;
        descPso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        descPso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        descPso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        descPso.SampleMask = UINT_MAX;
        descPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        descPso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        descPso.SampleDesc.Count = 1;
        descPso.NodeMask = 0;
        ThrowIfFailed(
            m_pDevice->GetDevice()->CreateGraphicsPipelineState(&descPso, IID_PPV_ARGS(&pPrimitive->m_pipelineRender))
        );
        SetName(pPrimitive->m_pipelineRender, "GltfDepthPass::m_PipelineRender");
    }

    //--------------------------------------------------------------------------------------
    //
    // SetPerFrameConstants
    //
    //--------------------------------------------------------------------------------------
    GltfDepthPass::per_frame *GltfDepthPass::SetPerFrameConstants()
    {
        GltfDepthPass::per_frame *cbPerFrame;
        m_pDynamicBufferRing->AllocConstantBuffer(sizeof(GltfDepthPass::per_frame), (void **)&cbPerFrame, &m_perFrameDesc);

        return cbPerFrame;
    }

    //--------------------------------------------------------------------------------------
    //
    // Draw
    //
    //--------------------------------------------------------------------------------------
    void GltfDepthPass::Draw(ID3D12GraphicsCommandList* pCommandList)
    {
        UserMarker marker(pCommandList, "DepthPass");

        // Set descriptor heaps
        pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12DescriptorHeap *pDescriptorHeaps[] = { m_pResourceViewHeaps->GetCBV_SRV_UAVHeap(), m_pResourceViewHeaps->GetSamplerHeap() };
        pCommandList->SetDescriptorHeaps(2, pDescriptorHeaps);

        // loop through nodes
        //
        std::vector<tfNode> *pNodes = &m_pGLTFTexturesAndBuffers->m_pGLTFCommon->m_nodes;
        XMMATRIX *pNodesMatrices = m_pGLTFTexturesAndBuffers->m_pGLTFCommon->m_pCurrentFrameTransformedData->m_worldSpaceMats.data();

        for (uint32_t i = 0; i < pNodes->size(); i++)
        {
            tfNode *pNode = &pNodes->at(i);
            if ((pNode == NULL) || (pNode->meshIndex < 0))
                continue;

            // skinning matrices constant buffer
            D3D12_GPU_VIRTUAL_ADDRESS pPerSkeleton = m_pGLTFTexturesAndBuffers->GetSkinningMatricesBuffer(pNode->skinIndex);

            DepthMesh *pMesh = &m_meshes[pNode->meshIndex];
            for (int p = 0; p < pMesh->m_pPrimitives.size(); p++)
            {
                DepthPrimitives *pPrimitive = &pMesh->m_pPrimitives[p];

                if (pPrimitive->m_pipelineRender == NULL)
                    continue;

                // Set per Object constants
                //
                per_object cbPerObject;
                cbPerObject.mWorld = pNodesMatrices[i];
                D3D12_GPU_VIRTUAL_ADDRESS perObjectDesc= m_pDynamicBufferRing->AllocConstantBuffer(sizeof(per_object), &cbPerObject);

                // Bind indices and vertices using the right offsets into the buffer
                //
                Geometry *pGeometry = &pPrimitive->m_geometry;

                pCommandList->IASetIndexBuffer(&pGeometry->m_IBV);
                pCommandList->IASetVertexBuffers(0, (UINT)pGeometry->m_VBV.size(), pGeometry->m_VBV.data());

                // Bind Descriptor sets
                //                
                pCommandList->SetGraphicsRootSignature(pPrimitive->m_rootSignature);

                if (pPrimitive->m_pMaterial->m_pTransparency == NULL)
                {
                    pCommandList->SetGraphicsRootConstantBufferView(0, m_perFrameDesc);
                    pCommandList->SetGraphicsRootConstantBufferView(1, perObjectDesc);
                    if (pPerSkeleton != 0)
                        pCommandList->SetGraphicsRootConstantBufferView(2, pPerSkeleton);
                }
                else
                {
                    pCommandList->SetGraphicsRootConstantBufferView(0, m_perFrameDesc);
                    pCommandList->SetGraphicsRootDescriptorTable(1, pPrimitive->m_pMaterial->m_pTransparency->GetGPU());
                    pCommandList->SetGraphicsRootConstantBufferView(2, perObjectDesc);
                    if (pPerSkeleton != 0)
                        pCommandList->SetGraphicsRootConstantBufferView(3, pPerSkeleton);
                }

                // Bind Pipeline
                //
                pCommandList->SetPipelineState(pPrimitive->m_pipelineRender);

                // Draw
                //
                pCommandList->DrawIndexedInstanced(pGeometry->m_NumIndices, 1, 0, 0, 0);
            }
        }
    }
}
