#pragma once
// Forced into every SKSE source file (and first in the helper's PCH). SKSE 1.7.3's headers use std::string,
// std::vector... without including them; older Visual Studio versions pulled them in by accident, VS2022 does not.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/IPrefix.h"
