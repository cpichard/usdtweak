#pragma once
#include <vector>
#include <functional>

using SdfCommandFunction = std::function<void()>;

class SdfCommandGroup {

public:
    SdfCommandGroup();
    ~SdfCommandGroup();

    /// Was it recorded
    bool IsEmpty() const;

    /// Run the commands
    void operator () ();

    void AddFunction(SdfCommandFunction &&);

private:
    std::vector<SdfCommandFunction> _commands; // TODO: measure the size of functions, how big is that ?
};