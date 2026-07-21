#include "std_include.hpp"
#include "imgui_internal.h"
#include "imgui_helper.hpp"

namespace common::imgui
{
	namespace
	{
		std::atomic<std::uint64_t> g_world2screen_calls = 0u;
		std::atomic<std::uint64_t> g_world2screen_failures = 0u;
		std::atomic<std::uint64_t> g_world2screen_fallback_successes = 0u;

		bool finite_matrix(const VMatrix& matrix)
		{
			bool any_non_zero = false;
			for (int row = 0; row < 4; ++row)
			{
				for (int column = 0; column < 4; ++column)
				{
					const float value = matrix.m[row][column];
					if (!std::isfinite(value) || std::fabs(value) > 1000000.0f) return false;
					any_non_zero |= std::fabs(value) > 0.000001f;
				}
			}
			return any_non_zero;
		}

		bool camera_basis_projection(const Vector& in, Vector& out, const int screen_w, const int screen_h)
		{
			Vector origin = {}, forward = {}, right = {}, up = {};
			if (!game::get_current_view_basis(origin, forward, right, up)) return false;
			const Vector delta = in - origin;
			const float depth = delta.Dot(forward);
			if (!std::isfinite(depth) || depth <= 0.001f) return false;

			float horizontal_fov = 90.0f;
			if (const auto* fov = game::find_cvar_const("fov_desired"); fov)
			{
				const float candidate = fov->m_Value.m_fValue;
				if (std::isfinite(candidate) && candidate >= 20.0f && candidate <= 160.0f) horizontal_fov = candidate;
			}
			const float tan_half_horizontal = std::tan(DEG2RAD(horizontal_fov * 0.5f));
			if (!std::isfinite(tan_half_horizontal) || tan_half_horizontal <= 0.0001f) return false;
			const float aspect = static_cast<float>(screen_w) / static_cast<float>(screen_h);
			const float tan_half_vertical = tan_half_horizontal / std::max(0.1f, aspect);
			out.x = static_cast<float>(screen_w) * 0.5f +
				(delta.Dot(right) / (depth * tan_half_horizontal)) * static_cast<float>(screen_w) * 0.5f;
			out.y = static_cast<float>(screen_h) * 0.5f -
				(delta.Dot(up) / (depth * tan_half_vertical)) * static_cast<float>(screen_h) * 0.5f;
			out.z = depth;
			return std::isfinite(out.x) && std::isfinite(out.y);
		}
	}

	bool world2screen(const Vector& in, Vector& out)
	{
		++g_world2screen_calls;
		out.x = 0.0f;
		out.y = 0.0f;
		out.z = 0.0f;
		const auto interfaces_ptr = interfaces::get();
		if (!interfaces_ptr || !interfaces_ptr->m_engine)
		{
			++g_world2screen_failures;
			return false;
		}

		int screen_w = 0, screen_h = 0;
		interfaces_ptr->m_engine->get_screen_size(screen_w, screen_h);
		if (screen_w <= 0 || screen_h <= 0)
		{
			++g_world2screen_failures;
			return false;
		}

		const auto& matrix = interfaces_ptr->m_engine->world_to_screen_matrix();
		if (finite_matrix(matrix))
		{
			const float x = in.Dot(matrix.m[0]) + matrix.m[0][3];
			const float y = in.Dot(matrix.m[1]) + matrix.m[1][3];
			const float perspective_div = in.Dot(matrix.m[3]) + matrix.m[3][3];
			if (std::isfinite(x) && std::isfinite(y) && std::isfinite(perspective_div) && perspective_div >= 0.001f)
			{
				out.x = static_cast<float>(screen_w) * 0.5f + (x / perspective_div) * static_cast<float>(screen_w) * 0.5f;
				out.y = static_cast<float>(screen_h) * 0.5f - (y / perspective_div) * static_cast<float>(screen_h) * 0.5f;
				out.z = perspective_div;
				const float generous_x = static_cast<float>(screen_w) * 8.0f;
				const float generous_y = static_cast<float>(screen_h) * 8.0f;
				if (std::isfinite(out.x) && std::isfinite(out.y) &&
					std::fabs(out.x) <= generous_x && std::fabs(out.y) <= generous_y) return true;
			}
			// A non-zero but stale Source matrix was one of the gizmo regressions after
			// the Steam update. Do not fail here; try the validated camera basis below.
		}

		if (camera_basis_projection(in, out, screen_w, screen_h))
		{
			++g_world2screen_fallback_successes;
			return true;
		}
		++g_world2screen_failures;
		return false;
	}

	std::uint64_t world2screen_calls() { return g_world2screen_calls.load(); }
	std::uint64_t world2screen_failures() { return g_world2screen_failures.load(); }
	std::uint64_t world2screen_fallback_successes() { return g_world2screen_fallback_successes.load(); }

	void get_and_add_integers_to_set(char* str, std::unordered_set<std::uint32_t>& set, const std::uint32_t& buf_len, const bool clear_buf)
	{
		std::vector<int> temp_vec;
		utils::extract_integer_words(str, temp_vec, true);
		set.insert(temp_vec.begin(), temp_vec.end());

		if (clear_buf) {
			memset(str, 0, buf_len);
		}
	}

	void get_and_remove_integers_from_set(char* str, std::unordered_set<std::uint32_t>& set, const std::uint32_t& buf_len, const bool clear_buf)
	{
		std::vector<int> temp_vec;
		utils::extract_integer_words(str, temp_vec, true);

		for (const auto& v : temp_vec) {
			set.erase(v);
		}

		if (clear_buf) {
			memset(str, 0, buf_len);
		}
	}

