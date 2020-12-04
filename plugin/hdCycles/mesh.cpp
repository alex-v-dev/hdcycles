//  Copyright 2020 Tangent Animation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
//  including without limitation, as related to merchantability and fitness
//  for a particular purpose.
//
//  In no event shall any copyright holder be liable for any damages of any kind
//  arising from the use of this software, whether in contract, tort or otherwise.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "mesh.h"

#include "config.h"
#include "instancer.h"
#include "material.h"
#include "renderDelegate.h"
#include "renderParam.h"
#include "utils.h"

#include "Mikktspace/mikktspace.h"

#include <vector>

#include <render/mesh.h>
#include <render/object.h>
#include <render/scene.h>
#include <render/shader.h>
#include <subd/subd_dice.h>
#include <subd/subd_split.h>
#include <util/util_math_float2.h>
#include <util/util_math_float3.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshUtil.h>
#include <pxr/imaging/hd/points.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/smoothNormals.h>
#include <pxr/imaging/pxOsd/tokens.h>

#ifdef USE_USD_CYCLES_SCHEMA
#    include <usdCycles/tokens.h>
#endif

PXR_NAMESPACE_OPEN_SCOPE

// clang-format off
TF_DEFINE_PRIVATE_TOKENS(_tokens, 
    (st)
    (uv)
);
// clang-format on

HdCyclesMesh::HdCyclesMesh(SdfPath const& id, SdfPath const& instancerId,
                           HdCyclesRenderDelegate* a_renderDelegate)
    : HdMesh(id, instancerId)
    , m_renderDelegate(a_renderDelegate)
    , m_cyclesMesh(nullptr)
    , m_cyclesObject(nullptr)
    , m_hasVertexColors(false)
    , m_visibilityFlags(ccl::PATH_RAY_ALL_VISIBILITY)
    , m_visCamera(true)
    , m_visDiffuse(true)
    , m_visGlossy(true)
    , m_visScatter(true)
    , m_visShadow(true)
    , m_visTransmission(true)
    , m_velocityScale(1.0f)
{
    static const HdCyclesConfig& config = HdCyclesConfig::GetInstance();
    config.enable_subdivision.eval(m_subdivEnabled, true);
    config.subdivision_dicing_rate.eval(m_dicingRate, true);
    config.max_subdivision.eval(m_maxSubdivision, true);
    config.enable_motion_blur.eval(m_useMotionBlur, true);

    m_cyclesObject = _CreateCyclesObject();

    m_cyclesMesh = _CreateCyclesMesh();

    m_cyclesObject->geometry = m_cyclesMesh;

    m_renderDelegate->GetCyclesRenderParam()->AddGeometry(m_cyclesMesh);
    m_renderDelegate->GetCyclesRenderParam()->AddObject(m_cyclesObject);
}

HdCyclesMesh::~HdCyclesMesh()
{
    if (m_cyclesMesh) {
        m_renderDelegate->GetCyclesRenderParam()->RemoveMesh(m_cyclesMesh);
        delete m_cyclesMesh;
    }

    if (m_cyclesObject) {
        m_renderDelegate->GetCyclesRenderParam()->RemoveObject(m_cyclesObject);
        delete m_cyclesObject;
    }

    if (m_cyclesInstances.size() > 0) {
        for (auto instance : m_cyclesInstances) {
            if (instance) {
                m_renderDelegate->GetCyclesRenderParam()->RemoveObject(
                    instance);
                delete instance;
            }
        }
    }
}

HdDirtyBits
HdCyclesMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::Clean | HdChangeTracker::DirtyPoints
           | HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyPrimvar
           | HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyVisibility
           | HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtySubdivTags
           | HdChangeTracker::DirtyPrimID | HdChangeTracker::DirtyDisplayStyle
           | HdChangeTracker::DirtyDoubleSided;
}
template<typename T>
bool
HdCyclesMesh::GetPrimvarData(TfToken const& name,
                             HdSceneDelegate* sceneDelegate,
                             std::map<HdInterpolation, HdPrimvarDescriptorVector>
                                 primvarDescsPerInterpolation,
                             VtArray<T>& out_data, VtIntArray& out_indices)
{
    out_data.clear();
    out_indices.clear();

    for (auto& primvarDescsEntry : primvarDescsPerInterpolation) {
        for (auto& pv : primvarDescsEntry.second) {
            if (pv.name == name) {
                auto value = GetPrimvar(sceneDelegate, name);
                if (value.IsHolding<VtArray<T>>()) {
                    out_data = value.UncheckedGet<VtArray<T>>();
                    if (primvarDescsEntry.first == HdInterpolationFaceVarying) {
                        out_indices.reserve(m_faceVertexIndices.size());
                        for (int i = 0; i < m_faceVertexIndices.size(); ++i) {
                            out_indices.push_back(i);
                        }
                    }
                    return true;
                }
                return false;
            }
        }
    }

    return false;
}
template bool
HdCyclesMesh::GetPrimvarData<GfVec2f>(
    TfToken const&, HdSceneDelegate*,
    std::map<HdInterpolation, HdPrimvarDescriptorVector>, VtArray<GfVec2f>&,
    VtIntArray&);
template bool
HdCyclesMesh::GetPrimvarData<GfVec3f>(
    TfToken const&, HdSceneDelegate*,
    std::map<HdInterpolation, HdPrimvarDescriptorVector>, VtArray<GfVec3f>&,
    VtIntArray&);

HdDirtyBits
HdCyclesMesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void
HdCyclesMesh::_InitRepr(TfToken const& reprToken, HdDirtyBits* dirtyBits)
{
}

void
HdCyclesMesh::_ComputeTangents(bool needsign)
{
    // This is likely deprecated now
    ccl::AttributeSet* attributes = (m_useSubdivision)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;

    ccl::Attribute* attr = attributes->find(ccl::ATTR_STD_UV);
    if (attr) {
        mikk_compute_tangents(attr->standard_name(ccl::ATTR_STD_UV),
                              m_cyclesMesh, needsign, true);
    }
}

