#include "std_include.hpp"

namespace utils
{
	namespace mem
	{
		namespace
		{
			struct parsed_pattern
			{
				std::vector<uint8_t> bytes;
				std::vector<bool> wildcard;
			};

			parsed_pattern parse_pattern(const std::string_view signature)
			{
				parsed_pattern out;
				auto nibble = [](const char c) -> uint8_t
				{
					if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
					if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
					if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
					throw std::runtime_error("Malformed signature: non-hex character");
				};

				for (size_t i = 0; i < signature.size();)
				{
					if (signature[i] == ' ') { ++i; continue; }
					if (signature[i] == '?')
					{
						out.bytes.push_back(0u);
						out.wildcard.push_back(true);
						++i;
						if (i < signature.size() && signature[i] == '?') ++i;
						continue;
					}
					if (i + 1u >= signature.size()) throw std::runtime_error("Malformed signature: truncated byte");
					out.bytes.push_back(static_cast<uint8_t>((nibble(signature[i]) << 4u) | nibble(signature[i + 1u])));
					out.wildcard.push_back(false);
					i += 2u;
				}
				if (out.bytes.empty() || out.bytes.size() != out.wildcard.size()) {
					throw std::runtime_error("Invalid empty pattern");
				}
				return out;
			}

			bool is_readable_protection(const DWORD protection)
			{
				if (protection & (PAGE_GUARD | PAGE_NOACCESS)) return false;
				switch (protection & 0xFFu)
				{
				case PAGE_READONLY:
				case PAGE_READWRITE:
				case PAGE_WRITECOPY:
				case PAGE_EXECUTE_READ:
				case PAGE_EXECUTE_READWRITE:
				case PAGE_EXECUTE_WRITECOPY:
					return true;
				default:
					return false;
				}
			}

			DWORD scan_readable_range(const uintptr_t begin, const size_t length, const parsed_pattern& pattern,
				const DWORD offset)
			{
				if (!begin || pattern.bytes.size() > length) return 0u;

				const auto* base = reinterpret_cast<const uint8_t*>(begin);
				size_t anchor = 0u;
				while (anchor < pattern.bytes.size() && pattern.wildcard[anchor]) ++anchor;

				for (size_t i = 0; i <= length - pattern.bytes.size(); ++i)
				{
					if (anchor < pattern.bytes.size() && base[i + anchor] != pattern.bytes[anchor]) continue;
					bool match = true;
					for (size_t j = 0; j < pattern.bytes.size(); ++j)
					{
						if (!pattern.wildcard[j] && base[i + j] != pattern.bytes[j])
						{
							match = false;
							break;
						}
					}
					if (match) return static_cast<DWORD>(begin + i + offset);
				}
				return 0u;
			}

			void scan_readable_range_all(const uintptr_t begin, const size_t length, const parsed_pattern& pattern,
				const DWORD offset, std::vector<DWORD>& matches)
			{
				if (!begin || pattern.bytes.size() > length) return;
				const auto* base = reinterpret_cast<const uint8_t*>(begin);
				size_t anchor = 0u;
				while (anchor < pattern.bytes.size() && pattern.wildcard[anchor]) ++anchor;
				for (size_t i = 0; i <= length - pattern.bytes.size(); ++i)
				{
					if (anchor < pattern.bytes.size() && base[i + anchor] != pattern.bytes[anchor]) continue;
					bool match = true;
					for (size_t j = 0; j < pattern.bytes.size(); ++j)
					{
						if (!pattern.wildcard[j] && base[i + j] != pattern.bytes[j]) { match = false; break; }
					}
					if (match) matches.push_back(static_cast<DWORD>(begin + i + offset));
				}
			}

			void scan_readable_range_nearest(const uintptr_t begin, const size_t length, const parsed_pattern& pattern,
				const uintptr_t preferred, const DWORD offset, DWORD& best, std::uint64_t& best_distance)
			{
				if (!begin || pattern.bytes.size() > length) return;
				const auto* base = reinterpret_cast<const uint8_t*>(begin);
				size_t anchor = 0u;
				while (anchor < pattern.bytes.size() && pattern.wildcard[anchor]) ++anchor;
				for (size_t i = 0; i <= length - pattern.bytes.size(); ++i)
				{
					if (anchor < pattern.bytes.size() && base[i + anchor] != pattern.bytes[anchor]) continue;
					bool match = true;
					for (size_t j = 0; j < pattern.bytes.size(); ++j)
					{
						if (!pattern.wildcard[j] && base[i + j] != pattern.bytes[j]) { match = false; break; }
					}
					if (!match) continue;
					const uintptr_t address = begin + i;
					const std::uint64_t distance = address >= preferred
						? static_cast<std::uint64_t>(address - preferred)
						: static_cast<std::uint64_t>(preferred - address);
					if (!best || distance < best_distance)
					{
						best = static_cast<DWORD>(address + offset);
						best_distance = distance;
					}
				}
			}