	namespace blur
	{
		namespace
		{
			IDirect3DSurface9* rt_backup = nullptr;
			IDirect3DPixelShader9* shader_x = nullptr;
			IDirect3DPixelShader9* shader_y = nullptr;
			IDirect3DTexture9* texture = nullptr;
			std::uint32_t backbuffer_width = 0u;
			std::uint32_t backbuffer_height = 0u;

			void begin_blur([[maybe_unused]] const ImDrawList* parent_list, const ImDrawCmd* cmd)
			{
				const auto device = static_cast<IDirect3DDevice9*>(cmd->UserCallbackData);

				if (!shader_x) {
					device->CreatePixelShader(reinterpret_cast<const DWORD*>(blur_x.data()), &shader_x);
				}

				if (!shader_y) {
					device->CreatePixelShader(reinterpret_cast<const DWORD*>(blur_y.data()), &shader_y);
				}

				IDirect3DSurface9* backBuffer;
				device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

				D3DSURFACE_DESC desc;
				backBuffer->GetDesc(&desc);

				if (backbuffer_width != desc.Width || backbuffer_height != desc.Height)
				{
					if (texture) {
						texture->Release();
					}

					backbuffer_width = desc.Width;
					backbuffer_height = desc.Height;
					device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr);
				}

				device->GetRenderTarget(0, &rt_backup);

				{
					IDirect3DSurface9* surface;
					texture->GetSurfaceLevel(0, &surface);
					device->StretchRect(backBuffer, nullptr, surface, nullptr, D3DTEXF_NONE);
					device->SetRenderTarget(0, surface);
					surface->Release();
				}

				backBuffer->Release();

				device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			}

			void first_blur_pass([[maybe_unused]] const ImDrawList* parent_list, const ImDrawCmd* cmd)
			{
				const auto device = static_cast<IDirect3DDevice9*>(cmd->UserCallbackData);

				device->SetPixelShader(shader_x);
				const float params[4] = { 1.0f / (float)backbuffer_width };
				device->SetPixelShaderConstantF(0, params, 1);
			}

			void second_blur_pass([[maybe_unused]] const ImDrawList* parent_list, const ImDrawCmd* cmd)
			{
				const auto device = static_cast<IDirect3DDevice9*>(cmd->UserCallbackData);

				device->SetPixelShader(shader_y);
				const float params[4] = { 1.0f / (float)backbuffer_height };
				device->SetPixelShaderConstantF(0, params, 1);
			}

			void end_blur([[maybe_unused]] const ImDrawList* parent_list, const ImDrawCmd* cmd)
			{
				const auto device = static_cast<IDirect3DDevice9*>(cmd->UserCallbackData);

				device->SetRenderTarget(0, rt_backup);
				rt_backup->Release();

				device->SetPixelShader(nullptr);
				device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
				device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
			}
		}
	}

	void draw_blur(ImDrawList* draw_list)
	{
		const auto dev = game::get_d3d_device();

		const ImVec2 img_size = { (float)blur::backbuffer_width, (float)blur::backbuffer_height };

		draw_list->AddCallback(blur::begin_blur, dev);
		for (int i = 0; i < 8; ++i)
		{
			draw_list->AddCallback(blur::first_blur_pass, dev);
			draw_list->AddImage((ImTextureID)blur::texture, { 0.0f, 0.0f }, img_size);
			draw_list->AddCallback(blur::second_blur_pass, dev);
			draw_list->AddImage((ImTextureID)blur::texture, { 0.0f, 0.0f }, img_size);
		}

		draw_list->AddCallback(blur::end_blur, dev);
		draw_list->AddImageRounded((ImTextureID)blur::texture,
			{ 0.0f, 0.0f },
			img_size,
			{ 0.00f, 0.00f },
			{ 1.00f, 1.00f },
			IM_COL32(255, 255, 255, 255),
			0.f);
	}

	// V21.14.7 renderer-safe mode: keep the legacy blur entry points as no-ops.
	// The original DX9 callback path sampled a render-target texture while writing
	// to it, which is undefined under DXVK/RTX Remix and may tint the backbuffer.
	void draw_window_blur()
	{
	}

	void draw_background_blur()
	{
	}
}

namespace ImGui
{
	namespace
	{
		StyleColorRecoveryStats g_style_color_recovery_stats = {};
	}

	void SafePopStyleColor(const int count, const int source_line)
	{
		if (count <= 0) return;
		ImGuiContext* context = GetCurrentContext();
		const int available = context ? context->ColorStack.Size : 0;
		const int safe_count = std::clamp(count, 0, available);
		if (safe_count > 0) {
			PopStyleColor(safe_count);
		}
		if (safe_count != count)
		{
			g_style_color_recovery_stats.prevented_underflows += static_cast<std::uint64_t>(count - safe_count);
			g_style_color_recovery_stats.last_source_line = source_line;
			g_style_color_recovery_stats.last_requested_count = count;
			g_style_color_recovery_stats.last_available_count = available;
		}
	}

	int GetStyleColorStackSize()
	{
		const ImGuiContext* context = GetCurrentContext();
		return context ? context->ColorStack.Size : 0;
	}

	void RecoverStyleColorStack(const int target_size, const int source_line)
	{
		ImGuiContext* context = GetCurrentContext();
		if (!context) return;
		const int current = context->ColorStack.Size;
		if (current <= target_size) return;
		const int leaked = current - target_size;
		PopStyleColor(leaked);
		g_style_color_recovery_stats.recovered_leaks += static_cast<std::uint64_t>(leaked);
		g_style_color_recovery_stats.last_source_line = source_line;
		g_style_color_recovery_stats.last_requested_count = leaked;
		g_style_color_recovery_stats.last_available_count = current;
	}