void
HdCyclesMesh::_AddUVSet(TfToken name, VtVec2fArray& uvs, ccl::Scene* scene,
                        HdInterpolation interpolation)
{
    ccl::AttributeSet* attributes = (m_useSubdivision)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;
    bool subdivide_uvs = false;

    ccl::ustring uv_name      = ccl::ustring(name.GetString());
    ccl::ustring tangent_name = ccl::ustring(name.GetString() + ".tangent");

    bool need_uv = m_cyclesMesh->need_attribute(scene, uv_name)
                   || m_cyclesMesh->need_attribute(scene, ccl::ATTR_STD_UV);
    bool need_tangent
        = m_cyclesMesh->need_attribute(scene, tangent_name)
          || m_cyclesMesh->need_attribute(scene, ccl::ATTR_STD_UV_TANGENT);

    // Forced true for now... Should be based on shader compilation needs
    need_tangent = true;

    ccl::Attribute* attr = attributes->add(ccl::ATTR_STD_UV, uv_name);
    ccl::float2* fdata   = attr->data_float2();

    if (m_useSubdivision && subdivide_uvs)
        attr->flags |= ccl::ATTR_SUBDIVIDED;

    if (interpolation == HdInterpolationVertex) {
        VtIntArray::const_iterator idxIt = m_faceVertexIndices.begin();

        // TODO: Add support for subd faces?
        for (int i = 0; i < m_faceVertexCounts.size(); i++) {
            const int vCount = m_faceVertexCounts[i];

            if (m_useSubdivision) {
                for (int j = 0; j < vCount; ++j) {
                    if (m_orientation == HdTokens->rightHanded) {
                        fdata[0] = vec2f_to_float2(uvs[*(idxIt + j)]);
                    } else {
                        fdata[0] = vec2f_to_float2(
                            uvs[*(idxIt + (vCount - j - 1))]);
                    }
                    fdata++;
                }
            } else {
                for (int j = 1; j < vCount - 1; ++j) {
                    int v0 = *idxIt;
                    int v1 = *(idxIt + j + 0);
                    int v2 = *(idxIt + j + 1);


                    if (m_orientation == HdTokens->leftHanded)
                        v1 = *(idxIt + ((vCount - 1) - j) + 0);
                    if (m_orientation == HdTokens->leftHanded)
                        v2 = *(idxIt + ((vCount - 1) - j) + 1);

                    fdata[0] = vec2f_to_float2(uvs[v0]);
                    fdata[1] = vec2f_to_float2(uvs[v1]);
                    fdata[2] = vec2f_to_float2(uvs[v2]);
                    fdata += 3;
                }
            }
            idxIt += vCount;
        }
    } else if (interpolation == HdInterpolationFaceVarying) {
        if (m_useSubdivision) {
            int idx = 0;
            if (m_orientation == HdTokens->rightHanded) {
                for (int i = m_faceVertexCounts.size(); i > 0; i--) {
                    const int vCount = m_faceVertexCounts[i - 1];

                    for (int j = 0; j < vCount; ++j) {
                        fdata[0] = vec2f_to_float2(uvs[idx + j]);
                        fdata += 1;
                    }
                    idx += vCount;
                }
            } else {
                for (int i = 0; i < m_faceVertexCounts.size(); i++) {
                    const int vCount = m_faceVertexCounts[i];

                    for (int j = vCount; j > 0; --j) {
                        int ii = idx + (j - 1);

                        fdata[0] = vec2f_to_float2(uvs[ii]);
                        fdata += 1;
                    }
                    idx += vCount;
                }
            }
        } else {
            VtIntArray::const_iterator idxIt = m_faceVertexIndices.begin();

            int idx = 0;
            for (int i = 0; i < m_faceVertexCounts.size(); i++) {
                const int vCount = m_faceVertexCounts[i];
                int faceidx      = 0;

                for (int j = 1; j < vCount - 1; ++j) {
                    int iter = 3 * (j - 1);
                    if (m_orientation == HdTokens->leftHanded)
                        iter = (vCount - 1) - (j - 1);
                    iter += idx;
                    int v0 = iter + 0;
                    int v1 = iter + 1;
                    int v2 = iter + 2;

                    // TODO: Currently faceVarying left handed is broken
                    if (m_orientation == HdTokens->leftHanded) {
                        //if((j-1) %  )
                        //v1 = *(idxIt + ((vCount - 1) - j) + 0);

                        //v2 = *(idxIt + ((vCount - 1) - j) + 1);
                    }

                    fdata[0] = vec2f_to_float2(uvs[v0]);
                    fdata[1] = vec2f_to_float2(uvs[v1]);
                    fdata[2] = vec2f_to_float2(uvs[v2]);
                    fdata += 3;

                    faceidx += 3;
                }
                //}
                idx += faceidx;
            }
        }
    }

    if (need_tangent) {
        ccl::ustring sign_name = ccl::ustring(name.GetString()
                                              + ".tangent_sign");
        bool need_sign
            = m_cyclesMesh->need_attribute(scene, sign_name)
              || m_cyclesMesh->need_attribute(scene,
                                              ccl::ATTR_STD_UV_TANGENT_SIGN);


        // Forced for now
        need_sign = true;
        mikk_compute_tangents(name.GetString().c_str(), m_cyclesMesh, need_sign,
                              true);
    }
}

