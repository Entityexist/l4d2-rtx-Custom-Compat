#include "std_include.hpp"

namespace components
{
	namespace cmd
	{
		bool debug_pos_time = false;
		bool show_api_lights = false;
		bool show_mesh_bone_info_attached = false;
		bool show_mesh_bone_info = false;
	}

	namespace
	{
		bool light_group_matches_runtime_filter(const map_settings::remix_light_settings_s& def)
		{
			if (!remix_lights::runtime_group_filter_enabled()) {
				return true;
			}

			const auto& filter = remix_lights::runtime_group_filter();
			if (filter.empty()) {
				return true;
			}

			return utils::str_to_lower(def.group) == utils::str_to_lower(filter);
		}

		bool light_is_runtime_enabled(const map_settings::remix_light_settings_s& def)
		{
			// V20.9 file-backed live-linked records are editor/database controllers.
			// Their actual runtime light is owned by the Source entity tracker, so the
			// ordinary map_settings spawn/event paths must not create a static duplicate.
			if (def.persistent_map_light_from_file && def.generated_source_live_link) return false;
			return def.enabled && light_group_matches_runtime_filter(def);
		}

		int apply_ies_cluster_quality_mode(const int authored_samples)
		{
			const int samples = std::clamp(authored_samples, 0, 24);
			switch (std::clamp(remix_lights::ies_cluster_quality_mode(), 0, 4))
			{
			case 0: return 0;
			case 1: return std::min(samples, 2);
			case 2: return std::min(samples, std::max(1, (samples + 1) / 2));
			case 4: return std::min(24, std::max(samples, samples + 2));
			case 3:
			default: return samples;
			}
		}


		std::wstring utf8_to_wide_light_path(const std::string& value)
		{
			if (value.empty()) {
				return {};
			}

			const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
			if (required <= 0)
			{
				// Source configuration files have historically been ANSI. Keep that fallback for old maps.
				const int ansi_required = MultiByteToWideChar(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
				if (ansi_required <= 0) return {};
				std::wstring result(static_cast<size_t>(ansi_required), L'\0');
				MultiByteToWideChar(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()), result.data(), ansi_required);
				return result;
			}

			std::wstring result(static_cast<size_t>(required), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()), result.data(), required);
			return result;
		}

