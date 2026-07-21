#include "std_include.hpp"

namespace components
{
	namespace
	{
		bool remix_gameplay_active()
		{
			const auto* intf = interfaces::get();
			return intf && intf->m_engine && intf->m_engine->is_playing();
		}
	}
	// called on device->BeginScene
	void remix_api::begin_scene_callback()
	{
		if (!loader::is_runtime_ready()) return;
		if (!remix_gameplay_active()) return;

		const auto api = get();
		if (!api || !api->m_initialized) {
			return;
		}
		if (api->m_debug_line_amount)
		{
			for (auto l = 1u; l < api->m_debug_line_amount + 1; l++)
			{
				if (api->m_debug_line_list[l])
				{
					remixapi_Transform t0 = {};
					t0.matrix[0][0] = 1.0f;
					t0.matrix[1][1] = 1.0f;
					t0.matrix[2][2] = 1.0f;

					const remixapi_InstanceInfo inst =
					{
						.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO,
						.pNext = nullptr,
						.categoryFlags = 0,
						.mesh = api->m_debug_line_list[l],
						.transform = t0,
						.doubleSided = true
					};

					api->m_bridge.DrawInstance(&inst);
				}
			}
		}

		// --

		//const auto& im = imgui::get();
		if (!api->m_debug_circles.empty())
		{
			for (const auto circle : api->m_debug_circles)
			{
				if (circle.handle)
				{
					remixapi_Transform t0 = {};

					if (!circle.uses_custom_transform)
					{
						t0.matrix[0][0] = 1.0f;
						t0.matrix[1][1] = 1.0f;
						t0.matrix[2][2] = 1.0f;
					}

					const remixapi_InstanceInfo inst =
					{
						.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO,
						.pNext = nullptr,
						.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_LIGHTS,
						//.categoryFlags = 0,
						.mesh = circle.handle,
						.transform = circle.uses_custom_transform ? circle.transform : t0,
						.doubleSided = true,
					};

					api->m_bridge.DrawInstance(&inst);
				}
			}

			// remove all instances
			for (auto& circle : api->m_debug_circles)
			{
				if (circle.handle) {
					api->m_bridge.DestroyMesh(circle.handle);
				}
			}
			api->m_debug_circles.clear();
		}

		// --

		for (const auto& [n, fl] : api->m_flashlights)
		{
			auto submit_layer = [api](const remixapi_LightHandle handle, std::uint64_t& submitted)
			{
				if (!handle) return;
				if (api->m_bridge.DrawLightInstance(handle) == REMIXAPI_ERROR_CODE_SUCCESS) {
					++submitted;
				}
				else {
					// Compatibility lifecycle recreates every handle in flashlight_frame().
					// Never mutate or destroy the rig from BeginScene; the next render frame
					// replaces it through the original proven path.
					++api->m_flashlight_runtime_stats.draw_failures;
				}
			};
			submit_layer(fl.handle, api->m_flashlight_runtime_stats.submitted_main);
			submit_layer(fl.handle_inner, api->m_flashlight_runtime_stats.submitted_core);
			submit_layer(fl.handle_hotspot, api->m_flashlight_runtime_stats.submitted_hotspot);
			submit_layer(fl.handle_spill, api->m_flashlight_runtime_stats.submitted_spill);
		}
	}