void
HdCyclesMesh::_AddVelocities(VtVec3fArray& velocities,
                             HdInterpolation interpolation)
{
    ccl::AttributeSet* attributes = (m_useSubdivision)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;

    m_cyclesMesh->use_motion_blur = true;
    m_cyclesMesh->motion_steps    = 3;

    ccl::Attribute* attr_mP = attributes->find(
        ccl::ATTR_STD_MOTION_VERTEX_POSITION);

    if (attr_mP)
        attributes->remove(attr_mP);

    if (!attr_mP) {
        attr_mP = attributes->add(ccl::ATTR_STD_MOTION_VERTEX_POSITION);
    }
    //ccl::float3* vdata = attr_mP->data_float3();

    /*if (interpolation == HdInterpolationVertex) {
        VtIntArray::const_iterator idxIt = m_faceVertexIndices.begin();

        // TODO: Add support for subd faces?
        for (int i = 0; i < m_faceVertexCounts.size(); i++) {
            const int vCount = m_faceVertexCounts[i];

            for (int i = 1; i < vCount - 1; ++i) {
                int v0 = *idxIt;
                int v1 = *(idxIt + i + 0);
                int v2 = *(idxIt + i + 1);

                if (m_orientation == HdTokens->leftHanded) {
                    int temp = v2;
                    v2       = v0;
                    v0       = temp;
                }

                vdata[0] = vec3f_to_float3(velocities[v0]);
                vdata[1] = vec3f_to_float3(velocities[v1]);
                vdata[2] = vec3f_to_float3(velocities[v2]);
                vdata += 3;
            }
            idxIt += vCount;
        }
    } else {*/

    ccl::float3* mP = attr_mP->data_float3();

    for (size_t i = 0; i < m_cyclesMesh->motion_steps; ++i) {
        //VtVec3fArray pp;
        //pp = m_pointSamples.values.data()[i].Get<VtVec3fArray>();

        for (size_t j = 0; j < velocities.size(); ++j, ++mP) {
            *mP = vec3f_to_float3(m_points[j]
                                  + (velocities[j] * m_velocityScale));
        }
    }
}

void
HdCyclesMesh::_AddColors(TfToken name, VtVec3fArray& colors, ccl::Scene* scene,
                         HdInterpolation interpolation)
{
    if (colors.size() <= 0)
        return;

    ccl::AttributeSet* attributes = (m_useSubdivision)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;

    ccl::AttributeStandard vcol_std = ccl::ATTR_STD_VERTEX_COLOR;
    ccl::ustring vcol_name          = ccl::ustring(name.GetString());

    const bool need_vcol = m_cyclesMesh->need_attribute(scene, vcol_name)
                           || m_cyclesMesh->need_attribute(scene, vcol_std);

    ccl::Attribute* vcol_attr = NULL;
    vcol_attr                 = attributes->add(vcol_std, vcol_name);

    ccl::uchar4* cdata = vcol_attr->data_uchar4();

    if (interpolation == HdInterpolationVertex) {
        VtIntArray::const_iterator idxIt = m_faceVertexIndices.begin();

        // TODO: Add support for subd faces?
        for (int i = 0; i < m_faceVertexCounts.size(); i++) {
            const int vCount = m_faceVertexCounts[i];

            if (m_useSubdivision) {
                for (int j = 0; j < vCount; ++j) {
                    if (m_orientation == HdTokens->rightHanded) {
                        cdata[0] = ccl::color_float4_to_uchar4(
                            ccl::color_srgb_to_linear_v4(
                                vec3f_to_float4(colors[*(idxIt + j)])));
                    } else {
                        cdata[0] = ccl::color_float4_to_uchar4(
                            ccl::color_srgb_to_linear_v4(vec3f_to_float4(
                                colors[*(idxIt + (vCount - j - 1))])));
                    }
                    cdata++;
                }
            } else {
                for (int j = 1; j < vCount - 1; ++j) {
                    int v0 = *idxIt;
                    int v1 = *(idxIt + j + 0);
                    int v2 = *(idxIt + j + 1);

                    if (m_orientation == HdTokens->leftHanded) {
                        int temp = v2;
                        v2       = v0;
                        v0       = temp;
                    }

                    cdata[0] = ccl::color_float4_to_uchar4(
                        ccl::color_srgb_to_linear_v4(
                            vec3f_to_float4(colors[v0])));
                    cdata[1] = ccl::color_float4_to_uchar4(
                        ccl::color_srgb_to_linear_v4(
                            vec3f_to_float4(colors[v1])));
                    cdata[2] = ccl::color_float4_to_uchar4(
                        ccl::color_srgb_to_linear_v4(
                            vec3f_to_float4(colors[v2])));
                    cdata += 3;
                }
            }
            idxIt += vCount;
        }

    } else if (interpolation == HdInterpolationUniform) {
        if (m_useSubdivision) {
            for (size_t i = 0; i < m_numMeshFaces; i++) {
                GfVec3f pv_col  = colors[i];
                ccl::float4 col = vec3f_to_float4(pv_col);

                cdata[0] = ccl::color_float4_to_uchar4(
                    ccl::color_srgb_to_linear_v4(col));
                cdata += 1;
            }
        } else {
            for (size_t i = 0; i < m_numMeshFaces * 2; i++) {
                GfVec3f pv_col  = colors[floor(i / 2)];
                ccl::float4 col = vec3f_to_float4(pv_col);

                cdata[0] = ccl::color_float4_to_uchar4(
                    ccl::color_srgb_to_linear_v4(col));
                cdata += 1;
            }
        }
    } else if (interpolation == HdInterpolationConstant) {
        if (m_useSubdivision) {
            for (size_t i = 0; i < m_numMeshFaces * 3; i++) {
                GfVec3f pv_col  = colors[0];
                ccl::float4 col = vec3f_to_float4(pv_col);

                cdata[0] = ccl::color_float4_to_uchar4(
                    ccl::color_srgb_to_linear_v4(col));
                cdata += 1;
            }
        } else {
            for (size_t i = 0; i < m_numMeshFaces * 3; i++) {
                GfVec3f pv_col  = colors[0];
                ccl::float4 col = vec3f_to_float4(pv_col);

                cdata[0] = ccl::color_float4_to_uchar4(
                    ccl::color_srgb_to_linear_v4(col));
                cdata += 1;
            }
        }
    } else if (interpolation == HdInterpolationFaceVarying) {
        if (m_useSubdivision) {
            int idx = 0;
            if (m_orientation == HdTokens->rightHanded) {
                for (int i = m_faceVertexCounts.size(); i > 0; i--) {
                    const int vCount = m_faceVertexCounts[i - 1];

                    for (int j = 0; j < vCount; ++j) {
                        GfVec3f pv_col = colors[idx + j];

                        ccl::float4 col = vec3f_to_float4(pv_col);

                        cdata[0] = ccl::color_float4_to_uchar4(
                            ccl::color_srgb_to_linear_v4(col));
                        cdata += 1;
                    }
                    idx += vCount;
                }
            } else {
                for (int i = 0; i < m_faceVertexCounts.size(); i++) {
                    const int vCount = m_faceVertexCounts[i];

                    for (int j = vCount; j > 0; --j) {
                        int ii         = idx + (j - 1);
                        GfVec3f pv_col = colors[ii];

                        ccl::float4 col = vec3f_to_float4(pv_col);

                        cdata[0] = ccl::color_float4_to_uchar4(
                            ccl::color_srgb_to_linear_v4(col));
                        cdata += 1;
                    }
                    idx += vCount;
                }
            }
        } else {
            VtIntArray::const_iterator idxIt = m_faceVertexIndices.begin();

            int idx = 0;
            // Currently faceVarying leftHanded orientation is broken
            for (int i = 0; i < m_faceVertexCounts.size(); i++) {
                const int vCount = m_faceVertexCounts[i];
                int faceidx      = 0;
                for (int j = 1; j < vCount - 1; ++j) {
                    int v0 = idx;
                    int v1 = idx + 1;
                    int v2 = idx + 2;

                    if (m_orientation == HdTokens->leftHanded)
                        v1 = *(idxIt + ((vCount - 1) - j) + 0);
                    if (m_orientation == HdTokens->leftHanded)
                        v2 = *(idxIt + ((vCount - 1) - j) + 1);

                    cdata[0] = ccl::color_float4_to_uchar4(
                        ccl::color_srgb_to_linear_v4(
                            vec3f_to_float4(colors[v0])));
                    cdata[1] = ccl::color_float4_to_uchar4(
                        ccl::color_srgb_to_linear_v4(
                            vec3f_to_float4(colors[v1])));
                    cdata[2] = ccl::color_float4_to_uchar4(
                        ccl::color_srgb_to_linear_v4(
                            vec3f_to_float4(colors[v2])));
                    cdata += 3;

                    idx += 3;
                }
            }
        }
    }
}

