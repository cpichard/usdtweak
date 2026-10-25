#include "ShaderDefinition.h"
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/sdr/registry.h>

SdrShaderNodeConstPtr GetShaderDefinition(const UsdPrim &prim) {
    UsdShadeShader shader(prim);
    if (!shader)
        return nullptr;
    TfToken shaderId;
    if (shader.GetShaderId(&shaderId)) {
        if (SdrShaderNodeConstPtr shaderNode = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(shaderId))
            return shaderNode;
    }
    for (const std::string &sourceType : shader.GetSourceTypes()) { // sourceAsset or sourceCode implementations
        if (SdrShaderNodeConstPtr shaderNode = shader.GetShaderNodeForSourceType(TfToken(sourceType)))
            return shaderNode;
    }
    return nullptr;
}

const std::vector<TfToken> &GetShaderDefinitionInputNames(SdrShaderNodeConstPtr shaderNode) {
#if PXR_VERSION >= 2505
    return shaderNode->GetShaderInputNames();
#else
    return shaderNode->GetInputNames();
#endif
}

const std::vector<TfToken> &GetShaderDefinitionOutputNames(SdrShaderNodeConstPtr shaderNode) {
#if PXR_VERSION >= 2505
    return shaderNode->GetShaderOutputNames();
#else
    return shaderNode->GetOutputNames();
#endif
}

SdfValueTypeName GetShaderPropertySdfType(SdrShaderPropertyConstPtr property) {
#if PXR_VERSION >= 2405 // the type indicator was a std::pair before
    return property->GetTypeAsSdfType().GetSdfType();
#else
    return property->GetTypeAsSdfType().first;
#endif
}
