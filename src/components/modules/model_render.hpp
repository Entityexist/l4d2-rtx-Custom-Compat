#pragma once

namespace components
{
	namespace cmd
	{
		extern bool model_info_vis;
		extern bool unbake_model_info_vis;
		extern std::uint32_t ms_unbake_info;
	}

	extern std::vector<Vector> g_sunoverlay_color;

	namespace tbl_hk::model_renderer
	{
		inline utils::vtable table;
		inline struct IVModelRender* _interface = nullptr;

		namespace DrawModelExecute
		{
			constexpr uint32_t index = 19u;
			using FN = void(__fastcall*)(void*, void*, const DrawModelState_t&, const ModelRenderInfo_t&, matrix3x4_t*);
			void __fastcall Detour(void* ecx, void* edx, const DrawModelState_t& state, const ModelRenderInfo_t& pInfo, matrix3x4_t* pCustomBoneToWorld);
		}
	}

	class prim_fvf_context
	{
	public:
		// retrieve information about the current pass - returns true if successful
		bool get_info_for_pass(IShaderAPIDX8* shaderapi)
		{
			if (shaderapi)
			{
				shaderapi->vtbl->GetBufferedState(shaderapi, nullptr, &info.buffer_state);

				if (info.material = shaderapi->vtbl->GetBoundMaterial(shaderapi, nullptr); 
					info.material)
				{
					info.material_name = info.material->vftable->GetName(info.material);
					info.shader_name = info.material->vftable->GetShaderName(info.material);

					return true;
				}
			}

			return false;
		}

		// Set texture 0 transform while preserving the exact game state once.
		void set_texture_transform(IDirect3DDevice9* device, const D3DXMATRIX* matrix)
		{
			if (!matrix) {
				return;
			}

			if (!tex0_transform_set)
			{
				if (FAILED(device->GetTransform(D3DTS_TEXTURE0, &tex0_transform_))) {
					return;
				}
				tex0_transform_set = true;
			}

			device->SetTransform(D3DTS_TEXTURE0, matrix);
		}

		// Save the original programmable shaders only once per primitive context.
		// World FFP conversion must restore both stages because leaving a Source
		// pixel shader active makes DXVK treat the pass as programmable material input.
		void save_vs(IDirect3DDevice9* device)
		{
			if (vs_set) {
				return;
			}

			if (SUCCEEDED(device->GetVertexShader(&vs_))) {
				vs_set = true;
			}
		}

		void save_ps(IDirect3DDevice9* device)
		{
			if (ps_set) {
				return;
			}

			if (SUCCEEDED(device->GetPixelShader(&ps_))) {
				ps_set = true;
			}
		}

		// Save a texture binding used by the compatibility renderer. GetTexture
		// returns an owned COM reference; restore_texture releases that reference
		// after rebinding it to the device.
		void save_texture(IDirect3DDevice9* device, const std::uint32_t stage)
		{
			IDirect3DBaseTexture9** texture = nullptr;
			bool* saved = nullptr;

			switch (stage)
			{
			case 0u: texture = &tex0_; saved = &tex0_set; break;
			case 1u: texture = &tex1_; saved = &tex1_set; break;
			case 2u: texture = &tex2_; saved = &tex2_set; break;
			case 3u: texture = &tex3_; saved = &tex3_set; break;
			default:
#if DEBUG
				OutputDebugStringA("save_texture:: unsupported texture stage\n");
#endif
				return;
			}

			if (*saved)
			{
#if DEBUG
				OutputDebugStringA("save_texture:: texture stage was already saved\n");
#endif
				return;
			}

			if (SUCCEEDED(device->GetTexture(stage, texture))) {
				*saved = true;
			}
		}

		// save render state (e.g. D3DRS_TEXTUREFACTOR)
		void save_rs(IDirect3DDevice9* device, const D3DRENDERSTATETYPE& state)
		{
			if (saved_render_state_.contains(state)) {
				return;
			}

			DWORD temp;
			device->GetRenderState(state, &temp);
			saved_render_state_[state] = temp;
		}

		static std::uint64_t make_stage_state_key(const std::uint32_t stage, const std::uint32_t state)
		{
			return (static_cast<std::uint64_t>(stage) << 32u) | static_cast<std::uint64_t>(state);
		}

