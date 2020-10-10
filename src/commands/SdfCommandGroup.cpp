#include "SdfCommandGroup.h"
#include "SdfLayerInstructions.h"
#include <boost/range/adaptor/reversed.hpp> // why reverse adaptor is not in std ?? seriously ...
#include <memory>
#include <iostream>


bool SdfCommandGroup::IsEmpty() const { return _instructions.empty(); }

void SdfCommandGroup::Clear() { _instructions.clear(); }

template <typename InstructionT>
void SdfCommandGroup::StoreInstruction(InstructionT inst) {

    //std::cout << "Instruction contains " << inst._newValue << " " << inst._previousValue  << std::endl;
    InstructionWrapper wrapper(std::move(inst));
    // TODO: specialize by type to compress the commands,
    // typically we don't want to store thousand of setField instruction were only the last one matters
    // One optim would be to look for the previous instruction, check if it is a setfield on the same path, same layer ?
    // Update the latest instruction instead of inserting a new instruction
    // As StoreInstruction is templatized, it is possible to specialize it.
    _instructions.emplace_back(std::move(wrapper));
}


template void SdfCommandGroup::StoreInstruction<UndoRedoSetField>(UndoRedoSetField inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoSetFieldDictValueByKey>(UndoRedoSetFieldDictValueByKey inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoSetTimeSample>(UndoRedoSetTimeSample inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoCreateSpec>(UndoRedoCreateSpec inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoDeleteSpec>(UndoRedoDeleteSpec inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoMoveSpec>(UndoRedoMoveSpec inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoPushChild<TfToken>>(UndoRedoPushChild<TfToken> inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoPushChild<SdfPath>>(UndoRedoPushChild<SdfPath> inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoPopChild<TfToken>>(UndoRedoPopChild<TfToken> inst);
template void SdfCommandGroup::StoreInstruction<UndoRedoPopChild<SdfPath>>(UndoRedoPopChild<SdfPath> inst);

// Call all the functions stored in _commands in reverse order
void SdfCommandGroup::UndoIt() {
    for (auto &cmd : boost::adaptors::reverse(_instructions)) {
        cmd.ShowIt();  // FOR DEBUGGING now
        cmd.UndoIt();
    }
}

void SdfCommandGroup::DoIt() {
    for (auto &cmd : _instructions) {
        cmd.ShowIt();  // FOR DEBUGGING now
        cmd.DoIt();
    }
}
