#include "SdfCommandGroup.h"

#include <boost/range/adaptor/reversed.hpp> // why reverse adaptor is not in std ?? seriously ...


SdfCommandGroup::SdfCommandGroup() {}
SdfCommandGroup::~SdfCommandGroup() {}


bool SdfCommandGroup::IsEmpty() const { return _commands.empty(); }

void SdfCommandGroup::AddFunction(SdfCommandFunction &&function) {
    _commands.emplace_back(function);
}

// Call all the functions stored in _commands in reverse order
void SdfCommandGroup::operator () () {
    for (auto &cmd : boost::adaptors::reverse(_commands)) {
        cmd();
    }
}
