#pragma once
#include <vector>
#include <functional>
#include <memory>
#include <iostream>


class InstructionWrapper {
public:

    template <typename InstructionT>
    InstructionWrapper(InstructionT &&instruction) :
        _ref(std::make_unique<Storage<InstructionT>>(std::move(instruction))) {
        _ref->ShowIt();
    }

    void DoIt() {
        _ref->DoIt();
    }

    void UndoIt() {
        _ref->UndoIt();
    }

    void ShowIt() {
        _ref->ShowIt();
    }

    struct Interface {
        virtual ~Interface() = default;
        virtual void DoIt() = 0;
        virtual void UndoIt() = 0;
        virtual void ShowIt() = 0;
    };

    template <typename InstructionT>
    struct Storage : Interface {
        Storage(InstructionT &&inst) : _data(std::move(inst)) {
        }
        virtual ~Storage(){}
        void DoIt () override {
            _data.DoIt();
        }
        void UndoIt () override {
            _data.UndoIt();
        }

        void ShowIt() override {
            //std::cout << "InstructionWrapper contains: " << _data._path.GetString()
            //<< " " << _data._fieldName.GetString()
            //<< " " << _data._previousValue
            //<< " " << _data._newValue
            //<< std::endl;
        }

        InstructionT _data;
    };

    std::unique_ptr<Interface> _ref;
};

class SdfCommandGroup {

public:
    SdfCommandGroup() = default;
    ~SdfCommandGroup() = default;

    /// Was it recorded
    bool IsEmpty() const;
    void Clear();

    /// Run the commands as an undo
    void DoIt();
    void UndoIt();

    template <typename InstructionT>
    void StoreInstruction(InstructionT);

private:
    std::vector<InstructionWrapper> _instructions;
};