void
HdCyclesMesh::_AddNormals(VtVec3fArray& normals, HdInterpolation interpolation)
{
    ccl::AttributeSet* attributes = (m_useSubdivision)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;

    if (interpolation == HdInterpolationUniform) {
        ccl::Attribute* attr_fN = attributes->add(ccl::ATTR_STD_FACE_NORMAL);
        ccl::float3* fN         = attr_fN->data_float3();

        int idx = 0;
        for (int i = 0; i < m_faceVertexCounts.size(); i++) {
            const int vCount = m_faceVertexCounts[i];

            // This needs to be checked
            for (int j = 1; j < vCount - 1; ++idx) {
                fN[idx] = vec3f_to_float3(normals[idx]);
            }
        }

    } else if (interpolation == HdInterpolationVertex) {
        ccl::Attribute* attr = attributes->add(ccl::ATTR_STD_VERTEX_NORMAL);
        ccl::float3* cdata   = attr->data_float3();

        memset(cdata, 0, m_cyclesMesh->verts.size() * sizeof(ccl::float3));

        for (size_t i = 0; i < m_cyclesMesh->verts.size(); i++) {
            ccl::float3 n = vec3f_to_float3(normals[i]);
            if (m_orientation == HdTokens->leftHanded)
                n = -n;
            cdata[i] = n;
        }

    } else if (interpolation == HdInterpolationFaceVarying) {
        //ccl::Attribute* attr = attributes.add(ccl::ATTR_STD_VERTEX_NORMAL);
        //ccl::float3* cdata   = attr->data_float3();

        // TODO: For now, this method produces very wrong results. Some other solution will be needed

        m_cyclesMesh->add_face_normals();
        m_cyclesMesh->add_vertex_normals();

        return;

        //memset(cdata, 0, m_cyclesMesh->verts.size() * sizeof(ccl::float3));

        // Although looping through all faces, normals are averaged per
        // vertex. This seems to be a limitation of cycles. Not allowing
        // face varying/loop_normals/corner_normals natively.

        // For now, we add all corner normals and normalize separately.
        // TODO: Update when Cycles supports corner_normals
        /*for (size_t i = 0; i < m_numMeshFaces; i++) {
            for (size_t j = 0; j < 3; j++) {
                ccl::float3 n = vec3f_to_float3(normals[(i * 3) + j]);
                cdata[m_cyclesMesh->get_triangle(i).v[j]] += n;
            }
        }

        for (size_t i = 0; i < m_cyclesMesh->verts.size(); i++) {
            cdata[i] = ccl::normalize(cdata[i]);
        }*/
    }
}

ccl::Mesh*
HdCyclesMesh::_CreateCyclesMesh()
{
    ccl::Mesh* mesh = new ccl::Mesh();
    mesh->clear();

    if (m_useMotionBlur)
        mesh->use_motion_blur = true;

    m_numMeshVerts = 0;
    m_numMeshFaces = 0;

    mesh->subdivision_type = ccl::Mesh::SUBDIVISION_NONE;
    return mesh;
}

ccl::Object*
HdCyclesMesh::_CreateCyclesObject()
{
    ccl::Object* object = new ccl::Object();

    object->tfm     = ccl::transform_identity();
    object->pass_id = -1;

    object->visibility = ccl::PATH_RAY_ALL_VISIBILITY;

    return object;
}

void
HdCyclesMesh::_PopulateVertices()
{
    m_cyclesMesh->verts.reserve(m_numMeshVerts);
    for (int i = 0; i < m_points.size(); i++) {
        m_cyclesMesh->verts.push_back_reserved(vec3f_to_float3(m_points[i]));
    }
}

void
HdCyclesMesh::_PopulateMotion()
{
    if (m_pointSamples.count <= 1) {
        return;
    }

    ccl::AttributeSet* attributes = (m_useSubdivision)
                                        ? &m_cyclesMesh->subd_attributes
                                        : &m_cyclesMesh->attributes;

    m_cyclesMesh->use_motion_blur = true;

    m_cyclesMesh->motion_steps = m_pointSamples.count + 1;

    ccl::Attribute* attr_mP = attributes->find(
        ccl::ATTR_STD_MOTION_VERTEX_POSITION);

    if (attr_mP)
        attributes->remove(attr_mP);

    if (!attr_mP) {
        attr_mP = attributes->add(ccl::ATTR_STD_MOTION_VERTEX_POSITION);
    }

    ccl::float3* mP = attr_mP->data_float3();
    for (size_t i = 0; i < m_pointSamples.count; ++i) {
        if (m_pointSamples.times.data()[i] == 0.0f)
            continue;

        VtVec3fArray pp;
        pp = m_pointSamples.values.data()[i].Get<VtVec3fArray>();

        for (size_t j = 0; j < m_numMeshVerts; ++j, ++mP) {
            *mP = vec3f_to_float3(pp[j]);
        }
    }
}