			DWORD scan_nearest(const DWORD handle, const DWORD size, const std::string_view signature,
				const DWORD preferred_rva, const DWORD offset, const char* description)
			{
				if (!handle || !size) return 0u;
				try
				{
					const auto pattern = parse_pattern(signature);
					if (pattern.bytes.size() > size) return 0u;
					const uintptr_t module_begin = static_cast<uintptr_t>(handle);
					const uintptr_t module_end = module_begin + static_cast<uintptr_t>(size);
					const uintptr_t preferred = module_begin + static_cast<uintptr_t>(preferred_rva);
					uintptr_t cursor = module_begin;
					DWORD best = 0u;
					std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
					while (cursor < module_end)
					{
						MEMORY_BASIC_INFORMATION mbi = {};
						if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;
						const uintptr_t region_begin = std::max(cursor, reinterpret_cast<uintptr_t>(mbi.BaseAddress));
						const uintptr_t raw_region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
						const uintptr_t region_end = std::min(module_end, raw_region_end);
						if (region_end <= region_begin) break;
						if (mbi.State == MEM_COMMIT && is_readable_protection(mbi.Protect))
						{
							scan_readable_range_nearest(region_begin, static_cast<size_t>(region_end - region_begin),
								pattern, preferred, offset, best, best_distance);
						}
						cursor = region_end;
					}
					if (best) return best;
				}
				catch (const std::exception& error)
				{
					game::console();
					std::cerr << "[Pattern][ERROR] Invalid nearest signature for "
						<< (description ? description : "unnamed") << ": " << error.what() << std::endl;
					return 0u;
				}
				game::console();
				std::cerr << "[Pattern][ERROR] " << (description ? description : "unnamed")
					<< " not found near preferred RVA; signature: " << signature << std::endl;
				return 0u;
			}

			DWORD scan(const DWORD handle, const DWORD size, const std::string_view signature, const DWORD offset, const char* description)
			{
				if (!handle || !size) return 0u;

				try
				{
					static std::unordered_map<std::string, parsed_pattern> cache;
					const std::string cache_key(signature);
					auto it = cache.find(cache_key);
					if (it == cache.end())
					{
						// Parse before insertion. A malformed signature must never leave an
						// empty cache entry that could match the module base on a later call.
						auto parsed = parse_pattern(signature);
						it = cache.emplace(cache_key, std::move(parsed)).first;
					}
					const auto& pattern = it->second;
					if (pattern.bytes.size() > size) return 0u;

					const uintptr_t module_begin = static_cast<uintptr_t>(handle);
					const uintptr_t module_end = module_begin + static_cast<uintptr_t>(size);
					uintptr_t cursor = module_begin;

					while (cursor < module_end)
					{
						MEMORY_BASIC_INFORMATION mbi = {};
						if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;

						const uintptr_t region_begin = std::max(cursor, reinterpret_cast<uintptr_t>(mbi.BaseAddress));
						const uintptr_t raw_region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
						const uintptr_t region_end = std::min(module_end, raw_region_end);
						if (region_end <= region_begin) break;

						if (mbi.State == MEM_COMMIT && is_readable_protection(mbi.Protect))
						{
							if (const DWORD found = scan_readable_range(region_begin,
								static_cast<size_t>(region_end - region_begin), pattern, offset); found)
							{
								return found;
							}
						}
						cursor = region_end;
					}
				}
				catch (const std::exception& error)
				{
					game::console();
					std::cerr << "[Pattern][ERROR] Invalid signature for "
						<< (description ? description : "unnamed") << ": " << error.what() << std::endl;
					return 0u;
				}

				game::console();
				std::cerr << "[Pattern][ERROR] " << (description ? description : "unnamed")
					<< " not found in module; signature: " << signature << std::endl;
				return 0u;
			}

		}

		DWORD find_pattern_in_module(const HMODULE module, const std::string_view signature, const DWORD offset, const char* description)
		{
			if (!module) return 0u;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0u;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return 0u;
			return scan(reinterpret_cast<DWORD>(module), nt->OptionalHeader.SizeOfImage, signature, offset, description);
		}

