#include "std_include.hpp"
#include "source_bsp_lights.hpp"
#include <array>
#include <cstring>
#include <optional>
#include <string_view>
#include <limits>

namespace components::source_bsp_lights
{
	namespace
	{
		constexpr std::uint32_t VPK_SIGNATURE = 0x55AA1234u;
		constexpr std::uint16_t VPK_ARCHIVE_DIRECTORY = 0x7FFFu;
		constexpr std::uint16_t VPK_ENTRY_TERMINATOR = 0xFFFFu;
		constexpr std::int32_t VBSP_IDENT = 0x50534256; // 'VBSP'
		constexpr std::size_t BSP_LUMP_COUNT = 64u;
		constexpr std::size_t LUMP_ENTITIES = 0u;
		constexpr std::size_t LUMP_TEXDATA = 2u;
		constexpr std::size_t LUMP_TEXINFO = 6u;
		constexpr std::size_t LUMP_WORLDLIGHTS = 15u;
		constexpr std::size_t LUMP_TEXDATA_STRING_DATA = 43u;
		constexpr std::size_t LUMP_TEXDATA_STRING_TABLE = 44u;
		constexpr std::size_t LUMP_WORLDLIGHTS_HDR = 54u;
		constexpr std::size_t MAX_BSP_SIZE = 512u * 1024u * 1024u;
		constexpr std::size_t MAX_VPK_TREE_SIZE = 64u * 1024u * 1024u;

		struct bsp_vec3_disk_s
		{
			float x;
			float y;
			float z;
		};

		struct bsp_lump_raw_s
		{
			std::int32_t field0;
			std::int32_t field1;
			std::int32_t version;
			std::int32_t uncompressed_size;
		};

		struct bsp_header_raw_s
		{
			std::int32_t ident;
			std::int32_t version;
			bsp_lump_raw_s lumps[BSP_LUMP_COUNT];
			std::int32_t map_revision;
		};

		struct bsp_lump_view_s
		{
			std::size_t offset = 0u;
			std::size_t length = 0u;
			std::int32_t version = 0;
			std::int32_t uncompressed_size = 0;
		};

		struct world_light_disk_s
		{
			bsp_vec3_disk_s origin;
			bsp_vec3_disk_s intensity;
			bsp_vec3_disk_s normal;
			std::int32_t cluster;
			std::int32_t type;
			std::int32_t style;
			float stopdot;
			float stopdot2;
			float exponent;
			float radius;
			float constant_attn;
			float linear_attn;
			float quadratic_attn;
			std::int32_t flags;
			std::int32_t texinfo;
			std::int32_t owner;
		};

		struct texinfo_disk_s
		{
			float texture_vecs[2][4];
			float lightmap_vecs[2][4];
			std::int32_t flags;
			std::int32_t texdata;
		};

		struct texdata_disk_s
		{
			bsp_vec3_disk_s reflectivity;
			std::int32_t name_string_table_id;
			std::int32_t width;
			std::int32_t height;
			std::int32_t view_width;
			std::int32_t view_height;
		};

		static_assert(sizeof(bsp_header_raw_s) == 1036u);
		static_assert(sizeof(world_light_disk_s) == 88u);
		static_assert(sizeof(texinfo_disk_s) == 72u);
		static_assert(sizeof(texdata_disk_s) == 32u);

		struct vpk_header_s
		{
			std::uint32_t signature = 0u;
			std::uint32_t version = 0u;
			std::uint32_t tree_size = 0u;
			std::uint32_t file_data_section_size = 0u;
			std::uint32_t archive_md5_section_size = 0u;
			std::uint32_t other_md5_section_size = 0u;
			std::uint32_t signature_section_size = 0u;
			std::size_t serialized_size = 0u;
		};

		struct vpk_entry_s
		{
			std::uint32_t crc = 0u;
			std::uint16_t preload_bytes = 0u;
			std::uint16_t archive_index = 0u;
			std::uint32_t entry_offset = 0u;
			std::uint32_t entry_length = 0u;
			std::uint16_t terminator = 0u;
		};

		std::string normalize_slashes_lower(std::string value)
		{
			std::replace(value.begin(), value.end(), '\\', '/');
			return utils::str_to_lower(value);
		}

		bool read_binary_file(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
		{
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file.is_open()) {
				return false;
			}

			const auto end = file.tellg();
			if (end <= 0 || static_cast<std::uint64_t>(end) > MAX_BSP_SIZE) {
				return false;
			}

			out.resize(static_cast<std::size_t>(end));
			file.seekg(0, std::ios::beg);
			file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
			return static_cast<bool>(file);
		}

		template <typename T>
		bool read_pod(const std::vector<std::uint8_t>& data, std::size_t& cursor, T& out)
		{
			if (cursor > data.size() || sizeof(T) > data.size() - cursor) {
				return false;
			}
			std::memcpy(&out, data.data() + cursor, sizeof(T));
			cursor += sizeof(T);
			return true;
		}

