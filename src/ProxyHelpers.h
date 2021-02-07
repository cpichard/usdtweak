#pragma once

#include <pxr/usd/sdf/listEditorProxy.h>

// ExtraArgsT is used to pass additional arguments as the function passed as visitor
// might need more than the operation and the item
template <typename PolicyT, typename FuncT, typename ...ExtraArgsT>
static void IterateListEditorItems(SdfListEditorProxy<PolicyT> &listEditor, const FuncT &call, ExtraArgsT... args ) {
    // TODO: should we check if the list is already all explicit ??
    for (const PolicyT::value_type &item : listEditor.GetExplicitItems()) {
        call("explicit", item, args...);
    }
    for (const PolicyT::value_type &item : listEditor.GetAddedItems()) {
        call("add", item, args...); // return "add" as TfToken instead ?
    }
    for (const PolicyT::value_type &item : listEditor.GetPrependedItems()) {
        call("prepend", item, args...);
    }
    for (const PolicyT::value_type &item : listEditor.GetOrderedItems()) {
        call("ordered", item, args...);
    }
    for (const PolicyT::value_type &item : listEditor.GetAppendedItems()) {
        call("append", item, args...);
    }
    for (const PolicyT::value_type &item : listEditor.GetDeletedItems()) {
        call("delete", item, args...);
    }
}


