#pragma once
#include <cassert>
#include <pxr/usd/sdf/listEditorProxy.h>
#include <pxr/usd/sdf/reference.h>

// ExtraArgsT is used to pass additional arguments as the function passed as visitor
// might need more than the operation and the item
template <typename PolicyT, typename FuncT, typename... ExtraArgsT>
static void IterateListEditorItems(const SdfListEditorProxy<PolicyT> &listEditor, const FuncT &call, ExtraArgsT... args) {
    // TODO: should we check if the list is already all explicit ??
    for (const typename PolicyT::value_type &item : listEditor.GetExplicitItems()) {
        call(SdfListOpTypeExplicit, item, args...);
    }
    for (const typename PolicyT::value_type &item : listEditor.GetOrderedItems()) {
        call(SdfListOpTypeOrdered, item, args...);
    }
    for (const typename PolicyT::value_type &item : listEditor.GetAddedItems()) {
        call(SdfListOpTypeAdded, item, args...); // return "add" as TfToken instead ?
    }
    for (const typename PolicyT::value_type &item : listEditor.GetPrependedItems()) {
        call(SdfListOpTypePrepended, item, args...);
    }
    for (const typename PolicyT::value_type &item : listEditor.GetAppendedItems()) {
        call(SdfListOpTypeAppended, item, args...);
    }
    for (const typename PolicyT::value_type &item : listEditor.GetDeletedItems()) {
        call(SdfListOpTypeDeleted, item, args...);
    }
}

/// The list available on a SdfListEditor
constexpr int GetListEditorOperationSize() { return 6; }

template <typename IntOrSdfListOpT> inline const char *GetListEditorOperationName(IntOrSdfListOpT index) {
    constexpr const char *names[GetListEditorOperationSize()] = {"explicit", "add", "delete", "ordered", "prepend", "append"};
    return names[static_cast<int>(index)];
}

template <typename IntOrSdfListOpT> inline const char *GetListEditorOperationAbbreviation(IntOrSdfListOpT index) {
    constexpr const char *names[GetListEditorOperationSize()] = {"Ex", "Ad", "De", "Or", "Pr", "Ap"};
    return names[static_cast<int>(index)];
}

template <typename PolicyT>
inline void CreateListEditorOperation(SdfListEditorProxy<PolicyT> &&listEditor, SdfListOpType op,
                                      typename SdfListEditorProxy<PolicyT>::value_type item) {
    switch (op) {
    case SdfListOpTypeAdded:
        listEditor.GetAddedItems().push_back(item);
        break;
    case SdfListOpTypePrepended:
        listEditor.GetPrependedItems().push_back(item);
        break;
    case SdfListOpTypeAppended:
        listEditor.GetAppendedItems().push_back(item);
        break;
    case SdfListOpTypeDeleted:
        listEditor.GetDeletedItems().push_back(item);
        break;
    case SdfListOpTypeExplicit:
        listEditor.GetExplicitItems().push_back(item);
        break;
    case SdfListOpTypeOrdered:
        listEditor.GetOrderedItems().push_back(item);
        break;
    default:
        assert(0);
    }
}

template <typename ListEditorT, typename OpOrIntT> inline auto GetSdfListOp(ListEditorT &listEditor, OpOrIntT op_) {
    const SdfListOpType op = static_cast<SdfListOpType>(op_);
    if (op == SdfListOpTypeOrdered) {
        return listEditor.GetOrderedItems();
    } else if (op == SdfListOpTypeAppended) {
        return listEditor.GetAppendedItems();
    } else if (op == SdfListOpTypeAdded) {
        return listEditor.GetAddedItems();
    } else if (op == SdfListOpTypePrepended) {
        return listEditor.GetPrependedItems();
    } else if (op == SdfListOpTypeDeleted) {
        return listEditor.GetDeletedItems();
    }
    return listEditor.GetExplicitItems();
};

