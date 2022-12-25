#include "VtDictionaryEditor.h"
#include "Commands.h"
#include "Gui.h"
#include "VtValueEditor.h"
#include <string>

#include <pxr/base/vt/dictionary.h>

#include "Constants.h"
#include "ImGuiHelpers.h"
#include "ModalDialogs.h"
#include "TableLayouts.h"
#include "VtValueEditor.h"


static VtValue DrawEditableKeyName(const std::string &keyNameSrc, const std::string &typeName, int depth) {
    constexpr float indentSize = 20; // TODO move in Constants ??
    VtValue returnValue;
    ImGui::PushID(keyNameSrc.c_str());
    ImGuiContext &g = *GImGui;
    ImGuiWindow *window = g.CurrentWindow;
    auto cursor = ImGui::GetCursorPos();
    if (depth)
        ImGui::Indent((depth)*indentSize); // TODO: nicer indentation, add tree branches etc
    std::string keyName = keyNameSrc + " (" + typeName + ")";
    ImGui::Text("%s", keyName.c_str());
    if (depth)
        ImGui::Unindent((depth)*indentSize);

    static std::string editKeyName;
    static ImGuiID editingKey = 0; // Assuming there will be no collisions with 0

    if (!editingKey && ImGui::IsItemClicked() && ImGui::IsMouseDoubleClicked(0)) {
        editKeyName = keyNameSrc;
        editingKey = window->GetID(keyName.c_str());
    }

    if (editingKey == window->GetID(keyName.c_str())) {
        ImGui::SetCursorPos(cursor);
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::InputText("##Keyname", &editKeyName);
        if (ImGui::IsItemDeactivatedAfterEdit() || !ImGui::IsItemFocused()) {
            if (editKeyName != keyNameSrc) {
                returnValue = editKeyName;
            }
            editingKey = 0;
        }
    }
    ImGui::PopID();
    return returnValue;
}


template <> inline bool HasEdits<VtDictionary>(const VtDictionary &dict) {return !dict.empty();}


// We need to be able to notify client when a change of dict name happened

// TODO : show the types like "token" instead of TfToken
// TODO : allow an empty dict
// TODO : allow to clear the entry
VtValue DrawDictionaryRows(const VtDictionary &dictSource, const std::string &dictName, int depth) {

    ImGui::PushID(dictName.c_str());

    std::unique_ptr<VtDictionary> modifiedDict;
    std::string modifiedDictName;
    
    ScopedStyleColor style = GetRowStyle<VtDictionary>(0, dictSource);
    
    // DrawTableRow here ???
    ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowDefaultHeight);
    ImGui::TableSetColumnIndex(0);
    ImGui::Button(ICON_FA_BOOK_OPEN);
    if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
        if (ImGui::MenuItem(ICON_FA_TRASH " Clear")) {
            modifiedDict.reset(new VtDictionary()); // create an empty dict
        }
        if (ImGui::MenuItem(ICON_FA_KEY " Add key:value")) {
            modifiedDict.reset(new VtDictionary(dictSource));
            (*modifiedDict)["newvalue"] = VtValue(); // TODO find new name
        }
        ImGui::EndPopup();
    }
    ImGui::TableSetColumnIndex(1);

    // TODO: what happens when we modified the dict keyname ???
    VtValue editedDictName = DrawEditableKeyName(dictName, "dict", depth);
    if (editedDictName != VtValue()) {
        modifiedDictName = editedDictName.Get<std::string>();
    }

    for (auto &item : dictSource) {
        // Is it a dictionary ?
        // TODO test with list/vect of vtvalue or dict
        if (item.second.IsHolding<VtDictionary>()) {
            VtValue dictResult = DrawDictionaryRows(item.second.Get<VtDictionary>(), item.first, depth + 1);
            if (dictResult.IsHolding<VtDictionary>()) {
                modifiedDict.reset(new VtDictionary(dictSource));
                // If it returned an empty dict, then we remove the item
                // it's debatable
                if (dictResult.Get<VtDictionary>().empty()) {
                    modifiedDict->erase(item.first);
                } else {
                    (*modifiedDict)[item.first] = dictResult.Get<VtDictionary>();
                }
            } else if (dictResult.IsHolding<std::string>()) {
                // name is modified, change the key, keep the values
                modifiedDict.reset(new VtDictionary(dictSource));
                (*modifiedDict)[dictResult.Get<std::string>()] = item.second;
                modifiedDict->erase(item.first);
            }
        } else {
            // TODO: drawTableRow here ?
            ImGui::TableNextRow(ImGuiTableRowFlags_None, TableRowDefaultHeight);
            ImGui::TableSetColumnIndex(0);

            ImGui::PushID(item.first.c_str()); // FOR THE BUTTON
            ImGui::Button(ICON_FA_EDIT);
            if (ImGui::BeginPopupContextItem(nullptr, ImGuiPopupFlags_MouseButtonLeft)) {
                if (ImGui::MenuItem(ICON_FA_TRASH " Clear")) {
                    modifiedDict.reset(new VtDictionary(dictSource));
                    modifiedDict->erase(item.first);
                }

                if (ImGui::BeginMenu(ICON_FA_UTENSILS " Change value type")) {
                    if (ImGui::Selectable("dict")) {
                        modifiedDict.reset(new VtDictionary(dictSource));
                        (*modifiedDict)[item.first] = VtDictionary();
                    }
                    for (int i = 0; i < GetAllValueTypeNames().size(); i++) {
                        if (ImGui::Selectable(GetAllValueTypeNames()[i].GetAsToken().GetString().c_str(), false)) {
                            modifiedDict.reset(new VtDictionary(dictSource));
                            (*modifiedDict)[item.first] = GetAllValueTypeNames()[i].GetDefaultValue();
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            VtValue editedKeyName = DrawEditableKeyName(item.first, item.second.GetTypeName(), depth + 1);
            if (editedKeyName != VtValue()) {
                modifiedDict.reset(new VtDictionary(dictSource));
                modifiedDict->insert({editedKeyName.Get<std::string>(), item.second});
                modifiedDict->erase(item.first);
            }

            ImGui::TableSetColumnIndex(2);
            VtValue result = DrawVtValue(item.first, item.second);
            if (result != VtValue()) {
                modifiedDict.reset(new VtDictionary(dictSource));
                (*modifiedDict)[item.first] = result;
            }
        }
    }
    ImGui::PopID();
    // We return either a modified dict or a modified dict name
    if (modifiedDict)
        return VtValue(*modifiedDict);
    else if (!modifiedDictName.empty())
        return VtValue(modifiedDictName);
    else
        return {};
}