		bool read_cstring(const std::vector<std::uint8_t>& data, std::size_t& cursor, const std::size_t end, std::string& out)
		{
			out.clear();
			if (cursor >= end || end > data.size()) {
				return false;
			}

			const auto begin = cursor;
			while (cursor < end && data[cursor] != 0u) {
				++cursor;
			}
			if (cursor >= end) {
				return false;
			}

			out.assign(reinterpret_cast<const char*>(data.data() + begin), cursor - begin);
			++cursor;
			return true;
		}


		bool is_finite_vec(const bsp_vec3_disk_s& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
				std::fabs(v.x) < 1.0e9f && std::fabs(v.y) < 1.0e9f && std::fabs(v.z) < 1.0e9f;
		}

		world_light_disk_s read_worldlight_disk(const std::uint8_t* data, const std::size_t stride)
		{
			world_light_disk_s disk = {};
			disk.owner = -1;
			std::memcpy(&disk, data, std::min(stride, sizeof(disk)));
			return disk;
		}

		float score_worldlight_record(const world_light_disk_s& disk)
		{
			float score = 0.0f;
			if (disk.type >= static_cast<std::int32_t>(emit_type::surface) &&
				disk.type <= static_cast<std::int32_t>(emit_type::skyambient)) score += 7.0f;
			else score -= 20.0f;

			if (is_finite_vec(disk.origin)) score += 2.0f; else score -= 10.0f;
			if (is_finite_vec(disk.intensity)) score += 3.0f; else score -= 10.0f;
			if (is_finite_vec(disk.normal)) score += 2.0f; else score -= 8.0f;

			if (disk.cluster >= -1 && disk.cluster < 4'000'000) score += 1.0f;
			else score -= 3.0f;

			if (disk.style >= 0 && disk.style <= 255) score += 1.0f;
			else score -= 3.0f;

			const std::array<float, 7> scalars = {
				disk.stopdot, disk.stopdot2, disk.exponent, disk.radius,
				disk.constant_attn, disk.linear_attn, disk.quadratic_attn
			};
			for (const auto value : scalars) {
				score += std::isfinite(value) && std::fabs(value) < 1.0e9f ? 0.5f : -3.0f;
			}

			if (std::isfinite(disk.radius) && disk.radius >= 0.0f && disk.radius < 10'000'000.0f) score += 1.0f;
			if (std::isfinite(disk.intensity.x) && std::isfinite(disk.intensity.y) && std::isfinite(disk.intensity.z) &&
				disk.intensity.x >= -0.01f && disk.intensity.y >= -0.01f && disk.intensity.z >= -0.01f) score += 1.0f;

			const auto type = static_cast<emit_type>(disk.type);
			const float normal_length_sqr = disk.normal.x * disk.normal.x + disk.normal.y * disk.normal.y + disk.normal.z * disk.normal.z;
			const bool unit_normal = std::isfinite(normal_length_sqr) && normal_length_sqr >= 0.25f && normal_length_sqr <= 2.25f;
			if (type == emit_type::spotlight)
			{
				score += unit_normal ? 4.0f : -10.0f;
				const bool cone_range = std::isfinite(disk.stopdot) && std::isfinite(disk.stopdot2) &&
					disk.stopdot >= -1.001f && disk.stopdot <= 1.001f && disk.stopdot2 >= -1.001f && disk.stopdot2 <= 1.001f;
				const bool cone_order = cone_range && disk.stopdot + 0.02f >= disk.stopdot2;
				score += cone_range ? 3.0f : -10.0f;
				score += cone_order ? 2.0f : -6.0f;
			}
			else if (type == emit_type::surface || type == emit_type::skylight)
			{
				score += unit_normal ? 3.0f : -7.0f;
			}
			return score;
		}

		struct worldlight_layout_guess_s
		{
			std::size_t base = 0u;
			std::size_t stride = 0u;
			std::size_t count = 0u;
			float average_score = -1000.0f;
		};

