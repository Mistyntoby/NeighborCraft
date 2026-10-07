// Hello Neighbor side of the passthrough. Every engine tick (game thread): send HN's camera to the Minecraft mod,
// feed it HN's floors around the player as barrier columns, forward the mouse/number keys in "Minecraft hands"
// mode, and drive the ReShade compositor that draws Minecraft into HN's frame.
//
// Coordinates: 1 metre = 1 block. Unreal (x, y, z) in cm, z up, left-handed -> Minecraft (x/100, z/100 + yOffset,
// y/100) (swapping y and z also turns the handedness right). Minecraft yaw = Unreal yaw - 90, pitch = -pitch.
// yOffset puts the ground under the player on a whole block.
//
// Keys: F6 Minecraft hands (mouse + 1-9 go to Minecraft), F7 passthrough on/off, F8 re-level, F9 reflection dump.
#include "bridge.hpp"
#include "compositor.h"
#include "ue_call.hpp"
#include "ws.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nc
{
	namespace
	{
		constexpr int kPort = 25620;
		constexpr int kGroundRadius = 24;      // blocks around the player that get collision
		constexpr int kProbesPerTick = 48;     // line traces per tick for that
		constexpr int kGroundDepth = 2;        // barrier layers under each surface
		constexpr float kFloorBand = 220.0f;   // cm: columns are re-probed when the player changes storey
		constexpr float kHostNear = 0.1f;      // HN's NearClipPlane (10 uu); Unreal's far plane is infinite

		WsClient g_ws;
		void *g_module = nullptr;
		bool g_enabled = true;
		bool g_hands = false;
		int g_generation = -1;
		long long g_frame = 0;
		double g_yOffset = 0.0;
		bool g_haveOffset = false;
		std::unordered_set<unsigned long long> g_sampled;
		std::vector<std::pair<int, int>> g_spiral;
		ObjRef g_pc, g_kismet;
		int g_viewW = 0, g_viewH = 0;
		bool g_keyDown[256] = {};

		void log(const std::wstring &text)
		{
			Output::send<LogLevel::Normal>(STR("[NeighborCraft] {}\n"), text);
		}

		void sendf(const char *format, ...)
		{
			char buffer[2048];
			va_list args;
			va_start(args, format);
			vsnprintf(buffer, sizeof(buffer), format, args);
			va_end(args);
			g_ws.send(buffer);
		}

		bool game_focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		/// True once per press (only while HN has the focus).
		bool pressed(int vk)
		{
			const bool down = game_focused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool was = g_keyDown[vk];
			g_keyDown[vk] = down;
			return down && !was;
		}

		/// A held key's change: +1 pressed, -1 released, 0 unchanged.
		int edge(int vk)
		{
			const bool down = game_focused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool was = g_keyDown[vk];
			g_keyDown[vk] = down;
			return down == was ? 0 : (down ? 1 : -1);
		}

		UObject *player_controller()
		{
			if (UObject *pc = g_pc.get())
				return pc;
			static DWORD next = 0;
			if (GetTickCount() < next)
				return nullptr;
			next = GetTickCount() + 500;
			UObject *pc = UObjectGlobals::FindFirstOf(STR("PlayerController"));
			g_pc = ObjRef(pc);
			if (pc)
				log(L"player controller: " + pc->GetFullName());
			return pc;
		}

		UObject *kismet()
		{
			if (UObject *k = g_kismet.get())
				return k;
			UObject *k = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
			g_kismet = ObjRef(k);
			return k;
		}

		UObject *pawn_of(UObject *pc)
		{
			Call c(pc, STR("K2_GetPawn"));
			if (!c.invoke())
				return nullptr;
			UObject **p = c.param<UObject *>(STR("ReturnValue"));
			return p ? *p : nullptr;
		}

		bool actor_location(UObject *actor, Vec3f &out)
		{
			Call c(actor, STR("K2_GetActorLocation"));
			if (!c.invoke())
				return false;
			if (Vec3f *v = c.param<Vec3f>(STR("ReturnValue")))
				out = *v;
			return true;
		}

		struct Camera
		{
			Vec3f loc;
			Rot3f rot;
			float hfov = 90.0f;
		};

		bool read_camera(UObject *pc, Camera &cam)
		{
			UObject **pcm = pc->GetValuePtrByPropertyNameInChain<UObject *>(STR("PlayerCameraManager"));
			if (!pcm || !*pcm)
				return false;
			Call loc(*pcm, STR("GetCameraLocation")), rot(*pcm, STR("GetCameraRotation")), fov(*pcm, STR("GetFOVAngle"));
			if (!loc.invoke() || !rot.invoke() || !fov.invoke())
				return false;
			cam.loc = *loc.param<Vec3f>(STR("ReturnValue"));
			cam.rot = *rot.param<Rot3f>(STR("ReturnValue"));
			cam.hfov = *fov.param<float>(STR("ReturnValue"));
			return true;
		}

		/// A Visibility line trace from `a` to `b` (cm), ignoring `self`'s actor; the impact point on a hit.
		bool trace(UObject *self, const Vec3f &a, const Vec3f &b, Vec3f &hit)
		{
			Call c(kismet(), STR("LineTraceSingle"));
			if (!c)
				return false;
			c.set<UObject *>(STR("WorldContextObject"), self)
				.set(STR("Start"), a)
				.set(STR("End"), b)
				.set<uint8_t>(STR("TraceChannel"), 0) // TraceTypeQuery1 = Visibility
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			if (!ok || !*ok)
				return false;
			static int32 hitOffset = -1, impactOffset = -1;
			if (impactOffset < 0)
			{
				auto *outHit = static_cast<FStructProperty *>(c.property(STR("OutHit")));
				FProperty *impact = outHit ? find_property(outHit->GetStruct(), STR("ImpactPoint")) : nullptr;
				if (!impact)
					return false;
				hitOffset = outHit->GetOffset_Internal();
				impactOffset = impact->GetOffset_Internal();
			}
			hit = *reinterpret_cast<const Vec3f *>(c.data() + hitOffset + impactOffset);
			return true;
		}

		bool ground_below(UObject *pawn, const Vec3f &at, float &groundZ)
		{
			Vec3f hit;
			if (!trace(pawn, {at.x, at.y, at.z + 150.0f}, {at.x, at.y, at.z - 3000.0f}, hit))
				return false;
			groundZ = hit.z;
			return true;
		}

		/// Put the ground under the player on a whole block, and start the barriers over.
		void relevel(UObject *pawn, const Vec3f &p)
		{
			float groundZ = 0.0f;
			if (!ground_below(pawn, p, groundZ))
				return;
			const double metres = groundZ / 100.0;
			g_yOffset = std::round(metres) - metres;
			g_haveOffset = true;
			g_sampled.clear();
			g_ws.send("{\"t\":\"clear\"}");
			log(L"re-levelled: ground " + std::to_wstring(groundZ) + L" cm, yOffset " + std::to_wstring(g_yOffset));
		}

		/// HN's floor under columns around the player that haven't been probed on this storey yet, nearest first.
		void sample_ground(UObject *pawn, const Vec3f &p)
		{
			if (g_spiral.empty())
			{
				for (int dx = -kGroundRadius; dx <= kGroundRadius; ++dx)
					for (int dz = -kGroundRadius; dz <= kGroundRadius; ++dz)
						if (dx * dx + dz * dz <= kGroundRadius * kGroundRadius)
							g_spiral.emplace_back(dx, dz);
				std::sort(g_spiral.begin(), g_spiral.end(), [](auto &a, auto &b) {
					return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
				});
			}
			const int px = int(std::floor(p.x / 100.0f)), pz = int(std::floor(p.y / 100.0f));
			const int band = int(std::floor(p.z / kFloorBand));
			std::string columns;
			int probes = 0;
			for (const auto &[dx, dz] : g_spiral)
			{
				const int x = px + dx, z = pz + dz;
				const unsigned long long key = (unsigned long long)(uint32_t(x) & 0xFFFFF) << 40 |
											   (unsigned long long)(uint32_t(z) & 0xFFFFF) << 20 | (uint32_t(band) & 0xFFFFF);
				if (g_sampled.count(key))
					continue;
				if (++probes > kProbesPerTick)
					break;
				g_sampled.insert(key);
				// Minecraft column (x, z) covers Unreal x in [100x, 100x+100) and y in [100z, 100z+100): probe its centre,
				// from just above the player's head (indoors that finds this storey's floor, not the roof).
				float groundZ = 0.0f;
				if (!ground_below(pawn, {(x + 0.5f) * 100.0f, (z + 0.5f) * 100.0f, p.z}, groundZ))
					continue;
				const int top = int(std::floor(groundZ / 100.0 + g_yOffset + 0.5)) - 1;
				char entry[64];
				snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", x, z, top - kGroundDepth + 1, top);
				columns += entry;
			}
			if (!columns.empty())
				g_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
		}

		void forward_key(int vk, const char *key)
		{
			if (const int e = edge(vk))
				sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", key, e > 0 ? "true" : "false");
		}

		void dump_object(std::wofstream &f, UObject *o, const wchar_t *label)
		{
			f << L"==== " << label << L": " << (o ? o->GetFullName() : StringType(L"(none)")) << L"\n";
			if (!o)
				return;
#pragma warning(push)
#pragma warning(disable : 4996)
			for (UStruct *s = o->GetClassPrivate(); s; s = s->GetSuperStruct())
			{
				f << L"  class " << s->GetName() << L"\n";
				for (FProperty *p : s->ForEachProperty())
					f << L"    prop " << p->GetName() << L" @0x" << std::hex << p->GetOffset_Internal() << std::dec << L" size " << p->GetSize() << L"\n";
				for (UFunction *fn : s->ForEachFunction())
					f << L"    func " << fn->GetName() << L"\n";
			}
#pragma warning(pop)
		}

		/// F9: what this mod needs to know about HN, from the live game (written next to the mod's DLL).
		void dump(UObject *pc, UObject *pawn)
		{
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(static_cast<HMODULE>(g_module), path, MAX_PATH);
			std::wstring file(path);
			file = file.substr(0, file.find_last_of(L'\\') + 1) + L"NeighborCraft_dump.txt";
			std::wofstream f(file);
			UObject **pcm = pc ? pc->GetValuePtrByPropertyNameInChain<UObject *>(STR("PlayerCameraManager")) : nullptr;
			dump_object(f, pc, L"PlayerController");
			dump_object(f, pcm ? *pcm : nullptr, L"PlayerCameraManager");
			dump_object(f, pawn, L"Pawn");
			std::vector<UObject *> neighbors;
			UObjectGlobals::FindAllOf(STR("BP_Sosed_C"), neighbors);
			f << L"==== BP_Sosed_C instances: " << neighbors.size() << L"\n";
			dump_object(f, neighbors.empty() ? nullptr : neighbors[0], L"Neighbor");
			if (!neighbors.empty())
			{
				UObject **controller = neighbors[0]->GetValuePtrByPropertyNameInChain<UObject *>(STR("Controller"));
				dump_object(f, controller ? *controller : nullptr, L"Neighbor controller");
			}
			std::vector<UObject *> navmeshes;
			UObjectGlobals::FindAllOf(STR("RecastNavMesh"), navmeshes);
			f << L"==== RecastNavMesh instances: " << navmeshes.size() << L"\n";
			for (UObject *n : navmeshes)
			{
				const uint8_t *gen = n->GetValuePtrByPropertyNameInChain<uint8_t>(STR("RuntimeGeneration"));
				f << L"  " << n->GetFullName() << L"  RuntimeGeneration=" << (gen ? int(*gen) : -1)
				  << L" (0 Static, 1 DynamicModifiersOnly, 2 Dynamic)\n";
			}
			dump_object(f, navmeshes.empty() ? nullptr : navmeshes[0], L"RecastNavMesh");
			log(L"dump written: " + file + L" (" + std::to_wstring(neighbors.size()) + L" neighbours, " +
				std::to_wstring(navmeshes.size()) + L" navmeshes)");
		}

		void handle(const std::string &m)
		{
			if (m.find("\"t\":\"hello\"") != std::string::npos)
				log(L"Minecraft says hello");
		}
	}

	void start(void *module)
	{
		g_module = module;
		g_ws.start("127.0.0.1", kPort);
		compositor::set_host_planes(kHostNear, 1.0e7f);
		compositor::set_pose_lag(0);
		log(L"started; Minecraft link on 127.0.0.1:" + std::to_wstring(kPort));
	}

	void stop()
	{
		compositor::set_active(false);
		compositor::unregister(g_module);
		g_ws.stop();
	}

	void tick(float)
	{
		compositor::try_register(g_module);
		++g_frame;

		if (pressed(VK_F7))
		{
			g_enabled = !g_enabled;
			log(g_enabled ? L"passthrough on" : L"passthrough off");
		}
		if (pressed(VK_F6))
		{
			g_hands = !g_hands;
			log(g_hands ? L"Minecraft hands on (mouse and 1-9 go to Minecraft)" : L"Minecraft hands off");
		}

		UObject *pc = player_controller();
		UObject *pawn = pc ? pawn_of(pc) : nullptr;
		if (pressed(VK_F9))
			dump(pc, pawn);

		Camera cam;
		const bool live = g_enabled && g_ws.connected() && pawn && read_camera(pc, cam);
		compositor::set_active(live);
		if (!live)
			return;

		Vec3f p;
		actor_location(pawn, p);
		if (g_ws.generation() != g_generation)
		{
			// (re)connected: start the ground over and show Minecraft's HUD
			g_generation = g_ws.generation();
			g_haveOffset = false;
			g_viewW = g_viewH = 0;
			g_ws.send("{\"t\":\"hud\",\"hidden\":false}");
		}
		if (!g_haveOffset || pressed(VK_F8))
			relevel(pawn, p);

		// Minecraft's window matches HN's picture
		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		if (bw > 0 && bh > 0 && (bw != g_viewW || bh != g_viewH))
		{
			g_viewW = bw;
			g_viewH = bh;
			sendf("{\"t\":\"view\",\"w\":%d,\"h\":%d}", bw, bh);
		}

		// the camera, in Minecraft's terms (Unreal's FOV is horizontal, Minecraft's vertical)
		const double aspect = g_viewH > 0 ? double(g_viewW) / g_viewH : 16.0 / 9.0;
		const float vfov = float(2.0 * std::atan(std::tan(cam.hfov * 3.14159265358979 / 360.0) / aspect) * 180.0 / 3.14159265358979);
		const double mx = cam.loc.x / 100.0, my = cam.loc.z / 100.0 + g_yOffset, mz = cam.loc.y / 100.0;
		const float yaw = cam.rot.yaw - 90.0f, pitch = -cam.rot.pitch, roll = cam.rot.roll;
		compositor::set_host_pose(yaw, pitch, roll, vfov, mx, my, mz);
		sendf("{\"t\":\"cam\",\"f\":%lld,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":true}",
			g_frame, mx, my, mz, yaw, pitch, roll, vfov);

		if (g_hands)
		{
			forward_key(VK_LBUTTON, "attack");
			forward_key(VK_RBUTTON, "use");
			forward_key(VK_MBUTTON, "pick");
			for (int i = 0; i < 9; ++i)
				if (pressed('1' + i))
					sendf("{\"t\":\"slot\",\"n\":%d}", i);
		}

		sample_ground(pawn, p);

		std::string message;
		while (g_ws.poll(message))
			handle(message);
	}
}
