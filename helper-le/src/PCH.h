#pragma once
// ReSaverHelper for Skyrim Legendary Edition (TESV.exe 1.9.32.0, SKSE 1.7.3), 32-bit.
// SKSE's own headers first (they bring in Windows.h through common/IPrefix.h). NOMINMAX is not set
// because SKSE's code uses the min / max macros: write (std::min)(a, b) in this project.

#include "common/IPrefix.h"

#include "skse/PluginAPI.h"
#include "skse/skse_version.h"
#include "skse/GameAPI.h"
#include "skse/GameBSExtraData.h"
#include "skse/GameData.h"
#include "skse/GameExtraData.h"
#include "skse/GameFormComponents.h"
#include "skse/GameForms.h"
#include "skse/GameMenus.h"
#include "skse/GameObjects.h"
#include "skse/GameRTTI.h"
#include "skse/GameReferences.h"
#include "skse/GameThreads.h"
#include "skse/GameTypes.h"
#include "skse/PapyrusArgs.h"
#include "skse/PapyrusVM.h"

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

#include "Log.h"

using namespace std::literals;
