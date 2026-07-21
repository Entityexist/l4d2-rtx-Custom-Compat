#pragma once

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#define COMPMOD_NAME "L4D2-RTX"
#define COMPMOD_ASSET_DIR "l4d2-rtx\\"
#define WINDOW_TITLE_STR "Left 4 Dead 2 - Direct3D 9"

// enable/disable benchmark logic
//#define BENCHMARK

// Version number
#include <version.hpp>

#define NOMINMAX
#include <windows.h>
#include <functional>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <cwctype>
#include <cctype>
#include <shellapi.h>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <filesystem>
#include <cassert>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <unordered_set>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <xmmintrin.h>
#include <intrin.h>
#include <numbers>
#include <algorithm>
#include <format>
#include <cfloat>
#include <cstdint>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <array>
#include <sstream>
#include <iomanip>

#pragma warning(push)
#pragma warning(disable: 26495)
#include <d3d9.h>
#include <d3dx9.h>
#pragma warning(pop)

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

#define STRINGIZE_(x) #x
#define STRINGIZE(x) STRINGIZE_(x)
#define AssertSize(x, size)								static_assert(sizeof(x) == size, STRINGIZE(x) " structure has an invalid size.")
#define STATIC_ASSERT_SIZE(struct, size)				static_assert(sizeof(struct) == size, "Size check")
#define STATIC_ASSERT_OFFSET(struct, member, offset)	static_assert(offsetof(struct, member) == offset, "Offset check")
#define XASSERT(x) if (x) MessageBoxA(HWND_DESKTOP, #x, "FATAL ERROR", MB_ICONERROR)

#include "MinHook.h"

// toml11 is third-party code. Keep MSVC Code Analysis diagnostics from the
// dependency out of the project warning list without modifying vendor files.
#pragma warning(push)
#pragma warning(disable: 26439 26478 26495 26498)
#include "toml.hpp"
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable: 6011)
#pragma warning(disable: 28182)
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <misc/cpp/imgui_stdlib.h>
#pragma warning(pop)

#include "bridge_remix_api.h"

#include "game/structs.hpp"
#include "utils/fnv.hpp"
#include "utils/utils.hpp"
#include "utils/vector.hpp"

#include "sdk/netvar/netvar.hpp"
#include "sdk/client/c_base_client.hpp"
#include "sdk/client/c_player_info_manager.hpp"
#include "sdk/engine/c_engine_client.hpp"
#include "sdk/engine/c_engine_effects.hpp"
#include "sdk/engine/c_engine_trace.hpp"
#include "sdk/client/c_collideable.hpp"
#include "sdk/entity/c_base_entity.hpp"
#include "sdk/entity/c_entity_list.hpp"
#include "sdk/vgui/surface/c_surface_mgr.hpp"
#include "sdk/cvar/cvar.hpp"

#include "utils/hooking.hpp"
#include "utils/memory.hpp"
#include "utils/function.hpp"
#include "source_compat.hpp"
#include "game/l4d2.hpp"
#include "game/l4d1.hpp"
#include "game/functions.hpp"

#include "components/loader.hpp"

using namespace std::literals;
