#include <iostream>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/abstractData.h>
#include "SdfLayerInstructions.h"

static void _CopySpec(const SdfAbstractData &src, SdfAbstractData *dst, const SdfPath &path) {
    if (!dst) {
        std::cerr << "ERROR: when copying the destination prim is null at path " << path.GetString() << std::endl;
        return;
    }
    dst->CreateSpec(path, src.GetSpecType(path));
    const TfTokenVector &fields = src.List(path);
    TF_FOR_ALL(i, fields) { dst->Set(path, *i, src.Get(path, *i)); }
}

UndoRedoDeleteSpec::_SpecCopier::_SpecCopier(SdfAbstractData *dst_) : dst(dst_) {}

bool UndoRedoDeleteSpec::_SpecCopier::VisitSpec(const SdfAbstractData &src, const SdfPath &path) {
    _CopySpec(src, dst, path);
    return true;
}

void UndoRedoDeleteSpec::_SpecCopier::Done(const SdfAbstractData &) {}

UndoRedoDeleteSpec::UndoRedoDeleteSpec(SdfLayerHandle layer, const SdfPath &path, bool inert, SdfAbstractDataPtr layerData)
    : _layer(layer), _path(path), _inert(inert), _deletedSpecType(_layer->GetSpecType(path)), _layerData(layerData) {
    // TODO: is there a faster way of copying and restoring the data ?
    // This can be really slow on big scenes
    SdfChangeBlock changeBlock;
    _deletedData = TfCreateRefPtr(new SdfData());
    SdfLayer::TraversalFunction copyFunc = std::bind(&_CopySpec, boost::cref(*boost::get_pointer(_layerData)),
                                                     boost::get_pointer(_deletedData), std::placeholders::_1);
    _layer->Traverse(path, copyFunc);
}


void UndoRedoDeleteSpec::DoIt() {
    if (_layer && _layer->GetStateDelegate()) {
        _layer->GetStateDelegate()->DeleteSpec(_path, _inert);
    }
}

void UndoRedoDeleteSpec::UndoIt() {
    if (_layer && _layer->GetStateDelegate()) {
        SdfChangeBlock changeBlock;
        _SpecCopier copier(boost::get_pointer(_layerData));
        _layer->GetStateDelegate()->CreateSpec(_path, _deletedSpecType, _inert);
        _deletedData->VisitSpecs(&copier);
    }
}