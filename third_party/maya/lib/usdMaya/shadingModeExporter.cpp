//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "usdMaya/shadingModeExporter.h"

#include "usdMaya/util.h"

#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usdGeom/scope.h"
#include "pxr/usd/usdShade/look.h"
#include "pxr/usd/usdShade/parameter.h"
#include "pxr/usd/usdShade/shader.h"

#include <maya/MDagPath.h>
#include <maya/MFnDagNode.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MNamespace.h>
#include <maya/MObjectArray.h>
#include <maya/MString.h>


PxrUsdMayaShadingModeExportContext::PxrUsdMayaShadingModeExportContext(
        const MObject& shadingEngine,
        const UsdStageRefPtr& stage,
        bool mergeTransformAndShape,
        const PxrUsdMayaUtil::ShapeSet& bindableRoots,
        SdfPath overrideRootPath) : 
    _shadingEngine(shadingEngine),
    _stage(stage),
    _mergeTransformAndShape(mergeTransformAndShape),
    _overrideRootPath(overrideRootPath)
{
    if (bindableRoots.empty()) {
        // if none specified, push back '/' which encompasses all
        _bindableRoots.insert(SdfPath::AbsoluteRootPath());
    }
    else {
        TF_FOR_ALL(bindableRootIter, bindableRoots) {
            const MDagPath& bindableRootDagPath = *bindableRootIter;


            SdfPath usdPath = PxrUsdMayaUtil::MDagPathToUsdPath(bindableRootDagPath, 
                _mergeTransformAndShape);

            // If _overrideRootPath is not empty, replace the root namespace with it
            if (!_overrideRootPath.IsEmpty() ) {
                usdPath = usdPath.ReplacePrefix(usdPath.GetPrefixes()[0], _overrideRootPath);
            }

            _bindableRoots.insert(usdPath);
        }
    }
}

MObject
PxrUsdMayaShadingModeExportContext::GetSurfaceShader() const
{
    MStatus status;
    MFnDependencyNode seDepNode(_shadingEngine, &status);
    if (not status) {
        return MObject();
    }

    MPlug ssPlug = seDepNode.findPlug("surfaceShader", true, &status);
    if (not status) {
        return MObject();
    }

    MObject ss(ssPlug.asMObject());
    if (ss.isNull()) {
        return MObject();
    }

    return PxrUsdMayaUtil::GetConnected(ssPlug).node();
}

PxrUsdMayaShadingModeExportContext::AssignmentVector
PxrUsdMayaShadingModeExportContext::GetAssignments() const
{
    AssignmentVector ret;

    MStatus status;
    MFnDependencyNode seDepNode(_shadingEngine, &status);
    if (not status) {
        return ret;
    }

    MPlug dsmPlug = seDepNode.findPlug("dagSetMembers", true, &status);
    if (not status) {
        return ret;
    }

    SdfPathSet seenBoundPrimPaths;
    for (unsigned int i = 0; i < dsmPlug.numConnectedElements(); i++) {
        MPlug dsmElemPlug(dsmPlug.connectionByPhysicalIndex(i));
        MStatus status = MS::kFailure;
        MFnDagNode dagNode(PxrUsdMayaUtil::GetConnected(dsmElemPlug).node(), &status);
        if (not status) {
            continue;
        }

        MDagPath dagPath;
        if (not dagNode.getPath(dagPath))
            continue;

        SdfPath usdPath = PxrUsdMayaUtil::MDagPathToUsdPath(dagPath, 
            _mergeTransformAndShape);

        // If _overrideRootPath is not empty, replace the root namespace with it
        if (!_overrideRootPath.IsEmpty() ) {
            usdPath = usdPath.ReplacePrefix(usdPath.GetPrefixes()[0], _overrideRootPath);
        }
        
        // If this path has already been processed, skip it.
        if (not seenBoundPrimPaths.insert(usdPath).second)
            continue;

        // If the bound prim's path is not below a bindable root, skip it.
        if (SdfPathFindLongestPrefix(_bindableRoots.begin(), 
            _bindableRoots.end(), usdPath) == _bindableRoots.end()) {
            continue;
        }

        MObjectArray sgObjs, compObjs;
        // Assuming that instancing is not involved.
        status = dagNode.getConnectedSetsAndMembers(0, sgObjs, compObjs, true);
        if (not status)
            continue;

        for (size_t j = 0; j < sgObjs.length(); j++) {
            // If the shading group isn't the one we're interested in, skip it.
            if (sgObjs[j] != _shadingEngine)
                continue;

            VtIntArray faceIndices;
            if (not compObjs[j].isNull()) {
                MItMeshPolygon faceIt(dagPath, compObjs[j]);
                faceIndices.reserve(faceIt.count());
                for ( faceIt.reset() ; !faceIt.isDone() ; faceIt.next() ) {
                    faceIndices.push_back(faceIt.index());
                }
            }
            ret.push_back(std::make_pair(usdPath, faceIndices));
        }
    }
    return ret;
}

