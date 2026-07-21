#pragma once

// These declarations are intentionally local to this header.  The PCH includes
// l4d2.hpp before game/functions.hpp establishes `using namespace components`.
namespace components
{
	struct CRender;
	struct CSkyCamera;
	struct mnode_t;
	enum view_id : __int32;
}

namespace l4d2
{
	extern bool address_resolution_ok;

	extern void* mdl_cache;
	extern components::CRender* engine_renderer;
	extern DWORD* hoststate_worldbrush_data_ptr;
	extern Vector* current_view_origin;
	extern Vector* current_view_forward;
	extern int* visframecount;
	extern Vector* g_vecCurrentRenderOrigin;
	extern Vector4D* s_viewFadeColor;
	extern DWORD* material_system_ptr;
	extern DWORD* modelinfo_ptr;
	extern Vector* camera_forward_vector;
	extern components::view_id* viewid;
	extern DWORD* d3d_device_ptr;
	extern DWORD* shaderapi_ptr;

	using GetCurrentSkyCamera_t = components::CSkyCamera* (__cdecl*)();
	extern GetCurrentSkyCamera_t GetCurrentSkyCamera;
	using R_CullNode_t = bool (__cdecl*)(components::mnode_t*);
	extern R_CullNode_t R_CullNode;
	using CM_LeafArea_t = int (__cdecl*)(int leaf_num);
	extern CM_LeafArea_t CM_LeafArea;

	extern uint32_t hk_addr__scene_ent_on_start_event;
	extern uint32_t hk_addr__scene_ent_on_finish_event;
	extern uint32_t fn_addr__util_remove;
	extern uint32_t hk_addr__on_map_load;
	extern uint32_t hk_addr__on_host_disconnect;
	extern uint32_t hk_addr__on_host_change_level;
	extern uint32_t nop_addr__cdispinfo_render;
	extern uint32_t hk_addr__pre_recursive_world_node;
	extern uint32_t jmp_addr__cullnode01;
	extern uint32_t retn_addr__cullnode_cull;
	extern uint32_t retn_addr__cullnode_skip;
	extern uint32_t nop_addr__cullnode_backface_check01;
	extern uint32_t nop_addr__cullnode_backface_check02;
	extern uint32_t nop_addr__drawleaf_backface_check;
	extern uint32_t nop_addr__draw_opaque_bmodel_backface_check;
	extern uint32_t hk_addr__start_sound;
	extern uint32_t fn_addr__debug_overlay_add_text;
	extern uint32_t fn_addr__debug_overlay_add_text_colored;
	extern uint32_t fn_addr__point_leafnum;
	extern uint32_t hk_addr__cviewrenderer_renderview;
	extern uint32_t hk_addr__skyboxview_draw_internal;
	extern uint32_t jmp_addr__extract_culled_renderables;
	extern uint32_t nop_addr__simple_world_view_intersect_water_check;
	extern uint32_t hk_addr__draw_player_thirdperson_mesh_check01;
	extern uint32_t hk_addr__draw_player_thirdperson_mesh_check02;
	extern uint32_t hk_addr__draw_player_thirdperson_mesh_check03;
	extern uint32_t retn_addr__draw_player_thirdperson_mesh;
	extern uint32_t hk_addr__impact_marks_pshadow;
	extern uint32_t retn_addr__impact_marks_pshadow_skip;
	extern uint32_t hk_addr__render_spritecard_new;
	extern uint32_t hk_addr__rope_mgr_draw_render_cache;
	extern uint32_t hk_addr__glow_overlay_draw;
	extern uint32_t nop_addr__func_area_portal_window_draw_mdl;
	extern uint32_t fn_addr__add_console_cmd;
	extern uint32_t fn_addr__get_bone_transform;
	extern uint32_t fn_addr__lookup_bone;
	extern uint32_t fn_addr__get_model_ptr;
	extern uint32_t kh_addr__cmeshdx8_renderpass_pre_draw;
	extern uint32_t kh_addr__cmeshdx8_renderpass_post_draw;
	extern uint32_t retn_addr__cmeshdx8_renderpass_post_draw;
	extern uint32_t hk_addr__studio_draw_static_mesh;

	bool init_game_addresses();
}
