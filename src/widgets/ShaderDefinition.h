#pragma once
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>

PXR_NAMESPACE_USING_DIRECTIVE

/// Helpers around the shader definitions (Sdr) of UsdShade shaders, shared by the connection and prim editors.
/// They hide the differences between the USD versions.

/// Returns the shader definition of a UsdShade shader prim, looked up from its id, or its sourceAsset/sourceCode
/// implementations. Returns nullptr if the prim is not a shader or the definition is not found.
SdrShaderNodeConstPtr GetShaderDefinition(const UsdPrim &prim);

/// Names of the inputs and outputs, without the "inputs:" and "outputs:" namespace, in the definition order
const std::vector<TfToken> &GetShaderDefinitionInputNames(SdrShaderNodeConstPtr shaderNode);
const std::vector<TfToken> &GetShaderDefinitionOutputNames(SdrShaderNodeConstPtr shaderNode);

/// Name and value of an option of a shader property
using ShaderOption = std::pair<TfToken, TfToken>;

/// Returns true if the definition asks for this page to be open by default. Always false before USD 25.11.
bool IsShaderDefinitionPageOpen(SdrShaderNodeConstPtr shaderNode, const TfToken &page);

/// Type of the attribute holding the shader property
SdfValueTypeName GetShaderPropertySdfType(SdrShaderPropertyConstPtr property);