		// Save sampler state for any sampler. The original helper only tracked
		// sampler 0, which was insufficient once world FFP explicitly disabled
		// stale stages used by the Source pixel shader.
		void save_ss(IDirect3DDevice9* device, const std::uint32_t sampler, const D3DSAMPLERSTATETYPE& state)
		{
			const auto key = make_stage_state_key(sampler, static_cast<std::uint32_t>(state));
			if (saved_sampler_state_.contains(key)) {
				return;
			}

			DWORD temp = 0u;
			if (SUCCEEDED(device->GetSamplerState(sampler, state, &temp))) {
				saved_sampler_state_[key] = temp;
			}
		}

		void save_ss(IDirect3DDevice9* device, const D3DSAMPLERSTATETYPE& state)
		{
			save_ss(device, 0u, state);
		}

		// Save texture-stage state for any stage while retaining the old stage-0 overload.
		void save_tss(IDirect3DDevice9* device, const std::uint32_t stage, const D3DTEXTURESTAGESTATETYPE& type)
		{
			const auto key = make_stage_state_key(stage, static_cast<std::uint32_t>(type));
			if (saved_texture_stage_state_.contains(key)) {
				return;
			}

			DWORD temp = 0u;
			if (SUCCEEDED(device->GetTextureStageState(stage, type, &temp))) {
				saved_texture_stage_state_[key] = temp;
			}
		}

		void save_tss(IDirect3DDevice9* device, const D3DTEXTURESTAGESTATETYPE& type)
		{
			save_tss(device, 0u, type);
		}

		// save D3DTS_WORLD
		void save_world_transform(IDirect3DDevice9* device, D3DXMATRIX* other = nullptr)
		{
			if (other) {
				memcpy_s(&world_transform_, sizeof(D3DXMATRIX), other, sizeof(D3DMATRIX));
			} else {
				device->GetTransform(D3DTS_WORLD, &world_transform_);
			}
			world_transform_set_ = true;
		}

		// save D3DTS_VIEW
		void save_view_transform(IDirect3DDevice9* device)
		{
			device->GetTransform(D3DTS_VIEW, &view_transform_);
			view_transform_set_ = true;
		}

		// save D3DTS_PROJECTION
		void save_projection_transform(IDirect3DDevice9* device)
		{
			device->GetTransform(D3DTS_PROJECTION, &projection_transform_);
			projection_transform_set_ = true;
		}

		//// save steamsource data
		//void save_streamsource_data(IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride)
		//{
		//	streamsource_ = buffer;
		//	streamsource_offset_ = offset;
		//	streamsource_stride_ = stride;
		//}

		// Restore programmable shaders and release the COM references returned by Get*Shader.
		void restore_vs(IDirect3DDevice9* device)
		{
			if (vs_set)
			{
				device->SetVertexShader(vs_);
				if (vs_) {
					vs_->Release();
					vs_ = nullptr;
				}
				vs_set = false;
			}
		}

		void restore_ps(IDirect3DDevice9* device)
		{
			if (ps_set)
			{
				device->SetPixelShader(ps_);
				if (ps_) {
					ps_->Release();
					ps_ = nullptr;
				}
				ps_set = false;
			}
		}

		// Restore a saved texture binding and release the reference returned by
		// GetTexture. Stages 2 and 3 are required by the L4D2 infected detail/wound payload.
		void restore_texture(IDirect3DDevice9* device, const std::uint32_t stage)
		{
			IDirect3DBaseTexture9** texture = nullptr;
			bool* saved = nullptr;

			switch (stage)
			{
			case 0u: texture = &tex0_; saved = &tex0_set; break;
			case 1u: texture = &tex1_; saved = &tex1_set; break;
			case 2u: texture = &tex2_; saved = &tex2_set; break;
			case 3u: texture = &tex3_; saved = &tex3_set; break;
			default: return;
			}

			if (*saved)
			{
				device->SetTexture(stage, *texture);
				if (*texture) {
					(*texture)->Release();
					*texture = nullptr;
				}
				*saved = false;
			}
		}

		// restore a specific render state (e.g. D3DRS_TEXTUREFACTOR)
		void restore_render_state(IDirect3DDevice9* device, const D3DRENDERSTATETYPE& state)
		{
			if (saved_render_state_.contains(state)) {
				device->SetRenderState(state, saved_render_state_[state]);
			}
		}

