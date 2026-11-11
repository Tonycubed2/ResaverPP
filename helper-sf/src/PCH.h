#pragma once
// ReSaverHelper for Starfield (SFSE 0.2.21+, Address Library format 5), 64-bit. No SFSE sources or CommonLib needed:
// see SFSE.h and Game.h.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <ShlObj.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
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
