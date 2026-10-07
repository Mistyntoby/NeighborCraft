// NeighborCraft: the Hello Neighbor half of the Minecraft passthrough (a UE4SS C++ mod that is also a ReShade add-on).
#include "bridge.hpp"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Mod/CppUserModBase.hpp>
#include <Unreal/Hooks/Hooks.hpp>

#include <windows.h>

using namespace RC;

namespace
{
	void *this_module()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&this_module), &module);
		return module;
	}
}

class NeighborCraftMod : public CppUserModBase
{
public:
	NeighborCraftMod()
	{
		ModName = STR("NeighborCraftMod");
		ModVersion = STR("0.1.0");
		ModDescription = STR("Minecraft passthrough for Hello Neighbor");
		ModAuthors = STR("Mistyntoby");
	}

	~NeighborCraftMod() override
	{
		nc::stop();
	}

	auto on_unreal_init() -> void override
	{
		nc::start(this_module());
		Unreal::Hook::RegisterEngineTickPostCallback(
			[](auto &, Unreal::UEngine *, float delta_seconds, bool) { nc::tick(delta_seconds); },
			{false, false, STR("NeighborCraftMod"), STR("Tick")});
	}
};

extern "C"
{
	__declspec(dllexport) CppUserModBase *start_mod()
	{
		return new NeighborCraftMod();
	}

	__declspec(dllexport) void uninstall_mod(CppUserModBase *mod)
	{
		delete mod;
	}
}