		void restore_sampler_state(IDirect3DDevice9* device, const std::uint32_t sampler, const D3DSAMPLERSTATETYPE& state)
		{
			const auto key = make_stage_state_key(sampler, static_cast<std::uint32_t>(state));
			if (saved_sampler_state_.contains(key)) {
				device->SetSamplerState(sampler, state, saved_sampler_state_[key]);
			}
		}

		void restore_sampler_state(IDirect3DDevice9* device, const D3DSAMPLERSTATETYPE& state)
		{
			restore_sampler_state(device, 0u, state);
		}

		void restore_texture_stage_state(IDirect3DDevice9* device, const std::uint32_t stage, const D3DTEXTURESTAGESTATETYPE& type)
		{
			const auto key = make_stage_state_key(stage, static_cast<std::uint32_t>(type));
			if (saved_texture_stage_state_.contains(key)) {
				device->SetTextureStageState(stage, type, saved_texture_stage_state_[key]);
			}
		}

		void restore_texture_stage_state(IDirect3DDevice9* device, const D3DTEXTURESTAGESTATETYPE& type)
		{
			restore_texture_stage_state(device, 0u, type);
		}

		// Restore the exact texture 0 transform captured before the first override.
		void restore_texture_transform(IDirect3DDevice9* device)
		{
			if (tex0_transform_set)
			{
				device->SetTransform(D3DTS_TEXTURE0, &tex0_transform_);
				tex0_transform_set = false;
			}
		}

		// restore saved D3DTS_WORLD
		void restore_world_transform(IDirect3DDevice9* device)
		{
			if (world_transform_set_)
			{
				device->SetTransform(D3DTS_WORLD, &world_transform_);
				world_transform_set_ = false;
			}
		}

		// restore saved D3DTS_VIEW
		void restore_view_transform(IDirect3DDevice9* device)
		{
			if (view_transform_set_)
			{
				device->SetTransform(D3DTS_VIEW, &view_transform_);
				view_transform_set_ = false;
			}
		}

		// restore saved D3DTS_PROJECTION
		void restore_projection_transform(IDirect3DDevice9* device)
		{
			if (projection_transform_set_)
			{
				device->SetTransform(D3DTS_PROJECTION, &projection_transform_);
				projection_transform_set_ = false;
			}
		}

		// restore all changes
		void restore_all(IDirect3DDevice9* device)
		{
			restore_ps(device);
			restore_vs(device);
			restore_texture(device, 0);
			restore_texture(device, 1);
			restore_texture(device, 2);
			restore_texture(device, 3);
			restore_texture_transform(device);
			restore_world_transform(device);
			restore_view_transform(device);
			restore_projection_transform(device);

			for (auto& rs : saved_render_state_) {
				device->SetRenderState(rs.first, rs.second);
			}

			for (auto& ss : saved_sampler_state_) {
				const auto sampler = static_cast<std::uint32_t>(ss.first >> 32u);
				const auto state = static_cast<D3DSAMPLERSTATETYPE>(static_cast<std::uint32_t>(ss.first));
				device->SetSamplerState(sampler, state, ss.second);
			}

			for (auto& tss : saved_texture_stage_state_) {
				const auto stage = static_cast<std::uint32_t>(tss.first >> 32u);
				const auto type = static_cast<D3DTEXTURESTAGESTATETYPE>(static_cast<std::uint32_t>(tss.first));
				device->SetTextureStageState(stage, type, tss.second);
			}
		}

		// reset the stored context data
		void reset_context()
		{
			// Normally restore_all() releases these references first. Keep reset
			// leak-safe for early-out/error paths as well.
			if (vs_) { vs_->Release(); vs_ = nullptr; }
			if (ps_) { ps_->Release(); ps_ = nullptr; }
			if (tex0_) { tex0_->Release(); tex0_ = nullptr; }
			if (tex1_) { tex1_->Release(); tex1_ = nullptr; }
			if (tex2_) { tex2_->Release(); tex2_ = nullptr; }
			if (tex3_) { tex3_->Release(); tex3_ = nullptr; }
			vs_set = false;
			ps_set = false;
			tex0_set = false;
			tex1_set = false;
			tex2_set = false;
			tex3_set = false;
			tex0_transform_set = false;
			world_transform_set_ = false;
			view_transform_set_ = false;
			projection_transform_set_ = false;
			saved_render_state_.clear();
			saved_sampler_state_.clear();
			saved_texture_stage_state_.clear();
			modifiers.reset();
			info.reset();
		}

