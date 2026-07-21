#pragma once

namespace components::source_bsp_lights
{
	enum class emit_type : std::int32_t
	{
		surface = 0,
		point = 1,
		spotlight = 2,
		skylight = 3,
		quakelight = 4,
		skyambient = 5,
	};

	enum class source_backend : std::uint8_t
	{
		none,
		engine_memory,
		loose_bsp,
		vpk_bsp,
	};

	struct map_file_s
	{
		std::vector<std::uint8_t> bytes;
		std::string source_path;
		bool from_vpk = false;
		std::int32_t bsp_version = 0;
	};

	struct world_light_s
	{
		Vector origin;
		Vector intensity;
		Vector normal;
		std::int32_t cluster = -1;
		emit_type type = emit_type::point;
		std::int32_t style = 0;
		float stopdot = 0.0f;
		float stopdot2 = 0.0f;
		float exponent = 0.0f;
		float radius = 0.0f;
		float constant_attn = 0.0f;
		float linear_attn = 0.0f;
		float quadratic_attn = 0.0f;
		std::int32_t flags = 0;
		std::int32_t texinfo = -1;
		std::string material_name;
		std::int32_t owner = -1;
	};

	struct world_light_result_s
	{
		std::vector<world_light_s> lights;
		std::string source_path;
		std::string error;
		std::string diagnostics;
		source_backend backend = source_backend::none;
		bool from_vpk = false;
		bool used_hdr = false;
		bool swapped_lump_fields = false;
		std::int32_t bsp_version = 0;
		std::int32_t lump_version = 0;
		std::size_t record_stride = 0u;
		std::size_t source_bytes = 0u;
		std::ptrdiff_t runtime_count_offset = -1;
		std::ptrdiff_t runtime_pointer_offset = -1;
	};

	bool load_map_file(const std::string& game_root, const std::string& map_name, map_file_s& out, std::string& error);
	bool read_entity_lump(const map_file_s& map, std::string& out, std::string& error);
	bool read_texinfo_materials(const map_file_s& map, std::unordered_map<std::int32_t, std::string>& out, std::string& error);
	bool read_world_lights(const map_file_s& map, bool prefer_hdr, world_light_result_s& out);
	bool read_world_lights_from_engine(const void* worldbrush, world_light_result_s& out);
	const char* emit_type_name(emit_type type);
	const char* source_backend_name(source_backend backend);
}