	void RunWithStyleColorCheckpoint(const std::function<void()>& callback, const int source_line)
	{
		const int target_size = GetStyleColorStackSize();
		callback();
		RecoverStyleColorStack(target_size, source_line);
	}

	StyleColorRecoveryStats GetStyleColorRecoveryStats()
	{
		return g_style_color_recovery_stats;
	}

	void ResetStyleColorRecoveryStats()
	{
		g_style_color_recovery_stats = {};
	}
	void Spacing(const float& x, const float& y) {
		Dummy(ImVec2(x, y));
	}

	void PushFont(common::imgui::font::FONTS font)
	{
		ImGuiIO& io = GetIO();

		if (io.Fonts->Fonts[font]) {
			PushFont(io.Fonts->Fonts[font]);
		}
		else {
			PushFont(GetDefaultFont());
		}
	}

	void SeparatorTextLarge(const char* text, bool pre_spacing)
	{
		if (pre_spacing) {
			Spacing(0, 12);
		}

		PushFont(common::imgui::font::BOLD_LARGE);
		SeparatorText(text);
		PopFont();
		Spacing(0, 4);
	}

	// Calculates the width for each buttons to fit in a single row, accounting for ImGui's content region and inter-button spacing
	float CalcButtonWidthSameRow(std::uint8_t btn_count)
	{
		const auto b = static_cast<float>(btn_count);
		return (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * std::max(b, 0.0f)) / std::max(b, 1.0f);
	}

	// Draw wrapped text containing all unsigned integers from the provided unordered_set
	void TextWrapped_IntegersFromUnorderedSet(const std::unordered_set<std::uint32_t>& set)
	{
		std::string arr_str;
		for (auto it = set.begin(); it != set.end(); ++it)
		{
			if (it != set.begin()) {
				arr_str += ", ";
			} arr_str += std::to_string(*it);
		}
		if (arr_str.empty()) {
			arr_str = "// empty";
		}
		TextWrapped("%s", arr_str.c_str());
	}

	void Widget_UnorderedSetModifier(const char* id, Widget_UnorderedSetModifierFlags flag, std::unordered_set<std::uint32_t>& set, char* buffer, std::uint32_t buffer_len)
	{
		const auto txt_input_full = "Add/Remove..";
		const auto txt_input_full_width = CalcTextSize(txt_input_full).x;
		const auto txt_input_min = "...";
		const auto txt_input_min_width = CalcTextSize(txt_input_min).x;

		const bool narrow = GetContentRegionAvail().x < 100.0f;

		PushID(id);

		if (!narrow) 
		{
			if (Button("-##Remove"))
			{
				common::imgui::get_and_remove_integers_from_set(buffer, set, buffer_len, true);
				main_module::trigger_vis_logic();
			}
			SetCursorScreenPos(ImVec2(GetItemRectMax().x, GetItemRectMin().y));
		}

		const auto spos = GetCursorScreenPos();
		
		SetNextItemWidth(GetContentRegionAvail().x - (narrow ? 0.0f : 40.0f));
		if (InputText("##Input", buffer, buffer_len, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_EscapeClearsAll)) {
			common::imgui::get_and_add_integers_to_set(buffer, set, buffer_len, true);
		}

		SetCursorScreenPos(spos);
		if (!buffer[0])
		{
			const auto min_content_area_width = GetContentRegionAvail().x - 40.0f;
			ImVec2 pos = GetCursorScreenPos() + ImVec2(8.0f, CalcTextSize("A").y * 0.45f);
			if (min_content_area_width > txt_input_full_width) {
				GetWindowDrawList()->AddText(pos, GetColorU32(ImGuiCol_TextDisabled), txt_input_full);
			}
			else if (min_content_area_width > txt_input_min_width) {
				GetWindowDrawList()->AddText(pos, GetColorU32(ImGuiCol_TextDisabled), txt_input_min);
			}
		}

		if (narrow) 
		{
			// next line :>
			Dummy(ImVec2(0, GetFrameHeight()));
			if (Button("-##Remove"))
			{
				common::imgui::get_and_remove_integers_from_set(buffer, set, buffer_len, true);
				main_module::trigger_vis_logic();
			}
		}

		SetCursorScreenPos(ImVec2(GetItemRectMax().x, GetItemRectMin().y));
		if (Button("+##Add")) {
			common::imgui::get_and_add_integers_to_set(buffer, set, buffer_len, true);
		}
		SetCursorScreenPos(ImVec2(GetItemRectMax().x + 1.0f, GetItemRectMin().y));
		if (Button("P##Picker"))
		{
			const auto c_str = utils::va("%d", flag == Widget_UnorderedSetModifierFlags_Leaf ? g_current_leaf : g_current_area);
			common::imgui::get_and_add_integers_to_set((char*)c_str, set);
			main_module::trigger_vis_logic();
		}
		SetItemTooltipBlur(flag == Widget_UnorderedSetModifierFlags_Leaf ? "Pick Current Leaf" : "Pick Current Area");
		PopID();
	}

	// #

