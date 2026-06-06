#pragma once

#include <pxr/base/js/json.h>
#include <pxr/base/js/types.h>
#include <pxr/base/js/value.h>
#include <pxr/pxr.h>

#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace UsdAgent {

bool        JsGetBool  (const JsObject& obj, const std::string& key, bool        def = false);
int         JsGetInt   (const JsObject& obj, const std::string& key, int         def = 0);
double      JsGetDouble(const JsObject& obj, const std::string& key, double      def = 0.0);
std::string JsGetString(const JsObject& obj, const std::string& key,
                        const std::string& def = "");
JsObject    JsGetObject(const JsObject& obj, const std::string& key);
JsArray     JsGetArray (const JsObject& obj, const std::string& key);

bool JsHasKey(const JsObject& obj, const std::string& key);

std::string JsToString(const JsValue& value);

JsObject MakeStringParam(const std::string& description);
JsObject MakeBoolParam  (const std::string& description);
JsObject MakeNumberParam(const std::string& description);

} // namespace UsdAgent
