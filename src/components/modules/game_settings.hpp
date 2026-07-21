#pragma once

namespace components
{
	class game_settings : public component
	{
	public:
		game_settings();
		~game_settings() = default;

		static inline game_settings* p_this = nullptr;
		static auto get() { return &vars; }

		static void write_toml();
		static bool parse_toml();
		static void mark_dirty(const char* reason = nullptr);
		static void on_frame();
		static bool is_dirty() { return m_dirty; }
		static bool last_save_succeeded() { return m_last_save_succeeded; }
		static const std::string& last_save_status() { return m_last_save_status; }
		static std::uint64_t save_generation() { return m_save_generation; }
		static void reset_all_to_defaults();

		static void xo_gamesettings_update_fn();

	private:
		static void apply_persistent_quick_actions();
		union var_value
		{
			bool boolean;
			int integer;
			float value[4] = {};
		};

		enum var_type : std::uint8_t
		{
			var_type_boolean = 0,
			var_type_integer = 1,
			var_type_value = 2,
			var_type_vec2 = 3,
			var_type_vec3 = 4,
			var_type_vec4 = 5,
		};

		class variable
		{
		public:
			// bool
			variable(const char* name, const char* desc, const bool boolean) :
				m_name(name), m_desc(desc), m_type(var_type_boolean)
			{
				m_var.boolean = boolean;
				m_var_default.boolean = boolean;
			}

			// int
			variable(const char* name, const char* desc, const int integer) :
				m_name(name), m_desc(desc), m_type(var_type_integer)
			{
				m_var.integer = integer;
				m_var_default.integer = integer;
			}

			// float
			variable(const char* name, const char* desc, const float value) :
				m_name(name), m_desc(desc), m_type(var_type_value)
			{
				m_var.value[0] = value;
				m_var_default.value[0] = value;
			}

			// vec2
			variable(const char* name, const char* desc, const float x, const float y) :
				m_name(name), m_desc(desc), m_type(var_type_vec2)
			{
				m_var.value[0] = x; m_var.value[1] = y;
				m_var_default.value[0] = x; m_var_default.value[1] = y;
			}

			// vec3
			variable(const char* name, const char* desc, const float x, const float y, const float z) :
				m_name(name), m_desc(desc), m_type(var_type_vec3)
			{
				m_var.value[0] = x; m_var.value[1] = y; m_var.value[2] = z;
				m_var_default.value[0] = x; m_var_default.value[1] = y; m_var_default.value[2] = z;
			}

			// vec4
			variable(const char* name, const char* desc, const float x, const float y, const float z, const float w) :
				m_name(name), m_desc(desc), m_type(var_type_vec4)
			{
				m_var.value[0] = x; m_var.value[1] = y; m_var.value[2] = z; m_var.value[3] = w;
				m_var_default.value[0] = x; m_var_default.value[1] = y; m_var_default.value[2] = z; m_var_default.value[3] = w;
			}

			const char* get_str_value(bool get_default = false) const
			{
				const auto pvec = !get_default ? &m_var.value[0] : &m_var_default.value[0];

				switch (m_type)
				{
				case var_type_boolean:
					return utils::va("%s", (!get_default ? m_var.boolean : m_var_default.boolean) ? "true" : "false");

				case var_type_integer:
					return utils::va("%d", !get_default ? m_var.integer : m_var_default.integer);

				case var_type_value:
					return utils::va("%.9g", pvec[0]);

				case var_type_vec2:
					return utils::va("[ %.9g, %.9g ]", pvec[0], pvec[1]);
				
				case var_type_vec3:
					return utils::va("[ %.9g, %.9g, %.9g ]", pvec[0], pvec[1], pvec[2]);

				case var_type_vec4:
					return utils::va("[ %.9g, %.9g, %.9g, %.9g ]", pvec[0], pvec[1], pvec[2], pvec[3]);

				}

				return nullptr;
			}

			const char* get_str_type() const
			{
				switch (m_type)
				{
				case var_type_boolean:
					return "BOOL";

				case var_type_integer:
					return "INT";

				case var_type_value:
					return "FLOAT";

				case var_type_vec2:
					return "VEC2";

				case var_type_vec3:
					return "VEC3";

				case var_type_vec4:
					return "VEC4";
				}

				return nullptr;
			}

