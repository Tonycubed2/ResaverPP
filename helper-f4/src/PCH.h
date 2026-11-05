#pragma once
// ReSaverHelper for Fallout 4 (1.10.163, 1.10.984, 1.11.x), 64-bit. No F4SE sources or CommonLib needed:
// see F4SE.h and Game.h.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::literals;

#include "Log.h"