	// called on device->EndScene
	void remix_api::end_scene_callback()
	{
		if (!loader::is_runtime_ready()) return;
		if (!remix_gameplay_active()) return;

		// Gameplay UI stays inside the original Xorxor/Remix EndScene callback.
		// The direct Present hook is reserved for the Source front-end only.
		imgui::endscene_stub(false);

#if 0
		if (!model_render::get()->m_drew_hud)
		{
			const auto dev = game::get_d3d_device();
			IDirect3DVertexShader9* og_vs = nullptr;
			dev->GetVertexShader(&og_vs);
			dev->SetVertexShader(nullptr);

			IDirect3DPixelShader9* og_ps = nullptr;
			dev->GetPixelShader(&og_ps);
			dev->SetPixelShader(nullptr);

			IDirect3DBaseTexture9* og_tex = nullptr;
			dev->GetTexture(0, &og_tex);
			dev->SetTexture(0, nullptr);

			DWORD og_zwrite;
			dev->GetRenderState(D3DRS_ZWRITEENABLE, &og_zwrite);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

			D3DMATRIX* og_view = nullptr;
			dev->GetTransform(D3DTS_VIEW, og_view);

			D3DMATRIX* og_proj = nullptr;
			dev->GetTransform(D3DTS_PROJECTION, og_proj);

			dev->SetTransform(D3DTS_WORLD, &game::IDENTITY);
			dev->SetTransform(D3DTS_VIEW, &game::IDENTITY);
			dev->SetTransform(D3DTS_PROJECTION, &game::IDENTITY);

			struct CUSTOMVERTEX
			{
				float x, y, z, rhw;
				D3DCOLOR color = D3DCOLOR_COLORVALUE(0.0f, 0.0f, 0.0f, 0.0f);
			};

			CUSTOMVERTEX vertices[] =
			{
				{ -0.5f,  -0.5f,  0.0f, 1.0f }, // tl
				{ -0.49f, -0.5f,  0.0f, 1.0f }, // tr
				{ -0.5f,  -0.49f, 0.0f, 1.0f }, // bl
				{ -0.49f, -0.49f, 0.0f, 1.0f }  // br
			};

			DWORD og_unused;
			dev->GetRenderState((D3DRENDERSTATETYPE)42, &og_unused);
			dev->SetRenderState((D3DRENDERSTATETYPE)42, REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_MATTE);

			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(CUSTOMVERTEX));

			dev->SetVertexShader(og_vs);
			dev->SetPixelShader(og_ps);
			dev->SetTexture(0, og_tex);
			dev->SetRenderState(D3DRS_ZWRITEENABLE, og_zwrite);
			dev->SetTransform(D3DTS_VIEW, og_view);
			dev->SetTransform(D3DTS_PROJECTION, og_proj);

			model_render::get()->m_drew_hud = true;
		}
#endif
	}

	// called on device->Present
	void remix_api::on_present_callback()
	{
		if (!loader::is_runtime_ready()) return;
		if (!remix_gameplay_active()) return;

		// Generic Source mode owns no L4D2 main_module/model_render instances.
		// Guard this callback so the Remix bridge can remain active without touching
		// version-specific state.
		if (!source_compat::uses_l4d2_runtime()) {
			return;
		}

		if (main_module::get()) {
			main_module::hud_draw_area_info();
		}

		// Draw marker fallback from Present only when DrawModelExecute did not submit
		// the marker pass. This avoids duplicate geometry while still covering maps
		// whose updated render path reaches Present without a normal model draw.
		if (remix_markers::get() && (!model_render::get() || !model_render::get()->m_drew_model)) {
			remix_markers::draw_nocull_markers(true);
		}
		if (model_render::get()) {
			model_render::on_present();
		}
	}

	// #
	// #

	void remix_api::init_debug_lines()
	{
		if (!m_debug_lines_initialized)
		{
			remixapi_MaterialInfoOpaqueEXT ext = {};
			{
				ext.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
				ext.useDrawCallAlphaState = 1;
				ext.opacityConstant = 1.0f;
				ext.roughnessConstant = 1.0f;
				ext.metallicConstant = 1.0f;
				ext.roughnessTexture = L"";
				ext.metallicTexture = L"";
				ext.heightTexture = L"";
			}

			remixapi_MaterialInfo info
			{
				.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO,
				.pNext = &ext,
				.hash = utils::string_hash64("linemat0"),
				.albedoTexture = L"",
				.normalTexture = L"",
				.tangentTexture = L"",
				.emissiveTexture = L"",
				.emissiveIntensity = 0.1f,
				.emissiveColorConstant = { 1.0f, 0.0f, 0.0f },
			};
			m_bridge.CreateMaterial(&info, &m_debug_line_materials[0]);

			info.hash = utils::string_hash64("linemat1");
			info.emissiveColorConstant = { 0.0f, 1.0f, 0.0f };
			m_bridge.CreateMaterial(&info, &m_debug_line_materials[1]);

			info.hash = utils::string_hash64("linemat3");
			info.emissiveColorConstant = { 0.0f, 1.0f, 1.0f };
			m_bridge.CreateMaterial(&info, &m_debug_line_materials[2]);

			info.hash = utils::string_hash64("linemat4");
			info.emissiveColorConstant = { 1.0f, 1.0f, 1.0f };
			m_bridge.CreateMaterial(&info, &m_debug_line_materials[3]);

			m_debug_lines_initialized = true;
		}
	}

	void remix_api::create_quad(remixapi_HardcodedVertex* v_out, uint32_t* i_out, const float scale)
	{
		if (!v_out || !i_out) {
			return;
		}

		auto make_vertex = [&](const float x, const float y, const float z, const float u, const float v)
			{
				const remixapi_HardcodedVertex vert =
				{
				  .position = { x, y, z },
				  .normal = { 0.0f, 0.0f, -1.0f },
				  .texcoord = { u, v },
				  .color = 0xFFFFFFFF,
				};
				return vert;
			};

		v_out[0] = make_vertex(-1.0f * scale, 1, -1.0f * scale, 0.0f, 0.0f); // bottom left
		v_out[1] = make_vertex(-1.0f * scale, 1, 1.0f * scale, 0.0f, 1.0f); // top left
		v_out[2] = make_vertex(1.0f * scale, 1, -1.0f * scale, 1.0f, 0.0f); // bottom right
		v_out[3] = make_vertex(1.0f * scale, 1, 1.0f * scale, 1.0f, 1.0f); // top right

		i_out[0] = 0; i_out[1] = 1; i_out[2] = 2;
		i_out[3] = 3; i_out[4] = 2; i_out[5] = 1;
	}

	void remix_api::create_line_quad(remixapi_HardcodedVertex* v_out, uint32_t* i_out, const Vector& p1, const Vector& p2, const float width)
	{
		if (!v_out || !i_out) {
			return;
		}

		auto make_vertex = [&](const Vector& pos, const float u, const float v)
			{
				const remixapi_HardcodedVertex vert =
				{
				  .position = { pos.x, pos.y, pos.z },
				  .normal = { 0.0f, 0.0f, -1.0f },
				  .texcoord = { u, v },
				  .color = 0xFFFFFFFF,
				};
				return vert;
			};

		Vector up = { 0.0f, 0.0f, 1.0f };
		Vector dir = p2 - p1;
		dir.Normalize();

		// check if dir is parallel or very close to the up vector
		if (fabs(DotProduct(dir, up)) > 0.999f) {
			up = { 1.0f, 0.0f, 0.0f }; // if parallel, choose a different up vector
		}

		// perpendicular vector to line
		Vector perp = dir.Cross(up); 
		perp.Normalize();

		// scale by half width to offset vertices
		const Vector offset = perp * (width * 0.5f);

		v_out[0] = make_vertex(p1 - offset, 0.0f, 0.0f); // bottom left
		v_out[1] = make_vertex(p1 + offset, 0.0f, 1.0f); // top left
		v_out[2] = make_vertex(p2 - offset, 1.0f, 0.0f); // bottom right
		v_out[3] = make_vertex(p2 + offset, 1.0f, 1.0f); // top right

		i_out[0] = 0; i_out[1] = 1; i_out[2] = 2;
		i_out[3] = 3; i_out[4] = 2; i_out[5] = 1;
	}

	void remix_api::add_debug_line(const Vector& p1, const Vector& p2, const float width, DEBUG_REMIX_LINE_COLOR color)
	{
		if (m_debug_line_materials[color])
		{
			if (can_add_debug_lines())
			{
				m_debug_line_amount++;
				remixapi_HardcodedVertex verts[4] = {};
				uint32_t indices[6] = {};
				create_line_quad(verts, indices, p1, p2, width);

				remixapi_MeshInfoSurfaceTriangles triangles =
				{
				  .vertices_values = verts,
				  .vertices_count = ARRAYSIZE(verts),
				  .indices_values = indices,
				  .indices_count = 6,
				  .skinning_hasvalue = FALSE,
				  .skinning_value = {},
				  .material = m_debug_line_materials[color],
				};

				remixapi_MeshInfo info
				{
					.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO,
					.hash = utils::string_hash64(utils::va("line%d", m_debug_last_line_hash ? m_debug_last_line_hash : 1)),
					.surfaces_values = &triangles,
					.surfaces_count = 1,
				};

				m_bridge.CreateMesh(&info, &m_debug_line_list[m_debug_line_amount]);
				m_debug_last_line_hash = reinterpret_cast<std::uint64_t>(m_debug_line_list[m_debug_line_amount]);
			}
		}
	}

	/// This re-uses the previously added circle and re-draws it using a different transform (instanced drawing)
	/// @param center	center point of circle
	/// @param rot		rotation in degrees
	/// @param scale	scale of circle
	void remix_api::add_debug_circle_based_on_previous(const Vector& center, const Vector& rot, const Vector& scale)
	{
		if (m_debug_circles.empty()) {
			return;
		}

		m_debug_circles.push_back({});
		auto& circle = m_debug_circles.back();
		circle.handle = m_debug_circles[m_debug_circles.size() - 2u].handle; // previous handle

		D3DXMATRIX scale_matrix, rotation_x, rotation_y, rotation_z, mat_rotation, mat_translation, world, transposed;

		D3DXMatrixScaling(&scale_matrix, scale.x, scale.y, scale.z);
		D3DXMatrixRotationX(&rotation_x, DEG2RAD(rot.x)); // pitch
		D3DXMatrixRotationY(&rotation_y, DEG2RAD(rot.y)); // yaw
		D3DXMatrixRotationZ(&rotation_z, DEG2RAD(rot.z)); // roll
		mat_rotation = rotation_z * rotation_y * rotation_x; // combine rotations (order: Z * Y * X)

		D3DXMatrixTranslation(&mat_translation, center.x, center.y, center.z);
		world = scale_matrix * mat_rotation * mat_translation;

		D3DXMatrixTranspose(&transposed, &world);

		circle.transform.matrix[0][0] = transposed.m[0][0];
		circle.transform.matrix[0][1] = transposed.m[0][1];
		circle.transform.matrix[0][2] = transposed.m[0][2];
		circle.transform.matrix[0][3] = transposed.m[0][3];

		circle.transform.matrix[1][0] = transposed.m[1][0];
		circle.transform.matrix[1][1] = transposed.m[1][1];
		circle.transform.matrix[1][2] = transposed.m[1][2];
		circle.transform.matrix[1][3] = transposed.m[1][3];

		circle.transform.matrix[2][0] = transposed.m[2][0];
		circle.transform.matrix[2][1] = transposed.m[2][1];
		circle.transform.matrix[2][2] = transposed.m[2][2];
		circle.transform.matrix[2][3] = transposed.m[2][3];

		circle.uses_custom_transform = true;
	}

	void remix_api::add_debug_circle(const Vector& center, const Vector& normal, const float radius, const float thickness, const Vector& color, bool drawcall_alpha)
	{
		// material
		{
			/*	// BlendType 
				kAlpha = 0,
				kAlphaEmissive = 1,
				kReverseAlphaEmissive = 2,
				kColor = 3,
				kColorEmissive = 4,
				kReverseColorEmissive = 5,
				kEmissive = 6,
				kMultiplicative = 7,
				kDoubleMultiplicative = 8,
				kReverseAlpha = 9,
				kReverseColor = 10,
			
				kMinValue = 0, // kAlpha
				kMaxValue = 10, // kReverseColor */

			remixapi_MaterialInfoOpaqueEXT ext = {};
			{
				//ext.pNext = &blend;
				ext.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
				ext.useDrawCallAlphaState = drawcall_alpha;
				ext.blendType_hasvalue = true;
				ext.blendType_value = 4;
				ext.opacityConstant = 1.0f;
				ext.albedoConstant = color.ToRemixFloat3D();
				ext.roughnessConstant = 1.0f;
				ext.metallicConstant = 1.0f;
				ext.roughnessTexture = L"";
				ext.metallicTexture = L"";
				ext.heightTexture = L"";
			}

			remixapi_MaterialInfo mat_info
			{
				.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO,
				.pNext = &ext,
				.hash = utils::string_hash64("circlemat" + std::to_string(m_debug_circle_materials.size())),
				.albedoTexture = L"",
				.normalTexture = L"",
				.tangentTexture = L"",
				.emissiveTexture = L"",
				.emissiveIntensity = 1.0f,
				.emissiveColorConstant = color.ToRemixFloat3D(),
			};

			m_debug_circle_materials.push_back(nullptr); 
			m_bridge.CreateMaterial(&mat_info, &m_debug_circle_materials.back());
		}

		// mesh
		const uint32_t segments = 48u;

		std::vector<remixapi_HardcodedVertex> vertices;
		std::vector<uint32_t> indices;

		Vector up = fabs(normal.y) < 0.99f ? Vector(0, 1, 0) : Vector(1, 0, 0);
		Vector tangent = up.Cross(normal); tangent.Normalize();
		Vector bitangent = normal.Cross(tangent); bitangent.Normalize();

		// flat circle
		//for (auto i = 0u; i < segments; ++i)
		//{
		//	float angle = (2.0f * M_PI * (float)i) / segments;
		//	float next_angle = (2.0f * M_PI * (float)(i + 1)) / segments;

		//	Vector offset_outer = tangent * cosf(angle) * radius + bitangent * sinf(angle) * radius;
		//	Vector offset_inner = tangent * cosf(angle) * (radius - thickness) + bitangent * sinf(angle) * (radius - thickness);

		//	Vector next_offset_outer = tangent * cosf(next_angle) * radius + bitangent * sinf(next_angle) * radius;
		//	Vector next_offset_inner = tangent * cosf(next_angle) * (radius - thickness) + bitangent * sinf(next_angle) * (radius - thickness);

		//	Vector outer1 = /*center +*/ offset_outer;
		//	Vector inner1 = /*center +*/ offset_inner;
		//	Vector outer2 = /*center +*/ next_offset_outer;
		//	Vector inner2 = /*center +*/ next_offset_inner;

		//	uint32_t idx = vertices.size();
		//	vertices.push_back({ { outer1.x, outer1.y, outer1.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF });
		//	vertices.push_back({ { inner1.x, inner1.y, inner1.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF });
		//	vertices.push_back({ { outer2.x, outer2.y, outer2.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF });
		//	vertices.push_back({ { inner2.x, inner2.y, inner2.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF });
		//	indices.insert(indices.end(), { idx, idx + 1, idx + 2, idx + 1, idx + 3, idx + 2 });
		//}

		// torus
		Vector normal_offset = normal; normal_offset.Normalize();
		normal_offset *= 0.05f;  // torus thickness

		for (size_t i = 0u; i < segments; ++i)
		{
			float angle = (2.0f * M_PI * i) / segments;
			float next_angle = (2.0f * M_PI * (i + 1)) / segments;

			// uuter and inner ring calculations
			Vector offset_outer = tangent * cosf(angle) * radius + bitangent * sinf(angle) * radius;
			Vector offset_inner = tangent * cosf(angle) * (radius - thickness) + bitangent * sinf(angle) * (radius - thickness);
			Vector next_offset_outer = tangent * cosf(next_angle) * radius + bitangent * sinf(next_angle) * radius;
			Vector next_offset_inner = tangent * cosf(next_angle) * (radius - thickness) + bitangent * sinf(next_angle) * (radius - thickness);

			// top and bottom vertices for outer and inner ring
			Vector outer1_top = offset_outer + normal_offset;
			Vector outer1_bottom = offset_outer - normal_offset;
			Vector inner1_top = offset_inner + normal_offset;
			Vector inner1_bottom = offset_inner - normal_offset;

			Vector outer2_top = next_offset_outer + normal_offset;
			Vector outer2_bottom = next_offset_outer - normal_offset;
			Vector inner2_top = next_offset_inner + normal_offset;
			Vector inner2_bottom =  next_offset_inner - normal_offset;

			uint32_t idx = vertices.size();
			vertices.push_back({ { outer1_top.x, outer1_top.y, outer1_top.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF });  // 0
			vertices.push_back({ { outer1_bottom.x, outer1_bottom.y, outer1_bottom.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF }); // 1
			vertices.push_back({ { inner1_top.x, inner1_top.y, inner1_top.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF }); // 2
			vertices.push_back({ { inner1_bottom.x, inner1_bottom.y, inner1_bottom.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF }); // 3

			vertices.push_back({ { outer2_top.x, outer2_top.y, outer2_top.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF }); // 4
			vertices.push_back({ { outer2_bottom.x, outer2_bottom.y, outer2_bottom.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF }); // 5
			vertices.push_back({ { inner2_top.x, inner2_top.y, inner2_top.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF }); // 6
			vertices.push_back({ { inner2_bottom.x, inner2_bottom.y, inner2_bottom.z }, { normal.x, normal.y, normal.z }, {}, 0xFFFFFFFF }); // 7

			// side walls (connect top/bottom)
			indices.insert(indices.end(), { idx, idx + 1, idx + 4, idx + 1, idx + 5, idx + 4 }); // Outer wall
			indices.insert(indices.end(), { idx + 2, idx + 6, idx + 3, idx + 3, idx + 6, idx + 7 }); // Inner wall

			// top and bottom caps
			indices.insert(indices.end(), { idx, idx + 2, idx + 4, idx + 2, idx + 6, idx + 4 }); // Top cap
			indices.insert(indices.end(), { idx + 1, idx + 5, idx + 3, idx + 3, idx + 5, idx + 7 }); // Bottom cap
		}

		remixapi_MeshInfoSurfaceTriangles triangles =
		{
		  .vertices_values = vertices.data(),
		  .vertices_count = vertices.size(),
		  .indices_values = indices.data(),
		  .indices_count = indices.size(),
		  .skinning_hasvalue = FALSE,
		  .skinning_value = {},
		  .material = m_debug_circle_materials.back(),
		};

		remixapi_MeshInfo info
		{
			.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO,
			.hash = utils::string_hash64(utils::va("circle%d", m_debug_circles_last_hash ? m_debug_circles_last_hash : 1)),
			.surfaces_values = &triangles,
			.surfaces_count = 1,
		};

		m_debug_circles.push_back({}); 

		auto &circle = m_debug_circles.back();

		circle.transform.matrix[0][0] = 1.0f;
		circle.transform.matrix[1][1] = 1.0f;
		circle.transform.matrix[2][2] = 1.0f;

		circle.transform.matrix[0][3] = center.x;
		circle.transform.matrix[1][3] = center.y;
		circle.transform.matrix[2][3] = center.z;
		circle.uses_custom_transform = true;

		m_bridge.CreateMesh(&info, &m_debug_circles.back().handle);
		m_debug_circles_last_hash = reinterpret_cast<std::uint64_t>(m_debug_circles.back().handle);
	}

	/**
	 * Draw a wireframe box using the remix api
	 * @param mins			Bound mins
	 * @param maxs			Bound maxs
	 * @param line_width	Line width
	 * @param color			Line color
	 */
	void remix_api::debug_draw_box(const Vector& mins, const Vector& maxs, const float line_width, const DEBUG_REMIX_LINE_COLOR& color)
	{
		Vector corners[8];

		// get the corners of the cube
		corners[0] = Vector(mins.x, mins.y, mins.z);
		corners[1] = Vector(mins.x, mins.y, maxs.z);
		corners[2] = Vector(mins.x, maxs.y, mins.z);
		corners[3] = Vector(mins.x, maxs.y, maxs.z);
		corners[4] = Vector(maxs.x, mins.y, mins.z);
		corners[5] = Vector(maxs.x, mins.y, maxs.z);
		corners[6] = Vector(maxs.x, maxs.y, mins.z);
		corners[7] = Vector(maxs.x, maxs.y, maxs.z);

		// define the edges
		Vector lines[12][2];
		lines[0][0] = corners[0];	lines[0][1] = corners[1]; // Edge 1
		lines[1][0] = corners[0];	lines[1][1] = corners[2]; // Edge 2
		lines[2][0] = corners[0];	lines[2][1] = corners[4]; // Edge 3
		lines[3][0] = corners[1];	lines[3][1] = corners[3]; // Edge 4
		lines[4][0] = corners[1];	lines[4][1] = corners[5]; // Edge 5
		lines[5][0] = corners[2];	lines[5][1] = corners[3]; // Edge 6
		lines[6][0] = corners[2];	lines[6][1] = corners[6]; // Edge 7
		lines[7][0] = corners[3];	lines[7][1] = corners[7]; // Edge 8
		lines[8][0] = corners[4];	lines[8][1] = corners[5]; // Edge 9
		lines[9][0] = corners[4];	lines[9][1] = corners[6]; // Edge 10
		lines[10][0] = corners[5];	lines[10][1] = corners[7]; // Edge 11
		lines[11][0] = corners[6];	lines[11][1] = corners[7]; // Edge 12

		for (auto e = 0u; e < 12; e++) {
			add_debug_line(lines[e][0], lines[e][1], line_width, color);
		}
	}

	/**
	 * Draw a wireframe box using the remix api
	 * @param center		Center of the cube
	 * @param half_diagonal Half diagonal distance of the box
	 * @param line_width	Line width
	 * @param color			Line color
	 */
	void remix_api::debug_draw_box(const VectorAligned& center, const VectorAligned& half_diagonal, const float line_width, const DEBUG_REMIX_LINE_COLOR& color)
	{
		debug_draw_box(center - half_diagonal, center + half_diagonal, line_width, color);
	}

	void remix_api::flashlight_create_or_update(const char* player_name, const Vector& pos, const Vector& fwd, const Vector& rt, const Vector& up, bool is_enabled, bool is_player)
	{
		// The entity layout and flashlight offsets below are native-L4D2 data. Never
		// manufacture ASI flashlight handles in HL2/Portal, even when the double-unsafe
		// runtime override is being tested.
		if (!source_compat::is_native_l4d2()) {
			return;
		}

		if (!remix_api::is_initialized() || !player_name || !*player_name) {
			return;
		}

		if (const auto it = m_flashlights.find(player_name);
			it == m_flashlights.end())
		{
			// insert new flashlight data
			m_flashlights[player_name] =
			{
				.def = {.pos = pos, .fwd = fwd, .rt = rt, .up = up },
				.last_seen_frame = main_module::framecount,
				.is_player = is_player,
				.is_enabled = is_enabled,
				.is_alive = true
			};
		}
		else
		{
			// update existing flashlight data
			it->second.def.pos = pos;
			it->second.def.fwd = fwd;
			it->second.def.rt = rt;
			it->second.def.up = up;
			it->second.is_player = is_player;
			it->second.is_enabled = is_enabled;
			it->second.last_seen_frame = main_module::framecount;
			it->second.missed_owner_frames = 0u;
			it->second.is_alive = true;
		}
	}

	remix_api::flashlight_runtime_stats_s remix_api::flashlight_runtime_stats()
	{
		const auto* api = remix_api::get();
		return api ? api->m_flashlight_runtime_stats : flashlight_runtime_stats_s{};
	}

	void remix_api::reset_flashlight_runtime_stats()
	{
		if (auto* api = remix_api::get()) {
			api->m_flashlight_runtime_stats = {};
		}
	}

	void remix_api::clear_flashlights()
	{
		auto* api = remix_api::get();
		if (!api) return;
		auto& stats = api->m_flashlight_runtime_stats;
		for (auto& [name, fl] : api->m_flashlights)
		{
			for (auto* handle : { &fl.handle, &fl.handle_inner, &fl.handle_hotspot, &fl.handle_spill })
			{
				if (!*handle) continue;
				if (api->m_initialized) api->m_bridge.DestroyLight(*handle);
				*handle = nullptr;
				++stats.destroyed_handles;
			}
		}
		api->m_flashlights.clear();
		stats.tracked_owners = 0u;
		stats.active_handles = 0u;
		stats.requested_layers = 0u;
		stats.granted_layers = 0u;
	}

	void remix_api::flashlight_frame()
	{
		if (!source_compat::is_native_l4d2()) {
			return;
		}

		auto* api = remix_api::get();
		if (!api || !remix_api::is_initialized()) {
			return;
		}

		auto* gs = game_settings::get();
		if (!gs) {
			return;
		}

		auto& stats = api->m_flashlight_runtime_stats;
		stats.requested_layers = 0u;
		stats.granted_layers = 0u;
		stats.active_handles = 0u;
		stats.tracked_owners = static_cast<std::uint32_t>(api->m_flashlights.size());

		constexpr std::uint8_t k_layer_main = 1u << 0u;
		constexpr std::uint8_t k_layer_core = 1u << 1u;
		constexpr std::uint8_t k_layer_hotspot = 1u << 2u;
		constexpr std::uint8_t k_layer_spill = 1u << 3u;

		auto destroy_handle = [api, &stats](remixapi_LightHandle& handle)
		{
			if (!handle) return;
			api->m_bridge.DestroyLight(handle);
			handle = nullptr;
			++stats.destroyed_handles;
		};

		auto layer_position = [](const flashlight_s& fl, const float* offset)
		{
			Vector position = fl.def.pos;
			if (offset) {
				position += (fl.def.fwd * offset[0]) + (fl.def.rt * offset[2]) + (fl.def.up * offset[1]);
			}
			return position;
		};

		auto layer_direction = [](const flashlight_s& fl, const float* offset)
		{
			Vector direction = fl.def.fwd;
			if (offset)
			{
				const float pitch = DEG2RAD(std::clamp(offset[0], -45.0f, 45.0f));
				const float yaw = DEG2RAD(std::clamp(offset[1], -45.0f, 45.0f));
				direction = fl.def.fwd + fl.def.up * std::tan(pitch) + fl.def.rt * std::tan(yaw);
			}
			direction.NormalizeChecked();
			return direction;
		};

		auto create_layer = [api, &stats](
			remixapi_LightHandle& handle,
			remixapi_LightInfoSphereEXT& ext,
			remixapi_LightInfo& info,
			const char* hash_prefix,
			const std::string& owner_name,
			const Vector& position,
			const Vector& color,
			const Vector& direction,
			const float intensity,
			const float radius,
			const bool shaped,
			const float angle,
			const float softness,
			const float exponent)
		{
			ext = {};
			ext.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			ext.pNext = nullptr;
			ext.position = position.ToRemixFloat3D();
			ext.radius = std::max(0.001f, radius);
			ext.shaping_hasvalue = shaped ? TRUE : FALSE;
			ext.shaping_value = {};
			if (shaped)
			{
				ext.shaping_value.direction = direction.ToRemixFloat3D();
				ext.shaping_value.coneAngleDegrees = std::clamp(angle, 0.1f, 179.0f);
				ext.shaping_value.coneSoftness = std::clamp(softness, 0.0f, 1.0f);
				ext.shaping_value.focusExponent = std::max(0.0f, exponent);
			}

			info = {};
			info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			info.pNext = &ext;
			info.hash = utils::string_hash64(utils::va("%s%s", hash_prefix, owner_name.c_str()));
			const float energy = 20.0f * std::max(0.0f, intensity);
			info.radiance = remixapi_Float3D{
				energy * std::max(0.0f, color.x),
				energy * std::max(0.0f, color.y),
				energy * std::max(0.0f, color.z)
			};

			++stats.create_attempts;
			if (api->m_bridge.CreateLight(&info, &handle) == REMIXAPI_ERROR_CODE_SUCCESS) {
				return true;
			}
			handle = nullptr;
			++stats.create_failures;
			return false;
		};

		for (auto& [name, fl] : api->m_flashlights)
		{
			// Compatibility lifecycle restored from the original working implementation:
			// Remix light descriptors are rebuilt every rendered Source frame so position,
			// orientation and shaping always follow the flashlight owner exactly.
			fl.is_alive = false;
			destroy_handle(fl.handle);
			destroy_handle(fl.handle_inner);
			destroy_handle(fl.handle_hotspot);
			destroy_handle(fl.handle_spill);
			fl.active_layer_mask = 0u;
			fl.desired_layer_mask = 0u;
			fl.has_built_state = false;
			fl.stale_rig_fallback = false;
			fl.consecutive_build_failures = 0u;
			fl.retry_after = {};

			if (!fl.is_enabled || !gs->flashlight_enabled.get_as<bool>()) {
				continue;
			}

			std::uint8_t requested_mask = 0u;
			if (gs->flashlight_main_enabled.get_as<bool>()) requested_mask |= k_layer_main;
			if (gs->flashlight_inner_enabled.get_as<bool>()) requested_mask |= k_layer_core;
			if (gs->flashlight_hotspot_enabled.get_as<bool>()) requested_mask |= k_layer_hotspot;
			if (gs->flashlight_spill_enabled.get_as<bool>()) requested_mask |= k_layer_spill;
			fl.desired_layer_mask = requested_mask;
			stats.requested_layers += static_cast<std::uint32_t>(
				((requested_mask & k_layer_main) ? 1 : 0) +
				((requested_mask & k_layer_core) ? 1 : 0) +
				((requested_mask & k_layer_hotspot) ? 1 : 0) +
				((requested_mask & k_layer_spill) ? 1 : 0));

			if (requested_mask & k_layer_main)
			{
				const float* offset = fl.is_player ? gs->flashlight_offset_player.get_as<float*>() : gs->flashlight_offset_bot.get_as<float*>();
				const float* color = gs->flashlight_main_color.get_as<float*>();
				if (create_layer(fl.handle, fl.ext, fl.info, "fl", name,
					layer_position(fl, offset), Vector(color[0], color[1], color[2]),
					layer_direction(fl, gs->flashlight_main_direction_offset.get_as<float*>()),
					gs->flashlight_intensity.get_as<float>(), gs->flashlight_radius.get_as<float>(), true,
					gs->flashlight_angle.get_as<float>(), gs->flashlight_softness.get_as<float>(), gs->flashlight_expo.get_as<float>()))
				{
					fl.active_layer_mask |= k_layer_main;
					++stats.granted_layers;
				}
			}

			if (requested_mask & k_layer_core)
			{
				const float* offset = fl.is_player ? gs->flashlight_inner_offset_player.get_as<float*>() : gs->flashlight_inner_offset_bot.get_as<float*>();
				const float* color = gs->flashlight_inner_color.get_as<float*>();
				if (create_layer(fl.handle_inner, fl.ext_inner, fl.info_inner, "flcore", name,
					layer_position(fl, offset), Vector(color[0], color[1], color[2]), fl.def.fwd,
					gs->flashlight_inner_intensity.get_as<float>(), gs->flashlight_inner_radius.get_as<float>(), false,
					180.0f, 0.0f, 0.0f))
				{
					fl.active_layer_mask |= k_layer_core;
					++stats.granted_layers;
				}
			}

			if (requested_mask & k_layer_hotspot)
			{
				const float* offset = fl.is_player ? gs->flashlight_hotspot_offset_player.get_as<float*>() : gs->flashlight_hotspot_offset_bot.get_as<float*>();
				const float* color = gs->flashlight_hotspot_color.get_as<float*>();
				if (create_layer(fl.handle_hotspot, fl.ext_hotspot, fl.info_hotspot, "flhotspot", name,
					layer_position(fl, offset), Vector(color[0], color[1], color[2]),
					layer_direction(fl, gs->flashlight_hotspot_direction_offset.get_as<float*>()),
					gs->flashlight_hotspot_intensity.get_as<float>(), gs->flashlight_hotspot_radius.get_as<float>(), true,
					gs->flashlight_hotspot_angle.get_as<float>(), gs->flashlight_hotspot_softness.get_as<float>(), gs->flashlight_hotspot_expo.get_as<float>()))
				{
					fl.active_layer_mask |= k_layer_hotspot;
					++stats.granted_layers;
				}
			}

			if (requested_mask & k_layer_spill)
			{
				const float* offset = fl.is_player ? gs->flashlight_spill_offset_player.get_as<float*>() : gs->flashlight_spill_offset_bot.get_as<float*>();
				const float* color = gs->flashlight_spill_color.get_as<float*>();
				if (create_layer(fl.handle_spill, fl.ext_spill, fl.info_spill, "flspill", name,
					layer_position(fl, offset), Vector(color[0], color[1], color[2]),
					layer_direction(fl, gs->flashlight_spill_direction_offset.get_as<float*>()),
					gs->flashlight_spill_intensity.get_as<float>(), gs->flashlight_spill_radius.get_as<float>(), true,
					gs->flashlight_spill_angle.get_as<float>(), gs->flashlight_spill_softness.get_as<float>(), gs->flashlight_spill_expo.get_as<float>()))
				{
					fl.active_layer_mask |= k_layer_spill;
					++stats.granted_layers;
				}
			}

			stats.active_handles += fl.handle ? 1u : 0u;
			stats.active_handles += fl.handle_inner ? 1u : 0u;
			stats.active_handles += fl.handle_hotspot ? 1u : 0u;
			stats.active_handles += fl.handle_spill ? 1u : 0u;
			++stats.frame_updates;
		}
	}


	// called from main_module::on_renderview()
	void remix_api::on_renderview()
	{
		if (!try_initialize(false)) {
			return;
		}

		if (is_initialized()) 
		{
			// CViewRender::RenderView may be entered more than once for one Source
			// frame (main view, skybox or auxiliary passes). Entity scanning,
			// flashlight bridge transactions and debug-mesh retirement are frame
			// services, not per-view services, so execute them only once.
			if (const auto* intf = interfaces::get(); intf && intf->m_globals)
			{
				const int source_frame = intf->m_globals->framecount;
				const float source_realtime = intf->m_globals->realtime;
				if (m_last_source_render_frame == source_frame && m_last_source_realtime == source_realtime)
				{
					++m_duplicate_render_views_skipped;
					return;
				}
				m_last_source_render_frame = source_frame;
				m_last_source_realtime = source_realtime;
			}

			main_module::iterate_entities();
			remix_api::flashlight_frame();

			init_debug_lines();

			// destroy all lines added the prev. frame
			if (m_debug_line_amount)
			{
				for (auto& line : m_debug_line_list)
				{
					if (line)
					{
						m_bridge.DestroyMesh(line);
						line = nullptr;
					}
				}
				m_debug_line_amount = 0;
			}

			// destroy all circles materials added the prev. frame
			if (!m_debug_circle_materials.empty())
			{
				for (auto& m : m_debug_circle_materials)
				{
					if (m) {
						m_bridge.DestroyMaterial(m);
					}
				}
				m_debug_circle_materials.clear();
			}

			// destroy all circles added the prev. frame
			/*if (!m_debug_circles.empty())
			{
				for (auto& circle : m_debug_circles) 
				{
					if (circle.handle) {
						m_bridge.DestroyMesh(circle.handle);
					}
				}
				m_debug_circles.clear();
			}*/
		}
	}

	// #
	// #

	bool remix_api::try_initialize(const bool allow_console_log)
	{
		if (m_initialized) {
			return true;
		}

		m_bridge = {};
		const auto status = remixapi::bridge_initRemixApi(&m_bridge);
		m_last_init_status = status;

		if (status == REMIXAPI_ERROR_CODE_SUCCESS)
		{
			m_initialized = true;
			m_reported_init_failure = false;

			const auto cb_status = remixapi::bridge_setRemixApiCallbacks(begin_scene_callback, end_scene_callback, on_present_callback);
			if (cb_status != REMIXAPI_ERROR_CODE_SUCCESS)
			{
				game::console();
				printf("[!][RemixApi] Remix API initialized, but callback registration failed - Code: %d\n", cb_status);
			}

			printf("[+][RemixApi] Initialized successfully.\n");
			return true;
		}

		m_initialized = false;
		m_bridge = {};

		if (allow_console_log && !m_reported_init_failure)
		{
			m_reported_init_failure = true;
			game::console();
			printf("[!][RemixApi] Failed to initialize the remixApi - Code: %d (11 = NOT_INITIALIZED, will retry during RenderView)\n", status);
		}

		return false;
	}

	remix_api::remix_api()
	{
		p_this = this;
		m_bridge = {};
		try_initialize(true);
	}
}