			std::string get_tooltip_string() const
			{
				std::string out;
				out += "# " + std::string(this->m_desc) + "\n";
				out += "# Type: " + std::string(this->get_str_type()) + " || Default: " + std::string(this->get_str_value(true));
				return out;
			}

			template <typename T>
			T get_as(bool default_val = false)
			{
				// if T is a pointer type, return a ptr
				if constexpr (std::is_pointer_v<T>) 
				{
					// get the underlying type (e.g., int from int*)
					using base_type = std::remove_pointer_t<T>;

					if constexpr (std::is_same_v<base_type, bool>) {
						return &(!default_val ? m_var.boolean : m_var_default.boolean);
					}

					else if constexpr (std::is_same_v<base_type, int>) {
						return &(!default_val ? m_var.integer : m_var_default.integer);
					}

					else if constexpr (std::is_same_v<base_type, float>) {
						return &(!default_val ? m_var.value[0] : m_var_default.value[0]);
					}

					// vec2, vec3, vec4 
					else if constexpr (std::is_same_v<base_type, float[4]>) { 
						return !default_val ? m_var.value : m_var_default.value;
					}

					else {
						static_assert(std::is_same_v<T, void>, "Unsupported pointer type in get_as");
						return nullptr;
					}
				}

				// return by value for non-pointer types
				else 
				{
					if constexpr (std::is_same_v<T, bool>) {
						return static_cast<T>(!default_val ? m_var.boolean : m_var_default.boolean);
					}

					else if constexpr (std::is_same_v<T, int>) {
						return static_cast<T>(!default_val ? m_var.integer : m_var_default.integer);
					}

					else if constexpr (std::is_same_v<T, float>) {
						return static_cast<T>(!default_val ? m_var.value[0] : m_var_default.value[0]);
					}

					else {
						static_assert(std::is_same_v<T, void>, "Unsupported return type in get_as");
						return 0;
					}
				}
			}

			var_type get_type() const {
				return m_type;
			}

			// sets var and writes toml (bool)
			void set_var(const bool boolean, bool no_toml_update = false)
			{
				if (m_var.boolean == boolean) return;
				m_var.boolean = boolean;
				if (!no_toml_update) mark_dirty(m_name);
			}

			// sets var and writes toml (integer)
			void set_var(const int integer, bool no_toml_update = false)
			{
				if (m_var.integer == integer) return;
				m_var.integer = integer;
				if (!no_toml_update) mark_dirty(m_name);
			}

			// sets var and writes toml (float)
			void set_var(const float value, bool no_toml_update = false)
			{
				if (m_var.value[0] == value) return;
				m_var.value[0] = value;
				if (!no_toml_update) mark_dirty(m_name);
			}

			// sets var and writes toml (vec4)
			void set_vec(const float* v, bool no_toml_update = false)
			{
				if (!v) return;
				const std::size_t count = m_type == var_type_vec2 ? 2u : m_type == var_type_vec3 ? 3u : m_type == var_type_vec4 ? 4u : 1u;
				bool changed = false;
				for (std::size_t i = 0; i < count; ++i) changed = changed || m_var.value[i] != v[i];
				if (!changed) return;
				for (std::size_t i = 0; i < count; ++i) m_var.value[i] = v[i];
				if (!no_toml_update) mark_dirty(m_name);
			}

			void reset_to_default(bool no_toml_update = false)
			{
				m_var = m_var_default;
				if (!no_toml_update) mark_dirty(m_name);
			}

			const char* m_name;
			const char* m_desc;

		private:
			var_value m_var;
			var_value m_var_default;
			var_type m_type;
		};

		// note:
		// cba. to impl. automatic detection of newlines in comments -> add '# ' manually :>

		struct var_definitions
		{
			variable lod_forcing =
			{
				"lod_forcing",
				"The mod normally forces LOD0 for everything. Setting this to false disables that.",
				true
			};

			variable force_graphic_settings =
			{
				"force_graphic_settings",
				"This forces required graphic settings (Shader/Effect etc.)",
				true
			};

			variable source_threading_mode =
			{
				"source_threading_mode",
				("Experimental Source renderer threading mode. 0 = safe forced single-threaded compat path, "
				 "1 = stable hybrid non-render worker cvars with mat_queue_mode 0, "
				 "2 = queued renderer with flicker guards, 3 = queued renderer performance profile, "
				 "4 = unsafe stress test. Modes 2-4 can improve FPS but may cause Remix capture flicker/disappearing objects."),
				0
			};