		DWORD find_pattern(module_info& module, const std::string_view signature, const DWORD offset, const char* description, const bool is_active, const DWORD inactive_offset)
		{
			if (!module.handle || !module.size)
			{
				game::console();
				std::cerr << "[Pattern][ERROR] Module unavailable for "
					<< (description ? description : "unnamed") << std::endl;
				return 0u;
			}
			if (!is_active) return module.handle + inactive_offset + offset;
			return scan(module.handle, module.size, signature, offset, description);
		}

		std::vector<DWORD> find_pattern_matches(module_info& module, const std::string_view signature, const DWORD offset)
		{
			std::vector<DWORD> matches;
			if (!module.handle || !module.size) return matches;
			try
			{
				const auto pattern = parse_pattern(signature);
				const uintptr_t module_begin = static_cast<uintptr_t>(module.handle);
				const uintptr_t module_end = module_begin + static_cast<uintptr_t>(module.size);
				uintptr_t cursor = module_begin;
				while (cursor < module_end)
				{
					MEMORY_BASIC_INFORMATION mbi = {};
					if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi))) break;
					const uintptr_t queried_begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
					const uintptr_t region_begin = cursor > queried_begin ? cursor : queried_begin;
					const uintptr_t max_span = (std::numeric_limits<uintptr_t>::max)() - queried_begin;
					const uintptr_t queried_span = mbi.RegionSize > static_cast<SIZE_T>(max_span)
						? max_span
						: static_cast<uintptr_t>(mbi.RegionSize);
					const uintptr_t queried_end = queried_begin + queried_span;
					const uintptr_t region_end = module_end < queried_end ? module_end : queried_end;
					if (region_end <= region_begin) break;
					if (mbi.State == MEM_COMMIT && is_readable_protection(mbi.Protect))
					{
						scan_readable_range_all(region_begin, static_cast<size_t>(region_end - region_begin), pattern, offset, matches);
					}
					cursor = region_end;
				}
			}
			catch (...) { matches.clear(); }
			return matches;
		}

		DWORD find_pattern_nearest(module_info& module, const std::string_view signature, const DWORD preferred_rva,
			const DWORD offset, const char* description, const bool is_active, const DWORD inactive_offset)
		{
			if (!module.handle || !module.size)
			{
				game::console();
				std::cerr << "[Pattern][ERROR] Module unavailable for "
					<< (description ? description : "unnamed") << std::endl;
				return 0u;
			}
			if (!is_active) return module.handle + inactive_offset + offset;
			return scan_nearest(module.handle, module.size, signature, preferred_rva, offset, description);
		}

		uint32_t resolve_relative_call_address(const uint32_t call_instruction_addr)
		{
			if (!call_instruction_addr) return 0u;
			const auto displacement = *reinterpret_cast<const int32_t*>(call_instruction_addr + 1u);
			return call_instruction_addr + 5u + displacement;
		}

		uint32_t resolve_relative_jump_address(const uint32_t instruction_addr, const uint32_t instruction_size, const uint32_t bytes_until_relative_addr)
		{
			if (!instruction_addr || instruction_size <= bytes_until_relative_addr) return 0u;
			const auto offset_size = instruction_size - bytes_until_relative_addr;
			int32_t displacement = 0;
			const auto* ptr = reinterpret_cast<const uint8_t*>(instruction_addr + bytes_until_relative_addr);
			if (offset_size == 1u) displacement = *reinterpret_cast<const int8_t*>(ptr);
			else if (offset_size == 2u) displacement = *reinterpret_cast<const int16_t*>(ptr);
			else if (offset_size == 4u) displacement = *reinterpret_cast<const int32_t*>(ptr);
			else return 0u;
			return instruction_addr + instruction_size + displacement;
		}
	}

	hook::~hook()
	{
		if (this->initialized)
		{
			this->uninstall();
		}
	}

	hook* hook::initialize(DWORD _place, void(*_stub)(), bool _useJump)
	{
		return this->initialize(_place, reinterpret_cast<void*>(_stub), _useJump);
	}

	hook* hook::initialize(DWORD _place, void* _stub, bool _useJump)
	{
		return this->initialize(reinterpret_cast<void*>(_place), _stub, _useJump);
	}

	hook* hook::initialize(void* _place, void* _stub, bool _useJump)
	{
		if (this->initialized) return this;
		this->initialized = true;

		this->useJump = _useJump;
		this->place = _place;
		this->stub = _stub;

		this->original = static_cast<char*>(this->place) + 5 + *reinterpret_cast<DWORD*>((static_cast<char*>(this->place) + 1));

		return this;
	}

	hook* hook::install(bool unprotect, bool keepUnportected)
	{
		std::lock_guard<std::mutex> _(this->stateMutex);

		if (!this->initialized || this->installed) {
			return this;
		}

		this->installed = true;

		if (unprotect) VirtualProtect(this->place, sizeof(this->buffer), PAGE_EXECUTE_READWRITE, &this->protection);
		std::memcpy(this->buffer, this->place, sizeof(this->buffer));

		char* code = static_cast<char*>(this->place);

		*code = static_cast<char>(this->useJump ? 0xE9 : 0xE8);

		*reinterpret_cast<size_t*>(code + 1) = reinterpret_cast<size_t>(this->stub) - (reinterpret_cast<size_t>(this->place) + 5);

		if (unprotect && !keepUnportected) VirtualProtect(this->place, sizeof(this->buffer), this->protection, &this->protection);

		FlushInstructionCache(GetCurrentProcess(), this->place, sizeof(this->buffer));

		return this;
	}

	void hook::quick()
	{
		if (hook::installed) {
			hook::installed = false;
		}
	}

	hook* hook::uninstall(bool unprotect)
	{
		std::lock_guard<std::mutex> _(this->stateMutex);

		if (!this->initialized || !this->installed) {
			return this;
		}

		this->installed = false;

		if (unprotect) {
			VirtualProtect(this->place, sizeof(this->buffer), PAGE_EXECUTE_READWRITE, &this->protection);
		}

		std::memcpy(this->place, this->buffer, sizeof(this->buffer));

		if (unprotect) {
			VirtualProtect(this->place, sizeof(this->buffer), this->protection, &this->protection);
		}

		FlushInstructionCache(GetCurrentProcess(), this->place, sizeof(this->buffer));
		return this;
	}

	void* hook::get_address()
	{
		return this->place;
	}

	void hook::nop(void* place, size_t length)
	{
		DWORD oldProtect;
		VirtualProtect(place, length, PAGE_EXECUTE_READWRITE, &oldProtect);

		memset(place, 0x90, length);

		VirtualProtect(place, length, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), place, length);
	}

	void hook::nop(DWORD place, size_t length)
	{
		nop(reinterpret_cast<void*>(place), length);
	}

	void hook::set_string(void* place, const char* string, size_t length)
	{
		DWORD oldProtect;
		VirtualProtect(place, length + 1, PAGE_EXECUTE_READWRITE, &oldProtect);

		strncpy_s(static_cast<char*>(place), length, string, length);

		VirtualProtect(place, length + 1, oldProtect, &oldProtect);
	}

	void hook::set_string(DWORD place, const char* string, size_t length)
	{
		hook::set_string(reinterpret_cast<void*>(place), string, length);
	}

	void hook::set_string(void* place, const char* string)
	{
		hook::set_string(place, string, strlen(static_cast<char*>(place)));
	}

	void hook::set_string(DWORD place, const char* string)
	{
		hook::set_string(reinterpret_cast<void*>(place), string);
	}

	void hook::write_string(void* place, const std::string& string)
	{
		DWORD old_protect;
		VirtualProtect(place, string.size() + 1, PAGE_EXECUTE_READWRITE, &old_protect);

		memcpy(place, &string[0], string.size() + 1);

		VirtualProtect(place, string.size() + 1, old_protect, &old_protect);
		FlushInstructionCache(GetCurrentProcess(), place, string.size());
	}

	void hook::write_string(const DWORD place, const std::string& string)
	{
		write_string(reinterpret_cast<void*>(place), string);
	}

	void hook::redirect_jump(void* place, void* stub)
	{
		char* operandPtr = static_cast<char*>(place) + 2;
		int newOperand = reinterpret_cast<int>(stub) - (reinterpret_cast<int>(place) + 6);
		utils::hook::set<int>(operandPtr, newOperand);
	}

	void hook::redirect_jump(DWORD place, void* stub)
	{
		hook::redirect_jump(reinterpret_cast<void*>(place), stub);
	}


	// #
	// #

	void* cinterface::get_interface(const HMODULE hmodule, const char* const sz_object)
	{
		if (const auto addr = GetProcAddress(hmodule, "CreateInterface"); addr)
		{
			if (const auto pfCreateInterface = reinterpret_cast<void* (*)(const char*, int*)>(addr); pfCreateInterface) {
				return pfCreateInterface(sz_object, nullptr);
			}
		}
		return nullptr;
	}
}
