#pragma once

#include "saturn/core/memory_map.hpp"

namespace saturn::core {

// Backward-compatible name for callers that still load one contiguous file.
// New boot/runtime code should use MemoryMap directly.
using MemoryImage = MemoryMap;

} // namespace saturn::core