			variable source_queue_frame_sync_guard =
			{
				"source_queue_frame_sync_guard",
				"When queued Source rendering is enabled, keep mat_frame_sync_enable enabled. This can reduce one-frame geometry dropouts at the cost of some performance.",
				true
			};

			variable source_queue_flicker_mitigation =
			{
				"source_queue_flicker_mitigation",
				"Queued renderer guard level. 0 = off/fastest, 1 = balanced frame-sync guard, 2 = strict sync/quarantine for testing maps with one-frame object flicker.",
				1
			};

			variable source_queue_geometry_quarantine =
			{
				"source_queue_geometry_quarantine",
				"When queued rendering is enabled, keep the riskiest extra threaded geometry/effect paths disabled. Use this while hunting Remix capture flickers.",
				true
			};

			variable source_queue_debug_overlay =
			{
				"source_queue_debug_overlay",
				"UI-only debug helper: show the resolved Source threading cvar plan and flicker-risk notes in the renderer settings panel.",
				false
			};

			variable source_threaded_particles =
			{
				"source_threaded_particles",
				"Experimental: allow r_threaded_particles when source_threading_mode is not 0. Stable hybrid mode keeps mat_queue_mode 0 but can still test this worker cvar.",
				false
			};

			variable source_threaded_detailprops =
			{
				"source_threaded_detailprops",
				"Experimental: allow r_threadeddetailprops when source_threading_mode is not 0. Usually has no benefit while detail props are disabled.",
				false
			};

			variable source_queued_ropes =
			{
				"source_queued_ropes",
				"Experimental: allow r_queued_ropes when source_threading_mode is not 0. Disable if ropes or transparent effects flicker/disappear.",
				false
			};

			variable enable_3d_sky =
			{
				"enable_3d_sky",
				"Enable tweaks required for the 3D skybox. Requires proper 3D skybox remix-runtime settings (sky auto detect). Can/will crash the game when its getting unfocused.",
				false
			};

			variable sky3d_diagnostic_logging =
			{
				"sky3d_diagnostic_logging",
				"Enable rate-limited 3D skybox diagnostic logging (maximum one summary every two seconds).",
				false
			};

			variable sky3d_payload_max_age_frames =
			{
				"sky3d_payload_max_age_frames",
				"Maximum age of a captured sky_camera payload accepted by the static-scene path. Older data falls back to normal Source rendering.",
				8
			};

			variable sky3d_safe_source_fallback =
			{
				"sky3d_safe_source_fallback",
				"Allow entity iteration to refresh sky_camera metadata for diagnostics when the SkyboxView hook has not produced a fresh payload.",
				true
			};

			variable sky3d_require_hook_confirmation =
			{
				"sky3d_require_hook_confirmation",
				"Require a recent VIEW_3DSKY hook-confirmed payload before static-scene capture may suppress Source sky geometry. Entity recovery remains diagnostic-only while enabled.",
				true
			};

			variable sky3d_max_scale =
			{
				"sky3d_max_scale",
				"Maximum accepted sky_camera scale. Values above this limit are treated as corrupted or layout-mismatched payloads.",
				1024
			};

			variable default_nocull_distance =
			{
				"default_nocull_distance",
				("The default distance (radius around player) where nothing will get culled.\n"
				 "# Value is only used by certain anti-culling modes & if there isn't a manual area/leaf override via a MapSettings entry."),
				600.0f
			};

			variable ui_advanced_mode =
			{
				"ui_advanced_mode",
				"Show advanced authoring, offsets, diagnostics and low-level compatibility controls. Basic mode keeps the common workflow concise.",
				false
			};

			variable ui_compact_descriptions =
			{
				"ui_compact_descriptions",
				"Use shorter descriptions in the main interface. Tooltips still expose the complete technical explanation.",
				true
			};

			variable quick_auto_stop_director =
			{
				"quick_auto_stop_director",
				"Persistently enforce director_stop after map loads and at a low retry frequency. The command is never sent every frame.",
				false
			};

			variable quick_auto_kick_bots =
			{
				"quick_auto_kick_bots",
				"Persistently remove survivor bots after map loads using a throttled retry. The command is never sent every frame.",
				false
			};