		worldlight_layout_guess_s detect_worldlight_layout(const std::uint8_t* data, const std::size_t size,
			const std::optional<std::size_t> forced_count = std::nullopt)
		{
			worldlight_layout_guess_s best = {};
			constexpr std::array<std::size_t, 12> candidate_strides = {
				84u, 88u, 92u, 96u, 100u, 104u, 108u, 112u, 116u, 120u, 124u, 128u
			};
			constexpr std::array<std::size_t, 5> candidate_bases = { 0u, 4u, 8u, 12u, 16u };

			for (const auto base : candidate_bases)
			{
				if (base >= size) continue;
				for (const auto stride : candidate_strides)
				{
					std::size_t count = 0u;
					if (forced_count.has_value())
					{
						count = *forced_count;
						if (count == 0u || count > 262'144u) continue;
						if (count > (size - base) / stride) continue;
					}
					else
					{
						if ((size - base) % stride != 0u) continue;
						count = (size - base) / stride;
						if (count == 0u || count > 262'144u) continue;
					}

					const auto sample_count = std::min<std::size_t>(count, 96u);
					float total = 0.0f;
					for (std::size_t i = 0u; i < sample_count; ++i) {
						total += score_worldlight_record(read_worldlight_disk(data + base + i * stride, stride));
					}
					const float average = total / static_cast<float>(sample_count);
					if (average > best.average_score)
					{
						best.base = base;
						best.stride = stride;
						best.count = count;
						best.average_score = average;
					}
				}
			}
			return best;
		}

		bool make_lump_view(const bsp_lump_raw_s& raw, const bool swapped, const std::size_t file_size, bsp_lump_view_s& out)
		{
			const auto raw_offset = swapped ? raw.field1 : raw.field0;
			const auto raw_length = swapped ? raw.field0 : raw.field1;
			if (raw_offset < 0 || raw_length < 0) return false;

			const auto offset = static_cast<std::size_t>(raw_offset);
			const auto length = static_cast<std::size_t>(raw_length);
			if (length == 0u)
			{
				out = {};
				out.offset = offset;
				out.version = raw.version;
				out.uncompressed_size = raw.uncompressed_size;
				return true;
			}
			if (offset > file_size || length > file_size - offset) return false;

			out.offset = offset;
			out.length = length;
			out.version = raw.version;
			out.uncompressed_size = raw.uncompressed_size;
			return true;
		}

		int score_bsp_lump_layout(const bsp_header_raw_s& header, const std::vector<std::uint8_t>& bytes, const bool swapped)
		{
			int score = 0;
			for (std::size_t i = 0u; i < BSP_LUMP_COUNT; ++i)
			{
				bsp_lump_view_s view = {};
				if (!make_lump_view(header.lumps[i], swapped, bytes.size(), view)) {
					score -= 40;
					continue;
				}
				score += view.length == 0u ? 1 : 4;
				if (view.length > 0u && view.offset >= sizeof(bsp_header_raw_s)) score += 1;
			}

			bsp_lump_view_s entities = {};
			if (make_lump_view(header.lumps[LUMP_ENTITIES], swapped, bytes.size(), entities) && entities.length > 0u)
			{
				const auto probe_len = std::min<std::size_t>(entities.length, 8192u);
				const auto* begin = reinterpret_cast<const char*>(bytes.data() + entities.offset);
				std::string_view probe(begin, probe_len);
				const auto first = probe.find_first_not_of(" \t\r\n\0");
				if (first != std::string_view::npos && probe[first] == '{') score += 60;
				if (probe.find("\"classname\"") != std::string_view::npos) score += 30;
			}

			for (const auto index : { LUMP_WORLDLIGHTS, LUMP_WORLDLIGHTS_HDR })
			{
				bsp_lump_view_s view = {};
				if (!make_lump_view(header.lumps[index], swapped, bytes.size(), view) || view.length == 0u) continue;
				if (view.length % sizeof(world_light_disk_s) == 0u) score += 25;
				const auto guess = detect_worldlight_layout(bytes.data() + view.offset, view.length);
				if (guess.stride != 0u && guess.average_score >= 8.0f) score += 15;
			}
			return score;
		}

		bool parse_bsp_header(const std::vector<std::uint8_t>& bytes, bsp_header_raw_s& header, bool& swapped)
		{
			if (bytes.size() < sizeof(header)) return false;
			std::memcpy(&header, bytes.data(), sizeof(header));
			if (header.ident != VBSP_IDENT) return false;

			const int standard_score = score_bsp_lump_layout(header, bytes, false);
			const int swapped_score = score_bsp_lump_layout(header, bytes, true);
			swapped = swapped_score > standard_score + 8;
			return true;
		}

		bool is_readable_memory_range(const void* address, const std::size_t length)
		{
			if (!address || length == 0u) return false;
			std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(address);
			const std::uintptr_t end = cursor + static_cast<std::uintptr_t>(length);
			if (end < cursor) return false;

			while (cursor < end)
			{
				MEMORY_BASIC_INFORMATION mbi = {};
				if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
				if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0u || (mbi.Protect & PAGE_NOACCESS) != 0u) return false;
				const std::uintptr_t region_begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
				const std::uintptr_t region_end = region_begin + static_cast<std::uintptr_t>(mbi.RegionSize);
				if (region_end <= cursor) return false;
				cursor = (region_end < end) ? region_end : end;
			}
			return true;
		}

		bool append_worldlights(const std::uint8_t* data, const worldlight_layout_guess_s& layout, std::vector<world_light_s>& out)
		{
			if (!data || layout.stride < 84u || layout.count == 0u || layout.average_score < 8.0f) return false;
			out.clear();
			out.reserve(layout.count);
			for (std::size_t i = 0u; i < layout.count; ++i)
			{
				const auto disk = read_worldlight_disk(data + layout.base + i * layout.stride, layout.stride);
				if (disk.type < static_cast<std::int32_t>(emit_type::surface) ||
					disk.type > static_cast<std::int32_t>(emit_type::skyambient))
				{
					continue;
				}
				if (!is_finite_vec(disk.origin) || !is_finite_vec(disk.intensity) || !is_finite_vec(disk.normal)) continue;

				world_light_s light = {};
				light.origin = Vector(disk.origin.x, disk.origin.y, disk.origin.z);
				light.intensity = Vector(disk.intensity.x, disk.intensity.y, disk.intensity.z);
				light.normal = Vector(disk.normal.x, disk.normal.y, disk.normal.z);
				light.cluster = disk.cluster;
				light.type = static_cast<emit_type>(disk.type);
				light.style = disk.style;
				light.stopdot = disk.stopdot;
				light.stopdot2 = disk.stopdot2;
				light.exponent = disk.exponent;
				light.radius = disk.radius;
				light.constant_attn = disk.constant_attn;
				light.linear_attn = disk.linear_attn;
				light.quadratic_attn = disk.quadratic_attn;
				light.flags = disk.flags;
				light.texinfo = disk.texinfo;
				light.owner = layout.stride >= sizeof(world_light_disk_s) ? disk.owner : -1;
				out.emplace_back(light);
			}
			return !out.empty();
		}

		bool parse_vpk_header(const std::vector<std::uint8_t>& data, vpk_header_s& header)
		{
			std::size_t cursor = 0u;
			if (!read_pod(data, cursor, header.signature) || header.signature != VPK_SIGNATURE ||
				!read_pod(data, cursor, header.version) || !read_pod(data, cursor, header.tree_size))
			{
				return false;
			}

			if (header.version == 1u)
			{
				header.serialized_size = cursor;
				return true;
			}

			if (header.version != 2u) {
				return false;
			}

			if (!read_pod(data, cursor, header.file_data_section_size) ||
				!read_pod(data, cursor, header.archive_md5_section_size) ||
				!read_pod(data, cursor, header.other_md5_section_size) ||
				!read_pod(data, cursor, header.signature_section_size))
			{
				return false;
			}

			header.serialized_size = cursor;
			return true;
		}

		std::filesystem::path numbered_archive_path(const std::filesystem::path& directory_vpk, const std::uint16_t archive_index)
		{
			const auto stem = directory_vpk.stem().string();
			std::string base = stem;
			if (base.size() >= 4u && utils::str_to_lower(base.substr(base.size() - 4u)) == "_dir") {
				base.resize(base.size() - 4u);
			}

			return directory_vpk.parent_path() / std::format("{}_{:03}.vpk", base, archive_index);
		}

		bool read_vpk_entry(const std::filesystem::path& vpk_path, const std::string& requested_path,
			std::vector<std::uint8_t>& out, std::string& error)
		{
			// Do not read a whole single-file addon VPK just to inspect its directory tree.
			// Workshop packages can be several gigabytes while the tree is usually small.
			std::ifstream directory_file(vpk_path, std::ios::binary | std::ios::ate);
			if (!directory_file.is_open()) {
				return false;
			}
			const auto file_end = directory_file.tellg();
			const auto file_size = static_cast<std::streamoff>(file_end);
			if (file_size < 12) {
				return false;
			}
			directory_file.seekg(0, std::ios::beg);

			std::vector<std::uint8_t> directory_data(12u);
			directory_file.read(reinterpret_cast<char*>(directory_data.data()), static_cast<std::streamsize>(directory_data.size()));
			if (!directory_file) {
				return false;
			}

			std::uint32_t version = 0u;
			std::memcpy(&version, directory_data.data() + sizeof(std::uint32_t), sizeof(version));
			if (version == 2u)
			{
				directory_data.resize(28u);
				directory_file.read(reinterpret_cast<char*>(directory_data.data() + 12u), 16);
				if (!directory_file) {
					return false;
				}
			}

			vpk_header_s header = {};
			if (!parse_vpk_header(directory_data, header) || header.tree_size > MAX_VPK_TREE_SIZE) {
				return false;
			}

			const std::size_t tree_begin = header.serialized_size;
			const std::size_t tree_end = tree_begin + static_cast<std::size_t>(header.tree_size);
			if (tree_end < tree_begin || static_cast<std::uint64_t>(tree_end) > static_cast<std::uint64_t>(file_size)) {
				error = "invalid VPK directory tree";
				return false;
			}

			directory_data.resize(tree_end);
			directory_file.seekg(static_cast<std::streamoff>(tree_begin), std::ios::beg);
			directory_file.read(reinterpret_cast<char*>(directory_data.data() + tree_begin), static_cast<std::streamsize>(header.tree_size));
			if (!directory_file) {
				error = "failed to read VPK directory tree";
				return false;
			}

			const std::string requested = normalize_slashes_lower(requested_path);
			std::size_t cursor = tree_begin;
			std::string extension;
			while (read_cstring(directory_data, cursor, tree_end, extension) && !extension.empty())
			{
				std::string path;
				while (read_cstring(directory_data, cursor, tree_end, path) && !path.empty())
				{
					std::string filename;
					while (read_cstring(directory_data, cursor, tree_end, filename) && !filename.empty())
					{
						vpk_entry_s entry = {};
						if (!read_pod(directory_data, cursor, entry.crc) ||
							!read_pod(directory_data, cursor, entry.preload_bytes) ||
							!read_pod(directory_data, cursor, entry.archive_index) ||
							!read_pod(directory_data, cursor, entry.entry_offset) ||
							!read_pod(directory_data, cursor, entry.entry_length) ||
							!read_pod(directory_data, cursor, entry.terminator) ||
							entry.terminator != VPK_ENTRY_TERMINATOR)
						{
							error = "invalid VPK entry";
							return false;
						}

						if (cursor > tree_end || entry.preload_bytes > tree_end - cursor) {
							error = "invalid VPK preload data";
							return false;
						}

						const std::string normalized_path = path == " " ? std::string{} : path;
						const std::string full_path = normalize_slashes_lower(
							(normalized_path.empty() ? std::string{} : normalized_path + "/") + filename + "." + extension);
						const auto* preload_ptr = directory_data.data() + cursor;
						cursor += entry.preload_bytes;

						if (full_path != requested) {
							continue;
						}

						const std::size_t total_size = static_cast<std::size_t>(entry.preload_bytes) + entry.entry_length;
						if (total_size == 0u || total_size > MAX_BSP_SIZE) {
							error = "invalid VPK BSP entry size";
							return false;
						}

						out.resize(total_size);
						if (entry.preload_bytes > 0u) {
							std::memcpy(out.data(), preload_ptr, entry.preload_bytes);
						}

						if (entry.entry_length == 0u) {
							return true;
						}

						std::filesystem::path data_path = vpk_path;
						std::uint64_t data_offset = entry.entry_offset;
						if (entry.archive_index == VPK_ARCHIVE_DIRECTORY)
						{
							data_offset += static_cast<std::uint64_t>(header.serialized_size) + header.tree_size;
						}
						else {
							data_path = numbered_archive_path(vpk_path, entry.archive_index);
						}

						std::ifstream data_file(data_path, std::ios::binary);
						if (!data_file.is_open()) {
							error = std::format("could not open VPK data archive {}", data_path.string());
							return false;
						}

						data_file.seekg(static_cast<std::streamoff>(data_offset), std::ios::beg);
						data_file.read(reinterpret_cast<char*>(out.data() + entry.preload_bytes), entry.entry_length);
						if (!data_file) {
							error = "failed to read BSP from VPK";
							out.clear();
							return false;
						}

						return true;
					}
				}
			}

			return false;
		}

		int vpk_priority(const std::filesystem::path& path)
		{
			const auto lower = normalize_slashes_lower(path.string());
			// Core archives are checked first so an official campaign does not open every
			// installed Workshop VPK. A successful addon VPK is cached and tried first for
			// the following maps in the same custom campaign.
			if (lower.find("/update/") != std::string::npos) return 600;
			if (lower.find("left4dead2_dlc3") != std::string::npos) return 530;
			if (lower.find("left4dead2_dlc2") != std::string::npos) return 520;
			if (lower.find("left4dead2_dlc1") != std::string::npos) return 510;
			if (lower.find("/left4dead2/") != std::string::npos && lower.find("/addons/") == std::string::npos) return 400;
			if (lower.find("/addons/") != std::string::npos) return 300;
			return 100;
		}

		std::vector<std::filesystem::path> collect_vpk_candidates(const std::filesystem::path& root)
		{
			std::vector<std::filesystem::path> result;
			std::error_code ec;
			if (!std::filesystem::exists(root, ec)) {
				return result;
			}

			std::filesystem::recursive_directory_iterator it(root,
				std::filesystem::directory_options::skip_permission_denied, ec);
			const std::filesystem::recursive_directory_iterator end;
			for (; !ec && it != end; it.increment(ec))
			{
				if (it.depth() > 3) {
					it.disable_recursion_pending();
					continue;
				}
				if (!it->is_regular_file(ec)) {
					continue;
				}

				const auto extension = utils::str_to_lower(it->path().extension().string());
				if (extension != ".vpk") {
					continue;
				}

				const auto stem = utils::str_to_lower(it->path().stem().string());
				const bool is_dir_vpk = stem.ends_with("_dir");
				const auto lower_path = normalize_slashes_lower(it->path().string());
				const bool is_addon_vpk = lower_path.find("/addons/") != std::string::npos;
				if (is_dir_vpk || is_addon_vpk) {
					result.emplace_back(it->path());
				}
			}

			std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs)
			{
				const int lp = vpk_priority(lhs);
				const int rp = vpk_priority(rhs);
				if (lp != rp) return lp > rp;
				return lhs.string() > rhs.string();
			});
			result.erase(std::unique(result.begin(), result.end()), result.end());
			return result;
		}