void
HdCyclesMesh::_PopulateFaces(const std::vector<int>& a_faceMaterials)
{
    if (m_useSubdivision) {
        m_cyclesMesh->subdivision_type = ccl::Mesh::SUBDIVISION_CATMULL_CLARK;

        // Unknown if this is 100% necessary for subdiv
        m_cyclesMesh->reserve_mesh(m_numMeshVerts, m_numMeshFaces);
        m_cyclesMesh->reserve_subd_faces(m_numMeshFaces, m_numNgons,
                                         m_numCorners);
    } else {
        m_cyclesMesh->reserve_mesh(m_numMeshVerts, m_numMeshFaces);
    }

    VtIntArray::const_iterator idxIt = m_faceVertexIndices.begin();

    if (m_useSubdivision) {
        for (int i = 0; i < m_numMeshFaces; i++) {
            std::vector<int> vi;
            const int vCount = m_faceVertexCounts[i];
            int materialId   = 0;

            if (i < a_faceMaterials.size()) {
                materialId = a_faceMaterials[i];
            }

            vi.resize(vCount);

            for (int j = 0; j < vCount; ++j) {
                if (m_orientation == HdTokens->rightHanded) {
                    vi[j] = *(idxIt + j);
                } else {
                    vi[j] = *(idxIt + (vCount - j - 1));
                }
            }

            m_cyclesMesh->add_subd_face(&vi[0], vCount, materialId, true);

            idxIt += vCount;
        }
    } else {
        for (int i = 0; i < m_faceVertexCounts.size(); i++) {
            const int vCount = m_faceVertexCounts[i];
            int materialId   = 0;

            if (i < a_faceMaterials.size()) {
                materialId = a_faceMaterials[i];
            }

            for (int j = 1; j < vCount - 1; ++j) {
                int v0 = *idxIt;
                int v1 = *(idxIt + j + 0);
                int v2 = *(idxIt + j + 1);

                if (m_orientation == HdTokens->leftHanded)
                    v1 = *(idxIt + ((vCount - 1) - j) + 0);
                if (m_orientation == HdTokens->leftHanded)
                    v2 = *(idxIt + ((vCount - 1) - j) + 1);

                if (v0 < m_numMeshVerts && v1 < m_numMeshVerts
                    && v2 < m_numMeshVerts) {
                    m_cyclesMesh->add_triangle(v0, v1, v2, materialId, true);
                }
            }
            idxIt += vCount;
        }
    }
}

void
HdCyclesMesh::_PopulateCreases()
{
    size_t num_creases = m_creaseLengths.size();

    m_cyclesMesh->subd_creases.resize(num_creases);

    ccl::Mesh::SubdEdgeCrease* crease = m_cyclesMesh->subd_creases.data();
    for (int i = 0; i < num_creases; i++) {
        crease->v[0]   = m_creaseIndices[(i * 2) + 0];
        crease->v[1]   = m_creaseIndices[(i * 2) + 1];
        crease->crease = m_creaseWeights[i];

        crease++;
    }
}

void
HdCyclesMesh::_MeshTextureSpace(ccl::float3& loc, ccl::float3& size)
{
    // m_cyclesMesh->compute_bounds must be called before this
    loc  = (m_cyclesMesh->bounds.max + m_cyclesMesh->bounds.min) / 2.0f;
    size = (m_cyclesMesh->bounds.max - m_cyclesMesh->bounds.min) / 2.0f;

    if (size.x != 0.0f)
        size.x = 0.5f / size.x;
    if (size.y != 0.0f)
        size.y = 0.5f / size.y;
    if (size.z != 0.0f)
        size.z = 0.5f / size.z;

    loc = loc * size - ccl::make_float3(0.5f, 0.5f, 0.5f);
}

void
HdCyclesMesh::_PopulateGenerated(ccl::Scene* scene)
{
    if (m_cyclesMesh->need_attribute(scene, ccl::ATTR_STD_GENERATED)) {
        ccl::AttributeSet* attributes = (m_useSubdivision)
                                            ? &m_cyclesMesh->subd_attributes
                                            : &m_cyclesMesh->attributes;
        ccl::Attribute* attr = attributes->add(ccl::ATTR_STD_GENERATED);

        ccl::float3* generated = attr->data_float3();
        for (int i = 0; i < m_cyclesMesh->verts.size(); i++) {
            generated[i] = m_cyclesMesh->verts[i] * size - loc;
        }
    }
}

void
HdCyclesMesh::_FinishMesh(ccl::Scene* scene)
{
    // Deprecated in favour of adding when uv's are added
    // This should no longer be necessary
    //_ComputeTangents(true);

    // This must be done first, because _MeshTextureSpace requires computed min/max
    m_cyclesMesh->compute_bounds();

    _PopulateGenerated(scene);
}