static UsdPrim
_GetLookParent(const UsdStageRefPtr& stage,
               const PxrUsdMayaShadingModeExportContext::AssignmentVector& assignments)
{
    SdfPath commonAncestor;
    TF_FOR_ALL(iter, assignments) {
        const SdfPath& assn = iter->first;
        if (stage->GetPrimAtPath(assn)) {
            if (commonAncestor.IsEmpty()) {
                commonAncestor = assn;
            }
            else {
                commonAncestor = commonAncestor.GetCommonPrefix(assn);
            }
        }
    }

    if (commonAncestor.IsEmpty()) {
        return UsdPrim();
    }

    if (commonAncestor == SdfPath::AbsoluteRootPath()) {
        return stage->GetPseudoRoot();
    }

    SdfPath shaderExportLocation = commonAncestor;
    while (not shaderExportLocation.IsRootPrimPath()) {
        shaderExportLocation = shaderExportLocation.GetParentPath();
    }
    shaderExportLocation = shaderExportLocation.AppendChild(TfToken("Looks"));

    return UsdGeomScope::Define(stage, shaderExportLocation).GetPrim();
}

UsdPrim
PxrUsdMayaShadingModeExportContext::MakeStandardLookPrim(
        const AssignmentVector& assignmentsToBind,
        const std::string& name) const
{
    UsdPrim ret;

    std::string lookName = name;
    if (lookName.empty()) {
        MStatus status;
        MFnDependencyNode seDepNode(_shadingEngine, &status);
        if (not status) {
            return ret;
        }
        MString seName = seDepNode.name();
        lookName = MNamespace::stripNamespaceFromName(seName).asChar();
    }

    lookName = PxrUsdMayaUtil::SanitizeName(lookName);
    UsdStageRefPtr stage = GetUsdStage();
    if (UsdPrim lookParent = _GetLookParent(stage, assignmentsToBind)) {
        SdfPath lookPath = lookParent.GetPath().AppendChild(TfToken(lookName));
        UsdShadeLook look = UsdShadeLook::Define(GetUsdStage(), lookPath);

        UsdPrim lookPrim = look.GetPrim();

        // could use this to determine where we want to export.  whatever.
        TF_FOR_ALL(iter, assignmentsToBind) {
            const SdfPath &boundPrimPath = iter->first;
            const VtIntArray &faceIndices = iter->second;

            UsdPrim boundPrim = stage->OverridePrim(boundPrimPath);
            if (faceIndices.empty()) {
                look.Bind(boundPrim);
            } else {
                UsdGeomFaceSetAPI faceSet = look.CreateLookFaceSet(boundPrim);
                faceSet.AppendFaceGroup(faceIndices, lookPath);
            }
        }

        return lookPrim;
    }

    return UsdPrim();
}

std::string
PxrUsdMayaShadingModeExportContext::GetStandardAttrName(const MPlug& attrPlug) const
{
    MString mayaPlgName = attrPlug.partialName(false, false, false, false, false, true);
    return mayaPlgName.asChar();
}
