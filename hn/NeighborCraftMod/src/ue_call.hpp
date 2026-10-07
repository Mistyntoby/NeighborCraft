// Calling Unreal functions through reflection: the parameter block is laid out from the UFunction's own property
// offsets, so nothing depends on struct layouts compiled into this DLL.
#pragma once
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nc
{
	using namespace RC;
	using namespace RC::Unreal;

	// Hello Neighbor is UE 4.20: vectors and rotators are floats (no large world coordinates).
	struct Vec3f
	{
		float x = 0, y = 0, z = 0;
	};
	struct Rot3f
	{
		float pitch = 0, yaw = 0, roll = 0;
	};

	inline FProperty *find_property(UStruct *s, const TCHAR *name)
	{
		return s ? s->FindProperty(FName(name, FNAME_Find)) : nullptr;
	}

	/// One call of `function` on `target` (for a static BlueprintFunctionLibrary function: on its default object).
	class Call
	{
	public:
		Call(UObject *target, const TCHAR *function) : m_target(target)
		{
			if (target)
				m_fn = lookup(target, function);
			if (m_fn)
				m_buf.assign(size_t(std::max<int32>(m_fn->GetPropertiesSize(), 1)), 0);
		}
		explicit operator bool() const { return m_fn != nullptr; }

		FProperty *property(const TCHAR *name) const { return find_property(m_fn, name); }

		/// Pointer to a parameter (or the return value, "ReturnValue") inside the parameter block.
		template <typename T>
		T *param(const TCHAR *name)
		{
			FProperty *p = property(name);
			return p ? reinterpret_cast<T *>(m_buf.data() + p->GetOffset_Internal()) : nullptr;
		}

		template <typename T>
		Call &set(const TCHAR *name, const T &value)
		{
			if (T *p = param<T>(name))
				*p = value;
			return *this;
		}

		/// bool parameters are whole bytes in a parameter block.
		Call &set_bool(const TCHAR *name, bool value) { return set<uint8_t>(name, value ? 1 : 0); }

		bool invoke()
		{
			if (!m_fn)
				return false;
			m_target->ProcessEvent(m_fn, m_buf.data());
			return true;
		}

		uint8_t *data() { return m_buf.data(); }

	private:
		/// Functions found once per (class, name): `function` is always a string literal, so its address is the key.
		static UFunction *lookup(UObject *target, const TCHAR *function)
		{
			struct Key
			{
				const void *cls;
				const TCHAR *name;
				bool operator==(const Key &) const = default;
			};
			struct Hash
			{
				size_t operator()(const Key &k) const { return std::hash<const void *>()(k.cls) ^ (std::hash<const void *>()(k.name) << 1); }
			};
			static std::unordered_map<Key, UFunction *, Hash> cache;
			const Key key{target->GetClassPrivate(), function};
			auto it = cache.find(key);
			if (it != cache.end())
				return it->second;
			UFunction *fn = target->GetFunctionByNameInChain(function);
			cache.emplace(key, fn);
			return fn;
		}

		UObject *m_target = nullptr;
		UFunction *m_fn = nullptr;
		std::vector<uint8_t> m_buf;
	};
}
