#include <iostream>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/gprim.h>
#include "Gui.h"
#include "PropertyEditor.h"
#include "ValueEditor.h"


PXR_NAMESPACE_USING_DIRECTIVE

void DrawUsdAttribute(UsdAttribute &attribute, UsdTimeCode currentTime) {
    auto attributeTypeName = attribute.GetTypeName();
    ImGui::Text("[%s]", attribute.IsAuthored() ? "*" : " ");
    ImGui::SameLine();
    VtValue value;
    if (attribute.Get(&value, currentTime)) {
        VtValue modified = DrawVtValue(attribute.GetBaseName().GetString(), value);
        if (!modified.IsEmpty()) {
            // TODO: DispatchCommand DispatchCommand<AttributeSet>(attribute, modified);
            attribute.Set(modified, currentTime);
        }
    } else {
        ImGui::Text("'%s': no value found", attribute.GetBaseName().GetString().c_str());
    }
}

void DrawUsdPrimProperties(UsdPrim &prim, UsdTimeCode currentTime) {
    if (prim) {
        ImGui::Text("%s", prim.GetPrimPath().GetString().c_str());

        // THIS IS A TEST to understand how to add new attributes
        UsdGeomXformable xform(prim);
        if (xform) {
            if (ImGui::Button("Add translate (test)")) {
                // TODO: test add a xformdd
                // TODO: suffix ???
                auto translateOp = xform.AddTranslateOp();
                GfVec3d origin(0.0, 0.0, 0.0);
                translateOp.Set(origin);
            }
        }
        auto attributes = prim.GetAttributes();
        for (auto &att : attributes) {
            DrawUsdAttribute(att, currentTime);
        }
    }
}
