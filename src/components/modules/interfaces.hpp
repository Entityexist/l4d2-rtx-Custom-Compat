#pragma once

namespace components
{
	class interfaces : public component
	{
	public:
		interfaces();

		static inline interfaces* p_this = nullptr;
		static interfaces* get() { return p_this; }

		sdk::base_client* m_client = nullptr;
		sdk::engine_client* m_engine = nullptr;
		sdk::engine_effects* m_effects = nullptr;
		sdk::engine_trace* m_engine_trace = nullptr;
		sdk::entity_list* m_entity_list = nullptr;
		sdk::surface* m_surface = nullptr;
		sdk::player_info_manager* m_player_manager = nullptr;
		IVModelInfo* m_model_info = nullptr;
		sdk::CCvar* m_cvar = nullptr;
		void* m_server_tools = nullptr;
		CGlobalVarsBase* m_globals = nullptr;

private:
		template <typename m_interface>
		static m_interface* get_interface(const std::string& module_name, const std::string& interface_name);
	};
}