		struct modifiers_s
		{
			bool do_not_render = false;
			bool with_high_gamma = false;
			bool as_sky = false;
			bool as_water = false;

			float og_mesh_z_offset = 0.0f;

			bool as_temp_unused = false;
			bool dual_render_with_basetexture2 = false; // render prim a second time with tex2 set as tex1
			bool dual_render_with_specified_texture = false; // render prim a second time with tex defined in 'dual_render_texture'
			bool dual_render_with_specified_texture_blend_add = false; // renders second prim using blend mode ADD
			IDirect3DBaseTexture9* dual_render_texture = nullptr;
			float dual_render_texture_z_offset = 0.0f;

			void reset()
			{
				do_not_render = false;
				with_high_gamma = false;
				as_sky = false;
				as_water = false;
				og_mesh_z_offset = 0.0f;

				as_temp_unused = false;
				dual_render_with_basetexture2 = false;
				dual_render_with_specified_texture = false;
				dual_render_with_specified_texture_blend_add = false;
				dual_render_texture = nullptr;
				dual_render_texture_z_offset = 0.0f;
			}
		};

		// special handlers for the next prim/s
		modifiers_s modifiers;

		struct info_s
		{
			IMaterialInternal* material = nullptr;
			std::string_view material_name;
			std::string_view shader_name;
			BufferedState_t buffer_state {};

			void reset()
			{
				material = nullptr;
				material_name = "";
				shader_name = "";
				memset(&buffer_state, 0, sizeof(BufferedState_t));
			}
		};

		// holds information about the current pass
		// use 'get_info_for_pass()' to populate struct
		info_s info;

		// constructor for singleton
		prim_fvf_context() = default;

	private:
		// Render states to save
		IDirect3DVertexShader9* vs_ = nullptr;
		IDirect3DPixelShader9* ps_ = nullptr;
		IDirect3DBaseTexture9* tex0_ = nullptr;
		IDirect3DBaseTexture9* tex1_ = nullptr;
		IDirect3DBaseTexture9* tex2_ = nullptr;
		IDirect3DBaseTexture9* tex3_ = nullptr;
		bool vs_set = false;
		bool ps_set = false;
		bool tex0_set = false;
		bool tex1_set = false;
		bool tex2_set = false;
		bool tex3_set = false;
		bool tex0_transform_set = false;
		D3DMATRIX tex0_transform_ = {};
		D3DMATRIX world_transform_ = {};
		D3DMATRIX view_transform_ = {};
		D3DMATRIX projection_transform_ = {};
		bool world_transform_set_ = false;
		bool view_transform_set_ = false;
		bool projection_transform_set_ = false;

		// store saved render states (with the type as the key)
		std::unordered_map<D3DRENDERSTATETYPE, DWORD> saved_render_state_;

		// store saved render states (with the type as the key)
		std::unordered_map<std::uint64_t, DWORD> saved_sampler_state_;

		// store saved texture stage states (stage and type are packed into the key)
		std::unordered_map<std::uint64_t, DWORD> saved_texture_stage_state_;
	};


	namespace xorxor_water
	{
		struct status_s
		{
			std::uint64_t water_shader_passes = 0u;
			std::uint64_t converted_dual_draws = 0u;
			std::uint64_t hidden_beneath_passes = 0u;
			std::uint64_t unsupported_vertex_formats = 0u;
			std::uint64_t missing_surface_textures = 0u;
			std::uint64_t stable_hash_bypasses = 0u;
			std::uint64_t static_cache_bypasses = 0u;
			std::uint32_t last_vertex_format = 0u;
			std::string last_material;
			std::string last_shader;
		};

		status_s snapshot();
		void note_static_cache_bypass();
		void print_status();
	}

	namespace tex_addons
	{
		extern LPDIRECT3DTEXTURE9 glass_shards;
		extern LPDIRECT3DTEXTURE9 rain_drop;
		extern LPDIRECT3DTEXTURE9 black;
		extern LPDIRECT3DTEXTURE9 white;
	}

	class model_render : public component
	{
	public:
		model_render();
		~model_render() = default;

		static inline model_render* p_this = nullptr;
		static model_render* get() { return p_this; }

		static void xo_debug_toggle_model_info_fn();
		//static void draw_nocull_markers();
		static void on_present();

		static void init_texture_addons(bool release = false);
		static inline prim_fvf_context primctx {};

		bool m_drew_model = false;
	};
}