void
HdCyclesMesh::Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
                   HdDirtyBits* dirtyBits, TfToken const& reprToken)
{
    HdCyclesRenderParam* param = (HdCyclesRenderParam*)renderParam;
    ccl::Scene* scene          = param->GetCyclesScene();


    scene->mutex.lock();

    const SdfPath& id = GetId();

    // -------------------------------------
    // -- Pull scene data

    bool mesh_updated = false;

    bool newMesh = false;

    bool pointsIsComputed = false;

    // TODO: Check if this code is ever executed... Only seems to be for points
    // and removing it seems to work for our tests
    auto extComputationDescs
        = sceneDelegate->GetExtComputationPrimvarDescriptors(
            id, HdInterpolationVertex);
    for (auto& desc : extComputationDescs) {
        if (desc.name != HdTokens->points)
            continue;

        if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, desc.name)) {
            mesh_updated    = true;
            auto valueStore = HdExtComputationUtils::GetComputedPrimvarValues(
                { desc }, sceneDelegate);
            auto pointValueIt = valueStore.find(desc.name);
            if (pointValueIt != valueStore.end()) {
                if (!pointValueIt->second.IsEmpty()) {
                    m_points       = pointValueIt->second.Get<VtVec3fArray>();
                    m_numMeshVerts = m_points.size();

                    m_normalsValid   = false;
                    pointsIsComputed = true;
                    newMesh          = true;
                }
            }
        }
        break;
    }

    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        mesh_updated        = true;
        VtValue pointsValue = sceneDelegate->Get(id, HdTokens->points);
        if (!pointsValue.IsEmpty()) {
            m_points = pointsValue.Get<VtVec3fArray>();
            if (m_points.size() > 0) {
                m_numMeshVerts = m_points.size();

                m_normalsValid = false;
                newMesh        = true;
            }

            // TODO: Should we check if time varying?
            // TODO: can we use this for m_points too?
            sceneDelegate->SamplePrimvar(id, HdTokens->points, &m_pointSamples);
        } /*
        size_t maxSample = 3;

        HdCyclesSampledPrimvarType 4;
        sceneDelegate->SamplePrimvar(id, HdTokens->points, &samples );
        std::cout << "Found time sampled points "<< samples.count << '\n';*/
    }

    static const HdCyclesConfig& config = HdCyclesConfig::GetInstance();

    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        m_topology          = GetMeshTopology(sceneDelegate);
        m_faceVertexCounts  = m_topology.GetFaceVertexCounts();
        m_faceVertexIndices = m_topology.GetFaceVertexIndices();
        m_geomSubsets       = m_topology.GetGeomSubsets();
        m_orientation       = m_topology.GetOrientation();

        m_numMeshFaces = 0;

        m_numNgons   = 0;
        m_numCorners = 0;

        m_adjacencyValid = false;
        m_normalsValid   = false;

        if (m_subdivEnabled) {
            m_useSubdivision = m_topology.GetScheme()
                               == PxOsdOpenSubdivTokens->catmullClark;
        } else {
            m_useSubdivision = false;
        }

        if (m_useSubdivision) {
            m_numMeshFaces = m_faceVertexCounts.size();

            for (int i = 0; i < m_faceVertexCounts.size(); i++) {
                m_numNgons += (m_faceVertexCounts[i] == 4) ? 0 : 1;
                m_numCorners += m_faceVertexCounts[i];
            }

        } else {
            for (int i = 0; i < m_faceVertexCounts.size(); i++) {
                m_numMeshFaces += m_faceVertexCounts[i] - 2;
            }
        }

        newMesh = true;
    }

    std::map<HdInterpolation, HdPrimvarDescriptorVector>
        primvarDescsPerInterpolation = {
            { HdInterpolationFaceVarying, sceneDelegate->GetPrimvarDescriptors(
                                              id, HdInterpolationFaceVarying) },
            { HdInterpolationVertex,
              sceneDelegate->GetPrimvarDescriptors(id, HdInterpolationVertex) },
            { HdInterpolationConstant,
              sceneDelegate->GetPrimvarDescriptors(id,
                                                   HdInterpolationConstant) },
        };

    if (*dirtyBits & HdChangeTracker::DirtyDoubleSided) {
        mesh_updated  = true;
        m_doubleSided = sceneDelegate->GetDoubleSided(id);
    }

    // -------------------------------------
    // -- Resolve Drawstyles

    bool isRefineLevelDirty = false;
    if (*dirtyBits & HdChangeTracker::DirtyDisplayStyle) {
        mesh_updated = true;

        m_displayStyle = sceneDelegate->GetDisplayStyle(id);
        if (m_refineLevel != m_displayStyle.refineLevel) {
            isRefineLevelDirty = true;
            m_refineLevel      = m_displayStyle.refineLevel;
            newMesh            = true;
        }
    }

    if (HdChangeTracker::IsSubdivTagsDirty(*dirtyBits, id)) {
        const PxOsdSubdivTags subdivTags = GetSubdivTags(sceneDelegate);

        m_cornerIndices = subdivTags.GetCornerIndices();
        m_cornerWeights = subdivTags.GetCornerWeights();
        m_creaseIndices = subdivTags.GetCreaseIndices();
        m_creaseLengths = subdivTags.GetCreaseLengths();
        m_creaseWeights = subdivTags.GetCreaseWeights();

        newMesh = true;
    }

#ifdef USE_USD_CYCLES_SCHEMA
    for (auto& primvarDescsEntry : primvarDescsPerInterpolation) {
        for (auto& pv : primvarDescsEntry.second) {
            // Apply custom schema

            m_useMotionBlur = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectMblur, m_useMotionBlur);

            m_motionSteps = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectMblurSteps, m_motionSteps);

            m_cyclesObject->is_shadow_catcher = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectIs_shadow_catcher,
                m_cyclesObject->is_shadow_catcher);

            m_cyclesObject->pass_id = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectPass_id,
                m_cyclesObject->pass_id);

            m_cyclesObject->use_holdout = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectUse_holdout,
                m_cyclesObject->use_holdout);

            // Visibility

            m_visibilityFlags = 0;

            m_visCamera = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityCamera,
                m_visCamera);

            m_visDiffuse = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityDiffuse,
                m_visDiffuse);

            m_visGlossy = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityGlossy,
                m_visGlossy);

            m_visScatter = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityScatter,
                m_visScatter);

            m_visShadow = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityShadow,
                m_visShadow);

            m_visTransmission = _HdCyclesGetMeshParam<bool>(
                pv, dirtyBits, id, this, sceneDelegate,
                usdCyclesTokens->primvarsCyclesObjectVisibilityTransmission,
                m_visTransmission);

            m_visibilityFlags |= m_visCamera ? ccl::PATH_RAY_CAMERA : 0;
            m_visibilityFlags |= m_visDiffuse ? ccl::PATH_RAY_DIFFUSE : 0;
            m_visibilityFlags |= m_visGlossy ? ccl::PATH_RAY_GLOSSY : 0;
            m_visibilityFlags |= m_visScatter ? ccl::PATH_RAY_VOLUME_SCATTER
                                              : 0;
            m_visibilityFlags |= m_visShadow ? ccl::PATH_RAY_SHADOW : 0;
            m_visibilityFlags |= m_visTransmission ? ccl::PATH_RAY_TRANSMIT : 0;
        }
    }
