#include "std_include.hpp"
#include "source_compat.inl"
#include <wincrypt.h>
#include <Psapi.h>
#pragma comment(lib, "Psapi.lib")

std::string hash_file_sha1(const char* file_path)
{
	const auto file = CreateFileA(file_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return {};
	}

	HCRYPTPROV prov_handle = 0;
	HCRYPTHASH hash_handle = 0;

	BYTE buffer[4096];
	DWORD bytes_read = 0;

	BYTE hash[20]; // SHA-1 produces a 20-byte hash
	DWORD hash_len = sizeof(hash);

	if (!CryptAcquireContext(&prov_handle, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
		!CryptCreateHash(prov_handle, CALG_SHA1, 0, 0, &hash_handle))
	{
		CloseHandle(file);
		return {};
	}

	while (ReadFile(file, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0)
	{
		if (!CryptHashData(hash_handle, buffer, bytes_read, 0))
		{
			CryptDestroyHash(hash_handle);
			CryptReleaseContext(prov_handle, 0);
			CloseHandle(file);
			return {};
		}
	}

	std::string hash_string;
	if (CryptGetHashParam(hash_handle, HP_HASHVAL, hash, &hash_len, 0))
	{
		std::ostringstream oss;
		for (DWORD i = 0; i < hash_len; ++i) {
			oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
		}

		hash_string = oss.str();
	}

	CryptDestroyHash(hash_handle);
	CryptReleaseContext(prov_handle, 0);
	CloseHandle(file);
	return hash_string;
}

void init_fail_msg_setup()
{
	Beep(300, 100); Sleep(100); Beep(200, 100);
	game::console(); std::cout << "[!][INIT FAILED] Not loading " COMPMOD_NAME " Compatibility Mod" << std::endl;

	if (char file_path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, file_path, MAX_PATH))
	{
		std::string hash = hash_file_sha1(file_path);
		std::transform(hash.begin(), hash.end(), hash.begin(), [](const unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});
		const bool known_launcher =
			hash == "007f496dbac6c45a450e8966958cb741acee6702" ||
			hash == "098d07422acc07d560315fafebd021ea661121cd";
		if (!known_launcher)
		{
			std::cout << "---------------> Unexpected game exe hash. Hash was: " << hash.c_str() << std::endl;
			std::cout << "---------------> Path was: " << file_path << std::endl;
		}
	}
}

void init_fail_msg_post()
{
	std::cout << "\n\tMake sure that:" << std::endl;
	std::cout << "\t- Steam is running." << std::endl;
	std::cout << "\t- That it is a legit copy and the latest version of the game." << std::endl;
	std::cout << "\t- That you followed the install instructions and installed everything correctly." << std::endl;
	std::cout << "\n\tPlease attach l4d2-rtx\\logs\\compat.log when you open a GitHub issue." << std::endl;
}

#define GET_MODULE_HANDLE(INFO_OUT, NAME, T) \
	while (!(INFO_OUT).handle) { \
		if (const auto module_handle = GetModuleHandleA(NAME); module_handle) { \
			MODULEINFO module_info = {}; \
			if (!GetModuleInformation(GetCurrentProcess(), module_handle, &module_info, sizeof(module_info))) { \
				init_fail_msg_setup(); std::cout << "---------------> Failed to inspect module: " << (NAME) << std::endl; init_fail_msg_post(); \
				return TRUE; \
			} \
			(INFO_OUT).handle = reinterpret_cast<DWORD>(module_handle); \
			(INFO_OUT).size = module_info.SizeOfImage; \
			(INFO_OUT).name = (NAME); \
		} else { \
			Sleep(100); (T) += 100u; \
			if ((T) >= 30000) { \
				init_fail_msg_setup(); std::cout << "---------------> Failed to find module: " << (NAME) << std::endl; init_fail_msg_post(); \
				return TRUE; \
			} \
		} \
	}

DWORD WINAPI find_window_loop(LPVOID)
{
	char executable_path[MAX_PATH] = {};
	if (!GetModuleFileNameA(nullptr, executable_path, MAX_PATH))
	{
		game::console();
		std::cout << "[!][INIT FAILED] Could not resolve the executable path." << std::endl;
		return TRUE;
	}

	game::root_path = std::filesystem::path(executable_path).parent_path().string();
	if (!game::root_path.empty() && game::root_path.back() != '\\' && game::root_path.back() != '/') {
		game::root_path.push_back('\\');
	}

	source_compat::detect(executable_path, hash_file_sha1(executable_path));

	// Preserve normal L4D2 release behaviour: no extra console on a successful native start.
	if (!source_compat::is_native_l4d2()) {
		source_compat::print_startup_report();
	}

	if (!source_compat::should_load())
	{
		init_fail_msg_setup();
		std::cout << "---------------> Unsupported executable. Use -xo_allow_unsupported_source for the safe Generic Source core." << std::endl;
		std::cout << "---------------> Use -xo_force_l4d2_profile for the safe hybrid experiment." << std::endl;
		std::cout << "---------------> The old full L4D2 runtime additionally requires -xo_really_force_l4d2_runtime." << std::endl;
		init_fail_msg_post();
		return TRUE;
	}

	std::uint32_t T = 0;

	// Wait for the main game window. Native L4D2 still tries the old exact title first;
	// other Source games use a PID-owned window search.
	while (!glob::main_window)
	{
		glob::main_window = source_compat::find_main_window_for_current_process();

		Sleep(100); T += 100;
		if (T >= 30000)
		{
			init_fail_msg_setup();
			std::cout << "---------------> Failed to find the main visible window owned by process: " << GetCurrentProcessId() << std::endl;
			init_fail_msg_post();
			return TRUE;
		}
	}

	if (source_compat::uses_l4d2_runtime())
	{
		// Keep the original L4D2 module wait list for both native L4D2 and the explicit
		// unsafe compatibility experiment.
		GET_MODULE_HANDLE(game::shaderapidx9_module, "shaderapidx9.dll", T);
		GET_MODULE_HANDLE(game::studiorender_module, "studiorender.dll", T);
		//GET_MODULE_HANDLE(game::materialsystem_module, "materialsystem.dll", T);
		GET_MODULE_HANDLE(game::engine_module, "engine.dll", T);
		GET_MODULE_HANDLE(game::client_module, "client.dll", T);
		GET_MODULE_HANDLE(game::server_module, "server.dll", T);
		GET_MODULE_HANDLE(game::vstdlib_module, "vstdlib.dll", T);
	}
	else
	{
		// The L4D1 bootstrap uses exported Source interfaces and three read-only global
		// references, so engine/client/vstdlib must exist before components are built.
		GET_MODULE_HANDLE(game::shaderapidx9_module, "shaderapidx9.dll", T);
		if (source_compat::uses_l4d1_bootstrap())
		{
			GET_MODULE_HANDLE(game::engine_module, "engine.dll", T);
			GET_MODULE_HANDLE(game::client_module, "client.dll", T);
			GET_MODULE_HANDLE(game::vstdlib_module, "vstdlib.dll", T);
		}

		// Generic Source never dereferences the optional modules. Capture any modules
		// already present without delaying startup or applying fixed addresses.
		auto capture_optional_module = [](utils::mem::module_info& out, const char* name)
		{
			if (const auto handle = GetModuleHandleA(name); handle)
			{
				MODULEINFO info = {};
				if (GetModuleInformation(GetCurrentProcess(), handle, &info, sizeof(info)))
				{
					out.handle = reinterpret_cast<DWORD>(handle);
					out.size = info.SizeOfImage;
					out.name = name;
				}
			}
		};
		capture_optional_module(game::studiorender_module, "studiorender.dll");
		capture_optional_module(game::engine_module, "engine.dll");
		capture_optional_module(game::client_module, "client.dll");
		capture_optional_module(game::server_module, "server.dll");
		capture_optional_module(game::vstdlib_module, "vstdlib.dll");
	}

#undef GET_MODULE_HANDLE

#ifdef DEBUG
	Beep(523, 100);
#endif

	const auto window_title = source_compat::make_window_title();
	SetWindowTextA(glob::main_window, window_title.c_str());

	if (source_compat::uses_l4d1_bootstrap())
	{
		l4d1::init_bootstrap_addresses();
	}

	if (source_compat::uses_l4d2_runtime() && !l4d2::init_game_addresses())
	{
		init_fail_msg_setup();
		std::cout << "---------------> Steam update pattern recovery was incomplete. Runtime hooks were not installed." << std::endl;
		std::cout << "---------------> Start with -no_pattern only for controlled regression testing against the previous static offsets." << std::endl;
		init_fail_msg_post();
		return TRUE;
	}

	loader::initialize();
	source_compat::start_generic_overlay();
	return TRUE;
}

BOOL APIENTRY DllMain(HMODULE, const DWORD ul_reason_for_call, LPVOID)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH) 
	{
#if DEBUG
		game::console();
#endif

		if (const auto MH_INIT_STATUS = MH_Initialize(); MH_INIT_STATUS != MH_STATUS::MH_OK)
		{
			init_fail_msg_setup();
			std::cout << "[!][INIT FAILED] MinHook failed to initialize with code: " << MH_INIT_STATUS << std::endl;
			init_fail_msg_post();
			return TRUE;
		}

#if DEBUG
		// hook OutputDebugString
		game::SetupDebugOutputHook();
#endif

		CreateThread(nullptr, 0, find_window_loop, nullptr, 0, nullptr);
	}

	else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
		source_compat::stop_generic_overlay();
		loader::uninitialize();
	}

	return TRUE;
}
