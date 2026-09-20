#pragma once
#include <string>
#include <vector>
#include "ir/ir.h"

namespace lithon::typecheck {

struct RCRError {
    std::string message;
};

// Returns an empty vector on success. Non-empty means the module is
// rejected -- every entry is a distinct RCR violation.
std::vector<RCRError> check_module(const lithon::ir::Module& module);

} // namespace lithon::typecheck