			variable flashlight_enabled =
			{
				"flashlight_enabled",
				"Master switch for the complete layered player flashlight rig.",
				true
			};

			variable flashlight_preset =
			{
				"flashlight_preset",
				"Layered flashlight preset. 0 Performance, 1 Balanced, 2 Realistic LED, 3 Tactical, 4 Soft Cinematic, 5 Wide, 6 Narrow, 7 Source Classic, 8 Horror, 9 Custom.",
				1
			};

			variable flashlight_main_enabled =
			{
				"flashlight_main_enabled",
				"Enable the main spotlight layer.",
				true
			};

			variable flashlight_main_color =
			{
				"flashlight_main_color",
				"Linear RGB multiplier for the main spotlight layer.",
				1.0f, 0.96f, 0.88f
			};

			variable flashlight_main_direction_offset =
			{
				"flashlight_main_direction_offset",
				"Main spotlight local direction offset in degrees: pitch, yaw.",
				0.0f, 0.0f
			};

			variable flashlight_offset_player =
			{
				"flashlight_offset_player",
				"Offset (along forward vector) that will be applied to the remixApi flashlight of the player. ~~ F: Forward || H: Horizontal || V: Vertical",
				-1.5f, -3.9f, -4.8f
			};

			variable flashlight_offset_bot =
			{
				"flashlight_offset_bot",
				"Offset (along forward vector) that will be applied to the remixApi flashlight of bots. ~~ F: Forward || H: Horizontal || V: Vertical",
				22.0f, 1.0f, -4.0f
			};

			variable flashlight_intensity =
			{
				"flashlight_intensity",
				"Intensity of the remixApi flashlights.",
				20000.0f
			};

			variable flashlight_radius =
			{
				"flashlight_radius",
				"Radius of the remixApi flashlights.",
				0.5f
			};

			variable flashlight_angle =
			{
				"flashlight_angle",
				"Angle of the remixApi flashlights. (0-180)",
				25.0f
			};

			variable flashlight_softness =
			{
				"flashlight_softness",
				"Softness of the remixApi flashlights. (0-1)",
				0.34f
			};

			variable flashlight_expo =
			{
				"flashlight_expo",
				"Exponent of the remixApi flashlights. (0-1)",
				0.7f
			};

			variable flashlight_inner_enabled =
			{
				"flashlight_inner_enabled",
				"Enable the second small sphere light attached to the flashlight. This is an unshaped core/fill light controlled separately from the main cone.",
				true
			};

			variable flashlight_inner_offset_player =
			{
				"flashlight_inner_offset_player",
				"Offset for the player flashlight core sphere. ~~ F: Forward || H: Horizontal || V: Vertical",
				4.0f, 0.0f, -2.0f
			};

			variable flashlight_inner_offset_bot =
			{
				"flashlight_inner_offset_bot",
				"Offset for bot flashlight core sphere. ~~ F: Forward || H: Horizontal || V: Vertical",
				4.0f, 0.0f, -2.0f
			};

			variable flashlight_inner_intensity =
			{
				"flashlight_inner_intensity",
				"Intensity of the small near-field point light at the flashlight emitter.",
				9000.0f
			};

			variable flashlight_inner_radius =
			{
				"flashlight_inner_radius",
				"Physical emitter radius of the near-field point light. Keep this small; it is not the light reach.",
				0.10f
			};

			variable flashlight_inner_angle =
			{
				"flashlight_inner_angle",
				"Angle of the inner remixApi flashlight. (0-180) (player)",
				14.0f
			};

			variable flashlight_inner_softness =
			{
				"flashlight_inner_softness",
				"Softness of the inner remixApi flashlight. (0-1) (player)",
				0.06f
			};

			variable flashlight_inner_expo =
			{
				"flashlight_inner_expo",
				"Exponent of the inner remixApi flashlight. (0-1) (player)",
				0.8f
			};


			variable flashlight_inner_color =
			{
				"flashlight_inner_color",
				"Linear RGB multiplier for the core sphere layer.",
				1.0f, 0.88f, 0.70f
			};

			variable flashlight_hotspot_enabled =
			{
				"flashlight_hotspot_enabled",
				"Enable the narrow high-intensity hotspot spotlight.",
				true
			};

