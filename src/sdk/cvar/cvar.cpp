#include <std_include.hpp>
#include "cvar.hpp"

namespace sdk
{
	void CCvar::register_con_command(components::ConCommandBase* c)
	{
		using original_fn = void(__thiscall*)(CCvar*, components::ConCommandBase*);
		return (*(original_fn**)this)[6](this, c);
	}

	void CCvar::unregister_con_command(components::ConCommandBase* c)
	{
		using original_fn = void(__thiscall*)(CCvar*, components::ConCommandBase*);
		return (*(original_fn**)this)[7](this, c);
	}

	void CCvar::unregister_con_commands(int num)
	{
		using original_fn = void(__thiscall*)(CCvar*, int);
		return (*(original_fn**)this)[8](this, num);
	}

	const char* CCvar::get_command_line_value(const char* name)
	{
		using original_fn = const char* (__thiscall*)(CCvar*, const char*);
		return (*(original_fn**)this)[9](this, name);
	}

	const components::ConCommandBase* CCvar::find_command_base_const(const char* name)
	{
		using original_fn = const components::ConCommandBase* (__thiscall*)(CCvar*, const char*);
		return (*(original_fn**)this)[10](this, name);
	}

	components::ConCommandBase* CCvar::find_command_base(const char* name)
	{
		using original_fn = components::ConCommandBase * (__thiscall*)(CCvar*, const char*);
		return (*(original_fn**)this)[11](this, name);
	}

	const components::ConVar* CCvar::find_var_const(const char* name)
	{
		using original_fn = const components::ConVar* (__thiscall*)(CCvar*, const char*);
		return (*(original_fn**)this)[12](this, name);
	}

	components::ConVar* CCvar::find_var(const char* name)
	{
		using original_fn = components::ConVar * (__thiscall*)(CCvar*, const char*);
		return (*(original_fn**)this)[13](this, name);
	}

	const components::ConCommand* CCvar::find_command_const(const char* name)
	{
		using original_fn = const components::ConCommand* (__thiscall*)(CCvar*, const char*);
		return (*(original_fn**)this)[14](this, name);
	}

	components::ConCommand* CCvar::find_command(const char* name)
	{
		using original_fn = components::ConCommand * (__thiscall*)(CCvar*, const char*);
		return (*(original_fn**)this)[15](this, name);
	}
}
