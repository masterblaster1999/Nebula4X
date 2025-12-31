#pragma once

#include <string>

#include "nebula4x/core/entities.h"

namespace nebula4x {

// Shared string <-> enum conversion helpers.
//
// These are used by both serialization and UI/debug tooling. Keeping them
// in one place avoids drift between save-format strings and UI labels.

std::string body_type_to_string(BodyType t);
BodyType body_type_from_string(const std::string& s);

} // namespace nebula4x