	void Style_DeleteButtonPush()
	{
		PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.05f, 0.05f, 1.0f));
		PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.05f, 0.05f, 1.0f));
		PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.75f, 0.2f, 0.2f, 1.0f));
		PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 1.0f));

		PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		PushFont(common::imgui::font::BOLD);
	}

	void Style_DeleteButtonPop()
	{
		PopStyleVar(2);
		SafePopStyleColor(4, __LINE__);
		PopFont();
	}

	// #

	void Style_ColorButtonPush(const ImVec4& base_color, bool black_border)
	{
		PushStyleColor(ImGuiCol_Button, base_color);
		PushStyleColor(ImGuiCol_ButtonHovered, base_color * ImVec4(1.4f, 1.4f, 1.4f, 1.0f));
		PushStyleColor(ImGuiCol_ButtonActive, base_color * ImVec4(1.8f, 1.8f, 1.8f, 1.0f));

		PushStyleColor(ImGuiCol_Border, black_border 
			? ImVec4(0, 0, 0, 1.0f) 
			: GetStyleColorVec4(ImGuiCol_Border));
	}

	void Style_ColorButtonPop() {
		SafePopStyleColor(4, __LINE__);
	}

	// #

	void Style_InvisibleSelectorPush() {
		PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
		PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
	}

	void Style_InvisibleSelectorPop() {
		SafePopStyleColor(2, __LINE__);
	}

	// #

	bool BeginTooltipBlurEx(ImGuiTooltipFlags tooltip_flags, ImGuiWindowFlags extra_window_flags)
	{
		ImGuiContext& g = *GImGui;

		const bool is_dragdrop_tooltip = g.DragDropWithinSource || g.DragDropWithinTarget;
		if (is_dragdrop_tooltip)
		{
			if ((g.NextWindowData.Flags & ImGuiNextWindowDataFlags_HasPos) == 0)
			{
				ImVec2 tooltip_pos = (g.IO.MousePos * g.Style.MouseCursorScale);
				ImVec2 tooltip_pivot = ImVec2(0.0f, 0.0f);
				SetNextWindowPos(tooltip_pos, ImGuiCond_None, tooltip_pivot);
			}

			SetNextWindowBgAlpha(g.Style.Colors[ImGuiCol_PopupBg].w * 0.60f);
			tooltip_flags |= ImGuiTooltipFlags_OverridePrevious;
		}

		const char* window_name_template = is_dragdrop_tooltip ? "##Tooltip_DragDrop_%02d" : "##Tooltip_%02d";
		char window_name[32];
		ImFormatString(window_name, IM_ARRAYSIZE(window_name), window_name_template, g.TooltipOverrideCount);

		if ((tooltip_flags & ImGuiTooltipFlags_OverridePrevious) && g.TooltipPreviousWindow != nullptr && g.TooltipPreviousWindow->Active)
		{
			SetWindowHiddenAndSkipItemsForCurrentFrame(g.TooltipPreviousWindow);
			ImFormatString(window_name, IM_ARRAYSIZE(window_name), window_name_template, ++g.TooltipOverrideCount);
		}

		ImGuiWindowFlags flags = ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
		Begin(window_name, nullptr, flags | extra_window_flags, &common::imgui::draw_window_blur_callback);
		return true;
	}

	void SetItemTooltipBlur(const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);

		if (IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		{
			// (0.124f, 0.124f, 0.124f, 0.776f)
			PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.124f, 0.124f, 0.124f, 0.776f));

			if (!BeginTooltipBlurEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None)) {
				return;
			}
			SafePopStyleColor(1, __LINE__);

			const auto padding = 4.0f;

			Spacing(0, padding);			 // top padding
			Spacing(padding, 0); SameLine(); // left padding

			TextV(fmt, args);

			SameLine(); Spacing(padding, 0); // right padding
			Spacing(0, padding);			 // bottom padding

			EndTooltip();
		}

		va_end(args);
	}

	void TableHeadersRowWithTooltip(const char** tooltip_strings)
	{
		const float row_height = TableGetHeaderRowHeight();
		TableNextRow(ImGuiTableRowFlags_Headers, row_height);
		const float row_y1 = GetCursorScreenPos().y;

		const int columns_count = TableGetColumnCount();
		for (int column_n = 0; column_n < columns_count; column_n++)
		{
			if (!TableSetColumnIndex(column_n)) {
				continue;
			}

			TableHeader(TableGetColumnName(column_n));

			if (!std::string_view(tooltip_strings[column_n]).empty()) {
				SetItemTooltipBlur("%s", tooltip_strings[column_n]);
			}

			// Allow opening popup from the right-most section after the last column.
			ImVec2 mouse_pos = GetMousePos();

			if (IsMouseReleased(1) && TableGetHoveredColumn() == columns_count)
			{
				if (mouse_pos.y >= row_y1 && mouse_pos.y < row_y1 + row_height) {
					TableOpenContextMenu(columns_count); // Will open a non-column-specific popup.
				}
			}
		}
	}

	// #

	float CalcWidgetWidthForChild(const float label_width)
	{
		return GetContentRegionAvail().x - 4.0f - (label_width + GetStyle().ItemInnerSpacing.x + GetStyle().FramePadding.y);
	}

	void CenterText(const char* text, bool disabled)
	{
		SetCursorPosX(GetContentRegionAvail().x * 0.5f - CalcTextSize(text).x * 0.5f);
		if (!disabled) {
			TextUnformatted(text);
		}
		else {
			TextDisabled("%s", text);
		}
	}

	bool TextUnformatted_ClippedByColumnTooltip(const char* str)
	{
		TextUnformatted(str);
		if (CalcTextSize(str).x > GetColumnWidth()) 
		{
			SetItemTooltipBlur(str);
			return true;
		}

		return false;
	}

	void Draw3DCircle(ImDrawList* draw_list, const Vector& world_pos, const Vector& normal, const float radius, const bool filled, const ImColor& color, const float& thickness, int num_points)
	{
		num_points = std::clamp(num_points, 7, 200);
		static ImVec2 points[200];

		float step = 6.2831f / (float)num_points;
		float theta = 0.f;

		// Create a vector that's not parallel to the normal
		Vector temp = (std::abs(normal.x) > 0.9f) ? Vector(0, 1, 0) : Vector(1, 0, 0);

		// Right vector is perpendicular to the normal and temp vector
		Vector right = normal.Cross(temp);
		right.Normalize();

		// Up vector is perpendicular to both normal and right vectors
		Vector up = right.Cross(normal);
		up.Normalize();

		for (auto i = 0u; i < (std::uint32_t)num_points; i++, theta += step) 
		{
			//Vector world_space = { world_pos.x + radius * cos(theta), world_pos.y, world_pos.z - radius * sin(theta) };
			Vector world_space = world_pos + (right * cos(theta) + up * sin(theta)) * radius;
			Vector screen_space;
			common::imgui::world2screen(world_space, screen_space);

			points[i].x = screen_space.x;
			points[i].y = screen_space.y;
		}

		if (filled) {
			draw_list->AddConvexPolyFilled(points, num_points, color);
		} else {
			draw_list->AddPolyline(points, num_points, color, true, thickness);
		}
	}

	void TableHeaderDropshadow(const float height, const float max_alpha, const float neg_y_offset, const float custom_width)
	{
		const float dshadow_height = height;
		const auto dshadow_pmin = GetCursorScreenPos() - ImVec2(0, neg_y_offset);
		const auto dshadow_pmax = dshadow_pmin + ImVec2((custom_width > 0.0f ? custom_width : GetContentRegionAvail().x), dshadow_height);
		const auto col_top = ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.0f));
		const auto col_bottom = ColorConvertFloat4ToU32(ImVec4(0, 0, 0, max_alpha));
		GetWindowDrawList()->AddRectFilledMultiColor(dshadow_pmin, dshadow_pmax, col_top, col_top, col_bottom, col_bottom);
		Spacing(0, dshadow_height - 3.0f - neg_y_offset);
	}

	// Labelwidth = 80
	bool Widget_PrettyDragVec3WithColorPicker(const char* ID, float* vec_in, bool show_label, const float label_size, const float speed, const float min, const float max,
		const char* x_str, const char* y_str, const char* z_str)
	{
		auto left_label_button = [](const char* label, const ImVec2& button_size, const ImVec4& text_color, const ImVec4& bg_color)
			{
				bool clicked = false;

				//PushFont(common::imgui::font::REGULAR);
				PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 5.0f));
				PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

				PushStyleColor(ImGuiCol_Text, text_color);
				PushStyleColor(ImGuiCol_Border, GetColorU32(ImGuiCol_Border));
				PushStyleColor(ImGuiCol_Button, bg_color); // GetColorU32(ImGuiCol_FrameBg)
				PushStyleColor(ImGuiCol_ButtonHovered, bg_color);

				if (ButtonEx(label, button_size, ImGuiButtonFlags_MouseButtonMiddle)) {
					clicked = true;
				}

				SafePopStyleColor(4, __LINE__);
				PopStyleVar(2);
				//PopFont();

				SameLine();
				SetCursorPosX(GetCursorPosX() - 1.0f);

				return clicked;
			};

		// ---------------
		bool dirty = false;

		PushID(ID);
		PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));

		const float line_height = GetFrameHeight();
		const auto  button_size = ImVec2(line_height - 2.0f, line_height);
		const float widget_spacing = 4.0f;

		//ImVec2 label_size = CalcTextSize(ID, nullptr, true);
		//label_size.x = ImMax(label_size.x, 80.0f);

		const float widget_width_horz = (GetContentRegionAvail().x - 3.0f * button_size.x - 2.0f * widget_spacing -
			(show_label ? label_size + GetStyle().ItemInnerSpacing.x + GetStyle().FramePadding.y : 0.0f)) * 0.333333f;

		/*const float widget_width_vert = (GetContentRegionAvail().x - 3.0f * button_size.x - 2.0f * widget_spacing -
			(show_label ? label_size.x + GetStyle().ItemInnerSpacing.x + GetStyle().FramePadding.y : 0.0f));*/

		const bool  narrow_window = GetWindowWidth() < 440.0f;

		// label if window width < min
		if (narrow_window) {
			SeparatorText(ID);
		}

		// -------
		// -- X --

		if (left_label_button(x_str, button_size, ImVec4(0.84f, 0.55f, 0.53f, 1.0f), ImVec4(0.21f, 0.16f, 0.16f, 1.0f))) {
			vec_in[0] = 0.0f; dirty = true;
		}

		SetNextItemWidth(!narrow_window ? widget_width_horz : -1);
		if (DragFloat("##X", &vec_in[0], speed, min, max, "%.2f")) {
			dirty = true;
		}


		// -------
		// -- Y --

		if (!narrow_window) {
			SameLine(0, widget_spacing);
		}

		if (left_label_button(y_str, button_size, ImVec4(0.73f, 0.78f, 0.5f, 1.0f), ImVec4(0.17f, 0.18f, 0.15f, 1.0f))) {
			vec_in[1] = 0.0f; dirty = true;
		}

		SetNextItemWidth(!narrow_window ? widget_width_horz : -1);
		if (DragFloat("##Y", &vec_in[1], speed, min, max, "%.2f")) {
			dirty = true;
		}

		// -------
		// -- Z --

		if (!narrow_window) {
			SameLine(0, widget_spacing);
		}

		if (left_label_button(z_str, button_size, ImVec4(0.67f, 0.71f, 0.79f, 1.0f), ImVec4(0.18f, 0.21f, 0.23f, 1.0f))) {
			vec_in[2] = 0.0f; dirty = true;
		}

		SetNextItemWidth(!narrow_window ? widget_width_horz : -1);
		if (DragFloat("##Z", &vec_in[2], speed, min, max, "%.2f")) {
			dirty = true;
		}

		SameLine(0, widget_spacing);
		ColorEdit4("##Colorpicker", &vec_in[0], ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_PickerHueBar);

		PopStyleVar();
		PopID();

		// right label if window width > min 
		if (!narrow_window)
		{
			SameLine(0, GetStyle().ItemInnerSpacing.x);
			TextUnformatted(ID);
		}

		return dirty;
	}

	// Labelwidth = 80
	bool Widget_PrettyDragVec3(const char* ID, float* vec_in, bool show_label, const float label_size, const float speed, const float min, const float max,
		const char* x_str, const char* y_str, const char* z_str)
	{
		auto left_label_button = [](const char* label, const ImVec2& button_size, const ImVec4& text_color, const ImVec4& bg_color)
			{
				bool clicked = false;

				//PushFont(common::imgui::font::REGULAR);
				PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 5.0f));
				PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

				PushStyleColor(ImGuiCol_Text, text_color);
				PushStyleColor(ImGuiCol_Border, GetColorU32(ImGuiCol_Border));
				PushStyleColor(ImGuiCol_Button, bg_color); // GetColorU32(ImGuiCol_FrameBg)
				PushStyleColor(ImGuiCol_ButtonHovered, bg_color);

				if (ButtonEx(label, button_size, ImGuiButtonFlags_MouseButtonMiddle)) {
					clicked = true;
				}

				SafePopStyleColor(4, __LINE__);
				PopStyleVar(2);
				//PopFont();

				SameLine();
				SetCursorPosX(GetCursorPosX() - 1.0f);

				return clicked;
			};

		// ---------------
		bool dirty = false;

		PushID(ID);
		PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));

		const float line_height = GetFrameHeight();
		const auto  button_size = ImVec2(line_height - 2.0f, line_height);
		const float widget_spacing = 4.0f;

		//ImVec2 label_size = CalcTextSize(ID, nullptr, true);
		//label_size.x = ImMax(label_size.x, 80.0f);

		const float widget_width_horz = (GetContentRegionAvail().x - 3.0f * button_size.x - 2.0f * widget_spacing -
			(show_label ? label_size + GetStyle().ItemInnerSpacing.x + GetStyle().FramePadding.y : 0.0f)) * 0.333333f;

		/*const float widget_width_vert = (GetContentRegionAvail().x - 3.0f * button_size.x - 2.0f * widget_spacing -
			(show_label ? label_size.x + GetStyle().ItemInnerSpacing.x + GetStyle().FramePadding.y : 0.0f));*/

		const bool  narrow_window = GetWindowWidth() < 440.0f;

		// label if window width < min
		if (narrow_window) {
			SeparatorText(ID);
		}

		// -------
		// -- X --

		if (left_label_button(x_str, button_size, ImVec4(0.84f, 0.55f, 0.53f, 1.0f), ImVec4(0.21f, 0.16f, 0.16f, 1.0f))) {
			vec_in[0] = 0.0f; dirty = true;
		}

		SetNextItemWidth(!narrow_window ? widget_width_horz : -1);
		if (DragFloat("##X", &vec_in[0], speed, min, max, "%.2f")) {
			dirty = true;
		}


		// -------
		// -- Y --

		if (!narrow_window) {
			SameLine(0, widget_spacing);
		}

		if (left_label_button(y_str, button_size, ImVec4(0.73f, 0.78f, 0.5f, 1.0f), ImVec4(0.17f, 0.18f, 0.15f, 1.0f))) {
			vec_in[1] = 0.0f; dirty = true;
		}

		SetNextItemWidth(!narrow_window ? widget_width_horz : -1);
		if (DragFloat("##Y", &vec_in[1], speed, min, max, "%.2f")) {
			dirty = true;
		}

		// -------
		// -- Z --

		if (!narrow_window) {
			SameLine(0, widget_spacing);
		}

		if (left_label_button(z_str, button_size, ImVec4(0.67f, 0.71f, 0.79f, 1.0f), ImVec4(0.18f, 0.21f, 0.23f, 1.0f))) {
			vec_in[2] = 0.0f; dirty = true;
		}

		SetNextItemWidth(!narrow_window ? widget_width_horz : -1);
		if (DragFloat("##Z", &vec_in[2], speed, min, max, "%.2f")) {
			dirty = true;
		}

		PopStyleVar();
		PopID();

		// right label if window width > min 
		if (!narrow_window)
		{
			SameLine(0, GetStyle().ItemInnerSpacing.x);
			TextUnformatted(ID);
		}

		return dirty;
	}

	bool Widget_PrettyStepVec3(const char* ID, float* vec_in, bool show_labels, const float label_size, const float step_amount,
		const char* x_str, const char* y_str, const char* z_str)
	{
		auto left_label_button = [](const char* label, const ImVec2& button_size, const ImVec4& text_color, const ImVec4& bg_color)
			{
				bool clicked = false;

				//PushFont(common::imgui::font::REGULAR);
				PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 5.0f));
				PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

				PushStyleColor(ImGuiCol_Text, text_color);
				PushStyleColor(ImGuiCol_Border, GetColorU32(ImGuiCol_Border));
				PushStyleColor(ImGuiCol_Button, bg_color); // GetColorU32(ImGuiCol_FrameBg)
				PushStyleColor(ImGuiCol_ButtonHovered, bg_color);

				if (ButtonEx(label, button_size, ImGuiButtonFlags_MouseButtonMiddle)) {
					clicked = true;
				}

				SafePopStyleColor(4, __LINE__);
				PopStyleVar(2);
				//PopFont();

				SameLine();
				SetCursorPosX(GetCursorPosX() - 1.0f);

				return clicked;
			};

		auto& style = GetStyle();

		// ---------------
		bool dirty = false;

		PushID(ID);
		PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 4));

		const float line_height = GetFrameHeight();
		const auto  button_size = ImVec2(line_height - 2.0f, line_height);
		const float widget_spacing = 4.0f;

		//ImVec2 label_size = CalcTextSize(ID, nullptr, true);
		//label_size.x = ImMax(label_size.x, 80.0f);

		const float widget_width_horz = (GetContentRegionAvail().x - 3.0f * button_size.x - 2.0f * widget_spacing -
			(show_labels ? label_size + /*style.ItemInnerSpacing.x +*/ style.FramePadding.y : style.ItemInnerSpacing.x + style.FramePadding.y)) * 0.333333f;

		const ImVec2 step_button_size = ImVec2(widget_width_horz * 0.5f - widget_spacing - 3.0f, line_height);

		// -------
		// -- X --

		if (left_label_button(x_str, button_size, ImVec4(0.84f, 0.55f, 0.53f, 1.0f), ImVec4(0.21f, 0.16f, 0.16f, 1.0f))) {
			vec_in[0] = 0.0f; dirty = true;
		}

		PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
		if (ButtonEx("-##X", step_button_size))
		{
			DataTypeApplyOp(ImGuiDataType_Float, '-', &vec_in[0], &vec_in[0], &step_amount);
			dirty = true;
		}
		SameLine(); SetCursorPosX(GetCursorPosX() - 1.0f);
		if (ButtonEx("+##X", step_button_size))
		{
			DataTypeApplyOp(ImGuiDataType_Float, '+', &vec_in[0], &vec_in[0], &step_amount);
			dirty = true;
		}
		PopItemFlag();

		// -------
		// -- Y --

		SameLine(0, widget_spacing - 1);
		if (left_label_button(y_str, button_size, ImVec4(0.73f, 0.78f, 0.5f, 1.0f), ImVec4(0.17f, 0.18f, 0.15f, 1.0f))) {
			vec_in[1] = 0.0f; dirty = true;
		}

		PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
		if (ButtonEx("-##Y", step_button_size))
		{
			DataTypeApplyOp(ImGuiDataType_Float, '-', &vec_in[1], &vec_in[1], &step_amount);
			dirty = true;
		}
		SameLine(); SetCursorPosX(GetCursorPosX() - 1.0f);
		if (ButtonEx("+##Y", step_button_size))
		{
			DataTypeApplyOp(ImGuiDataType_Float, '+', &vec_in[1], &vec_in[1], &step_amount);
			dirty = true;
		}
		PopItemFlag();

		// -------
		// -- Z --

		
		SameLine(0, widget_spacing - 1);
		if (left_label_button(z_str, button_size, ImVec4(0.67f, 0.71f, 0.79f, 1.0f), ImVec4(0.18f, 0.21f, 0.23f, 1.0f))) {
			vec_in[2] = 0.0f; dirty = true;
		}

		PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
		if (ButtonEx("-##Z", step_button_size))
		{
			DataTypeApplyOp(ImGuiDataType_Float, '-', &vec_in[2], &vec_in[2], &step_amount);
			dirty = true;
		}
		SameLine(); SetCursorPosX(GetCursorPosX() - 1.0f);
		if (ButtonEx("+##Z", step_button_size))
		{
			DataTypeApplyOp(ImGuiDataType_Float, '+', &vec_in[2], &vec_in[2], &step_amount);
			dirty = true;
		}
		PopItemFlag();

		PopStyleVar();
		PopID();

		if (show_labels)
		{
			SameLine(0, GetStyle().ItemInnerSpacing.x);
			TextUnformatted(ID);
		}

		return dirty;
	}

	/// Custom Collapsing Header with changeable height - Background color: ImGuiCol_HeaderActive 
	/// @param title_text	Label
	/// @param height		Header Height
	/// @param border_color Border Color
	/// @param default_open	True to collapse by default
	/// @param pre_spacing	8y Spacing in-front of Header
	/// @return				False if Header collapsed
	bool Widget_WrappedCollapsingHeader(const char* title_text, const float height, const ImVec4& border_color, const bool default_open, const bool pre_spacing)
	{
		if (pre_spacing) {
			Spacing(0.0f, 8.0f);
		}

		PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, height));
		PushStyleColor(ImGuiCol_Border, border_color);

		const auto open_flag = default_open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;

		ImGuiContext& g = *GImGui;
		ImGuiWindow* window = GetCurrentWindow();
		ImGuiID storage_id = (g.NextItemData.HasFlags & ImGuiNextItemDataFlags_HasStorageID) ? g.NextItemData.StorageId : window->GetID(title_text);
		const bool is_open = TreeNodeUpdateNextOpen(storage_id, open_flag);

		if (is_open) {
			PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
		}

		const auto state = CollapsingHeader(title_text, open_flag | ImGuiTreeNodeFlags_SpanFullWidth);

		if (is_open) {
			SafePopStyleColor(1, __LINE__);
		}

		if (IsItemHovered() && IsMouseClicked(ImGuiMouseButton_Middle, false)) {
			SetScrollHereY(0.0f);
		}

		// just toggled
		if (state && is_open != state) {
			//SetScrollHereY(0.0f); 
		}

		SafePopStyleColor(1, __LINE__);
		PopStyleVar(2);

		return state;
	}

	float Widget_ContainerWithCollapsingTitle(const char* child_name, const float child_height, const std::function<void()>& callback, const bool default_open, const char* icon, const ImVec4* bg_col, const ImVec4* border_col)
	{
		const std::string child_str = "[ "s + child_name + " ]"s;
		const float child_indent = 2.0f;

		const ImVec4 background_color = bg_col ? *bg_col : ImVec4(0.220f, 0.220f, 0.220f, 0.863f);
		const ImVec4 border_color = border_col ? *border_col : ImVec4(0.099f, 0.099f, 0.099f, 0.901f);

		const auto& style = GetStyle();

		const auto window = GetCurrentWindow();
		const auto min_x = window->WorkRect.Min.x - style.WindowPadding.x * 0.5f + 1.0f;
		const auto max_x = window->WorkRect.Max.x + style.WindowPadding.x * 0.5f - 1.0f;

		PushFont(common::imgui::font::BOLD);

		const auto spos_pre_header = GetCursorScreenPos();
		const auto expanded = Widget_WrappedCollapsingHeader(child_str.c_str(), 12.0f, border_color, default_open, false);

		PopFont();

		if (icon)
		{
			const auto spos_post_header = GetCursorScreenPos();
			const auto header_dims = GetItemRectSize();
			const auto icon_dims = CalcTextSize(icon);
			SetCursorScreenPos(spos_pre_header + ImVec2(header_dims.x - icon_dims.x - style.WindowPadding.x - 8.0f, header_dims.y * 0.5f - icon_dims.y * 0.5f));
			TextUnformatted(icon);
			SetCursorScreenPos(spos_post_header);
		}

		if (expanded)
		{
			const auto min = ImVec2(min_x, GetCursorScreenPos().y - style.ItemSpacing.y);
			const auto max = ImVec2(max_x, min.y + child_height);

			GetWindowDrawList()->AddRect(min + ImVec2(-1, 1), max + ImVec2(1, 1), ColorConvertFloat4ToU32(border_color), 10.0f, ImDrawFlags_RoundCornersBottom);
			GetWindowDrawList()->AddRectFilled(min, max, ColorConvertFloat4ToU32(background_color), 10.0f, ImDrawFlags_RoundCornersBottom);

			// dropshadow
			{
				const auto dshadow_pmin = GetCursorScreenPos() - ImVec2(GetStyle().WindowPadding.x * 0.5f, 4);
				const auto dshadow_pmax = dshadow_pmin + ImVec2(GetContentRegionAvail().x + GetStyle().WindowPadding.x, 48.0f);

				const auto col_bottom = ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.0f));
				const auto col_top = ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.4f));
				GetWindowDrawList()->AddRectFilledMultiColor(dshadow_pmin, dshadow_pmax, col_top, col_top, col_bottom, col_bottom);
			}

			PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.0f, 4.0f));
			PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f, 8.0f));
			BeginChild(child_name, ImVec2(max.x - min.x - style.FramePadding.x - 2.0f, 0.0f),
				/*ImGuiChildFlags_Borders | */ ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
				ImGuiWindowFlags_HorizontalScrollbar);

			Indent(child_indent);
			// V21.4: do not clip the auto-resizing child to the height cached by the
			// previous frame. The old clip rectangle made newly revealed controls
			// visible to layout but impossible to see or scroll to until the cached
			// height happened to catch up. The child itself already clips to the
			// parent window, so an additional stale-height clip is both redundant
			// and unsafe for dynamic/collapsible content.
			callback();
			Unindent(child_indent);

			EndChild();
			PopStyleVar(2);
		}
		SetCursorScreenPos(GetCursorScreenPos() + ImVec2(0, expanded ? 36.0f : 8.0f));
		return GetItemRectSize().y + 6.0f/*- 28.0f*/;
	}


	float Widget_ContainerWithDropdownShadow(const float container_height, const std::function<void()>& callback, const ImVec4* bg_col, const ImVec4* border_col)
	{
		const ImVec4 background_color = bg_col ? *bg_col : ImVec4(0.220f, 0.220f, 0.220f, 0.863f);
		const ImVec4 border_color = border_col ? *border_col : ImVec4(0.099f, 0.099f, 0.099f, 0.901f);

		const auto& style = GetStyle();

		const auto window = GetCurrentWindow();
		const auto min_x = window->WorkRect.Min.x - style.WindowPadding.x * 0.5f + 1.0f;
		const auto max_x = window->WorkRect.Max.x + style.WindowPadding.x * 0.5f - 1.0f;

		const auto min = ImVec2(min_x, GetCursorScreenPos().y - style.ItemSpacing.y);
		const auto max = ImVec2(max_x, min.y + container_height);

		GetWindowDrawList()->AddRect(min + ImVec2(-1, -1), max + ImVec2(1, 1), ColorConvertFloat4ToU32(border_color), 10.0f, ImDrawFlags_RoundCornersBottom);
		GetWindowDrawList()->AddRectFilled(min, max, ColorConvertFloat4ToU32(background_color), 10.0f, ImDrawFlags_RoundCornersBottom);

		// dropshadow
		{
			const auto dshadow_pmin = GetCursorScreenPos() - ImVec2(style.WindowPadding.x * 0.5f, 4);
			const auto dshadow_pmax = dshadow_pmin + ImVec2(GetContentRegionAvail().x + style.WindowPadding.x, 48.0f);

			const auto col_bottom = ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.0f));
			const auto col_top = ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.4f));
			GetWindowDrawList()->AddRectFilledMultiColor(dshadow_pmin, dshadow_pmax, col_top, col_top, col_bottom, col_bottom);
		}

		Indent(4);
		BeginGroup();
		Spacing(0, 4);
		callback();
		Spacing(0, 4);
		EndGroup();
		Unindent(4);

		return GetItemRectSize().y + 6.0f;
	}
}
