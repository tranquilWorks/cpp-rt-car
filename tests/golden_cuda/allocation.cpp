#include "allocation.hpp"
#include <algorithm>
// Keep the unchanged M24 allocation implementation in its own translation unit.
// Calls remain interposed and counted; no compiler warning is disabled.
#include "../cuda_physics/allocation_guard.hpp"