		const std::vector<std::filesystem::path>& cached_vpk_candidates(const std::filesystem::path& root)
		{
			static std::string cached_root;
			static std::vector<std::filesystem::path> cached_candidates;
			const auto key = normalize_slashes_lower(root.lexically_normal().string());
			if (key != cached_root)
			{
				cached_root = key;
				cached_candidates = collect_vpk_candidates(root);
			}
			return cached_candidates;
		}

		bool validate_bsp(const std::vector<std::uint8_t>& bytes, std::int32_t& version)
		{
			bsp_header_raw_s header = {};
			bool swapped = false;
			if (!parse_bsp_header(bytes, header, swapped)) return false;
			(void)swapped;
			version = header.version;
			return true;
		}

		bool get_lump_view(const map_file_s& map, const std::size_t index,
			const std::uint8_t*& data, std::size_t& size, std::int32_t& version,
			bool& swapped, std::string& error)
		{
			if (index >= BSP_LUMP_COUNT) {
				error = "invalid BSP lump index";
				return false;
			}

			bsp_header_raw_s header = {};
			if (!parse_bsp_header(map.bytes, header, swapped)) {
				error = "invalid BSP header";
				return false;
			}

			bsp_lump_view_s view = {};
			if (!make_lump_view(header.lumps[index], swapped, map.bytes.size(), view)) {
				const auto& raw = header.lumps[index];
				error = std::format("BSP lump {} range invalid (raw {}, {}, layout {})", index,
					raw.field0, raw.field1, swapped ? "swapped" : "standard");
				return false;
			}
			if (view.length == 0u) {
				error = std::format("BSP lump {} is empty", index);
				return false;
			}
			if (view.uncompressed_size != 0) {
				error = std::format("BSP lump {} is compressed (disk {}, unpacked {})", index,
					view.length, view.uncompressed_size);
				return false;
			}

			data = map.bytes.data() + view.offset;
			size = view.length;
			version = view.version;
			return true;
		}

	}

	bool load_map_file(const std::string& game_root, const std::string& map_name, map_file_s& out, std::string& error)
	{
		out = {};
		error.clear();
		if (game_root.empty() || map_name.empty()) {
			error = "missing game root or map name";
			return false;
		}

		std::string clean_map = normalize_slashes_lower(map_name);
		if (clean_map.starts_with("maps/")) clean_map.erase(0u, 5u);
		if (clean_map.ends_with(".bsp")) clean_map.resize(clean_map.size() - 4u);

		const std::filesystem::path root(game_root);
		const std::array<std::filesystem::path, 7> loose_paths = {
			root / "update" / "maps" / (clean_map + ".bsp"),
			root / "left4dead2_dlc3" / "maps" / (clean_map + ".bsp"),
			root / "left4dead2_dlc2" / "maps" / (clean_map + ".bsp"),
			root / "left4dead2_dlc1" / "maps" / (clean_map + ".bsp"),
			root / "left4dead2" / "maps" / (clean_map + ".bsp"),
			root / "maps" / (clean_map + ".bsp"),
			root.parent_path() / "left4dead2" / "maps" / (clean_map + ".bsp"),
		};

		for (const auto& path : loose_paths)
		{
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec)) {
				continue;
			}
			if (read_binary_file(path, out.bytes) && validate_bsp(out.bytes, out.bsp_version))
			{
				out.source_path = path.string();
				out.from_vpk = false;
				return true;
			}
		}

		const auto& vpks = cached_vpk_candidates(root);
		const std::string vpk_entry = "maps/" + clean_map + ".bsp";
		static std::string last_root;
		static std::filesystem::path last_successful_vpk;
		const auto root_key = normalize_slashes_lower(root.lexically_normal().string());
		if (root_key != last_root)
		{
			last_root = root_key;
			last_successful_vpk.clear();
		}

		auto try_vpk = [&](const std::filesystem::path& vpk) -> bool
		{
			std::vector<std::uint8_t> bytes;
			std::string vpk_error;
			if (!read_vpk_entry(vpk, vpk_entry, bytes, vpk_error)) {
				return false;
			}
			std::int32_t bsp_version = 0;
			if (!validate_bsp(bytes, bsp_version)) {
				error = std::format("invalid BSP entry in {}", vpk.string());
				return false;
			}

			out.bytes = std::move(bytes);
			out.source_path = std::format("{} :: {}", vpk.string(), vpk_entry);
			out.from_vpk = true;
			out.bsp_version = bsp_version;
			last_successful_vpk = vpk;
			return true;
		};

		if (!last_successful_vpk.empty() && try_vpk(last_successful_vpk)) {
			return true;
		}
		for (const auto& vpk : vpks)
		{
			if (!last_successful_vpk.empty() && vpk == last_successful_vpk) {
				continue;
			}
			if (try_vpk(vpk)) {
				return true;
			}
		}

		error = std::format("maps/{}.bsp not found as loose file or in scanned VPK archives", clean_map);
		return false;
	}

	bool read_texinfo_materials(const map_file_s& map, std::unordered_map<std::int32_t, std::string>& out, std::string& error)
	{
		out.clear();
		error.clear();
		const std::uint8_t* texinfo_data = nullptr;
		const std::uint8_t* texdata_data = nullptr;
		const std::uint8_t* string_data = nullptr;
		const std::uint8_t* string_table_data = nullptr;
		std::size_t texinfo_size = 0u, texdata_size = 0u, string_data_size = 0u, string_table_size = 0u;
		std::int32_t version = 0;
		bool swapped = false;
		if (!get_lump_view(map, LUMP_TEXINFO, texinfo_data, texinfo_size, version, swapped, error)) return false;
		if (!get_lump_view(map, LUMP_TEXDATA, texdata_data, texdata_size, version, swapped, error)) return false;
		if (!get_lump_view(map, LUMP_TEXDATA_STRING_DATA, string_data, string_data_size, version, swapped, error)) return false;
		if (!get_lump_view(map, LUMP_TEXDATA_STRING_TABLE, string_table_data, string_table_size, version, swapped, error)) return false;
		if (texinfo_size % sizeof(texinfo_disk_s) != 0u || texdata_size % sizeof(texdata_disk_s) != 0u ||
			string_table_size % sizeof(std::int32_t) != 0u)
		{
			error = "invalid TEXINFO/TEXDATA lump layout";
			return false;
		}

		const auto texinfo_count = texinfo_size / sizeof(texinfo_disk_s);
		const auto texdata_count = texdata_size / sizeof(texdata_disk_s);
		const auto string_count = string_table_size / sizeof(std::int32_t);
		for (std::size_t i = 0u; i < texinfo_count; ++i)
		{
			texinfo_disk_s info = {};
			std::memcpy(&info, texinfo_data + i * sizeof(info), sizeof(info));
			if (info.texdata < 0 || static_cast<std::size_t>(info.texdata) >= texdata_count) continue;
			texdata_disk_s data = {};
			std::memcpy(&data, texdata_data + static_cast<std::size_t>(info.texdata) * sizeof(data), sizeof(data));
			if (data.name_string_table_id < 0 || static_cast<std::size_t>(data.name_string_table_id) >= string_count) continue;
			std::int32_t string_offset = -1;
			std::memcpy(&string_offset, string_table_data + static_cast<std::size_t>(data.name_string_table_id) * sizeof(string_offset), sizeof(string_offset));
			if (string_offset < 0 || static_cast<std::size_t>(string_offset) >= string_data_size) continue;
			const char* begin = reinterpret_cast<const char*>(string_data + string_offset);
			const char* end = reinterpret_cast<const char*>(string_data + string_data_size);
			const char* nul = std::find(begin, end, '\0');
			if (nul == begin || nul == end) continue;
			std::string material(begin, nul);
			material = normalize_slashes_lower(std::move(material));
			while (material.starts_with("materials/")) material.erase(0u, 10u);
			if (material.ends_with(".vmt")) material.resize(material.size() - 4u);
			out.emplace(static_cast<std::int32_t>(i), std::move(material));
		}
		if (out.empty())
		{
			error = "TEXINFO material table contained no usable names";
			return false;
		}
		return true;
	}

	bool read_entity_lump(const map_file_s& map, std::string& out, std::string& error)
	{
		out.clear();
		const std::uint8_t* data = nullptr;
		std::size_t size = 0u;
		std::int32_t version = 0;
		bool swapped = false;
		if (!get_lump_view(map, LUMP_ENTITIES, data, size, version, swapped, error)) {
			return false;
		}
		(void)version;
		(void)swapped;
		out.assign(reinterpret_cast<const char*>(data), size);
		while (!out.empty() && out.back() == '\0') out.pop_back();
		return !out.empty();
	}

	bool read_world_lights(const map_file_s& map, const bool prefer_hdr, world_light_result_s& out)
	{
		out = {};
		out.source_path = map.source_path;
		out.from_vpk = map.from_vpk;
		out.backend = map.from_vpk ? source_backend::vpk_bsp : source_backend::loose_bsp;
		out.bsp_version = map.bsp_version;

		const std::array<std::size_t, 2> order = prefer_hdr
			? std::array<std::size_t, 2>{ LUMP_WORLDLIGHTS_HDR, LUMP_WORLDLIGHTS }
			: std::array<std::size_t, 2>{ LUMP_WORLDLIGHTS, LUMP_WORLDLIGHTS_HDR };

		std::string last_error;
		for (const auto lump_index : order)
		{
			const std::uint8_t* data = nullptr;
			std::size_t size = 0u;
			std::int32_t lump_version = 0;
			bool swapped = false;
			std::string error;
			if (!get_lump_view(map, lump_index, data, size, lump_version, swapped, error))
			{
				last_error = error;
				continue;
			}

			const auto layout = detect_worldlight_layout(data, size);
			if (layout.stride == 0u || layout.average_score < 8.0f)
			{
				bsp_header_raw_s header = {};
				bool header_swapped = false;
				parse_bsp_header(map.bytes, header, header_swapped);
				const auto& raw = header.lumps[lump_index];
				last_error = std::format(
					"WORLDLIGHTS lump {} could not be decoded: size {}, raw fields {}/{}, layout {}, best stride {}, score {:.2f}",
					lump_index, size, raw.field0, raw.field1, swapped ? "swapped" : "standard",
					layout.stride, layout.average_score);
				continue;
			}

			if (!append_worldlights(data, layout, out.lights))
			{
				last_error = std::format("WORLDLIGHTS lump {} decoded no valid records", lump_index);
				continue;
			}

			out.used_hdr = lump_index == LUMP_WORLDLIGHTS_HDR;
			out.lump_version = lump_version;
			out.swapped_lump_fields = swapped;
			out.record_stride = layout.stride;
			out.source_bytes = size;
			out.diagnostics = std::format("BSP {} fields, stride {}, base {}, score {:.2f}",
				swapped ? "swapped" : "standard", layout.stride, layout.base, layout.average_score);
			std::unordered_map<std::int32_t, std::string> texinfo_materials;
			std::string texinfo_error;
			if (read_texinfo_materials(map, texinfo_materials, texinfo_error))
			{
				std::size_t linked = 0u;
				for (auto& light : out.lights)
				{
					if (const auto it = texinfo_materials.find(light.texinfo); it != texinfo_materials.end())
					{
						light.material_name = it->second;
						++linked;
					}
				}
				out.diagnostics += std::format(" | texinfo materials linked {}", linked);
			}
			else if (!texinfo_error.empty()) out.diagnostics += std::format(" | texinfo material linkage: {}", texinfo_error);
			return true;
		}

		out.error = last_error.empty() ? "WORLDLIGHTS and WORLDLIGHTS_HDR are unavailable" : last_error;
		return false;
	}

	bool read_world_lights_from_engine(const void* worldbrush, world_light_result_s& out)
	{
		out = {};
		out.backend = source_backend::engine_memory;
		out.source_path = "engine worldbrush memory";
		if (!worldbrush)
		{
			out.error = "engine worldbrush is null";
			return false;
		}

		constexpr std::size_t scan_begin = 0x54u;
		constexpr std::size_t scan_end = 0x180u;
		constexpr std::size_t max_count = 262'144u;
		if (!is_readable_memory_range(worldbrush, scan_end))
		{
			out.error = "engine worldbrush header is not readable";
			return false;
		}

		struct candidate_s
		{
			std::size_t count_offset = 0u;
			std::size_t pointer_offset = 0u;
			std::size_t count = 0u;
			const std::uint8_t* data = nullptr;
			worldlight_layout_guess_s layout = {};
		};

		candidate_s best = {};
		for (std::size_t offset = scan_begin; offset + 8u <= scan_end; offset += 4u)
		{
			std::int32_t signed_count = 0;
			std::uint32_t pointer32 = 0u;
			std::memcpy(&signed_count, static_cast<const std::uint8_t*>(worldbrush) + offset, sizeof(signed_count));
			std::memcpy(&pointer32, static_cast<const std::uint8_t*>(worldbrush) + offset + 4u, sizeof(pointer32));
			if (signed_count <= 0 || static_cast<std::size_t>(signed_count) > max_count || pointer32 < 0x10000u) continue;

			const auto count = static_cast<std::size_t>(signed_count);
			const auto* pointer = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(pointer32));
			constexpr std::size_t largest_stride = 128u;
			if (count > (std::numeric_limits<std::size_t>::max)() / largest_stride) continue;
			const auto requested = count * largest_stride;
			if (!is_readable_memory_range(pointer, std::min<std::size_t>(requested, 96u * largest_stride))) continue;

			const auto sample_bytes = std::min<std::size_t>(count, 96u) * largest_stride;
			const auto layout = detect_worldlight_layout(pointer, sample_bytes, std::min<std::size_t>(count, 96u));
			if (layout.base != 0u || layout.stride == 0u || layout.average_score < 8.0f) continue;

			if (layout.average_score > best.layout.average_score)
			{
				best.count_offset = offset;
				best.pointer_offset = offset + 4u;
				best.count = count;
				best.data = pointer;
				best.layout = layout;
			}
		}

		if (!best.data || best.layout.stride == 0u)
		{
			out.error = "engine worldlight array was not found in worldbrush";
			return false;
		}

		if (best.count > (std::numeric_limits<std::size_t>::max)() / best.layout.stride)
		{
			out.error = "engine worldlight array size overflow";
			return false;
		}
		const auto full_size = best.count * best.layout.stride;
		if (!is_readable_memory_range(best.data, full_size))
		{
			out.error = "engine worldlight array is not fully readable";
			return false;
		}

		best.layout.base = 0u;
		best.layout.count = best.count;
		if (!append_worldlights(best.data, best.layout, out.lights))
		{
			out.error = "engine worldlight array contained no valid records";
			return false;
		}

		out.record_stride = best.layout.stride;
		out.source_bytes = full_size;
		out.runtime_count_offset = static_cast<std::ptrdiff_t>(best.count_offset);
		out.runtime_pointer_offset = static_cast<std::ptrdiff_t>(best.pointer_offset);
		out.diagnostics = std::format("worldbrush count +0x{:X}, pointer +0x{:X}, stride {}, score {:.2f}",
			best.count_offset, best.pointer_offset, best.layout.stride, best.layout.average_score);
		return true;
	}


	const char* emit_type_name(const emit_type type)
	{
		switch (type)
		{
		case emit_type::surface: return "surface";
		case emit_type::point: return "point";
		case emit_type::spotlight: return "spotlight";
		case emit_type::skylight: return "skylight";
		case emit_type::quakelight: return "quakelight";
		case emit_type::skyambient: return "skyambient";
		default: return "unknown";
		}
	}
	const char* source_backend_name(const source_backend backend)
	{
		switch (backend)
		{
		case source_backend::engine_memory: return "engine memory";
		case source_backend::loose_bsp: return "loose BSP";
		case source_backend::vpk_bsp: return "VPK BSP";
		case source_backend::none:
		default: return "none";
		}
	}

}