		std::filesystem::path light_game_root_path()
		{
			if (!game::root_path.empty()) {
				return std::filesystem::path(game::root_path);
			}

			wchar_t executable[MAX_PATH] = {};
			const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) return {};
			return std::filesystem::path(executable).parent_path();
		}

		std::filesystem::path resolve_native_ies_profile_path(const std::string& authored_path)
		{
			const auto wide = utf8_to_wide_light_path(authored_path);
			if (wide.empty()) return {};

			std::filesystem::path input(wide);
			if (!input.has_extension()) input += L".ies";

			std::vector<std::filesystem::path> candidates;
			if (input.is_absolute())
			{
				candidates.push_back(input);
			}
			else
			{
				const auto root = light_game_root_path();
				candidates.push_back(root / L"rtx-remix" / L"ies" / input);
				candidates.push_back(root / L"rtx-remix" / L"mods" / L"LegacyMaterials" / L"ies" / input);
				candidates.push_back(root / input);
			}

			std::error_code ec;
			for (auto candidate : candidates)
			{
				candidate = candidate.lexically_normal();
				if (std::filesystem::is_regular_file(candidate, ec))
				{
					ec.clear();
					auto canonical = std::filesystem::weakly_canonical(candidate, ec);
					return ec ? candidate : canonical;
				}
				ec.clear();
			}
			return {};
		}

		const char* light_rig_mode_name(const int mode)
		{
			switch (mode)
			{
			case map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES: return "Native IES Profile Lights";
			case map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES: return "Fake IES Profile Rig";
			case map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY:
			default: return "Legacy Light Rig";
			}
		}

		int resolved_runtime_light_shape(const map_settings::remix_light_settings_s::point_s& point)
		{
			if (point.authoring_shape != map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_AUTO) return point.authoring_shape;
			return point.use_shaping
				? map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_SPOT
				: map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_POINT;
		}

		void build_analytical_light_basis(Vector direction, Vector& right, Vector& up)
		{
			if (direction.LengthSqr() <= 0.000001f) direction = Vector(0.0f, 0.0f, -1.0f);
			direction.NormalizeChecked();
			const Vector reference = std::fabs(direction.z) < 0.92f ? Vector(0.0f, 0.0f, 1.0f) : Vector(0.0f, 1.0f, 0.0f);
			right = reference.Cross(direction);
			if (right.LengthSqr() <= 0.000001f) right = Vector(1.0f, 0.0f, 0.0f);
			right.NormalizeChecked();
			up = direction.Cross(right);
			if (up.LengthSqr() <= 0.000001f) up = Vector(0.0f, 1.0f, 0.0f);
			up.NormalizeChecked();
		}

		void clear_analytical_light_chains(remix_lights::light* light)
		{
			if (!light) return;
			light->m_ext.pNext = nullptr;
			light->m_rect_ext.pNext = nullptr;
			light->m_disk_ext.pNext = nullptr;
			light->m_cylinder_ext.pNext = nullptr;
			light->m_distant_ext.pNext = nullptr;
		}

		void* configure_analytical_light_geometry(remix_lights::light* light,
			const map_settings::remix_light_settings_s::point_s* point)
		{
			if (!light || !point) return nullptr;
			const void* ies_chain = point->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES
				? static_cast<const void*>(&light->m_native_ies_ext) : nullptr;
			void* next = const_cast<void*>(ies_chain);
			clear_analytical_light_chains(light);

			Vector direction(light->m_ext.shaping_value.direction.x, light->m_ext.shaping_value.direction.y, light->m_ext.shaping_value.direction.z);
			if (direction.LengthSqr() <= 0.000001f) direction = point->direction;
			if (direction.LengthSqr() <= 0.000001f) direction = Vector(0.0f, 0.0f, -1.0f);
			direction.NormalizeChecked();
			Vector right, up;
			build_analytical_light_basis(direction, right, up);

			const int shape = resolved_runtime_light_shape(*point);
			const float width = std::max(0.001f, point->authoring_width);
			const float height = std::max(0.001f, point->authoring_height);
			const float length = std::max(0.001f, point->authoring_length);
			const bool pending_attachment = light->m_ext.radius <= 0.0f;

			if (!pending_attachment && shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_RECT)
			{
				light->m_rect_ext = {};
				light->m_rect_ext.sType = static_cast<remixapi_StructType>(10);
				light->m_rect_ext.pNext = next;
				light->m_rect_ext.position = light->m_ext.position;
				light->m_rect_ext.xAxis = right.ToRemixFloat3D();
				light->m_rect_ext.xSize = width;
				light->m_rect_ext.yAxis = up.ToRemixFloat3D();
				light->m_rect_ext.ySize = height;
				light->m_rect_ext.direction = direction.ToRemixFloat3D();
				light->m_rect_ext.shaping_hasvalue = light->m_ext.shaping_hasvalue ? 1u : 0u;
				light->m_rect_ext.shaping_value = light->m_ext.shaping_value;
				light->m_rect_ext.volumetricRadianceScale = light->m_ext.volumetricRadianceScale;
				return &light->m_rect_ext;
			}
			if (!pending_attachment && shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISK)
			{
				light->m_disk_ext = {};
				light->m_disk_ext.sType = static_cast<remixapi_StructType>(9);
				light->m_disk_ext.pNext = next;
				light->m_disk_ext.position = light->m_ext.position;
				light->m_disk_ext.xAxis = right.ToRemixFloat3D();
				light->m_disk_ext.xRadius = width * 0.5f;
				light->m_disk_ext.yAxis = up.ToRemixFloat3D();
				light->m_disk_ext.yRadius = height * 0.5f;
				light->m_disk_ext.direction = direction.ToRemixFloat3D();
				light->m_disk_ext.shaping_hasvalue = light->m_ext.shaping_hasvalue ? 1u : 0u;
				light->m_disk_ext.shaping_value = light->m_ext.shaping_value;
				light->m_disk_ext.volumetricRadianceScale = light->m_ext.volumetricRadianceScale;
				return &light->m_disk_ext;
			}
			if (!pending_attachment && shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE)
			{
				light->m_cylinder_ext = {};
				light->m_cylinder_ext.sType = static_cast<remixapi_StructType>(8);
				light->m_cylinder_ext.pNext = next;
				light->m_cylinder_ext.position = light->m_ext.position;
				light->m_cylinder_ext.radius = width * 0.5f;
				light->m_cylinder_ext.axis = direction.ToRemixFloat3D();
				light->m_cylinder_ext.axisLength = length;
				light->m_cylinder_ext.volumetricRadianceScale = light->m_ext.volumetricRadianceScale;
				return &light->m_cylinder_ext;
			}
			if (!pending_attachment && shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT)
			{
				light->m_distant_ext = {};
				light->m_distant_ext.sType = static_cast<remixapi_StructType>(7);
				light->m_distant_ext.pNext = next;
				light->m_distant_ext.direction = direction.ToRemixFloat3D();
				light->m_distant_ext.angularDiameterDegrees = std::clamp(width, 0.01f, 180.0f);
				light->m_distant_ext.volumetricRadianceScale = light->m_ext.volumetricRadianceScale;
				return &light->m_distant_ext;
			}

			light->m_ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			light->m_ext.pNext = next;
			return &light->m_ext;
		}

		bool configure_light_backend(remix_lights::light* light,
			const map_settings::remix_light_settings_s::point_s* point,
			const remixapi_Float3D& current_direction)
		{
			if (!light || !point) return false;

			clear_analytical_light_chains(light);
			light->m_native_ies_ext = {};
			light->m_native_ies_ext.sType = static_cast<remixapi_StructType>(28);
			light->m_native_ies_status = light_rig_mode_name(point->light_rig_mode);

			if (point->light_rig_mode != map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES)
			{
				light->m_native_ies_authored_path.clear();
				light->m_native_ies_profile_path.clear();
				light->m_native_ies_resolution_failed = false;
				return true;
			}

			const int native_shape = resolved_runtime_light_shape(*point);
			if (native_shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_TUBE ||
				native_shape == map_settings::remix_light_settings_s::LIGHT_AUTHORING_SHAPE_DISTANT)
			{
				light->m_native_ies_status = "Native IES blocked: Tube and Distant geometry do not support photometric profiles";
				return false;
			}

			// Resolve a profile only when the authored value changes. Animated lights are recreated every
			// frame, so hitting exists()/canonical() on every CreateLight would turn IES into a filesystem
			// polling path. DXVK's own IES cache/hot-reload remains responsible for content changes.
			if (light->m_native_ies_authored_path != point->ies_file)
			{
				light->m_native_ies_authored_path = point->ies_file;
				light->m_native_ies_profile_path.clear();
				light->m_native_ies_resolution_failed = false;

				const auto resolved = resolve_native_ies_profile_path(point->ies_file);
				if (resolved.empty())
				{
					light->m_native_ies_resolution_failed = true;
					light->m_native_ies_status = point->ies_file.empty()
						? "Native IES blocked: no profile selected"
						: "Native IES blocked: profile file not found";
					return false;
				}

				light->m_native_ies_profile_path = resolved.wstring();
			}

			if (light->m_native_ies_resolution_failed || light->m_native_ies_profile_path.empty())
			{
				light->m_native_ies_status = point->ies_file.empty()
					? "Native IES blocked: no profile selected"
					: "Native IES blocked: profile file not found";
				return false;
			}

			Vector direction(current_direction.x, current_direction.y, current_direction.z);
			if (direction.LengthSqr() <= 0.000001f) direction = Vector(0.0f, 0.0f, -1.0f);
			direction.NormalizeChecked();

			light->m_native_ies_ext.pNext = nullptr;
			light->m_native_ies_ext.profilePath = light->m_native_ies_profile_path.c_str();
			light->m_native_ies_ext.direction = direction.ToRemixFloat3D();
			light->m_native_ies_ext.axisRotationDegrees = std::clamp(point->ies_axis_rotation, -360.0f, 360.0f);
			light->m_native_ies_ext.angleScale = std::clamp(point->ies_angle_scale, 0.01f, 8.0f);
			light->m_native_ies_ext.intensityScale = std::clamp(point->ies_intensity_scale, 0.0f, 32.0f);
			light->m_native_ies_ext.normalize = point->ies_normalize ? 1u : 0u;
			light->m_native_ies_status = "Native IES ready: " + std::filesystem::path(light->m_native_ies_profile_path).filename().string();
			return true;
		}

	}

	/**
	 * Initializes the light interpolator
	 * @param points			Reference to point-list
	 * @param looping			Light is looping
	 * @param loop_smoothing	Add additional segment between last and first point
	 * @return
	 */
	bool remix_lights::light::interpolator::init(const std::vector<map_settings::remix_light_settings_s::point_s>& points, const bool looping, const bool loop_smoothing)
	{
		if (points.size() <= 1)
		{
			// A single point is a static light, not an animated mover.
			// Reset the old state so Clear Animation actually disables the previous animation.
			m_initialized = false;
			m_points.clear();
			m_segment_durations.clear();
			m_looping = false;
			m_loop_smoothing = false;
			m_total_duration = 0.0f;
			return false;
		}

		m_initialized = true;
		m_points = points;
		m_looping = looping;
		m_loop_smoothing = loop_smoothing;

		// ensure first point has timepoint 0
		(m_points)[0].timepoint = 0.0f;

		// total duration defined by the last point
		m_total_duration = m_points.back().timepoint;

		if (points.size() > 1 && m_total_duration == 0.0f)
		{
			game::console();
			std::cout << "[RemixLights][light_interpolator::init] Encountered a light were the last point has no defined timepoint! Placeholder in-use, please fix!" << std::endl;

			// use timepoint of prev. point + 1.0
			m_total_duration = (m_points)[m_points.size() - 2].timepoint + 1.0f;

			// write the placeholder value into the last point
			m_points.back().timepoint = m_total_duration;
		}

		// calculate time for points with no defined timepoint
		bool needs_timepoint_calc = false;
		size_t calc_index_start = 0;

		for (size_t i = 1; i < m_points.size(); ++i)
		{
			// point has no defined timepoint
			if ((m_points)[i].timepoint == 0.0f)
			{
				needs_timepoint_calc = true;
				if (calc_index_start == 0) {
					calc_index_start = i;
				}
			}
			else // point with timepoint
			{
				if (needs_timepoint_calc) // check if previous points had no timepoint
				{
					interpolate_timepoints(calc_index_start, i); // evenly distribute time
					needs_timepoint_calc = false;
					calc_index_start = 0;
				}
			}
		}

		calculate_segment_durations();
		return true;
	}

	/**
	 * Advances time
	 * @param frametime		Time of the last frame
	 * @return				True if move is done (also true for looping lights)
	 */
	bool remix_lights::light::interpolator::advance_time(const float frametime)
	{
		m_elapsed_time += frametime;

		if (m_elapsed_time >= m_total_duration)
		{
			if (m_looping) {
				m_elapsed_time -= m_total_duration;
			}
			else {
				m_elapsed_time = m_total_duration;
			}

			return true;
		}

		return false;
	}

	Vector remix_lights::light::calculate_direction_for_point(const map_settings::remix_light_settings_s::point_s* point) const
	{
		if (is_attached())
		{
			// get direction vector for angles
			Vector offset_dir;
			utils::vector::AngleVectors(point->angle_offset_attached, &offset_dir);

			// apply offset in bone's local coordinate system
			Vector world_offset = offset_dir.x * m_attached_bone_forward + offset_dir.y * m_attached_bone_right + offset_dir.z * m_attached_bone_up;

			world_offset.NormalizeInPlace();
			return world_offset;
		}

		return point->direction;
	}

	Vector remix_lights::light::calculate_position_for_point(const map_settings::remix_light_settings_s::point_s* point) const
	{
		if (is_attached())
		{
			// apply local position offset using bone's original basis
			const Vector world_pos_offset = point->position.x * m_attached_bone_forward + point->position.y * m_attached_bone_right + point->position.z * m_attached_bone_up;
			return m_attached_position + world_pos_offset;
		}

		return point->position;
	}

	/**
	 * Calculate light properties for the current tick \n
	 * All arguments besides 'parent' are optional - use nullptr to not update a specific property
	 * @param parent			(pointer to owning light)
	 * @param position			(remixapi_LightInfoSphereEXT)
	 * @param radiance			(remixapi_LightInfo)
	 * @param radius			(remixapi_LightInfo)
	 * @param direction			(remixapi_LightInfoSphereEXT)
	 * @param degrees			(remixapi_LightInfoSphereEXT)
	 * @param softness			(remixapi_LightInfoSphereEXT)
	 * @param exponent			(remixapi_LightInfoSphereEXT)
	 * @param volumetric_scale	(remixapi_LightInfoSphereEXT)
	 */
	void remix_lights::light::interpolator::interpolate(light* parent, remixapi_Float3D* position, remixapi_Float3D* radiance, float* radius,
		remixapi_Float3D* direction, float* degrees, float* softness, float* exponent, float* volumetric_scale, bool is_attached)
	{
		assert(parent != nullptr && "m_parent must not be null");
		if (!parent) {
			return;
		}

		{
			map_settings::remix_light_settings_s::point_s* temp_pt = nullptr;
			if (m_elapsed_time <= 0.0f) {
				temp_pt = &m_points.front();
			}
			else if (m_elapsed_time >= m_total_duration) {
				temp_pt = &m_points.back();
			}

			if (temp_pt)
			{
				if (position) { *position = parent->calculate_position_for_point(temp_pt).ToRemixFloat3D(); }//(temp_pt->position + m_parent->m_attached_position).ToRemixFloat3D(); }
				if (radiance) { *radiance = (temp_pt->radiance * temp_pt->radiance_scalar).ToRemixFloat3D(); }
				if (radius) { *radius = temp_pt->radius; }
				if (direction) { *direction = parent->calculate_direction_for_point(temp_pt).ToRemixFloat3D(); }
				if (degrees) { *degrees = temp_pt->degrees; }
				if (softness) { *softness = temp_pt->softness; }
				if (exponent) { *exponent = temp_pt->exponent; }
				if (volumetric_scale) { *volumetric_scale = temp_pt->volumetric_scale; }
				return;
			}
		}

		float time = m_elapsed_time;
		for (size_t i = 0; i < m_segment_durations.size(); ++i)
		{
			if (time <= m_segment_durations[i])
			{
				const auto t = time / m_segment_durations[i];
				const auto& p0 = (m_points)[((i - 1) + m_points.size()) % m_points.size()];
				const auto& p1 = (m_points)[i % m_points.size()];
				const auto& p2 = (m_points)[(m_loop_smoothing && i == m_points.size() - 1) ? 0 : (i + 1) % m_points.size()];
				const auto& p3 = (m_points)[(i + 2) % m_points.size()];

				const float t2 = t * t;
				const float t3 = t2 * t;

				// hermite interpolation
				if (position)
				{
					if (is_attached)
					{
						if (position)
						{
							Vector p0_world_offset = p0.position.x * parent->m_attached_bone_forward + p0.position.y * parent->m_attached_bone_right + p0.position.z * parent->m_attached_bone_up;
							Vector p1_world_offset = p1.position.x * parent->m_attached_bone_forward + p1.position.y * parent->m_attached_bone_right + p1.position.z * parent->m_attached_bone_up;
							Vector p2_world_offset = p2.position.x * parent->m_attached_bone_forward + p2.position.y * parent->m_attached_bone_right + p2.position.z * parent->m_attached_bone_up;
							Vector p3_world_offset = p3.position.x * parent->m_attached_bone_forward + p3.position.y * parent->m_attached_bone_right + p3.position.z * parent->m_attached_bone_up;

							Vector world_pos_offset =
								p1_world_offset * (2.0f * t3 - 3.0f * t2 + 1.0f)
								+ p2_world_offset * (-2.0f * t3 + 3.0f * t2)
								+ (p2_world_offset - p0_world_offset) * p1.smoothness * (t3 - 2.0f * t2 + t)
								+ (p3_world_offset - p1_world_offset) * p2.smoothness * (t3 - t2);

							*position = (parent->m_attached_position + world_pos_offset).ToRemixFloat3D();


							//Vector finalPos = m_parent->m_attached_position + position_offset * final_dir;
							//*position = finalPos.ToRemixFloat3D();
						}
					}
					else
					{
						*position = (
							p1.position * (2.0f * t3 - 3.0f * t2 + 1.0f)
							+ p2.position * (-2.0f * t3 + 3.0f * t2)
							+ (p2.position - p0.position) * p1.smoothness * (t3 - 2.0f * t2 + t)
							+ (p3.position - p1.position) * p2.smoothness * (t3 - t2)
							).ToRemixFloat3D();
					}
				}

				if (direction)
				{
					if (is_attached)
					{
						// convert offsets to direction vectors
						Vector p0_offset, p1_offset, p2_offset, p3_offset;
						utils::vector::AngleVectors(p0.angle_offset_attached, &p0_offset);
						utils::vector::AngleVectors(p1.angle_offset_attached, &p1_offset);
						utils::vector::AngleVectors(p2.angle_offset_attached, &p2_offset);
						utils::vector::AngleVectors(p3.angle_offset_attached, &p3_offset);

						// transform offsets to bone's local coordinate system
						Vector p0_world_offset = p0_offset.x * parent->m_attached_bone_forward + p0_offset.y * parent->m_attached_bone_right + p0_offset.z * parent->m_attached_bone_up;
						Vector p1_world_offset = p1_offset.x * parent->m_attached_bone_forward + p1_offset.y * parent->m_attached_bone_right + p1_offset.z * parent->m_attached_bone_up;
						Vector p2_world_offset = p2_offset.x * parent->m_attached_bone_forward + p2_offset.y * parent->m_attached_bone_right + p2_offset.z * parent->m_attached_bone_up;
						Vector p3_world_offset = p3_offset.x * parent->m_attached_bone_forward + p3_offset.y * parent->m_attached_bone_right + p3_offset.z * parent->m_attached_bone_up;

						Vector interpolated_offset = (
							p1_world_offset * (2.0f * t3 - 3.0f * t2 + 1.0f)
							+ p2_world_offset * (-2.0f * t3 + 3.0f * t2)
							+ (p2_world_offset - p0_world_offset) * p1.smoothness * (t3 - 2.0f * t2 + t)
							+ (p3_world_offset - p1_world_offset) * p2.smoothness * (t3 - t2)
							);

						// combine with bone's forward (scale to achieve full offset)
						float offset_scale = std::numbers::sqrt2_v<float>; // ca. 90deg max offset

						Vector final_dir = parent->m_attached_bone_forward + offset_scale * interpolated_offset;
						final_dir.NormalizeInPlace();

						*direction = final_dir.ToRemixFloat3D();

						//if (position)
						//{

						//	// apply local position offset using bone's original basis
						//	const Vector world_pos_offset = point->position.x * m_attached_bone_forward + point->position.y * m_attached_bone_right + point->position.z * m_attached_bone_up;
						//	//return m_attached_position + world_pos_offset;


						//	// already up to date
						//	Vector position_offset { position->x, position->y, position->z };

						//	Vector finalPos = m_parent->m_attached_position + position_offset * final_dir;
						//	*position = finalPos.ToRemixFloat3D();
						//}
					}
					else
					{
						Vector dir = (
							p1.direction * (2.0f * t3 - 3.0f * t2 + 1.0f)
							+ p2.direction * (-2.0f * t3 + 3.0f * t2)
							+ (p2.direction - p0.direction) * p1.smoothness * (t3 - 2.0f * t2 + t)
							+ (p3.direction - p1.direction) * p2.smoothness * (t3 - t2)
							);

						dir.Normalize();
						*direction = dir.ToRemixFloat3D();
					}
				}

				// linear interpolations

				if (radiance) {
					*radiance = lerp((p1.radiance * p1.radiance_scalar), (p2.radiance * p2.radiance_scalar), t).ToRemixFloat3D();
				}

				if (radius) {
					*radius = lerp(p1.radius, p2.radius, t);
				}

				if (degrees) {
					*degrees = lerp(p1.degrees, p2.degrees, t);
				}

				if (softness) {
					*softness = lerp(p1.softness, p2.softness, t);
				}

				if (exponent) {
					*exponent = lerp(p1.exponent, p2.exponent, t);
				}

				if (volumetric_scale) {
					*volumetric_scale = lerp(p1.volumetric_scale, p2.volumetric_scale, t);
				}

				return;
			}

			time -= m_segment_durations[i];
		}

		// should not happen if durations are correct
		const auto last_pt = m_points.back();
		if (position) { *position = parent->calculate_position_for_point(&last_pt).ToRemixFloat3D(); } //(last_pt.position + m_parent->m_attached_position).ToRemixFloat3D(); }
		if (radiance) { *radiance = (last_pt.radiance * last_pt.radiance_scalar).ToRemixFloat3D(); }
		if (radius) { *radius = last_pt.radius; }
		if (direction) { *direction = parent->calculate_direction_for_point(&last_pt).ToRemixFloat3D(); }
		if (degrees) { *degrees = last_pt.degrees; }
		if (softness) { *softness = last_pt.softness; }
		if (exponent) { *exponent = last_pt.exponent; }
		if (volumetric_scale) { *volumetric_scale = last_pt.volumetric_scale; }
	}

	// ----

	namespace
	{
		Vector normalize_or(Vector value, const Vector& fallback)
		{
			if (value.LengthSqr() <= 0.0001f) { value = fallback; }
			if (value.LengthSqr() <= 0.0001f) { value = Vector(0.0f, 0.0f, -1.0f); }
			value.NormalizeChecked();
			return value;
		}

		void make_orthonormal_basis(const Vector& direction, Vector& right, Vector& up)
		{
			Vector dir = normalize_or(direction, Vector(0.0f, 0.0f, -1.0f));
			up = Vector(0.0f, 0.0f, 1.0f);
			if (std::fabs(dir.Dot(up)) > 0.96f) {
				up = Vector(1.0f, 0.0f, 0.0f);
			}

			right = dir.Cross(up);
			right.NormalizeChecked();
			up = right.Cross(dir);
			up.NormalizeChecked();
		}

		Vector calculate_ies_cluster_lateral_offset(const std::string& raw_pattern, const int index, const int samples, const float spread, const float aspect, const float twist_degrees, float* out_ring_weight = nullptr)
		{
			const float safe_samples = static_cast<float>(std::max(samples, 1));
			const float t = (static_cast<float>(index) + 0.5f) / safe_samples;
			const float golden_angle = 2.39996323f;
			const float two_pi = 6.28318531f;
			float ring = std::sqrt(std::clamp(t, 0.0f, 1.0f));
			float x = 0.0f;
			float y = 0.0f;

			auto pattern = utils::str_to_lower(raw_pattern.empty() ? "spiral" : raw_pattern);
			utils::replace_all(pattern, "-", "_");

			if (pattern == "ring" || pattern == "donut")
			{
				const float angle = two_pi * static_cast<float>(index) / safe_samples;
				x = std::cos(angle) * spread;
				y = std::sin(angle) * spread;
				ring = 1.0f;
			}
			else if (pattern == "line" || pattern == "tube")
			{
				const float denom = std::max(1.0f, safe_samples - 1.0f);
				const float u = samples <= 1 ? 0.0f : (static_cast<float>(index) / denom) * 2.0f - 1.0f;
				x = u * spread;
				y = 0.0f;
				ring = std::fabs(u);
			}
			else if (pattern == "cross" || pattern == "plus")
			{
				const int arm = index & 3;
				const float arm_step = static_cast<float>(index / 4 + 1) / std::max(1.0f, std::ceil(safe_samples / 4.0f));
				const float dist = std::clamp(arm_step, 0.0f, 1.0f) * spread;
				x = (arm == 0 ? dist : arm == 1 ? -dist : 0.0f);
				y = (arm == 2 ? dist : arm == 3 ? -dist : 0.0f);
				ring = dist / std::max(0.001f, spread);
			}
			else if (pattern == "beam" || pattern == "hotspot")
			{
				const float angle = static_cast<float>(index) * golden_angle;
				const float lateral = ring * spread;
				x = std::cos(angle) * lateral * 0.45f;
				y = std::sin(angle) * lateral;
			}
			else
			{
				const float angle = static_cast<float>(index) * golden_angle;
				const float lateral = ring * spread;
				x = std::cos(angle) * lateral;
				y = std::sin(angle) * lateral;
			}

			y *= std::clamp(aspect, 0.1f, 8.0f);
			const float twist = twist_degrees * 0.01745329252f;
			const float rx = x * std::cos(twist) - y * std::sin(twist);
			const float ry = x * std::sin(twist) + y * std::cos(twist);

			if (out_ring_weight) {
				*out_ring_weight = std::clamp(ring, 0.0f, 1.0f);
			}
			return Vector(rx, ry, 0.0f);
		}
	}

	std::uint32_t remix_lights::get_ies_cluster_child_count() const
	{
		std::uint32_t count = 0u;
		for (const auto& active_light : m_active_lights)
		{
			for (const auto& child : active_light.m_ies_children)
			{
				if (child.handle) {
					++count;
				}
			}
		}
		return count;
	}

	remix_lights::runtime_debug_stats_s remix_lights::build_runtime_debug_stats() const
	{
		runtime_debug_stats_s stats = {};
		stats.ies_budget_skipped = m_debug_ies_budget_skipped;
		stats.native_ies_failures = m_debug_native_ies_failures;
		stats.create_light_failures = m_debug_create_light_failures;

		for (const auto& l : m_active_lights)
		{
			++stats.active_lights;
			if (l.m_handle) { ++stats.spawned_handles; }
			if (l.m_mover.is_initialized() || l.m_def.points.size() > 1u || (!l.m_def.animation.empty() && l.m_def.animation != "stable")) { ++stats.animated_lights; }
			if (l.is_attached()) { ++stats.attached_lights; }
			if (!l.m_handle && (l.has_spawn_trigger() || l.m_def.trigger_delay > 0.0f)) { ++stats.pending_trigger_lights; }
			if (l.m_is_marked_for_destruction) { ++stats.marked_for_destroy; }
			if (!light_group_matches_runtime_filter(l.m_def)) { ++stats.runtime_group_filtered; }

			const int rig_mode = l.m_def.points.empty() ? map_settings::remix_light_settings_s::LIGHT_RIG_MODE_LEGACY : l.m_def.points.front().light_rig_mode;
			if (rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_NATIVE_IES) ++stats.native_ies_lights;
			else if (rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES) ++stats.fake_ies_rigs;
			else ++stats.legacy_rig_lights;

			for (const auto& child : l.m_ies_children)
			{
				if (child.handle) { ++stats.ies_child_handles; }
			}
		}

		return stats;
	}

	void remix_lights::push_debug_lifecycle_event(std::string_view event, const light* l)
	{
		if (!m_debug_lifecycle_log_enabled) {
			return;
		}

		auto frame_text = std::to_string(m_active_light_spawn_tracker);
		if (frame_text.size() < 6u) {
			frame_text.insert(frame_text.begin(), 6u - frame_text.size(), '0');
		}

		std::string line;
		line.reserve(160u);
		line += "[";
		line += frame_text;
		line += "] ";
		line.append(event.data(), event.size());

		if (l)
		{
			line += " | light=";
			line += std::to_string(l->m_light_num);
			line += " group=";
			line += (l->m_def.group.empty() ? std::string("none") : l->m_def.group);
			line += " comment=";
			line += (l->m_def.comment.empty() ? std::string("none") : l->m_def.comment);
			line += " pts=";
			line += std::to_string(l->m_def.points.size());
			line += " children=";
			line += std::to_string(l->m_ies_children.size());
		}

		m_debug_lifecycle_log.push_front(std::move(line));
		while (m_debug_lifecycle_log.size() > 96u) {
			m_debug_lifecycle_log.pop_back();
		}
	}

	void remix_lights::destroy_ies_emulation_lights(light* l)
	{
		if (!l) {
			return;
		}

		if (!remix_api::is_initialized()) {
			l->m_ies_children.clear();
			return;
		}

		for (auto& child : l->m_ies_children)
		{
			if (child.handle)
			{
				remix_api::get()->m_bridge.DestroyLight(child.handle);
				child.handle = nullptr;
			}
		}

		l->m_ies_children.clear();
	}

	void remix_lights::update_ies_emulation_lights(light* l, const map_settings::remix_light_settings_s::point_s* control_point)
	{
		destroy_ies_emulation_lights(l);

		if (!remix_api::is_initialized()) {
			return;
		}

		if (!l || !control_point ||
			control_point->light_rig_mode != map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES ||
			!control_point->ies_emulation || control_point->ies_emulation_samples <= 0 || l->m_ext.radius <= 0.0f)
		{
			return;
		}

		const int requested_samples = apply_ies_cluster_quality_mode(control_point->ies_emulation_samples);
		if (requested_samples <= 0) {
			return;
		}

		int active_cluster_children = 0;
		for (const auto& active_light : m_active_lights)
		{
			if (&active_light == static_cast<const light*>(l)) {
				continue;
			}
			for (const auto& child : active_light.m_ies_children)
			{
				if (child.handle) {
					++active_cluster_children;
				}
			}
		}

		const int helper_budget = std::clamp(m_ies_cluster_global_budget, 0, 512);
		const int samples = std::min(requested_samples, std::max(0, helper_budget - active_cluster_children));
		if (samples < requested_samples)
		{
			m_debug_ies_budget_skipped += static_cast<std::uint32_t>(requested_samples - std::max(samples, 0));
			push_debug_lifecycle_event("ies budget skip", l);
		}
		if (samples <= 0) {
			return;
		}

		Vector direction(l->m_ext.shaping_value.direction.x, l->m_ext.shaping_value.direction.y, l->m_ext.shaping_value.direction.z);
		if (!l->m_ext.shaping_hasvalue || direction.LengthSqr() <= 0.0001f) {
			direction = l->calculate_direction_for_point(control_point);
		}
		direction = normalize_or(direction, Vector(0.0f, 0.0f, -1.0f));

		Vector right, up;
		make_orthonormal_basis(direction, right, up);

		const Vector base_pos(l->m_ext.position.x, l->m_ext.position.y, l->m_ext.position.z);
		const Vector base_radiance(l->m_info.radiance.x, l->m_info.radiance.y, l->m_info.radiance.z);
		const float base_radius = std::max(0.001f, l->m_ext.radius);
		const float spread = std::max(0.0f, control_point->ies_emulation_spread) * base_radius;
		const float radius_scale = std::clamp(control_point->ies_emulation_radius_scale, 0.01f, 4.0f);
		const float intensity_scale = std::max(0.0f, control_point->ies_emulation_intensity_scale);
		const float forward_offset = control_point->ies_emulation_forward_offset * base_radius;
		const float aspect = std::clamp(control_point->ies_emulation_aspect, 0.1f, 8.0f);
		const float twist = std::clamp(control_point->ies_emulation_twist, -360.0f, 360.0f);

		l->m_ies_children.clear();
		l->m_ies_children.resize(static_cast<size_t>(samples));

		const auto api = remix_api::get();
		for (int i = 0; i < samples; ++i)
		{
			const float sample_count = static_cast<float>(samples);
			float ring = 0.0f;
			const Vector local_offset = calculate_ies_cluster_lateral_offset(control_point->ies_emulation_pattern, i, samples, spread, aspect, twist, &ring);
			const Vector offset = right * local_offset.x + up * local_offset.y + direction * forward_offset;
			const float edge_weight = 0.65f + 0.35f * (1.0f - ring);

			auto& child = l->m_ies_children[static_cast<size_t>(i)];
			child.ext = {};
			child.info = {};
			child.ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			child.ext.pNext = nullptr;
			child.ext.position = (base_pos + offset).ToRemixFloat3D();
			child.ext.radius = base_radius * radius_scale * (0.85f + 0.15f * (1.0f - ring));
			child.ext.shaping_hasvalue = l->m_ext.shaping_hasvalue;
			child.ext.shaping_value = l->m_ext.shaping_value;
			child.ext.volumetricRadianceScale = l->m_ext.volumetricRadianceScale * 0.65f;

			child.info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			child.info.pNext = &child.ext;
			child.info.hash = utils::string_hash64(utils::va("api-light%d-ies-child%d", l->m_light_num, i));
			child.info.radiance = (base_radiance * ((intensity_scale * edge_weight) / sample_count)).ToRemixFloat3D();

			if (api->m_bridge.CreateLight(&child.info, &child.handle) != REMIXAPI_ERROR_CODE_SUCCESS)
			{
				child.handle = nullptr;
				++m_debug_create_light_failures;
				push_debug_lifecycle_event("ies child CreateLight failed", l);
			}
		}
	}

	/**
	 * Update a remixApi light using an "external" point
	 * @param l			Light handle
	 * @param pt		External point handle
	 * @return			True if successfull
	 */
	bool remix_lights::update_static_remix_light(light* l, const map_settings::remix_light_settings_s::point_s* pt)
	{
		if (!remix_api::is_initialized()) {
			return false;
		}

		if (!l || !pt) {
			return false;
		}

		destroy_map_light(l);

		if (l)
		{
			l->m_ext.position = l->calculate_position_for_point(pt).ToRemixFloat3D(); //(pt->position + l->m_attached_position).ToRemixFloat3D();
			l->m_info.radiance = (pt->radiance * pt->radiance_scalar).ToRemixFloat3D();
			l->m_ext.radius = pt->radius;
			l->m_ext.shaping_hasvalue = pt->use_shaping;
			l->m_ext.shaping_value.direction = l->calculate_direction_for_point(pt).ToRemixFloat3D();
			l->m_ext.shaping_value.coneAngleDegrees = pt->degrees;
			l->m_ext.shaping_value.coneSoftness = pt->softness;
			l->m_ext.shaping_value.focusExponent = pt->exponent;
			l->m_ext.volumetricRadianceScale = pt->volumetric_scale;

			// Configure exactly one rig backend. Native IES is chained to the selected compatible analytical geometry.
			l->m_ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			if (!configure_light_backend(l, pt, l->m_ext.shaping_value.direction))
			{
				++m_debug_native_ies_failures;
				push_debug_lifecycle_event("native IES configuration failed", l);
				return false;
			}
			l->m_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			l->m_info.pNext = configure_analytical_light_geometry(l, pt);
			if (!l->m_info.pNext) return false;

			const auto api = remix_api::get();
			if (!api || !remix_api::is_initialized()) {
				return false;
			}

			const auto result = api->m_bridge.CreateLight(&l->m_info, &l->m_handle);
			if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
				push_debug_lifecycle_event("static light updated", l);
				if (pt->light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES) update_ies_emulation_lights(l, pt);
				else destroy_ies_emulation_lights(l);
			}
			else
			{
				++m_debug_create_light_failures;
				push_debug_lifecycle_event("static CreateLight failed", l);
			}
			return result == REMIXAPI_ERROR_CODE_SUCCESS;
		}

		return false;
	}

	/**
	 * Calculate and update remixApi light for the current tick
	 * @param l			The light
	 * @return			True if successfull
	 */
	bool remix_lights::update_remix_light(light* l)
	{
		if (!remix_api::is_initialized()) {
			return false;
		}

		if (!l || (l && !l->m_mover.is_initialized())) {
			return false;
		}

		destroy_map_light(l);

		if (l)
		{
			l->m_mover.interpolate(
				l,
				&l->m_ext.position,
				&l->m_info.radiance,
				&l->m_ext.radius,
				&l->m_ext.shaping_value.direction,
				&l->m_ext.shaping_value.coneAngleDegrees,
				&l->m_ext.shaping_value.coneSoftness,
				&l->m_ext.shaping_value.focusExponent,
				&l->m_ext.volumetricRadianceScale,
				l->is_attached());

			l->m_ext.shaping_hasvalue = l->m_ext.shaping_value.coneAngleDegrees != 180.0f;

			// The current interpolated direction also drives the native IES photometric axis.
			l->m_ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			const auto* backend_point = l->m_def.points.empty() ? nullptr : &l->m_def.points.front();
			if (!configure_light_backend(l, backend_point, l->m_ext.shaping_value.direction))
			{
				++m_debug_native_ies_failures;
				push_debug_lifecycle_event("animated native IES configuration failed", l);
				return false;
			}
			l->m_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			l->m_info.pNext = configure_analytical_light_geometry(l, backend_point);
			if (!l->m_info.pNext) return false;

			const auto api = remix_api::get();
			if (!api || !remix_api::is_initialized()) {
				return false;
			}

			const auto result = api->m_bridge.CreateLight(&l->m_info, &l->m_handle);
			if (result == REMIXAPI_ERROR_CODE_SUCCESS && !l->m_def.points.empty()) {
				push_debug_lifecycle_event("animated light updated", l);
				if (l->m_def.points.front().light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES) update_ies_emulation_lights(l, &l->m_def.points.front());
				else destroy_ies_emulation_lights(l);
			}
			else if (result != REMIXAPI_ERROR_CODE_SUCCESS)
			{
				++m_debug_create_light_failures;
				push_debug_lifecycle_event("animated CreateLight failed", l);
			}
			return result == REMIXAPI_ERROR_CODE_SUCCESS;
		}

		return false;
	}

	/**
	 * Spawns a remixApi light
	 * @param l			The light
	 * @return			True if successfull
	 */
	bool remix_lights::spawn_remix_light(light* l)
	{
		if (!remix_api::is_initialized()) {
			++m_debug_create_light_failures;
			push_debug_lifecycle_event("spawn skipped: Remix API not initialized", l);
			return false;
		}

		if (!l || l->m_def.points.empty()) {
			return false;
		}

		destroy_map_light(l);

		if (l)
		{
			const auto& pt = l->m_def.points[0];
			l->m_ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			l->m_ext.pNext = nullptr;
			l->m_ext.position = pt.position.ToRemixFloat3D();
			// disable single point lights with attach params until they get attached later down the line
			l->m_ext.radius = l->m_def.points.size() == 1u && l->has_attach_parms() ? 0.0f : pt.radius;
			l->m_ext.shaping_hasvalue = pt.use_shaping;
			l->m_ext.shaping_value = {};
			l->m_ext.shaping_value.direction = pt.direction.ToRemixFloat3D();
			l->m_ext.shaping_value.coneAngleDegrees = pt.degrees;
			l->m_ext.shaping_value.coneSoftness = pt.softness;
			l->m_ext.shaping_value.focusExponent = pt.exponent;
			l->m_ext.volumetricRadianceScale = pt.volumetric_scale;

			if (!configure_light_backend(l, &pt, l->m_ext.shaping_value.direction))
			{
				++m_debug_native_ies_failures;
				push_debug_lifecycle_event("spawn native IES configuration failed", l);
				return false;
			}

			l->m_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			l->m_info.pNext = configure_analytical_light_geometry(l, &pt);
			if (!l->m_info.pNext) return false;
			l->m_info.hash = utils::string_hash64(utils::va("api-light%d", l->m_light_num));
			l->m_info.radiance = (pt.radiance * pt.radiance_scalar).ToRemixFloat3D();

			const auto api = remix_api::get();
			if (!api || !remix_api::is_initialized()) {
				return false;
			}

			const auto result = api->m_bridge.CreateLight(&l->m_info, &l->m_handle);
			if (result == REMIXAPI_ERROR_CODE_SUCCESS) {
				push_debug_lifecycle_event("light spawned", l);
				if (pt.light_rig_mode == map_settings::remix_light_settings_s::LIGHT_RIG_MODE_FAKE_IES) update_ies_emulation_lights(l, &pt);
				else destroy_ies_emulation_lights(l);
			}
			else
			{
				++m_debug_create_light_failures;
				push_debug_lifecycle_event("spawn CreateLight failed", l);
			}
			return result == REMIXAPI_ERROR_CODE_SUCCESS;
		}

		return false;
	}

	/**
	 * Adds all map_setting lights to 'm_map_lights' that have no defined trigger and removes them from the map_settings vector
	 */
	void remix_lights::add_all_map_setting_lights_without_creation_trigger()
	{
		// should have happend already, just to make sure
		get()->destroy_all_map_lights();

		auto& msettings = map_settings::get_map_settings();
		for (auto it = msettings.remix_lights.begin(); it != msettings.remix_lights.end();)
		{
			if (!light_is_runtime_enabled(*it)) {
				++it;
				continue;
			}

			if (it->trigger_choreo_name.empty() && !it->trigger_sound_hash) // add lights without a trigger
			{
				m_active_lights.emplace_back(
					light{
						.m_def = *it, // do not move the light if it can be triggered multiple times
						.m_light_num = m_active_light_spawn_tracker++,
						.m_timer = it->kill_delay
					});

				// erase element from the mapsettings vector
				it = msettings.remix_lights.erase(it);

				auto* light = &m_active_lights.back();

				if (light->m_def.points.size() > 1) {
					light->m_mover.init(light->m_def.points, light->m_def.loop, light->m_def.loop_smoothing);
				}

				// spawn it
				get()->spawn_remix_light(light);
			}
			else { ++it; }
		}
	}


	void remix_lights::add_single_map_setting_light_for_editing(map_settings::remix_light_settings_s* def)
	{
		m_active_lights.emplace_back(
			light{
				.m_def = *def, // do not move the light if it can be triggered multiple times
				.m_light_num = m_active_light_spawn_tracker++
			});

		auto* light = &m_active_lights.back();

		if (light->m_def.points.size() > 1) {
			light->m_mover.init(light->m_def.points, true /* always loop*/, light->m_def.loop_smoothing);
		}

		// spawn it
		get()->spawn_remix_light(light);
	}

	/**
	 * Adds a single map setting light to 'm_map_lights' - Immediately spawns it if trigger is not defined
	 * @param def	The map_setting light definition
	 */
	void remix_lights::add_single_map_setting_light(map_settings::remix_light_settings_s* def)
	{
		add_single_map_setting_light_report(def);
	}

	bool remix_lights::add_single_map_setting_light_report(map_settings::remix_light_settings_s* def)
	{
		if (!def) {
			return false;
		}

		if (!imgui::get()->m_light_edit_mode && !light_is_runtime_enabled(*def)) {
			return false;
		}

		m_active_lights.emplace_back(
			light{
				.m_def = *def, // do not move the light if it can be triggered multiple times
				.m_light_num = m_active_light_spawn_tracker++
			});

		// spawn light if it does not use a trigger - triggered spawning is handled elsewhere
		if (auto* light = &m_active_lights.back();
			light && light->m_def.trigger_choreo_name.empty() && !light->m_def.trigger_sound_hash)
		{
			if (light->m_def.points.size() > 1) {
				light->m_mover.init(light->m_def.points, light->m_def.loop, light->m_def.loop_smoothing);
			}

			// spawn it
			return get()->spawn_remix_light(light);
		}

		return true;
	}


	void remix_lights::destroy_source_distant_light()
	{
		if (m_source_distant_handle && remix_api::is_initialized())
		{
			if (auto* api = remix_api::get()) {
				api->m_bridge.DestroyLight(m_source_distant_handle);
			}
		}
		if (const auto dev = game::get_d3d_device()) {
			dev->LightEnable(static_cast<DWORD>(std::clamp(m_source_directional_ff_index, 0, 7)), FALSE);
		}
		m_source_distant_handle = nullptr;
		m_source_distant_info = {};
		m_source_distant_ext = {};
		m_source_distant_ext.sType = static_cast<remixapi_StructType>(7);
		m_source_distant_last_error = REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
		m_source_distant_hash = 0u;
		m_source_distant_draw_calls = 0u;
		m_source_directional_ff_direction = Vector(0.0f, 0.0f, -1.0f);
		m_source_directional_ff_radiance = Vector(0.0f, 0.0f, 0.0f);
		m_source_directional_ff_active = false;
		m_source_directional_ff_draw_calls = 0u;
		m_source_distant_runtime_status = "Source environment light cleared";
	}

	bool remix_lights::upsert_source_distant_light(const std::uint64_t stable_hash, Vector direction,
		const Vector& radiance, const float angular_diameter_degrees, const float volumetric_scale)
	{
		if (direction.LengthSqr() <= 0.000001f || !std::isfinite(direction.x) ||
			!std::isfinite(direction.y) || !std::isfinite(direction.z))
		{
			m_source_distant_runtime_status = "Source environment blocked: invalid direction";
			return false;
		}
		direction.NormalizeChecked();

		Vector safe_radiance = radiance;
		if (!std::isfinite(safe_radiance.x)) safe_radiance.x = 0.0f;
		if (!std::isfinite(safe_radiance.y)) safe_radiance.y = 0.0f;
		if (!std::isfinite(safe_radiance.z)) safe_radiance.z = 0.0f;
		safe_radiance.x = std::max(0.0f, safe_radiance.x);
		safe_radiance.y = std::max(0.0f, safe_radiance.y);
		safe_radiance.z = std::max(0.0f, safe_radiance.z);
		if (std::max({ safe_radiance.x, safe_radiance.y, safe_radiance.z }) <= 0.000001f)
		{
			m_source_distant_runtime_status = "Source environment blocked: zero radiance";
			return false;
		}

		// Keep the Source-global light state independently of the native API result.
		// This gives the ASI a second translation path through D3D9 DIRECTIONAL state.
		destroy_source_distant_light();
		m_source_distant_hash = stable_hash != 0u ? stable_hash : 0x534F555243455355ull; // "SOURCESU"
		m_source_directional_ff_direction = direction;
		m_source_directional_ff_radiance = safe_radiance;
		m_source_directional_ff_active = true;

		if (!remix_api::is_initialized() || !remix_api::get())
		{
			m_source_distant_last_error = REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
			m_source_distant_runtime_status = m_source_directional_ff_enabled
				? "native Distant unavailable; D3D9 directional fallback armed"
				: "Source environment stored, but native API unavailable and FF fallback disabled";
			return m_source_directional_ff_enabled;
		}

		// The paired Remix bridge already serializes Distant as analytical light sType 7.
		// Keep the exact public C layout and verify it at compile time in the header.
		m_source_distant_ext = {};
		m_source_distant_ext.sType = static_cast<remixapi_StructType>(7);
		m_source_distant_ext.pNext = nullptr;
		m_source_distant_ext.direction = direction.ToRemixFloat3D();
		m_source_distant_ext.angularDiameterDegrees = std::clamp(
			std::isfinite(angular_diameter_degrees) ? angular_diameter_degrees : 0.53f, 0.01f, 180.0f);
		m_source_distant_ext.volumetricRadianceScale = std::max(0.0f,
			std::isfinite(volumetric_scale) ? volumetric_scale : 1.0f);

		m_source_distant_info = {};
		m_source_distant_info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
		m_source_distant_info.pNext = &m_source_distant_ext;
		m_source_distant_info.hash = m_source_distant_hash;
		m_source_distant_info.radiance = safe_radiance.ToRemixFloat3D();

		auto* api = remix_api::get();
		m_source_distant_last_error = api->m_bridge.CreateLight(&m_source_distant_info, &m_source_distant_handle);
		if (m_source_distant_last_error != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			m_source_distant_handle = nullptr;
			m_source_distant_runtime_status = std::format(
				"native Distant failed code {} sType 7 size {}; FF directional {} | hash 0x{:016X}",
				static_cast<int>(m_source_distant_last_error), sizeof(m_source_distant_ext),
				m_source_directional_ff_enabled ? "armed" : "disabled", m_source_distant_hash);
			++m_debug_create_light_failures;
			return m_source_directional_ff_enabled;
		}

		m_source_distant_runtime_status = std::format(
			"native Distant active + FF {} | hash 0x{:016X} | dir {:.4f} {:.4f} {:.4f} | diameter {:.3f} | radiance {:.3f} {:.3f} {:.3f}",
			m_source_directional_ff_enabled ? "armed" : "off", m_source_distant_hash,
			direction.x, direction.y, direction.z, m_source_distant_ext.angularDiameterDegrees,
			safe_radiance.x, safe_radiance.y, safe_radiance.z);
		return true;
	}

	bool remix_lights::has_light_with_exact_comment(const std::string_view comment) const
	{
		if (comment.empty()) {
			return false;
		}

		return std::any_of(m_active_lights.begin(), m_active_lights.end(), [&](const light& entry)
		{
			return entry.m_def.comment == comment;
		});
	}

	bool remix_lights::upsert_runtime_light(const map_settings::remix_light_settings_s& def, const bool enabled)
	{
		if (def.comment.empty()) {
			return false;
		}

		auto it = std::find_if(m_active_lights.begin(), m_active_lights.end(), [&](const light& entry)
		{
			return entry.m_def.comment == def.comment;
		});

		if (!enabled)
		{
			if (it != m_active_lights.end())
			{
				destroy_map_light(&*it);
				m_active_lights.erase(it);
			}
			return true;
		}

		if (def.points.empty()) {
			return false;
		}

		if (it == m_active_lights.end())
		{
			auto copy = def;
			return add_single_map_setting_light_report(&copy);
		}

		it->m_def = def;
		it->m_is_marked_for_destruction = false;
		it->m_timer = def.kill_delay;

		if (def.points.size() > 1u)
		{
			it->m_mover = light::interpolator{};
			it->m_mover.init(def.points, def.loop, def.loop_smoothing);
			return spawn_remix_light(&*it);
		}

		return update_static_remix_light(&*it, &def.points.front());
	}

	/**
	 * Destroys a light (remixApi light)
	 * @param l		The light to destroy
	 */
	void remix_lights::destroy_map_light(light* l)
	{
		if (!l) {
			return;
		}

		if (!remix_api::is_initialized()) {
			l->m_handle = nullptr;
			l->m_ies_children.clear();
			return;
		}

		if (l->m_handle || !l->m_ies_children.empty()) {
			push_debug_lifecycle_event("destroy light", l);
		}

		destroy_ies_emulation_lights(l);

		if (l->m_handle)
		{
			remix_api::get()->m_bridge.DestroyLight(l->m_handle);
			l->m_handle = nullptr;
		}
	}

	void remix_lights::destroy_lights_with_comment_prefix(std::string_view prefix)
	{
		if (prefix.empty()) {
			return;
		}

		for (auto it = m_active_lights.begin(); it != m_active_lights.end();)
		{
			if (it->m_def.comment.starts_with(prefix))
			{
				destroy_map_light(&*it);
				it = m_active_lights.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	/**
	 * Destroys all lights in 'm_map_lights' (remixApi lights)
	 */
	void remix_lights::destroy_all_map_lights()
	{
		for (auto& l : m_active_lights) {
			destroy_map_light(&l);
		}
	}

	/**
	 * Destroys all lights in 'm_map_lights' (remixApi lights) and clears 'm_map_lights'
	 */
	void remix_lights::destroy_and_clear_all_active_lights()
	{
		destroy_source_distant_light();
		destroy_all_map_lights();
		m_active_lights.clear();
	}

	/**
	 * Updates all lights in 'm_map_lights'
	 * Handles Destroying, choreo trigger spawning, tick advancing and updating of remixApi lights
	 */
	void remix_lights::update_all_active_lights()
	{
		const auto glob = interfaces::get()->m_globals;
		const auto edit_mode = imgui::get()->m_light_edit_mode;

		// destroy lights that are marked for destruction
		for (auto it = m_active_lights.begin(); it != m_active_lights.end();)
		{
			if (it->m_is_marked_for_destruction)
			{
				// kill delay timer
				if (it->m_timer > 0.0f)
				{
					it->m_timer -= glob->absoluteframetime;
					++it;
				}
				else
				{
					destroy_map_light(&*it);
					it = m_active_lights.erase(it);
				}
			}
			else { ++it; }
		}

		// iterate all map lights
		for (auto& l : m_active_lights)
		{
			if (l.m_mover.is_initialized())
			{
				const auto finished = l.m_mover.advance_time(glob->absoluteframetime);
				update_remix_light(&l);

				if (!edit_mode)
				{
					if (finished && l.m_def.run_once) { // destroy light on next frame
						l.m_is_marked_for_destruction = true;
					}
				}
			}
			else // single point lights
			{
				if (l.has_attach_parms())
				{
					if (l.is_attached()) { // update every frame when attached
						update_static_remix_light(&l, &l.m_def.points.front());
					}
					else if (l.m_ext.radius > 0.0f) // "disable" light when it gets unattached
					{
						auto temp_pt = l.m_def.points.front();
						temp_pt.radius = 0.0f;
						update_static_remix_light(&l, &temp_pt);
					}
				}
			}

			// if light is not yet spawned
			if (!l.m_handle)
			{
				// handle delayed triggering
				if (l.m_timer < l.m_def.trigger_delay) {
					l.m_timer += glob->absoluteframetime;
				}
				else
				{
					if (l.m_def.points.size() > 1) {
						l.m_mover.init(l.m_def.points, l.m_def.loop, l.m_def.loop_smoothing);
					}

					// spawn it
					get()->spawn_remix_light(&l);

					// set timer to kill delay
					l.m_timer = l.m_def.kill_delay;
				}
			}
		}
	}

	// Mirrors current active Remix lights into the old D3D9 fixed-function light table.
	// This does not replace the Remix API light path. It is a compatibility probe/backend for
	// experiments where Remix/bridge might understand FF SetLight state better than custom API lights.
	void remix_lights::emit_ff_setlight_mirror_for_active_lights()
	{
		m_ff_setlight_emitted = 0u;
		m_ff_setlight_failed = 0u;

		if (!m_ff_setlight_mirror_enabled) {
			return;
		}

		const auto dev = game::get_d3d_device();
		if (!dev) {
			++m_ff_setlight_failed;
			return;
		}

		const int start_index = std::clamp(m_ff_setlight_start_index, 0, 7);
		const int max_lights = std::clamp(m_ff_setlight_max_lights, 0, 8 - start_index);
		const float scalar = std::max(0.0f, m_ff_setlight_scalar);
		const float radius_scale = std::max(0.001f, m_ff_setlight_radius_scale);

		int emitted = 0;
		for (const auto& light : m_active_lights)
		{
			if (!light_group_matches_runtime_filter(light.m_def)) {
				continue;
			}

			if (emitted >= max_lights) {
				break;
			}

			if (m_ff_setlight_mirror_only_visible && !light.m_handle) {
				continue;
			}

			D3DLIGHT9 d3d = {};
			d3d.Type = light.m_ext.shaping_hasvalue ? D3DLIGHT_SPOT : D3DLIGHT_POINT;
			d3d.Diffuse.r = std::max(0.0f, light.m_info.radiance.x * scalar);
			d3d.Diffuse.g = std::max(0.0f, light.m_info.radiance.y * scalar);
			d3d.Diffuse.b = std::max(0.0f, light.m_info.radiance.z * scalar);
			d3d.Diffuse.a = 1.0f;
			d3d.Specular = d3d.Diffuse;
			d3d.Ambient.r = d3d.Ambient.g = d3d.Ambient.b = 0.0f;
			d3d.Ambient.a = 1.0f;
			d3d.Position.x = light.m_ext.position.x;
			d3d.Position.y = light.m_ext.position.y;
			d3d.Position.z = light.m_ext.position.z;

			const float range = std::max(0.05f, light.m_ext.radius * radius_scale);
			d3d.Range = range;
			d3d.Attenuation0 = 0.0f;
			d3d.Attenuation1 = 1.0f / range;
			d3d.Attenuation2 = 0.0f;

			if (d3d.Type == D3DLIGHT_SPOT)
			{
				Vector dir(light.m_ext.shaping_value.direction.x, light.m_ext.shaping_value.direction.y, light.m_ext.shaping_value.direction.z);
				if (dir.LengthSqr() <= 0.0001f) {
					dir = Vector(0.0f, 1.0f, 0.0f);
				}
				dir.Normalize();
				d3d.Direction.x = dir.x;
				d3d.Direction.y = dir.y;
				d3d.Direction.z = dir.z;

				const float phi = std::clamp(light.m_ext.shaping_value.coneAngleDegrees, 1.0f, 179.0f) * static_cast<float>(M_PI / 180.0);
				const float softness = std::clamp(light.m_ext.shaping_value.coneSoftness, 0.0f, 1.0f);
				d3d.Phi = phi;
				d3d.Theta = std::max(0.001f, phi * (0.55f + 0.35f * (1.0f - softness)));
				d3d.Falloff = std::max(0.001f, 1.0f + light.m_ext.shaping_value.focusExponent);
			}

			const DWORD index = static_cast<DWORD>(start_index + emitted);
			if (SUCCEEDED(dev->SetLight(index, &d3d)) && SUCCEEDED(dev->LightEnable(index, TRUE))) {
				++emitted;
				++m_ff_setlight_emitted;
			}
			else {
				++m_ff_setlight_failed;
			}
		}

		// Disable leftover mirror slots so stale FF lights do not stay active after a light budget drop.
		for (int i = emitted; i < max_lights; ++i) {
			dev->LightEnable(static_cast<DWORD>(start_index + i), FALSE);
		}
	}

	void remix_lights::emit_source_directional_ff_fallback()
	{
		if (!m_source_directional_ff_active || !m_source_directional_ff_enabled) return;
		const auto dev = game::get_d3d_device();
		if (!dev) return;

		Vector direction = m_source_directional_ff_direction;
		if (direction.LengthSqr() <= 0.000001f) return;
		direction.NormalizeChecked();
		const float peak = std::max({ m_source_directional_ff_radiance.x,
			m_source_directional_ff_radiance.y, m_source_directional_ff_radiance.z, 0.000001f });
		const float scale = std::max(0.0f, m_source_directional_ff_scalar);

		D3DLIGHT9 light = {};
		light.Type = D3DLIGHT_DIRECTIONAL;
		light.Direction.x = direction.x;
		light.Direction.y = direction.y;
		light.Direction.z = direction.z;
		light.Diffuse.r = std::clamp(m_source_directional_ff_radiance.x / peak * scale, 0.0f, 1.0f);
		light.Diffuse.g = std::clamp(m_source_directional_ff_radiance.y / peak * scale, 0.0f, 1.0f);
		light.Diffuse.b = std::clamp(m_source_directional_ff_radiance.z / peak * scale, 0.0f, 1.0f);
		light.Diffuse.a = 1.0f;
		light.Specular = light.Diffuse;
		light.Ambient.a = 1.0f;

		const DWORD index = static_cast<DWORD>(std::clamp(m_source_directional_ff_index, 0, 7));
		if (SUCCEEDED(dev->SetLight(index, &light)) && SUCCEEDED(dev->LightEnable(index, TRUE))) {
			++m_source_directional_ff_draw_calls;
		}
	}

	// Draw all active map lights
	void remix_lights::draw_all_active_lights()
	{
		emit_ff_setlight_mirror_for_active_lights();
		emit_source_directional_ff_fallback();

		if (!remix_api::is_initialized()) {
			return;
		}

		// CreateLight only allocates the native light. Remix requires each active handle
		// to be submitted every frame with DrawLightInstance. The old generic path did
		// this through m_active_lights; the dedicated Source sun must do it explicitly.
		if (m_source_distant_handle)
		{
			remix_api::get()->m_bridge.DrawLightInstance(m_source_distant_handle);
			++m_source_distant_draw_calls;
		}

		const bool edit_mode = imgui::get()->m_light_edit_mode;
		for (auto& l : m_active_lights)
		{
			if (!edit_mode && !light_group_matches_runtime_filter(l.m_def)) {
				continue;
			}

			if (l.m_handle) {
				remix_api::get()->m_bridge.DrawLightInstance(l.m_handle);
			}

			for (auto& child : l.m_ies_children)
			{
				if (child.handle) {
					remix_api::get()->m_bridge.DrawLightInstance(child.handle);
				}
			}
		}
	}

	// #
	// #

	bool light_attachment_is_matching_model(const remix_lights::light& light, const ModelRenderInfo_t& info)
	{
		const bool has_radius = light.m_def.attach_prop_radius != 0.0f;
		const bool has_name = !light.m_def.attach_prop_name.empty();

		if (!has_radius && !has_name) {
			return false;
		}

		// light is tracked via entity index if not -1
		if (light.m_entity_index >= 0)
		{
			if (light.m_entity_index == info.entity_index)
			{
				// sanity check - making sure the entity is still valid
				if (has_radius && utils::float_equal(info.pModel->radius, light.m_def.attach_prop_radius)) {
					return true;
				}

				if (has_name && std::string_view(info.pModel->szPathName).contains(light.m_def.attach_prop_name)) {
					return true;
				}
			}

			return false;
		}

		// radius check if specified
		if (has_radius && !utils::float_equal(info.pModel->radius, light.m_def.attach_prop_radius)) {
			return false;
		}

		// bounds check
		if (!light.m_def.attach_prop_mins.IsZero() || !light.m_def.attach_prop_maxs.IsZero())
		{
			if (!utils::vector::is_point_in_aabb(info.origin, light.m_def.attach_prop_mins, light.m_def.attach_prop_maxs)) {
				return false;
			}
		}

		// name substring check if specified
		if (has_name && !std::string_view(info.pModel->szPathName).contains(light.m_def.attach_prop_name)) {
			return false;
		}

		return true;
	}

	// called from model_renderer::DrawModelExecute::Detour
	void remix_lights::on_draw_model_exec(const ModelRenderInfo_t& info)
	{
		if (cmd::show_mesh_bone_info)
		{
			const auto cutoff_dist = game_settings::get()->debug_info_distance.get_as<float>();
			if (game::get_current_view_origin()->DistToSqr(info.origin) < cutoff_dist * cutoff_dist)
			{
				if (const auto base_animating = game::get_base_animating_for_client_renderable(info.pRenderable);
					base_animating)
				{
					if (const auto studio = game::namespaces::C_BaseAnimating::GetModelPtr(base_animating);
						studio)
					{
						auto studio_mdl = studio->m_pStudioHdr;
						Vector bonePos;
						Vector boneAngles;
						matrix3x4_t bone = {};

						// vis all bones
						for (int i = 0; i < studio_mdl->numbones; i++)
						{
							game::namespaces::C_BaseAnimating::GetBoneTransform(base_animating, i, &bone);

							// C_BaseAnimating::GetBonePosition
							utils::matrix_angles(bone, &boneAngles);
							bonePos.x = bone.m_flMatVal[0][3];
							bonePos.y = bone.m_flMatVal[1][3];
							bonePos.z = bone.m_flMatVal[2][3];

							const auto pBone = studio_mdl->pBone(i);
							const auto bname = pBone->pszName();
							game::debug_add_text_overlay(&bonePos.x, utils::va("Bone: '%d' -- '%s'", i, bname), 0);
							game::debug_add_text_overlay(&bonePos.x, utils::va("Ent Index: '%d'", info.entity_index), 1);
						}
					}
				}
			}
		}

		if (map_settings::get_map_settings().using_any_light_attached_to_prop || imgui::get()->m_light_edit_mode)
		{
			for (auto& light : m_active_lights)
			{
				if (light.has_attach_parms() && light.m_attachframe != m_attachframe_counter)
				{
					if (light_attachment_is_matching_model(light, info))
					{
						light.m_attachframe = m_attachframe_counter; // mark as processed this frame
						light.m_attached_position = info.origin;
						light.m_attached_angle = info.angles;
						light.m_entity_index = info.entity_index; // track entity_index so that the light stays attached even when the ent moves outside

						// logic if light should attach to a bone
						if (cmd::show_mesh_bone_info_attached || light.m_def.attach_bone_index >= 0 || !light.m_def.attach_bone_name.empty())
						{
							if (const auto base_animating = game::get_base_animating_for_client_renderable(info.pRenderable);
								base_animating)
							{
								if (const auto studio = game::namespaces::C_BaseAnimating::GetModelPtr(base_animating);
									studio)
								{
									matrix3x4_t bone = {};
									Vector bonePos;

									// do not redraw info if showing info for every mesh
									if (!cmd::show_mesh_bone_info && cmd::show_mesh_bone_info_attached)
									{
										auto studio_mdl = studio->m_pStudioHdr;
										Vector boneAngles;

										// vis all bones
										for (int i = 0; i < studio_mdl->numbones; i++)
										{
											game::namespaces::C_BaseAnimating::GetBoneTransform(base_animating, i, &bone);

											// C_BaseAnimating::GetBonePosition
											utils::matrix_angles(bone, &boneAngles);
											bonePos.x = bone.m_flMatVal[0][3];
											bonePos.y = bone.m_flMatVal[1][3];
											bonePos.z = bone.m_flMatVal[2][3];

											const auto pBone = studio_mdl->pBone(i);
											const auto bname = pBone->pszName();
											game::debug_add_text_overlay(&bonePos.x, utils::va("Bone: '%d' -- '%s'", i, bname), 0);
											game::debug_add_text_overlay(&bonePos.x, utils::va("Ent Index: '%d'", info.entity_index), 1);
										}
									}

									int bone_idx = light.m_def.attach_bone_index;
									if (bone_idx < 0 && !light.m_def.attach_bone_name.empty()) {
										bone_idx = game::namespaces::C_BaseAnimating::LookupBone(base_animating, light.m_def.attach_bone_name.c_str());
									}

									if (bone_idx >= 0)
									{
										game::namespaces::C_BaseAnimating::GetBoneTransform(base_animating, bone_idx, &bone);

										light.m_attached_bone_forward = {
											bone.m_flMatVal[0][0], bone.m_flMatVal[1][0], bone.m_flMatVal[2][0]
										};

										light.m_attached_bone_right = {
											bone.m_flMatVal[0][1], bone.m_flMatVal[1][1], bone.m_flMatVal[2][1]
										};

										light.m_attached_bone_up = {
											bone.m_flMatVal[0][2], bone.m_flMatVal[1][2], bone.m_flMatVal[2][2]
										};

										// GetBonePosition(i, bonePos, boneAngles);
										utils::matrix_angles(bone, &light.m_attached_angle);
										bonePos.x = bone.m_flMatVal[0][3];
										bonePos.y = bone.m_flMatVal[1][3];
										bonePos.z = bone.m_flMatVal[2][3];

										light.m_attached_position = bonePos;
									}
								}
							}
						}
					}
				}

				// reset tracked entity when the entity could not be found for 5 frames
				if (!light.is_attached() && m_attachframe_counter > light.m_attachframe + 5) {
					light.m_entity_index = -1;
				}
			}
		}
	}

	// called from: choreo_events::scene_ent_on_start_event_hk
	void remix_lights::on_event_start(const std::string_view& name, const std::string_view& actor, const std::string_view& event, const std::string_view& param1)
	{
		// no event trigger in edit mode
		if (imgui::get()->m_light_edit_mode) {
			return;
		}

		auto& msettings = map_settings::get_map_settings();
		for (auto it = msettings.remix_lights.begin(); it != msettings.remix_lights.end();)
		{
			if (!light_is_runtime_enabled(*it)) {
				++it;
				continue;
			}

			if (!it->trigger_choreo_name.empty() && name.contains(it->trigger_choreo_name))
			{
				// check if opt. actor is defined and matches event actor
				if (!it->trigger_choreo_actor.empty() && !actor.contains(it->trigger_choreo_actor)) {
					++it; continue;
				}

				// check if opt. event is defined and matches event string
				if (!it->trigger_choreo_event.empty() && !event.contains(it->trigger_choreo_event)) {
					++it; continue;
				}

				// check if opt. param1 is defined and matches event param1
				if (!it->trigger_choreo_param1.empty() && !param1.contains(it->trigger_choreo_param1)) {
					++it; continue;
				}

				get()->add_single_map_setting_light(&*it);

				// only spawn on the very first play of the vcd
				if (!it->trigger_always) {
					it = msettings.remix_lights.erase(it); // erase element from the mapsettings vector
				}
				else { ++it; }
			}
			else { ++it; }
		}
	}

	// called from: choreo_events::scene_ent_on_finish_event_hk
	void remix_lights::on_event_finish(const std::string_view& name)
	{
		// no event trigger in edit mode
		if (imgui::get()->m_light_edit_mode) {
			return;
		}

		for (auto& l : m_active_lights)
		{
			// only check active lights with a kill trigger not yet marked to be destroyed
			if (l.m_handle && !l.m_def.kill_choreo_name.empty() && !l.m_is_marked_for_destruction)
			{
				if (name.contains(l.m_def.kill_choreo_name)) {
					l.m_is_marked_for_destruction = true;
				}
			}
		}
	}

	void remix_lights::on_sound_start(const std::uint32_t hash)
	{
		// no event trigger in edit mode
		if (imgui::get()->m_light_edit_mode) {
			return;
		}

		// check for kill trigger
		for (auto& l : m_active_lights)
		{
			// only check active lights with a kill trigger not yet marked to be destroyed
			if (l.m_handle && l.m_def.kill_sound_hash && !l.m_is_marked_for_destruction)
			{
				if (l.m_def.kill_sound_hash == hash) {
					l.m_is_marked_for_destruction = true;
				}
			}
		}

		// check for spawn trigger
		auto& msettings = map_settings::get_map_settings();
		for (auto it = msettings.remix_lights.begin(); it != msettings.remix_lights.end();)
		{
			if (!light_is_runtime_enabled(*it)) {
				++it;
				continue;
			}

			if (it->trigger_sound_hash == hash)
			{
				get()->add_single_map_setting_light(&*it);

				// only spawn on the very first play of sound
				if (!it->trigger_always) {
					it = msettings.remix_lights.erase(it); // erase element from the mapsettings vector
				}
				else { ++it; }
			}
			else { ++it; }
		}
	}

	void remix_lights::on_client_frame()
	{
		const auto rml = remix_lights::get();
		if (!rml) {
			return;
		}
		const auto& glob = interfaces::get()->m_globals;

		// check if paused
		rml->m_is_paused = utils::float_equal(glob->frametime, 0.0f);

		if (!rml->m_is_paused && remix_api::is_initialized()) 
		{
			rml->update_all_active_lights();
			rml->debug_print_player_pos_time();

			++m_attachframe_counter;
		}

		rml->draw_all_active_lights();

		if (cmd::show_api_lights && remix_api::is_initialized())
		{
			bool first_done = false;
			for (const auto& l : m_active_lights)
			{
				const Vector circle_pos = &l.m_ext.position.x;
				const float radius = l.m_ext.radius;
				const Vector color = { 1.0f, 1.0f, 1.0f };

				const auto remixapi = remix_api::get();

				// we only need to craft one circle instance - everything else is instanced
				if (!first_done)
				{
					first_done = true;
					remixapi->add_debug_circle(circle_pos, Vector(0.0f, 0.0f, 1.0f), radius - 0.02f, radius * 0.1f, color);
				}
				else {
					remixapi->add_debug_circle_based_on_previous(circle_pos, Vector(0, 0, 90), Vector(1.0f, 1.0f, 1.0f));
				}

				remixapi->add_debug_circle_based_on_previous(circle_pos, Vector(0, 90, 0), Vector(1.0f, 1.0f, 1.0f));
				remixapi->add_debug_circle_based_on_previous(circle_pos, Vector(90, 0, 90), Vector(1.0f, 1.0f, 1.0f));
			}
		}
		
	}

	// called before map_settings
	void remix_lights::on_map_load()
	{
		if (auto* lights = get()) {
			lights->destroy_source_distant_light();
		}

		// reset spawn tracker
		m_active_light_spawn_tracker = 0u;
		m_attachframe_counter = 0u;
		m_debug_create_light_failures = 0u;
		m_debug_native_ies_failures = 0u;
		m_debug_ies_budget_skipped = 0u;
		m_debug_lifecycle_log.clear();
	}

	void remix_lights::debug_print_player_pos_time()
	{
		if (cmd::debug_pos_time)
		{
			const auto& glob = interfaces::get()->m_globals;

			m_dbgpos_last_curtime = glob->curtime;

			// update print timer
			m_dbgpos_print_timer += glob->absoluteframetime;

			const auto* curpos = game::get_current_view_origin();

			// check if player moves
			if (m_dbgpos_last_pos == *curpos)
			{
				m_dbgpos_timer_since_movement = 0.0f;

				if (m_dbgpos_on_steady_once) {
					game::print_ingame("[POS] Movement End\n");
				}

				m_dbgpos_on_steady_once = false;
			}
			else 
			{
				if (!m_dbgpos_on_steady_once)
				{
					m_dbgpos_on_steady_once = true;
					m_dbgpos_timepoint_on_movement = glob->curtime;
					game::print_ingame("[POS] Movement Start\n");
				}

				m_dbgpos_timer_since_movement += glob->absoluteframetime;

				game::print_ingame("> POS [%.2f, %.2f, %.2f] @ timer [%.3f] -- @ delta [%.3f] -- @ curtime [%.3f]\n",
					curpos->x, curpos->y, curpos->z, 
					m_dbgpos_timer_since_movement, 
					glob->curtime - m_dbgpos_timepoint_on_movement,
					glob->curtime);
			}

			// print every 0.2s
			/*if (m_dbgpos_print_timer >= 0.2f)
			{
				m_dbgpos_print_timer = 0.0f;
			}*/

			m_dbgpos_last_pos = *game::get_current_view_origin();
		}
	}

	ConCommand xo_debug_toggle_pos_time_cmd {};
	void xo_debug_toggle_pos_time_fn()
	{
		cmd::debug_pos_time = !cmd::debug_pos_time;
	}

	ConCommand xo_debug_toggle_show_api_lights_cmd {};
	void xo_debug_toggle_show_api_lights_fn()
	{
		cmd::show_api_lights = !cmd::show_api_lights;
	}

	ConCommand xo_debug_show_mesh_bone_info_attached_cmd{};
	void xo_debug_show_mesh_bone_info_attached_fn()
	{
		cmd::show_mesh_bone_info_attached = !cmd::show_mesh_bone_info_attached;

		// disable vis for ALL 
		if (cmd::show_mesh_bone_info_attached) {
			cmd::show_mesh_bone_info = false;
		}
	}

	ConCommand xo_debug_show_mesh_bone_info_cmd{};
	void xo_debug_show_mesh_bone_info_fn()
	{
		cmd::show_mesh_bone_info = !cmd::show_mesh_bone_info;

		// disable specific vis if still active
		if (!cmd::show_mesh_bone_info) {
			cmd::show_mesh_bone_info_attached = false;
		}
	}

	remix_lights::remix_lights()
	{
		p_this = this;

		// #
		// commands

		game::con_add_command(&xo_debug_toggle_pos_time_cmd, "xo_debug_toggle_pos_time", xo_debug_toggle_pos_time_fn, "Toggle debug prints about player position and time (useful for animated lights)");
		game::con_add_command(&xo_debug_toggle_show_api_lights_cmd, "xo_debug_toggle_show_api_lights", xo_debug_toggle_show_api_lights_fn, "Toggle debug vis for lights added via the remixapi");
		game::con_add_command(&xo_debug_show_mesh_bone_info_attached_cmd, "xo_debug_show_mesh_bone_info_attached", xo_debug_show_mesh_bone_info_attached_fn, "Edit Mode + Attached to mesh only: Show bone information of mesh with an attached remixApi light (names/indices)");
		game::con_add_command(&xo_debug_show_mesh_bone_info_cmd, "xo_debug_show_mesh_bone_info", xo_debug_show_mesh_bone_info_fn, "Show bone information for all nearby meshes (names/indices + entity indices)");
	}
}
