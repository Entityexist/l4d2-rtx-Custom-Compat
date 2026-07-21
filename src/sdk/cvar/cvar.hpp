#pragma once

// Keep the ICvar wrapper independent from the global `using namespace components`
// side effect in game/functions.hpp.  This header is parsed while the PCH is
// still being built, so all Source SDK types must be explicitly qualified.
namespace components
{
	struct ConCommandBase;
	struct ConVar;
	struct ConCommand;
}

namespace sdk
{
#define CVAR_INTERFACE_VERSION "VEngineCvar007"

	class CCvar
	{
	public:
		void register_con_command(components::ConCommandBase*);
		void unregister_con_command(components::ConCommandBase*);
		void unregister_con_commands(int);
		const char* get_command_line_value(const char*);
		const components::ConCommandBase* find_command_base_const(const char*);
		components::ConCommandBase* find_command_base(const char*);
		const components::ConVar* find_var_const(const char*);
		components::ConVar* find_var(const char*);
		const components::ConCommand* find_command_const(const char*);
		components::ConCommand* find_command(const char*);
	};
}