			variable flashlight_hotspot_offset_player = { "flashlight_hotspot_offset_player", "Player hotspot offset: forward, up, right.", 7.0f, -3.4f, -4.0f };
			variable flashlight_hotspot_offset_bot = { "flashlight_hotspot_offset_bot", "Bot hotspot offset: forward, up, right.", 23.0f, 1.0f, -4.0f };
			variable flashlight_hotspot_intensity = { "flashlight_hotspot_intensity", "Hotspot spotlight intensity.", 12000.0f };
			variable flashlight_hotspot_radius = { "flashlight_hotspot_radius", "Hotspot emitter radius.", 0.28f };
			variable flashlight_hotspot_angle = { "flashlight_hotspot_angle", "Hotspot cone angle in degrees.", 12.0f };
			variable flashlight_hotspot_softness = { "flashlight_hotspot_softness", "Hotspot cone softness.", 0.10f };
			variable flashlight_hotspot_expo = { "flashlight_hotspot_expo", "Hotspot focus exponent.", 0.95f };
			variable flashlight_hotspot_color = { "flashlight_hotspot_color", "Linear RGB multiplier for the hotspot layer.", 1.0f, 0.98f, 0.92f };
			variable flashlight_hotspot_direction_offset = { "flashlight_hotspot_direction_offset", "Hotspot local direction offset in degrees: pitch, yaw.", 0.0f, 0.0f };

			variable flashlight_spill_enabled =
			{
				"flashlight_spill_enabled",
				"Enable the broad low-intensity soft spill spotlight.",
				true
			};

			variable flashlight_spill_offset_player = { "flashlight_spill_offset_player", "Player spill offset: forward, up, right.", 4.0f, -3.0f, -4.2f };
			variable flashlight_spill_offset_bot = { "flashlight_spill_offset_bot", "Bot spill offset: forward, up, right.", 21.0f, 1.0f, -4.0f };
			variable flashlight_spill_intensity = { "flashlight_spill_intensity", "Soft spill spotlight intensity.", 5500.0f };
			variable flashlight_spill_radius = { "flashlight_spill_radius", "Soft spill emitter radius.", 0.85f };
			variable flashlight_spill_angle = { "flashlight_spill_angle", "Soft spill cone angle in degrees.", 46.0f };
			variable flashlight_spill_softness = { "flashlight_spill_softness", "Soft spill cone softness.", 0.72f };
			variable flashlight_spill_expo = { "flashlight_spill_expo", "Soft spill focus exponent.", 0.32f };
			variable flashlight_spill_color = { "flashlight_spill_color", "Linear RGB multiplier for the soft spill layer.", 1.0f, 0.90f, 0.78f };
			variable flashlight_spill_direction_offset = { "flashlight_spill_direction_offset", "Soft spill local direction offset in degrees: pitch, yaw.", 0.0f, 0.0f };

			variable debug_info_distance =
			{
				"debug_info_distance",
				"The distance cutoff (in units) were debug info such as static prop info, unbake info, bone info etc. no longer gets drawn at.",
				400.0f
			};

			variable player_backwards_offset =
			{
				"player_backwards_offset",
				"Can be used to offset the shadow casting first person player body backwards. Same logic as found within remix but without the body mesh getting smeary.",
				18.0f
			};
		};

		static std::uint64_t calculate_settings_fingerprint();
		static void restore_defaults_no_save();
		static void parse_runtime_ui_settings(const toml::value& config);
		static void write_runtime_ui_settings(std::ostream& file);
		static bool atomic_replace_file(const std::filesystem::path& temp_path, const std::filesystem::path& final_path);

		static inline bool m_dirty = false;
		static inline bool m_loaded = false;
		static inline bool m_last_save_succeeded = true;
		static inline std::string m_last_save_status = "Not saved yet";
		static inline std::string m_last_dirty_reason = "none";
		static inline std::uint64_t m_save_generation = 0u;
		static inline std::uint64_t m_last_fingerprint = 0u;
		static inline std::chrono::steady_clock::time_point m_dirty_since = {};
		static inline std::chrono::steady_clock::time_point m_last_change_time = {};
		static inline constexpr std::chrono::milliseconds m_autosave_debounce = std::chrono::milliseconds(650);

		static inline var_definitions vars = {};
	};
}
