#include "JsHelpers.h"

#include <sstream>

namespace UsdAgent {

namespace {

const JsValue* _Find(const JsObject& obj, const std::string& key) {
    auto it = obj.find(key);
    return (it == obj.end()) ? nullptr : &it->second;
}

} // namespace

bool JsHasKey(const JsObject& obj, const std::string& key) {
    return obj.find(key) != obj.end();
}

bool JsGetBool(const JsObject& obj, const std::string& key, bool def) {
    const JsValue* v = _Find(obj, key);
    if (!v || !v->IsBool()) return def;
    return v->GetBool();
}

int JsGetInt(const JsObject& obj, const std::string& key, int def) {
    const JsValue* v = _Find(obj, key);
    if (!v) return def;
    if (v->IsInt())  return v->GetInt();
    if (v->IsReal()) return static_cast<int>(v->GetReal());
    return def;
}

double JsGetDouble(const JsObject& obj, const std::string& key, double def) {
    const JsValue* v = _Find(obj, key);
    if (!v) return def;
    if (v->IsReal()) return v->GetReal();
    if (v->IsInt())  return static_cast<double>(v->GetInt64());
    return def;
}

std::string JsGetString(const JsObject& obj, const std::string& key,
                        const std::string& def) {
    const JsValue* v = _Find(obj, key);
    if (!v || !v->IsString()) return def;
    return v->GetString();
}

JsObject JsGetObject(const JsObject& obj, const std::string& key) {
    const JsValue* v = _Find(obj, key);
    if (!v || !v->IsObject()) return JsObject{};
    return v->GetJsObject();
}

JsArray JsGetArray(const JsObject& obj, const std::string& key) {
    const JsValue* v = _Find(obj, key);
    if (!v || !v->IsArray()) return JsArray{};
    return v->GetJsArray();
}

std::string JsToString(const JsValue& value) {
    return JsWriteToString(value);
}

JsObject MakeStringParam(const std::string& description) {
    JsObject p;
    p["type"]        = JsValue(std::string("string"));
    p["description"] = JsValue(description);
    return p;
}

JsObject MakeBoolParam(const std::string& description) {
    JsObject p;
    p["type"]        = JsValue(std::string("boolean"));
    p["description"] = JsValue(description);
    return p;
}

JsObject MakeNumberParam(const std::string& description) {
    JsObject p;
    p["type"]        = JsValue(std::string("number"));
    p["description"] = JsValue(description);
    return p;
}

} // namespace UsdAgent
