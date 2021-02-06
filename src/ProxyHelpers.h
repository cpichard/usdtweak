#pragma once

#include <pxr/usd/sdf/listEditorProxy.h>

template <typename PolicyT, typename FuncT>
static void IterateListEditorItems(SdfListEditorProxy<PolicyT> &listEditor, const FuncT &call) {
    for (const PolicyT::value_type &item : listEditor.GetAddedItems()) {
        call("add", item); // return "add" as TfToken instead ?
    }
    for (const std::string &item : listEditor.GetPrependedItems()) {
        call("prepend", item);
    }
    for (const std::string &item : listEditor.GetExplicitItems()) {
        call("explicit", item);
    }
    for (const std::string &item : listEditor.GetOrderedItems()) {
        call("ordered", item);
    }
    for (const std::string &item : listEditor.GetAppendedItems()) {
        call("append", item);
    }
    for (const std::string &item : listEditor.GetDeletedItems()) {
        call("delete", item);
    }
}


