// Hello Neighbor side of the passthrough. Every engine tick (game thread):
//   - HN moves the player with Minecraft's movement numbers (speeds, jump, gravity, step-up) and its own exact
//     collision; Minecraft follows HN's camera and knows when the player is airborne (critical hits);
//   - HN's house around the player goes to Minecraft as barrier voxels (surfaces to build on);
//   - F10 instead lets Minecraft move the player with its own physics against those voxels ("driving"): HN sends its
//     mouse look and the movement keys held, Minecraft reports its eye position, and HN's player is put there;
//   - the ReShade compositor draws Minecraft's picture into HN's frame against HN's depth;
//   - Minecraft is started (hidden, off-screen) when HN starts, and quits when HN does.
//
// Coordinates: 1 metre = 1 block. Unreal (x, y, z) in cm, z up, left-handed -> Minecraft (x/100, z/100 + yOffset,
// y/100) (swapping y and z also turns the handedness right). Minecraft yaw = Unreal yaw - 90, pitch = -pitch.
// yOffset puts the ground where the player first stands on a whole block.
//
// Keys: F6 Minecraft hands (mouse + 1-9 go to Minecraft), F7 passthrough on/off, F8 re-level, F9 reflection dump,
// F10 Minecraft physics (blocky collision) on/off.
#include "bridge.hpp"
#include "compositor.h"
#include "ue_call.hpp"
#include "ws.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nc
{
	namespace
	{
		constexpr int kPort = 25620;
		constexpr float kHostNear = 0.1f;      // HN's NearClipPlane (10 uu); Unreal's far plane is infinite
		// The voxels: cells around the player, nearest first, probed a few per tick.
		constexpr int kVoxRadius = 9;          // blocks, horizontally
		constexpr int kVoxBelow = 2, kVoxAbove = 4;
		constexpr int kVoxQueriesPerTick = 160;
		constexpr int kVoxRefreshRadius = 3;   // cells this close are re-probed (doors open and close)
		constexpr DWORD kVoxRefreshMs = 1500;
		constexpr int kDriveReadyRadius = 2;   // cells known around the player before Minecraft takes over
		constexpr float kResyncCm = 150.0f;    // HN moved its player itself (caught, cutscene): follow it

		WsClient g_ws;
		void *g_module = nullptr;
		bool g_enabled = true;
		bool g_hands = false;
		int g_generation = -1;
		long long g_frame = 0;
		double g_yOffset = 0.0;
		bool g_haveOffset = false;
		int g_viewW = 0, g_viewH = 0;
		bool g_keyDown[256] = {};

		// driving
		bool g_driving = false;
		bool g_gridPhysics = false;  // F10
		bool g_haveEye = false;
		Vec3f g_eye;                 // Minecraft's eye, Unreal coordinates
		bool g_placed = false;
		Vec3f g_lastPlaced;          // where we put HN's pawn last tick
		uint8_t g_savedMovementMode = 1;

		// Minecraft's process (from its hello): its window is hidden, and it can be started from here
		DWORD g_mcPid = 0;
		DWORD g_nextHide = 0;
		bool g_launched = false;
		DWORD g_launchAt = 0;

		// voxels: cell -> solid, and when it was probed
		struct Cell
		{
			bool solid;
			DWORD probed;
		};
		std::unordered_map<unsigned long long, Cell> g_cells;
		std::vector<int> g_voxOffsets; // dx, dy, dz triples, nearest first

		void log(const std::wstring &text)
		{
			Output::send<LogLevel::Normal>(STR("[NeighborCraft] {}\n"), text);
		}

		void sendf(const char *format, ...)
		{
			char buffer[4096];
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

		bool key_held(int vk)
		{
			return game_focused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

		/// True once per press (only while HN has the focus).
		bool pressed(int vk)
		{
			const bool down = key_held(vk);
			const bool was = g_keyDown[vk];
			g_keyDown[vk] = down;
			return down && !was;
		}

		/// A held key's change: +1 pressed, -1 released, 0 unchanged.
		int edge(int vk)
		{
			const bool down = key_held(vk);
			const bool was = g_keyDown[vk];
			g_keyDown[vk] = down;
			return down == was ? 0 : (down ? 1 : -1);
		}

		/// Whether `o`'s class is `class_name` or derives from it (checked by name: UE4SS's class lookups can't be
		/// trusted in HN, whose custom engine build confuses the hash tables).
		bool is_a(UObject *o, const wchar_t *class_name)
		{
			for (UStruct *s = o ? o->GetClassPrivate() : nullptr; s; s = s->GetSuperStruct())
				if (s->GetName() == class_name)
					return true;
			return false;
		}

		bool is_default(UObject *o)
		{
			return o->GetName().starts_with(L"Default__");
		}

		/// Every live, non-default instance of `class_name` (or a subclass).
		std::vector<UObject *> find_all(const wchar_t *class_name)
		{
			std::vector<UObject *> found, out;
			UObjectGlobals::FindAllOf(class_name, found);
			for (UObject *o : found)
				if (o && !is_default(o) && is_a(o, class_name))
					out.push_back(o);
			return out;
		}

		UObject *pawn_of(UObject *pc)
		{
			Call c(pc, STR("K2_GetPawn"));
			if (!c.invoke())
				return nullptr;
			UObject **p = c.param<UObject *>(STR("ReturnValue"));
			return p ? *p : nullptr;
		}

		/// The local player's controller, read fresh every frame from objects that live as long as the game:
		/// GameEngine -> GameInstance -> LocalPlayers[0] -> PlayerController (no cached pointer can go stale on a level
		/// load, and no object search is needed).
		UObject *player_controller(UObject *engine)
		{
			UObject **gi = engine ? engine->GetValuePtrByPropertyNameInChain<UObject *>(STR("GameInstance")) : nullptr;
			if (!gi || !*gi)
				return nullptr;
			struct PlayerArray
			{
				UObject **data;
				int32 num;
				int32 max;
			};
			auto *players = (*gi)->GetValuePtrByPropertyNameInChain<PlayerArray>(STR("LocalPlayers"));
			if (!players || players->num < 1 || !players->data || !players->data[0])
				return nullptr;
			UObject **pc = players->data[0]->GetValuePtrByPropertyNameInChain<UObject *>(STR("PlayerController"));
			UObject *found = pc ? *pc : nullptr;
			static UObject *last = nullptr;
			if (found && found != last)
				log(L"player controller: " + found->GetFullName());
			last = found;
			return found;
		}

		/// KismetSystemLibrary's default object (it lives as long as the game).
		UObject *kismet()
		{
			static UObject *k = nullptr;
			if (!k)
				k = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetSystemLibrary"));
			return k;
		}

		/// A one-line message on screen, in Minecraft's action bar (composited over HN's picture).
		void notify(const std::string &text)
		{
			sendf("{\"t\":\"cmd\",\"c\":\"title @a actionbar {\\\"text\\\":\\\"%s\\\",\\\"color\\\":\\\"yellow\\\"}\"}", text.c_str());
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

		void set_actor_location(UObject *actor, const Vec3f &at)
		{
			Call c(actor, STR("K2_SetActorLocation"));
			c.set(STR("NewLocation"), at).set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
		}

		UObject *movement_component(UObject *pawn)
		{
			UObject **m = pawn ? pawn->GetValuePtrByPropertyNameInChain<UObject *>(STR("CharacterMovement")) : nullptr;
			return m ? *m : nullptr;
		}

		/// HN's own movement off (MOVE_None: no walking, no gravity) while Minecraft moves the player, or back on.
		void hn_movement(UObject *pawn, bool on)
		{
			UObject *cmc = movement_component(pawn);
			if (!cmc)
				return;
			uint8_t *mode = cmc->GetValuePtrByPropertyNameInChain<uint8_t>(STR("MovementMode"));
			const uint8_t want = on ? (g_savedMovementMode ? g_savedMovementMode : 1) : 0;
			if (!mode || *mode == want)
				return;
			if (!on)
				g_savedMovementMode = *mode;
			Call c(cmc, STR("SetMovementMode"));
			c.set<uint8_t>(STR("NewMovementMode"), want).set<uint8_t>(STR("NewCustomMode"), 0).invoke();
		}

		/// HN's character movement with Minecraft's numbers (1 block = 1 m): walk 4.317 m/s, sprint 5.612, sneak 1.295;
		/// a jump peaks at 1.25 blocks after 0.3 s (v0 8.35 m/s, gravity 27.8 m/s2); 0.6 block step-up; snappy ground
		/// acceleration and little air control. HN's own collision stays (exact, off-grid). `on` false restores HN's.
		void minecraft_movement(UObject *pawn, bool on)
		{
			struct Setting
			{
				const TCHAR *name;
				float minecraft;
				float saved;
				bool haveSaved;
			};
			static Setting settings[] = {
				{STR("MaxWalkSpeed"), 431.7f}, {STR("MaxWalkSpeedCrouched"), 129.5f}, {STR("JumpZVelocity"), 835.0f},
				{STR("GravityScale"), 2.84f}, {STR("AirControl"), 0.35f}, {STR("MaxStepHeight"), 60.0f},
				{STR("MaxAcceleration"), 4000.0f}, {STR("BrakingDecelerationWalking"), 4000.0f},
			};
			UObject *cmc = movement_component(pawn);
			if (!cmc)
				return;
			for (Setting &s : settings)
			{
				float *v = cmc->GetValuePtrByPropertyNameInChain<float>(s.name);
				if (!v)
					continue;
				if (!s.haveSaved)
				{
					s.saved = *v;
					s.haveSaved = true;
				}
				float want = on ? s.minecraft : s.saved;
				// HN's run key is Shift: Minecraft's sprint speed
				if (on && s.name == settings[0].name && key_held(VK_SHIFT))
					want = 561.2f;
				*v = want;
			}
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

		// ---- collision queries -------------------------------------------------------------------------------

		/// A TArray the engine only reads (passed by const reference): points at our own storage.
		struct ScriptArray
		{
			const void *data;
			int32 num;
			int32 max;
		};
		// EObjectTypeQuery: ObjectTypeQuery1 = WorldStatic, ObjectTypeQuery2 = WorldDynamic (not pawns, not physics props)
		const uint8_t kSolidTypes[] = {0, 1};

		/// Offset of OutHit.ImpactPoint in a trace function's parameter block.
		bool impact_point(Call &c, Vec3f &hit)
		{
			auto *outHit = static_cast<FStructProperty *>(c.property(STR("OutHit")));
			FProperty *impact = outHit ? find_property(outHit->GetStruct(), STR("ImpactPoint")) : nullptr;
			if (!impact)
				return false;
			hit = *reinterpret_cast<const Vec3f *>(c.data() + outHit->GetOffset_Internal() + impact->GetOffset_Internal());
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
			return ok && *ok && impact_point(c, hit);
		}

		/// A line trace against HN's static and dynamic world (walls, doors, furniture), not pawns or loose props.
		bool solid_line(UObject *self, const Vec3f &a, const Vec3f &b)
		{
			Call c(kismet(), STR("LineTraceSingleForObjects"));
			if (!c)
				return false;
			c.set<UObject *>(STR("WorldContextObject"), self)
				.set(STR("Start"), a)
				.set(STR("End"), b)
				.set(STR("ObjectTypes"), ScriptArray{kSolidTypes, 2, 2})
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			return ok && *ok;
		}

		/// Whether HN's world overlaps a box (centre, half size in cm): a box sweep over 2 cm reports what it starts in.
		bool solid_box(UObject *self, const Vec3f &centre, const Vec3f &half)
		{
			Call c(kismet(), STR("BoxTraceSingleForObjects"));
			if (!c)
				return false;
			c.set<UObject *>(STR("WorldContextObject"), self)
				.set(STR("Start"), Vec3f{centre.x, centre.y, centre.z + 1.0f})
				.set(STR("End"), Vec3f{centre.x, centre.y, centre.z - 1.0f})
				.set(STR("HalfSize"), half)
				.set(STR("Orientation"), Rot3f{})
				.set(STR("ObjectTypes"), ScriptArray{kSolidTypes, 2, 2})
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			return ok && *ok;
		}

		bool ground_below(UObject *pawn, const Vec3f &at, float &groundZ)
		{
			Vec3f hit;
			if (!trace(pawn, {at.x, at.y, at.z + 50.0f}, {at.x, at.y, at.z - 3000.0f}, hit))
				return false;
			groundZ = hit.z;
			return true;
		}

		// ---- coordinates -------------------------------------------------------------------------------------

		void to_mc(const Vec3f &u, double &x, double &y, double &z)
		{
			x = u.x / 100.0;
			y = u.z / 100.0 + g_yOffset;
			z = u.y / 100.0;
		}

		Vec3f to_ue(double x, double y, double z)
		{
			return {float(x * 100.0), float(z * 100.0), float((y - g_yOffset) * 100.0)};
		}

		unsigned long long cell_key(int x, int y, int z)
		{
			return (unsigned long long)(uint32_t(x) & 0x1FFFFF) << 42 | (unsigned long long)(uint32_t(y) & 0x1FFFFF) << 21 |
				   (uint32_t(z) & 0x1FFFFF);
		}

		// ---- voxels ------------------------------------------------------------------------------------------

		/// Cell (x, y, z) is solid if HN's world fills its middle (floors, furniture), or a wall crosses it: traces from
		/// its centre towards each side, reaching 2 cm past the +x/+z faces and stopping 2 cm short of the -x/-z ones, so
		/// a thin wall on a cell boundary goes to exactly one of the two cells.
		bool probe_cell(UObject *pawn, int x, int y, int z)
		{
			const Vec3f c = to_ue(x + 0.5, y + 0.5, z + 0.5);
			if (solid_box(pawn, c, {40.0f, 40.0f, 45.0f}))
				return true;
			const float reach[4][2] = {{52, 0}, {-48, 0}, {0, 52}, {0, -48}};
			for (const auto &r : reach)
				if (solid_line(pawn, c, {c.x + r[0], c.y + r[1], c.z}))
					return true;
			return false;
		}

		/// Probe the cells around the player that are unknown, or near and stale, nearest first; send the changes.
		/// Returns whether every cell within kDriveReadyRadius is known.
		bool sample_voxels(UObject *pawn, const Vec3f &feet)
		{
			if (g_voxOffsets.empty())
			{
				std::vector<std::tuple<int, int, int, int>> all;
				for (int dx = -kVoxRadius; dx <= kVoxRadius; ++dx)
					for (int dz = -kVoxRadius; dz <= kVoxRadius; ++dz)
						for (int dy = -kVoxBelow; dy <= kVoxAbove; ++dy)
							if (dx * dx + dz * dz <= kVoxRadius * kVoxRadius)
								all.emplace_back(dx * dx + dz * dz + dy * dy, dx, dy, dz);
				std::sort(all.begin(), all.end());
				for (auto &[d, dx, dy, dz] : all)
					g_voxOffsets.insert(g_voxOffsets.end(), {dx, dy, dz});
			}
			double fx, fy, fz;
			to_mc(feet, fx, fy, fz);
			const int px = int(std::floor(fx)), py = int(std::floor(fy + 0.01)), pz = int(std::floor(fz));
			const DWORD now = GetTickCount();
			std::string solid, empty;
			int queries = 0;
			bool ready = true;
			for (size_t i = 0; i < g_voxOffsets.size() && queries < kVoxQueriesPerTick; i += 3)
			{
				const int dx = g_voxOffsets[i], dy = g_voxOffsets[i + 1], dz = g_voxOffsets[i + 2];
				const int x = px + dx, y = py + dy, z = pz + dz;
				const unsigned long long key = cell_key(x, y, z);
				auto it = g_cells.find(key);
				const bool close = std::abs(dx) <= kVoxRefreshRadius && std::abs(dz) <= kVoxRefreshRadius;
				if (it != g_cells.end() && !(close && now - it->second.probed > kVoxRefreshMs))
					continue;
				if (it == g_cells.end() && std::abs(dx) <= kDriveReadyRadius && std::abs(dz) <= kDriveReadyRadius)
					ready = false;
				queries += 5;
				const bool s = probe_cell(pawn, x, y, z);
				if (it == g_cells.end() || it->second.solid != s)
				{
					std::string &list = s ? solid : empty;
					char entry[48];
					snprintf(entry, sizeof(entry), "%s%d,%d,%d", list.empty() ? "" : ",", x, y, z);
					list += entry;
				}
				g_cells[key] = {s, now};
			}
			if (!solid.empty() || !empty.empty())
				g_ws.send("{\"t\":\"vox\",\"s\":[" + solid + "],\"e\":[" + empty + "]}");
			return ready;
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
			g_cells.clear();
			g_driving = false;
			g_ws.send("{\"t\":\"clear\"}");
			log(L"re-levelled: ground " + std::to_wstring(groundZ) + L" cm, yOffset " + std::to_wstring(g_yOffset));
			notify("NeighborCraft: ground re-levelled");
		}

		/// Minecraft's feet go where HN's player stands (on the ground under it).
		void teleport_minecraft(UObject *pawn, const Vec3f &p)
		{
			float groundZ = p.z - 90.0f;
			ground_below(pawn, p, groundZ);
			double x, y, z;
			to_mc({p.x, p.y, groundZ}, x, y, z);
			sendf("{\"t\":\"tp\",\"p\":[%.4f,%.4f,%.4f]}", x, y + 0.01, z);
			g_haveEye = false;
			g_placed = false;
		}

		// ---- Minecraft's process and window ------------------------------------------------------------------

		std::wstring module_dir()
		{
			wchar_t path[MAX_PATH] = {};
			GetModuleFileNameW(static_cast<HMODULE>(g_module), path, MAX_PATH);
			std::wstring dir(path);
			dir = dir.substr(0, dir.find_last_of(L'\\') + 1); // ...\NeighborCraftMod\dlls\.
			return dir;
		}

		/// Start Minecraft with the command in NeighborCraftMod\minecraft_launch.txt (written by install.sh).
		void launch_minecraft()
		{
			g_launched = true;
			std::wifstream f(module_dir() + L"..\\minecraft_launch.txt");
			std::wstring command;
			if (!f || !std::getline(f, command) || command.empty())
			{
				log(L"Minecraft isn't running and there's no minecraft_launch.txt to start it");
				return;
			}
			STARTUPINFOW si = {sizeof(si)};
			PROCESS_INFORMATION pi = {};
			if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
			{
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
				log(L"starting Minecraft: " + command);
			}
			else
				log(L"couldn't start Minecraft (" + std::to_wstring(GetLastError()) + L"): " + command);
		}

		struct WindowSearch
		{
			DWORD pid;
			HWND found;
		};

		/// Minecraft's window lives off-screen and out of the taskbar: only its picture shows, inside HN's.
		void hide_minecraft_window()
		{
			if (!g_mcPid)
				return;
			WindowSearch s{g_mcPid, nullptr};
			EnumWindows(
				[](HWND h, LPARAM p) -> BOOL {
					auto *s = reinterpret_cast<WindowSearch *>(p);
					DWORD pid = 0;
					GetWindowThreadProcessId(h, &pid);
					if (pid == s->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr)
					{
						s->found = h;
						return FALSE;
					}
					return TRUE;
				},
				reinterpret_cast<LPARAM>(&s));
			if (!s.found)
				return;
			RECT r;
			GetWindowRect(s.found, &r);
			const LONG_PTR ex = GetWindowLongPtrW(s.found, GWL_EXSTYLE);
			const bool hidden = r.left <= -10000 && (ex & WS_EX_TOOLWINDOW);
			if (hidden)
				return;
			SetWindowLongPtrW(s.found, GWL_EXSTYLE, (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW);
			SetWindowPos(s.found, HWND_BOTTOM, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
			// give the focus back to HN
			WindowSearch me{GetCurrentProcessId(), nullptr};
			EnumWindows(
				[](HWND h, LPARAM p) -> BOOL {
					auto *s = reinterpret_cast<WindowSearch *>(p);
					DWORD pid = 0;
					GetWindowThreadProcessId(h, &pid);
					wchar_t cls[64] = {};
					GetClassNameW(h, cls, 64);
					if (pid == s->pid && IsWindowVisible(h) && wcscmp(cls, L"UnrealWindow") == 0)
					{
						s->found = h;
						return FALSE;
					}
					return TRUE;
				},
				reinterpret_cast<LPARAM>(&me));
			if (me.found)
				SetForegroundWindow(me.found);
			log(L"Minecraft's window hidden");
		}

		// ---- messages from Minecraft -------------------------------------------------------------------------

		bool json_number(const std::string &m, const char *key, double &out)
		{
			const size_t at = m.find(std::string("\"") + key + "\":");
			if (at == std::string::npos)
				return false;
			out = std::strtod(m.c_str() + at + std::strlen(key) + 3, nullptr);
			return true;
		}

		/// The first three numbers of the array under `key`.
		bool json_vec3(const std::string &m, const char *key, double v[3])
		{
			size_t at = m.find(std::string("\"") + key + "\":[");
			if (at == std::string::npos)
				return false;
			const char *p = m.c_str() + at + std::strlen(key) + 4;
			for (int i = 0; i < 3; ++i)
			{
				char *end = nullptr;
				v[i] = std::strtod(p, &end);
				if (end == p)
					return false;
				p = end + (*end == ',' ? 1 : 0);
			}
			return true;
		}

		void handle(const std::string &m)
		{
			if (m.find("\"t\":\"mcpos\"") != std::string::npos)
			{
				double eye[3];
				if (json_vec3(m, "eye", eye))
				{
					g_eye = to_ue(eye[0], eye[1], eye[2]);
					g_haveEye = true;
				}
			}
			else if (m.find("\"t\":\"hello\"") != std::string::npos)
			{
				double pid = 0;
				if (json_number(m, "pid", pid))
					g_mcPid = DWORD(pid);
				g_nextHide = 0;
				log(L"Minecraft says hello (pid " + std::to_wstring(g_mcPid) + L")");
			}
		}

		void forward_key(int vk, const char *key)
		{
			if (const int e = edge(vk))
				sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", key, e > 0 ? "true" : "false");
		}

		// ---- F9 dump -----------------------------------------------------------------------------------------

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
			const std::wstring file = module_dir() + L"NeighborCraft_dump.txt";
			std::wofstream f(file);
			UObject **pcm = pc ? pc->GetValuePtrByPropertyNameInChain<UObject *>(STR("PlayerCameraManager")) : nullptr;
			dump_object(f, pc, L"PlayerController");
			dump_object(f, pcm ? *pcm : nullptr, L"PlayerCameraManager");
			dump_object(f, pawn, L"Pawn");
			dump_object(f, movement_component(pawn), L"Pawn movement");
			const std::vector<UObject *> neighbors = find_all(L"BP_Sosed_C");
			f << L"==== BP_Sosed_C instances: " << neighbors.size() << L"\n";
			dump_object(f, neighbors.empty() ? nullptr : neighbors[0], L"Neighbor");
			if (!neighbors.empty())
			{
				UObject **controller = neighbors[0]->GetValuePtrByPropertyNameInChain<UObject *>(STR("Controller"));
				dump_object(f, controller ? *controller : nullptr, L"Neighbor controller");
			}
			const std::vector<UObject *> navmeshes = find_all(L"RecastNavMesh");
			f << L"==== RecastNavMesh instances: " << navmeshes.size() << L"\n";
			for (UObject *n : navmeshes)
			{
				const uint8_t *gen = n->GetValuePtrByPropertyNameInChain<uint8_t>(STR("RuntimeGeneration"));
				f << L"  " << n->GetFullName() << L"  RuntimeGeneration=" << (gen ? int(*gen) : -1)
				  << L" (0 Static, 1 DynamicModifiersOnly, 2 Dynamic)\n";
			}
			log(L"dump written: " + file);
			notify("NeighborCraft: dump written");
		}
	}

	void start(void *module)
	{
		g_module = module;
		g_ws.start("127.0.0.1", kPort);
		g_launchAt = GetTickCount() + 3000; // Minecraft may be running already: give the link a moment first
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

	void tick(void *engine_object, float)
	{
		compositor::try_register(g_module);
		++g_frame;

		// Minecraft: started from here if it isn't running, and kept out of sight
		if (!g_launched && !g_ws.connected() && GetTickCount() > g_launchAt)
			launch_minecraft();
		if (g_ws.connected() && GetTickCount() > g_nextHide)
		{
			g_nextHide = GetTickCount() + 2000;
			hide_minecraft_window();
		}

		if (pressed(VK_F7))
		{
			g_enabled = !g_enabled;
			log(g_enabled ? L"passthrough on" : L"passthrough off");
			notify(g_enabled ? "NeighborCraft on" : "NeighborCraft off");
		}
		if (pressed(VK_F6))
		{
			g_hands = !g_hands;
			log(g_hands ? L"Minecraft hands on (mouse and 1-9 go to Minecraft)" : L"Minecraft hands off");
			notify(g_hands ? "Minecraft hands ON (mouse + 1-9 go to Minecraft)" : "Minecraft hands off");
		}

		UObject *pc = player_controller(static_cast<UObject *>(engine_object));
		UObject *pawn = pc ? pawn_of(pc) : nullptr;
		if (pressed(VK_F9))
			dump(pc, pawn);

		std::string message;
		while (g_ws.poll(message))
			handle(message);

		Camera cam;
		const bool live = g_enabled && g_ws.connected() && pawn && read_camera(pc, cam);
		compositor::set_active(live);
		if (!live)
		{
			if (pawn)
				minecraft_movement(pawn, false);
			if (pawn && g_driving)
				hn_movement(pawn, true);
			g_driving = false;
			return;
		}

		Vec3f p;
		actor_location(pawn, p);
		if (g_ws.generation() != g_generation)
		{
			// (re)connected: start the world over and show Minecraft's HUD
			g_generation = g_ws.generation();
			g_haveOffset = false;
			g_viewW = g_viewH = 0;
			g_ws.send("{\"t\":\"hud\",\"hidden\":false}");
		}
		if (!g_haveOffset || pressed(VK_F8))
		{
			if (g_driving)
				hn_movement(pawn, true);
			relevel(pawn, p);
		}

		// Minecraft's window matches HN's picture
		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		if (bw > 0 && bh > 0 && (bw != g_viewW || bh != g_viewH))
		{
			g_viewW = bw;
			g_viewH = bh;
			sendf("{\"t\":\"view\",\"w\":%d,\"h\":%d}", bw, bh);
			g_nextHide = 0; // resizing may bring the window back on screen
		}

		// HN's camera, in Minecraft's terms (Unreal's FOV is horizontal, Minecraft's vertical)
		const double aspect = g_viewH > 0 ? double(g_viewW) / g_viewH : 16.0 / 9.0;
		const float vfov = float(2.0 * std::atan(std::tan(cam.hfov * 3.14159265358979 / 360.0) / aspect) * 180.0 / 3.14159265358979);
		double cx, cy, cz;
		to_mc(cam.loc, cx, cy, cz);
		const float yaw = cam.rot.yaw - 90.0f, pitch = -cam.rot.pitch, roll = cam.rot.roll;
		compositor::set_host_pose(yaw, pitch, roll, vfov, cx, cy, cz);

		// F10: Minecraft's own physics against the voxels (blocky collision) instead of HN's exact collision
		if (pressed(VK_F10))
		{
			g_gridPhysics = !g_gridPhysics;
			notify(g_gridPhysics ? "Minecraft physics: blocky collision" : "HN collision, Minecraft movement");
		}
		const bool ready = sample_voxels(pawn, p);
		if (g_gridPhysics && !g_driving && ready)
		{
			// the house around the player is known: Minecraft takes over the walking
			teleport_minecraft(pawn, p);
			hn_movement(pawn, false);
			g_driving = true;
			log(L"Minecraft physics on");
		}
		else if (!g_gridPhysics && g_driving)
		{
			hn_movement(pawn, true);
			g_driving = false;
		}

		if (!g_driving)
			minecraft_movement(pawn, true);

		if (g_driving)
		{
			// HN moved its player itself (caught by the Neighbor, a cutscene, a level reload): Minecraft follows
			if (g_placed && std::hypot(p.x - g_lastPlaced.x, p.y - g_lastPlaced.y, p.z - g_lastPlaced.z) > kResyncCm)
			{
				log(L"HN moved the player: Minecraft follows");
				teleport_minecraft(pawn, p);
			}
			hn_movement(pawn, false);
			// HN's camera goes to Minecraft's eyes: move the pawn by the camera's offset from it
			if (g_haveEye)
			{
				const Vec3f target{g_eye.x - (cam.loc.x - p.x), g_eye.y - (cam.loc.y - p.y), g_eye.z - (cam.loc.z - p.z)};
				set_actor_location(pawn, target);
				g_lastPlaced = target;
				g_placed = true;
			}
			const int bits = (key_held('W') ? 1 : 0) | (key_held('S') ? 2 : 0) | (key_held('A') ? 4 : 0) | (key_held('D') ? 8 : 0) |
							 (key_held(VK_SPACE) ? 16 : 0) | (key_held(VK_CONTROL) ? 32 : 0) | (key_held(VK_SHIFT) ? 64 : 0);
			sendf("{\"t\":\"cam\",\"drive\":true,\"f\":%lld,\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f}", g_frame, yaw, pitch, roll, vfov);
			sendf("{\"t\":\"move\",\"m\":%d}", bits);
		}
		else
		{
			// HN moves the player (with Minecraft's speeds and jump) and collides it exactly; Minecraft follows, and
			// knows when the player is in the air (falling makes Minecraft's hits critical)
			UObject *cmc = movement_component(pawn);
			const uint8_t *mode = cmc ? cmc->GetValuePtrByPropertyNameInChain<uint8_t>(STR("MovementMode")) : nullptr;
			const bool air = mode && *mode == 3; // MOVE_Falling
			sendf("{\"t\":\"cam\",\"f\":%lld,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":true,\"air\":%s}",
				g_frame, cx, cy, cz, yaw, pitch, roll, vfov, air ? "true" : "false");
		}

		if (g_hands)
		{
			forward_key(VK_LBUTTON, "attack");
			forward_key(VK_RBUTTON, "use");
			forward_key(VK_MBUTTON, "pick");
			for (int i = 0; i < 9; ++i)
				if (pressed('1' + i))
					sendf("{\"t\":\"slot\",\"n\":%d}", i);
		}
	}
}