#endif

    // -------------------------------------
    // -- Create Cycles Mesh

    HdMeshUtil meshUtil(&m_topology, id);
    if (newMesh) {
        m_cyclesMesh->clear();

        _PopulateVertices();

        m_cyclesMesh->use_motion_blur = m_useMotionBlur;

        if (m_useMotionBlur)
            _PopulateMotion();

        std::vector<int> faceMaterials;
        faceMaterials.resize(m_numMeshFaces);

        for (auto const& subset : m_geomSubsets) {
            int subsetMaterialIndex = 0;

            if (!subset.materialId.IsEmpty()) {
                const HdCyclesMaterial* subMat
                    = static_cast<const HdCyclesMaterial*>(
                        sceneDelegate->GetRenderIndex().GetSprim(
                            HdPrimTypeTokens->material, subset.materialId));
                if (subMat && subMat->GetCyclesShader()) {
                    if (m_materialMap.find(subset.materialId)
                        == m_materialMap.end()) {
                        m_usedShaders.push_back(subMat->GetCyclesShader());
                        subMat->GetCyclesShader()->tag_update(scene);

                        m_materialMap.insert(
                            std::pair<SdfPath, int>(subset.materialId,
                                                    m_usedShaders.size()));
                        subsetMaterialIndex = m_usedShaders.size();
                    } else {
                        subsetMaterialIndex = m_materialMap.at(
                            subset.materialId);
                    }
                    m_cyclesMesh->used_shaders = m_usedShaders;
                }
            }

            for (int i : subset.indices) {
                faceMaterials[i] = std::max(subsetMaterialIndex - 1, 0);
            }
        }

        _PopulateFaces(faceMaterials);

        if (m_useSubdivision) {
            _PopulateCreases();

            if (!m_cyclesMesh->subd_params) {
                m_cyclesMesh->subd_params = new ccl::SubdParams(m_cyclesMesh);
            }

            ccl::SubdParams& subd_params = *m_cyclesMesh->subd_params;

            subd_params.dicing_rate = m_dicingRate
                                      / ((m_refineLevel + 1) * 2.0f);
            subd_params.max_level = m_maxSubdivision;

            subd_params.objecttoworld = ccl::transform_identity();
        }

        // Get all uvs (assumes all GfVec2f are uvs)
        for (auto& primvarDescsEntry : primvarDescsPerInterpolation) {
            for (auto& pv : primvarDescsEntry.second) {
                if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, pv.name)) {
                    auto value = GetPrimvar(sceneDelegate, pv.name);
                    VtValue triangulated;

                    if (!m_useSubdivision) {
                        if (pv.name == HdTokens->normals) {
                            VtVec3fArray normals;
                            normals = value.UncheckedGet<VtArray<GfVec3f>>();

                            if (m_useSubdivision
                                && primvarDescsEntry.first
                                       == HdInterpolationFaceVarying) {
                                // Triangulate primvar normals
                                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                                    normals.data(), normals.size(),
                                    HdTypeFloatVec3, &triangulated);
                                normals = triangulated.Get<VtVec3fArray>();
                            }

                            _AddNormals(normals, primvarDescsEntry.first);
                        }
                    }

                    // TODO: Properly implement
                    VtValue triangulatedVal;
                    if (pv.name == HdTokens->velocities) {
                        if (value.IsHolding<VtArray<GfVec3f>>()) {
                            VtVec3fArray vels;
                            vels = value.UncheckedGet<VtArray<GfVec3f>>();

                            /*meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                            vels.data(), vels.size(), HdTypeFloatVec3,
                            &triangulatedVal);
                        VtVec3fArray m_vels_tri
                            = triangulatedVal.Get<VtVec3fArray>();
                        _AddVelocities(m_vels_tri, primvarDescsEntry.first);*/


                            if (primvarDescsEntry.first
                                == HdInterpolationFaceVarying) {
                                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                                    vels.data(), vels.size(), HdTypeFloatVec3,
                                    &triangulated);

                                VtVec3fArray triangulatedVels
                                    = triangulated.Get<VtVec3fArray>();

                                //_AddVelocities(triangulatedVels,
                                //               primvarDescsEntry.first);
                            } else {
                                //_AddVelocities(vels, primvarDescsEntry.first);
                            }
                        }
                    }

                    if (pv.role == HdPrimvarRoleTokens->color) {
                        m_hasVertexColors = true;

                        if (value.IsHolding<VtArray<GfVec3f>>()) {
                            // Get primvar colors
                            VtVec3fArray colors;
                            colors = value.UncheckedGet<VtArray<GfVec3f>>();

                            // Only triangulate if not subdivision and faceVarying
                            if ((!m_useSubdivision)) {
                                if (primvarDescsEntry.first
                                    == HdInterpolationFaceVarying) {
                                    // Triangulate primvar colors
                                    meshUtil
                                        .ComputeTriangulatedFaceVaryingPrimvar(
                                            colors.data(), colors.size(),
                                            HdTypeFloatVec3, &triangulated);
                                    colors = triangulated.Get<VtVec3fArray>();
                                }
                            }

                            // Add colors to attribute
                            _AddColors(pv.name, colors, scene,
                                       primvarDescsEntry.first);
                        }
                    }

                    // TODO: Add more general uv support
                    //if (pv.role == HdPrimvarRoleTokens->textureCoordinate) {
                    if (value.IsHolding<VtArray<GfVec2f>>()) {
                        VtVec2fArray uvs
                            = value.UncheckedGet<VtArray<GfVec2f>>();

                        // Only triangulate if not subdivision and faceVarying
                        if ((!m_useSubdivision)) {
                            if (primvarDescsEntry.first
                                == HdInterpolationFaceVarying) {
                                // Triangulate primvar uvs
                                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                                    uvs.data(), uvs.size(), HdTypeFloatVec2,
                                    &triangulated);

                                uvs = triangulated.Get<VtVec2fArray>();
                            }
                        }
                        _AddUVSet(pv.name, uvs, scene, primvarDescsEntry.first);
                    }
                }
            }
        }

        // Apply existing shaders
        if (m_usedShaders.size() > 0)
            m_cyclesMesh->used_shaders = m_usedShaders;
    }

    if (*dirtyBits & HdChangeTracker::DirtyTransform) {
        // Something in this is the culprit for excessive deform motion blur render times
        m_transformSamples = HdCyclesSetTransform(m_cyclesObject, sceneDelegate,
                                                   id, m_useMotionBlur);

        if (m_cyclesMesh && m_cyclesMesh->subd_params) {
            m_cyclesMesh->subd_params->objecttoworld = m_cyclesObject->tfm;
        }

        mesh_updated = true;
    }

    ccl::Shader* fallbackShader = scene->default_surface;

    if (m_hasVertexColors) {
        fallbackShader = param->default_vcol_surface;
    }

    if (*dirtyBits & HdChangeTracker::DirtyPrimID) {
        // Offset of 1 added because Cycles primId pass needs to be shifted down to -1
        m_cyclesObject->pass_id = this->GetPrimId() + 1;
    }

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        if (m_cyclesMesh) {
            m_cachedMaterialId = sceneDelegate->GetMaterialId(id);
            if (m_faceVertexCounts.size() > 0) {
                if (!m_cachedMaterialId.IsEmpty()) {
                    const HdCyclesMaterial* material
                        = static_cast<const HdCyclesMaterial*>(
                            sceneDelegate->GetRenderIndex().GetSprim(
                                HdPrimTypeTokens->material, m_cachedMaterialId));

                    if (material && material->GetCyclesShader()) {
                        m_usedShaders.push_back(material->GetCyclesShader());

                        material->GetCyclesShader()->tag_update(scene);
                    } else {
                        m_usedShaders.push_back(fallbackShader);
                    }
                } else {
                    m_usedShaders.push_back(fallbackShader);
                }

                m_cyclesMesh->used_shaders = m_usedShaders;
            }
        }
    }

    if (*dirtyBits & HdChangeTracker::DirtyVisibility) {
        mesh_updated        = true;
        _sharedData.visible = sceneDelegate->GetVisible(id);
        if (!_sharedData.visible) {
            m_visibilityFlags = 0;  //~ccl::PATH_RAY_ALL_VISIBILITY;
        }
    }

    // -------------------------------------
    // -- Handle point instances

    if (newMesh || (*dirtyBits & HdChangeTracker::DirtyInstancer)) {
        mesh_updated = true;
        if (auto instancer = static_cast<HdCyclesInstancer*>(
                sceneDelegate->GetRenderIndex().GetInstancer(
                    GetInstancerId()))) {
            auto instanceTransforms = instancer->SampleInstanceTransforms(id);
            auto newNumInstances    = (instanceTransforms.count > 0)
                                       ? instanceTransforms.values[0].size()
                                       : 0;
            // Clear all instances...
            if (m_cyclesInstances.size() > 0) {
                for (auto instance : m_cyclesInstances) {
                    if (instance) {
                        m_renderDelegate->GetCyclesRenderParam()->RemoveObject(
                            instance);
                        delete instance;
                    }
                }
                m_cyclesInstances.clear();
            }

            if (newNumInstances != 0) {
                std::vector<TfSmallVector<GfMatrix4d, 1>> combinedTransforms;
                combinedTransforms.reserve(newNumInstances);
                for (size_t i = 0; i < newNumInstances; ++i) {
                    // Apply prototype transform (m_transformSamples) to all the instances
                    combinedTransforms.emplace_back(instanceTransforms.count);
                    auto& instanceTransform = combinedTransforms.back();

                    if (m_transformSamples.count == 0
                        || (m_transformSamples.count == 1
                            && (m_transformSamples.values[0]
                                == GfMatrix4d(1)))) {
                        for (size_t j = 0; j < instanceTransforms.count; ++j) {
                            instanceTransform[j]
                                = instanceTransforms.values[j][i];
                        }
                    } else {
                        for (size_t j = 0; j < instanceTransforms.count; ++j) {
                            GfMatrix4d xf_j = m_transformSamples.Resample(
                                instanceTransforms.times[j]);
                            instanceTransform[j]
                                = xf_j * instanceTransforms.values[j][i];
                        }
                    }
                }

                for (int j = 0; j < newNumInstances; ++j) {
                    ccl::Object* instanceObj = _CreateCyclesObject();

                    instanceObj->tfm = mat4d_to_transform(
                        combinedTransforms[j].data()[0]);
                    instanceObj->geometry = m_cyclesMesh;

                    // TODO: Implement motion blur for point instanced objects
                    /*if (m_useMotionBlur) {
                        m_cyclesMesh->motion_steps    = m_motionSteps;
                        m_cyclesMesh->use_motion_blur = m_useMotionBlur;

                        instanceObj->motion.clear();
                        instanceObj->motion.resize(m_motionSteps);
                        for (int j = 0; j < m_motionSteps; j++) {
                            instanceObj->motion[j] = mat4d_to_transform(
                                combinedTransforms[j].data()[j]);
                        }
                    }*/

                    m_cyclesInstances.push_back(instanceObj);

                    m_renderDelegate->GetCyclesRenderParam()->AddObject(
                        instanceObj);
                }

                // Hide prototype
                if (m_cyclesObject)
                    m_visibilityFlags = 0;
            }
        }
    }

    // -------------------------------------
    // -- Finish Mesh

    if (newMesh && m_cyclesMesh) {
        _FinishMesh(scene);
    }

    if (mesh_updated || newMesh) {
        m_cyclesObject->visibility = m_visibilityFlags;
        m_cyclesMesh->tag_update(scene, true);
        m_cyclesObject->tag_update(scene);
        param->Interrupt();
    }

    scene->mutex.unlock();

    *dirtyBits = HdChangeTracker::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
