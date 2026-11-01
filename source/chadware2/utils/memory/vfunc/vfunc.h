#pragma once

#include <cstddef>
#include <cstdint>

namespace M {
	template <typename T, std::size_t nIndex, class CBaseClass, typename... Args_t>
	static inline T CallVFunc(CBaseClass* thisptr, Args_t... argList)
	{
		using VirtualFn_t = T(__thiscall*)(const void*, decltype(argList)...);
		return (*reinterpret_cast<VirtualFn_t* const*>(reinterpret_cast<std::uintptr_t>(thisptr)))[nIndex](thisptr, argList...);
	}

	// Just get func ptr but don't call it
	template <std::size_t nIndex, class CBaseClass>
	[[nodiscard]] static inline void* GetVFunc(CBaseClass* thisPtr)
	{
		if (!thisPtr)
			return nullptr;

		return (*reinterpret_cast<void***>(thisPtr))[nIndex];
	}
}