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
// yOffset (~140) keeps all of HN's height inside Minecraft's world and puts the ground on a whole block.
//
// Keys: Minecraft's own (see minecraft_controls), F7 passthrough on/off, F8 re-level, F9 reflection dump,
// F10 Minecraft physics (blocky collision) on/off.
#include "bridge.hpp"
#include "compositor.h"
#include "ue_call.hpp"
#include "ws.h"

#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <windows.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
		constexpr int kVoxRadius = 7;          // blocks, horizontally
		constexpr int kVoxBelow = 2, kVoxAbove = 3;  // relative to the block the feet are in
		constexpr int kVoxQueriesPerTick = 400;
		constexpr int kVoxRefreshRadius = 3;   // cells this close are re-probed (doors open and close)
		constexpr DWORD kVoxRefreshMs = 1500;
		constexpr int kDriveReadyRadius = 2;   // cells known around the player before Minecraft takes over
		constexpr float kResyncCm = 150.0f;    // HN moved its player itself (caught, cutscene): follow it
		constexpr double kYBase = 140.0;       // Minecraft y of HN height 0 (see relevel)

		WsClient g_ws;
		void *g_module = nullptr;
		bool g_enabled = true;
		int g_generation = -1;
		long long g_frame = 0;
		double g_yOffset = 0.0;
		bool g_haveOffset = false;
		bool g_mcReady = false; // Minecraft's world is loaded and its player joined (its "ready" message)
		int g_viewW = 0, g_viewH = 0;
		bool g_keyDown[256] = {};

		// driving
		bool g_driving = false;
		bool g_gridPhysics = true;   // F10: Minecraft moves the player (else HN does, with Minecraft's numbers)
		bool g_haveEye = false;
		Vec3f g_eye;                 // Minecraft's eye, Unreal coordinates
		bool g_placed = false;
		Vec3f g_lastPlaced;          // where we put HN's pawn last tick
		uint8_t g_savedMovementMode = 1;
		bool g_haveFeet = false;
		Vec3f g_feet;                // Steve's feet (Unreal cm) and his hitbox (width, height, cm), from Minecraft
		float g_steveWidth = 60.0f, g_steveHeight = 180.0f;
		float g_savedCapsule[2] = {0, 0}; // HN's own capsule (radius, half height) while Minecraft drives

		// Minecraft's controls and camera
		UObject *g_controlsPawn = nullptr; // HN's player this tick (for the controls' traces)
		std::atomic<bool> g_minecraftInput{false};  // swallow HN's copies of Minecraft's keys
		std::atomic<bool> g_minecraftScreen{false}; // a Minecraft screen is open: the mouse is Minecraft's too
		std::atomic<int> g_wheel{0};                // wheel notches since the last tick
		std::atomic<int> g_injectedE{0};            // E messages we posted ourselves (let through)
		std::atomic<int> g_injectedClicks{0};       // left clicks we posted ourselves (HN's throw while holding)
		std::atomic<bool> g_minecraftDriving{false}; // HN's movement keys are Minecraft's too (swallowed)
		std::atomic<long> g_mouseDx{0}, g_mouseDy{0};  // raw mouse counts since the last tick
		std::atomic<bool> g_rawMouseSeen{false};
		double g_lookYaw = 0, g_lookPitch = 0;         // the look while Minecraft drives (Minecraft degrees, yaw unwrapped)
		bool g_lookOwned = false;
		bool g_haveMcCamera = false;
		Vec3f g_mcCamera;
		float g_mcYaw = 0, g_mcPitch = 0, g_mcRoll = 0, g_mcFov = 70; // Minecraft's camera (Minecraft degrees, vertical FOV)
		float g_mcLookYaw = 0, g_mcLookPitch = 0; // the look that camera was rendered for
		bool g_mcFirstPerson = true;
		float g_setHfov = 0;                      // the FOV last given to HN's camera (horizontal degrees)

		// Minecraft's process (from its hello): its window is hidden, and it can be started from here
		DWORD g_mcPid = 0;
		DWORD g_nextHide = 0;
		bool g_launched = false;
		DWORD g_launchAt = 0;

		// voxels: cell -> solid, and when it was probed
		struct Cell
		{
			unsigned long long signature; // 0 empty, 1 full, otherwise a hash of the partial shape
			DWORD probed;
		};
		std::unordered_map<unsigned long long, Cell> g_cells;
		std::vector<int> g_voxOffsets; // dx, dy, dz triples, nearest first

		// The log goes through UE4SS's Output, which takes a lock that other UE4SS threads hold too. HN froze twice
		// with the game thread waiting inside Output::send from our log() (the watchdog's dump: the game thread in
		// Output.hpp under bridge.cpp, ~20 engine workers parked in UE4SS, a UE4SS thread on a mutex). The game thread
		// now only queues the line; this thread hands it to UE4SS, and may wait there instead of the game.
		std::mutex g_logLock;
		std::condition_variable g_logReady;
		std::deque<std::wstring> g_logQueue;

		void log_writer()
		{
			for (;;)
			{
				std::wstring line;
				{
					std::unique_lock lock(g_logLock);
					g_logReady.wait(lock, [] { return !g_logQueue.empty(); });
					line = std::move(g_logQueue.front());
					g_logQueue.pop_front();
				}
				Output::send<LogLevel::Normal>(STR("[NeighborCraft] {}\n"), line);
			}
		}

		void log(const std::wstring &text)
		{
			static const bool started = [] {
				std::thread(log_writer).detach();
				return true;
			}();
			(void)started;
			{
				std::lock_guard lock(g_logLock);
				if (g_logQueue.size() < 4096) // if UE4SS's output is stuck, don't grow without end
					g_logQueue.push_back(text);
			}
			g_logReady.notify_one();
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

		UObject *capsule_of(UObject *pawn)
		{
			UObject *const *c = pawn ? pawn->GetValuePtrByPropertyNameInChain<UObject *>(STR("CapsuleComponent")) : nullptr;
			return c ? *c : nullptr;
		}

		void set_capsule(UObject *pawn, float radius, float half)
		{
			UObject *capsule = capsule_of(pawn);
			if (!capsule)
				return;
			Call(capsule, STR("SetCapsuleSize")).set(STR("InRadius"), radius).set(STR("InHalfHeight"), std::max(half, radius))
				.set_bool(STR("bUpdateOverlaps"), true).invoke();
			// BP_Human sizes its capsule itself when crouching (from these): Steve's standing and sneaking heights
			if (float *h = pawn->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleHalfHeight")))
				*h = std::max(half, radius);
			if (float *h = pawn->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleCrouchHalfHeight")))
				*h = std::max(half, radius);
		}

		/// While Minecraft drives, HN's player capsule is Steve's hitbox, where Steve is. HN's capsule (radius 51.4 cm,
		/// 216 cm tall) was teleported to Steve every frame: it reached 21 cm further than his 30 cm. Minecraft stops
		/// Steve at a door leaf, but HN's capsule was by then inside it, and HN's doors are physics (a leaf on a
		/// constraint, closed by a force, reacting to hits): a closing door met the capsule standing in its swing and
		/// stayed open. The same size, Steve's collision keeps the capsule out of everything Minecraft knows.
		void steve_capsule(UObject *pawn)
		{
			UObject *capsule = capsule_of(pawn);
			if (!capsule)
				return;
			const float *radius = capsule->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleRadius"));
			const float *half = capsule->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleHalfHeight"));
			if (!radius || !half)
				return;
			if (g_savedCapsule[0] <= 0.0f)
			{
				g_savedCapsule[0] = *radius;
				g_savedCapsule[1] = *half;
			}
			const float wantRadius = g_steveWidth * 0.5f, wantHalf = g_steveHeight * 0.5f;
			if (std::abs(*radius - wantRadius) > 0.5f || std::abs(*half - std::max(wantHalf, wantRadius)) > 0.5f)
			{
				static int logged = 0;
				if (logged++ < 6)
					log(L"HN's capsule set to Steve's hitbox: radius " + std::to_wstring(int(wantRadius)) + L" cm, half height " +
						std::to_wstring(int(wantHalf)) + L" cm (was " + std::to_wstring(int(*radius)) + L", " + std::to_wstring(int(*half)) + L")");
				set_capsule(pawn, wantRadius, wantHalf);
			}
		}

		/// HN's own capsule back (Minecraft stops driving).
		void hn_capsule(UObject *pawn)
		{
			if (g_savedCapsule[0] <= 0.0f)
				return;
			set_capsule(pawn, g_savedCapsule[0], g_savedCapsule[1]);
			g_savedCapsule[0] = g_savedCapsule[1] = 0.0f;
		}

		/// HN's own movement off (MOVE_None: no walking, no gravity) while Minecraft moves the player, or back on.
		void hn_movement(UObject *pawn, bool on)
		{
			if (on)
				hn_capsule(pawn);
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

		/// Set a bool property (bitfield-safe) on an object, by name.
		bool set_bool_property(UObject *o, const TCHAR *name, bool value)
		{
			auto *p = static_cast<FBoolProperty *>(o ? o->GetPropertyByNameInChain(name) : nullptr);
			if (!p)
				return false;
			p->SetPropertyValue(p->ContainerPtrToValuePtr<void>(o), value);
			return true;
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
			// HN's player (class Human) keeps its own speeds and copies them into the movement component itself
			static Setting pawnSettings[] = {
				{STR("m_fWalkSpeed"), 431.7f}, {STR("m_fRunSpeed"), 561.2f}, {STR("m_fWalk"), 431.7f}, {STR("m_fRun"), 561.2f},
			};
			const bool sprint = on && key_held(VK_SHIFT); // HN's run key is Shift: Minecraft's sprint speed
			auto apply = [&](UObject *o, Setting *list, size_t n) {
				for (size_t i = 0; o && i < n; ++i)
				{
					Setting &s = list[i];
					float *v = o->GetValuePtrByPropertyNameInChain<float>(s.name);
					if (!v)
						continue;
					if (!s.haveSaved)
					{
						s.saved = *v;
						s.haveSaved = true;
						log(L"movement: " + std::wstring(s.name) + L" was " + std::to_wstring(*v));
					}
					float want = on ? s.minecraft : s.saved;
					if (sprint && list == settings && i == 0)
						want = 561.2f;
					// something in HN rewrote it since last tick: say so once (the movement must then be set elsewhere)
					static std::unordered_set<const TCHAR *> reported;
					if (on && *v != want && *v != s.minecraft && *v != 561.2f && s.haveSaved && *v != s.saved && reported.insert(s.name).second)
						log(L"movement: HN changed " + std::wstring(s.name) + L" to " + std::to_wstring(*v));
					*v = want;
				}
			};
			apply(movement_component(pawn), settings, std::size(settings));
			apply(pawn, pawnSettings, std::size(pawnSettings));
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
		/// Actors the world probes don't see: our own colliders for Minecraft's blocks (they aren't HN's world) and the
		/// Neighbor (he moves).
		std::vector<UObject *> g_probeIgnore;

		std::vector<UObject *> g_probeIgnoreNow; // g_probeIgnore plus this tick's physics objects near Steve

		ScriptArray ignore_list()
		{
			return {g_probeIgnoreNow.data(), int32(g_probeIgnoreNow.size()), int32(g_probeIgnoreNow.size())};
		}

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

		// ---- colliders for Minecraft's blocks ----------------------------------------------------------------
		// Every solid block placed in Minecraft becomes an invisible 1 m box in HN: the player (and the Neighbor) collide
		// with it, and it is a dynamic navmesh obstacle, so the Neighbor paths around it (HN's navmeshes regenerate
		// around dynamic modifiers only: RuntimeGeneration = DynamicModifiersOnly).

		constexpr size_t kMaxColliders = 4000;
		std::unordered_map<unsigned long long, UObject *> g_colliders; // Minecraft block -> TriggerBox actor
		UObject *g_colliderLevel = nullptr;                           // the controller they were spawned under

		/// FTransform in UE 4.20 (floats): rotation quaternion, translation, scale, each 16-byte aligned.
		struct alignas(16) Transform4
		{
			float rot[4] = {0, 0, 0, 1};
			float pos[3] = {};
			float pad0 = 0;
			float scale[3] = {1, 1, 1};
			float pad1 = 0;
		};

		/// An FName as the engine passes it: name entry index + number.
		struct NameParam
		{
			uint32_t index;
			int32 number;
		};

		NameParam name_param(const TCHAR *text)
		{
			FName n(text, FNAME_Add);
			NameParam p{};
			FNameEntryId id = n.GetComparisonIndex();
			static_assert(sizeof(id) == 4);
			std::memcpy(&p.index, &id, 4);
			p.number = n.GetNumber();
			return p;
		}

		UObject *gameplay_statics()
		{
			static UObject *g = nullptr;
			if (!g)
				g = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__GameplayStatics"));
			return g;
		}

		UObject *trigger_box_class()
		{
			static UObject *c = nullptr;
			if (!c)
				c = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.TriggerBox"));
			return c;
		}

		std::vector<UObject *> g_extraIgnore; // the Neighbor, refreshed every second

		void rebuild_probe_ignore()
		{
			g_probeIgnore = g_extraIgnore;
			for (auto &[key, actor] : g_colliders)
				g_probeIgnore.push_back(actor);
		}

		void refresh_probe_ignore(UObject *pawn)
		{
			static DWORD next = 0;
			if (GetTickCount() < next)
				return;
			next = GetTickCount() + 1000;
			// only the Neighbor, who lives as long as the level: anything that may be destroyed within the second would
			// leave a dangling pointer in the traces (what the player carries is skipped by the probe's body check)
			g_extraIgnore = find_all(L"BP_Sosed_C");
			rebuild_probe_ignore();
		}

		UObject *spawn_collider(UObject *context, const Vec3f &centre)
		{
			UObject *statics = gameplay_statics(), *cls = trigger_box_class();
			if (!statics || !cls)
				return nullptr;
			Transform4 t;
			t.pos[0] = centre.x;
			t.pos[1] = centre.y;
			t.pos[2] = centre.z;
			Call begin(statics, STR("BeginDeferredActorSpawnFromClass"));
			begin.set<UObject *>(STR("WorldContextObject"), context)
				.set<UObject *>(STR("ActorClass"), cls)
				.set(STR("SpawnTransform"), t)
				.set<uint8_t>(STR("CollisionHandlingOverride"), 1) // AlwaysSpawn
				.set<UObject *>(STR("Owner"), nullptr);
			if (!begin.invoke())
				return nullptr;
			UObject **actor = begin.param<UObject *>(STR("ReturnValue"));
			if (!actor || !*actor)
				return nullptr;
			UObject **shape = (*actor)->GetValuePtrByPropertyNameInChain<UObject *>(STR("CollisionComponent"));
			if (shape && *shape)
			{
				// a solid 1 m box instead of a trigger, and an obstacle that carves the navmesh
				set_bool_property(*shape, STR("bDynamicObstacle"), true);
				Call(*shape, STR("SetBoxExtent")).set(STR("InBoxExtent"), Vec3f{50.0f, 50.0f, 50.0f}).set_bool(STR("bUpdateOverlaps"), false).invoke();
				Call(*shape, STR("SetCollisionProfileName")).set(STR("InCollisionProfileName"), name_param(STR("BlockAll"))).invoke();
			}
			Call finish(statics, STR("FinishSpawningActor"));
			finish.set<UObject *>(STR("Actor"), *actor).set(STR("SpawnTransform"), t).invoke();
			if (shape && *shape)
			{
				// re-register with the navigation system now that it is an obstacle
				Call(*shape, STR("SetCanEverAffectNavigation")).set_bool(STR("bRelevant"), false).invoke();
				Call(*shape, STR("SetCanEverAffectNavigation")).set_bool(STR("bRelevant"), true).invoke();
			}
			return *actor;
		}

		void destroy_colliders(bool alive)
		{
			if (alive)
				for (auto &[key, actor] : g_colliders)
					Call(actor, STR("K2_DestroyActor")).invoke();
			g_colliders.clear();
			g_probeIgnore.clear();
		}

		/// {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]} from Minecraft: solid blocks placed and removed.
		void apply_blocks(UObject *context, const std::vector<int> &set, const std::vector<int> &clear)
		{
			for (size_t i = 0; i + 2 < clear.size(); i += 3)
			{
				auto it = g_colliders.find(cell_key(clear[i], clear[i + 1], clear[i + 2]));
				if (it == g_colliders.end())
					continue;
				Call(it->second, STR("K2_DestroyActor")).invoke();
				g_colliders.erase(it);
			}
			for (size_t i = 0; i + 2 < set.size() && g_colliders.size() < kMaxColliders; i += 3)
			{
				const unsigned long long key = cell_key(set[i], set[i + 1], set[i + 2]);
				if (g_colliders.count(key))
					continue;
				if (UObject *a = spawn_collider(context, to_ue(set[i] + 0.5, set[i + 1] + 0.5, set[i + 2] + 0.5)))
					g_colliders[key] = a;
			}
			rebuild_probe_ignore();
		}

		// ---- HN's world as host blocks ------------------------------------------------------------------------
		// Each block around the player is probed against what blocks a player in HN (the "Pawn" collision profile:
		// walls, floors, furniture, closed doors; not triggers, not the Neighbor). Empty blocks stay air, solid ones
		// become host blocks, and blocks that HN geometry only crosses are subdivided down to 1/8 block (12.5 cm): an
		// 8x8x8 mask that Minecraft turns into the block's collision shape, so walls and floors sit where they really are.

		int g_probeStats[3] = {};

		/// Whether HN's world blocks a player anywhere in a box (centre and half size in cm).
		bool blocks_player(UObject *self, const Vec3f &centre, const Vec3f &half)
		{
			Call c(kismet(), STR("BoxTraceSingleByProfile"));
			if (!c)
			{
				static bool warned = false;
				if (!warned)
					log(L"BoxTraceSingleByProfile isn't available: HN's world can't be probed");
				warned = true;
				return false;
			}
			static const NameParam pawnProfile = name_param(STR("Pawn"));
			c.set<UObject *>(STR("WorldContextObject"), self)
				.set(STR("Start"), Vec3f{centre.x, centre.y, centre.z + 0.5f})
				.set(STR("End"), Vec3f{centre.x, centre.y, centre.z - 0.5f})
				.set(STR("HalfSize"), half)
				.set(STR("Orientation"), Rot3f{})
				.set(STR("ProfileName"), pawnProfile)
				.set(STR("ActorsToIgnore"), ignore_list())
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			return ok && *ok;
		}

		struct BlockProbe
		{
			UObject *self;
			Vec3f origin;      // the block's minimum corner, Unreal cm
			uint8_t bits[64];  // bit z of byte x * 8 + y: sub-cell (x, y, z), Minecraft axes, 8 per block
			int queries;
		};

		/// Sub-cube (x, y, z) of size n (in 1/8 block, Minecraft axes) as an Unreal box: Minecraft x -> Unreal x,
		/// Minecraft z -> Unreal y, Minecraft y -> Unreal z.
		bool probe_node(BlockProbe &p, int x, int y, int z, int n)
		{
			const float s = 12.5f;
			const Vec3f c{p.origin.x + (x + n * 0.5f) * s, p.origin.y + (z + n * 0.5f) * s, p.origin.z + (y + n * 0.5f) * s};
			const float h = n * s * 0.5f - 0.5f; // a hair inside, so a surface exactly on the edge isn't counted twice
			++p.queries;
			return blocks_player(p.self, c, {h, h, h});
		}

		void fill_node(BlockProbe &p, int x, int y, int z, int n)
		{
			for (int i = x; i < x + n; ++i)
				for (int j = y; j < y + n; ++j)
					for (int k = z; k < z + n; ++k)
						p.bits[i * 8 + j] |= uint8_t(1u << k);
		}

		void subdivide(BlockProbe &p, int x, int y, int z, int n)
		{
			if (!probe_node(p, x, y, z, n))
				return;
			if (n == 1)
			{
				fill_node(p, x, y, z, 1);
				return;
			}
			const int h = n / 2;
			for (int i = 0; i < 8; ++i)
				subdivide(p, x + (i & 1) * h, y + (i >> 1 & 1) * h, z + (i >> 2 & 1) * h, h);
		}

		/// 0: nothing of HN's in block (x, y, z); 1: all of it; 2: part of it (mask in `hex`).
		int probe_block(UObject *pawn, int x, int y, int z, std::string &hex, int &queries)
		{
			BlockProbe p{pawn, to_ue(x, y, z), {}, 0};
			int kind = 0;
			if (probe_node(p, 0, 0, 0, 8))
			{
				// solid through and through (inside a wall or the ground): its corners and middle are all inside
				bool full = true;
				for (int i = 0; i < 9 && full; ++i)
				{
					const int cx = i == 8 ? 4 : (i & 1) * 7, cy = i == 8 ? 4 : (i >> 1 & 1) * 7, cz = i == 8 ? 4 : (i >> 2 & 1) * 7;
					full = probe_node(p, cx, cy, cz, 1);
				}
				if (full)
					kind = 1;
				else
				{
					for (int i = 0; i < 8; ++i)
						subdivide(p, (i & 1) * 4, (i >> 1 & 1) * 4, (i >> 2 & 1) * 4, 4);
					int set = 0;
					for (uint8_t b : p.bits)
						set += std::popcount(b);
					kind = set == 0 ? 0 : set == 512 ? 1 : 2;
				}
			}
			queries += p.queries;
			if (kind == 2)
			{
				static const char digits[] = "0123456789abcdef";
				hex.resize(128);
				for (int i = 0; i < 64; ++i)
				{
					hex[i * 2] = digits[p.bits[i] >> 4];
					hex[i * 2 + 1] = digits[p.bits[i] & 15];
				}
			}
			return kind;
		}

		/// A line trace against what blocks a player in HN (the "Pawn" profile), against the rendered triangles
		/// (complex collision: simple collision shapes can sit above what is seen); the impact point on a hit.
		bool ray_blocks_player(UObject *self, const Vec3f &a, const Vec3f &b, Vec3f &hit)
		{
			Call c(kismet(), STR("LineTraceSingleByProfile"));
			if (!c)
				return false;
			static const NameParam pawnProfile = name_param(STR("Pawn"));
			c.set<UObject *>(STR("WorldContextObject"), self)
				.set(STR("Start"), a)
				.set(STR("End"), b)
				.set(STR("ProfileName"), pawnProfile)
				.set(STR("ActorsToIgnore"), ignore_list())
				.set_bool(STR("bTraceComplex"), true)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			return ok && *ok && impact_point(c, hit);
		}

		// Floors come from rays, not boxes: box overlaps only see the surface of a landscape heightfield (not the
		// ground under it) and miss a surface lying exactly on a block boundary. Each 1x1 block column gets 4x4 rays
		// straight down; every surface a ray meets (each storey: the ray goes on below what it hit) is kept at its exact
		// height (a thin slab under it in Minecraft), so stairs and door sills are their real steps.
		struct BlockBits
		{
			uint8_t box[64] = {};     // from the box probe (walls, furniture)
			float floor[64] = {};     // floor rays: highest surface in the block (0..1], per 1/8 x 1/8 column (x * 8 + z)
			float ceiling[16] = {};   // ceiling rays: lowest underside in the block [0..1), per 1/4 x 1/4 column, 0 none
			bool boxKnown = false;
		};
		std::unordered_map<unsigned long long, BlockBits> g_bits;
		std::unordered_map<unsigned long long, std::array<uint8_t, 64>> g_dynBits; // moving parts' sub-cells, per block
		/// A column's rays: when they were cast, and the Minecraft heights [lo, hi) they covered. The cache used to know
		/// only (x, z): a column first probed while Steve stood high up kept its range for good, and on Act 1's road,
		/// which falls ~12 blocks towards the Neighbor's house, the road surface ahead lay below every column's range.
		/// The recorder showed Steve running down it into no collision at all (0 boxes around him) and falling until
		/// HN's rescue put him back. A column is probed again (for the heights Steve now needs) when they aren't
		/// covered.
		struct Column
		{
			DWORD probed;
			int lo, hi;
		};
		std::unordered_map<unsigned long long, Column> g_columns; // column (x, z)
		std::unordered_set<unsigned long long> g_dirty;          // blocks whose shape may have changed, to send

		int cell_x(unsigned long long key)
		{
			return int(int64_t(key << 1) >> 43);
		}

		int cell_y(unsigned long long key)
		{
			return int(int64_t(key << 22) >> 43);
		}

		int cell_z(unsigned long long key)
		{
			return int(int64_t(key << 43) >> 43);
		}

		/// Cast the rays of column (x, z) between Minecraft heights yLo and yHi: 8x8 straight down (each surface a ray
		/// meets; it goes on below what it hit, so every storey), then 4x4 straight up from just above the lowest floor
		/// (ceiling undersides; starting above the ground, a ray can't catch the ground from below). Rewrites the floor
		/// and ceiling heights of the column's blocks.
		int probe_column(UObject *pawn, int x, int z, int yLo, int yHi)
		{
			for (int y = yLo; y < yHi; ++y)
			{
				const unsigned long long key = cell_key(x, y, z);
				BlockBits &b = g_bits[key];
				std::fill(std::begin(b.floor), std::end(b.floor), 0.0f);
				std::fill(std::begin(b.ceiling), std::end(b.ceiling), 0.0f);
				g_dirty.insert(key);
			}
			int queries = 0;
			double lowest[16];
			std::fill(std::begin(lowest), std::end(lowest), double(yLo));
			for (int i = 0; i < 8; ++i)
				for (int k = 0; k < 8; ++k)
				{
					const double mx = x + (i + 0.5) / 8.0, mz = z + (k + 0.5) / 8.0;
					Vec3f from = to_ue(mx, yHi, mz);
					const Vec3f to = to_ue(mx, yLo, mz);
					for (int surfaces = 0; surfaces < 6; ++surfaces)
					{
						Vec3f hit;
						++queries;
						if (!ray_blocks_player(pawn, from, to, hit))
							break;
						double hx, hy, hz;
						to_mc(hit, hx, hy, hz);
						// a surface on a block boundary belongs to the block below (its top face)
						const int by = int(std::floor(hy - 1e-4));
						if (by >= yLo && by < yHi)
						{
							float &top = g_bits[cell_key(x, by, z)].floor[i * 8 + k];
							top = std::max(top, float(hy - by));
						}
						double &low = lowest[(i / 2) * 4 + k / 2];
						low = surfaces == 0 ? hy : std::min(low, hy);
						from = {hit.x, hit.y, hit.z - 3.0f}; // on below this surface: the storey under it
						if (from.z <= to.z)
							break;
					}
				}
			for (int i = 0; i < 4; ++i)
				for (int k = 0; k < 4; ++k)
				{
					const double mx = x + (i + 0.5) / 4.0, mz = z + (k + 0.5) / 4.0;
					Vec3f from = to_ue(mx, lowest[i * 4 + k] + 0.02, mz);
					const Vec3f to = to_ue(mx, yHi, mz);
					for (int surfaces = 0; surfaces < 4 && from.z < to.z; ++surfaces)
					{
						Vec3f hit;
						++queries;
						if (!ray_blocks_player(pawn, from, to, hit))
							break;
						double hx, hy, hz;
						to_mc(hit, hx, hy, hz);
						const int by = int(std::floor(hy + 1e-4)); // on a boundary: the block above (its bottom face)
						if (by >= yLo && by < yHi)
						{
							float &bottom = g_bits[cell_key(x, by, z)].ceiling[i * 4 + k];
							const float local = std::max(float(hy - by), 0.0005f);
							bottom = bottom > 0.0f ? std::min(bottom, local) : local;
						}
						from = {hit.x, hit.y, hit.z + 3.0f}; // on above it: the storey over it
					}
				}
			return queries;
		}

		/// The block's shape: the box probe's cells, corrected by the rays. 0 empty, 1 full, 2 partial (mask in `hex`,
		/// floor heights in `floor`: 64 thousandths of a block, ceilings in `ceiling`: 16; "" when there are none).
		///  - Where a ray measured a floor, the box cells holding it (and the one above, where simple collision can sit a
		///    little over the seen surface) go: the box probe rounds surfaces up to its 12.5 cm cells, so Steve floated.
		///  - Step-sized obstacles (a neighbouring floor at most 0.6 blocks higher: stairs, curbs, sills) are floor heights
		///    only: box cells below that neighbouring floor go, so treads and curbs have no lips to catch on. Taller things
		///    (walls, tables) keep their box shape.
		///  - Where a ray measured a ceiling, the box cell holding it goes (rounded down, it took away clearance and made
		///    Minecraft crouch or crawl).
		///  - Floors within a few cm of a higher neighbouring floor are raised to it. HN's floors are not exactly level:
		///    neighbouring 1/8 columns measured 1-3 cm apart (seams, rugs, thresholds), and each difference was a ledge
		///    for Minecraft. The recorder caught sprints stopped dead by them (ledges of 0.7-2.4 cm under every step
		///    candidate, so the step-up failed and the horizontal collision zeroed Steve's speed). Real steps (over 5 cm)
		///    stay as they are.
		int block_shape(unsigned long long key, const BlockBits &b, std::string &hex, std::string &floor, std::string &ceiling)
		{
			// the floors of this block and of its 8 horizontal neighbours (a riser on a block border has its upper tread
			// in the next block)
			const int bx = cell_x(key), by = cell_y(key), bz = cell_z(key);
			const BlockBits *around[3][3] = {};
			for (int dx = -1; dx <= 1; ++dx)
				for (int dz = -1; dz <= 1; ++dz)
				{
					const auto it = g_bits.find(cell_key(bx + dx, by, bz + dz));
					around[dx + 1][dz + 1] = it == g_bits.end() ? nullptr : &it->second;
				}
			auto floor_at = [&](int sx, int sz) -> float {
				const int ox = sx < 0 ? -1 : sx > 7 ? 1 : 0, oz = sz < 0 ? -1 : sz > 7 ? 1 : 0;
				const BlockBits *n = around[ox + 1][oz + 1];
				return n ? n->floor[(sx - ox * 8) * 8 + (sz - oz * 8)] : 0.0f;
			};
			const float kLevel = 0.05f; // a floor this close under a neighbouring one is the same floor
			float level[64];
			for (int sx = 0; sx < 8; ++sx)
				for (int sz = 0; sz < 8; ++sz)
				{
					const float own = b.floor[sx * 8 + sz];
					float to = own;
					if (own > 0.0f)
						for (int dx = -1; dx <= 1; ++dx)
							for (int dz = -1; dz <= 1; ++dz)
								if (const float n = floor_at(sx + dx, sz + dz); n > to && n - own <= kLevel)
									to = n;
					level[sx * 8 + sz] = to;
				}
			uint8_t bits[64];
			std::memcpy(bits, b.box, 64);
			for (int sx = 0; sx < 8; ++sx)
				for (int sz = 0; sz < 8; ++sz)
				{
					const float own = level[sx * 8 + sz];
					float highest = own;
					for (int dx = -1; dx <= 1; ++dx)
						for (int dz = -1; dz <= 1; ++dz)
							highest = std::max(highest, floor_at(sx + dx, sz + dz));
					float clearBelow = own > 0.0f ? std::min(own + 0.125f, 1.0f) : 0.0f;
					if (highest > 0.0f && highest - own <= 0.6f)
						clearBelow = std::max(clearBelow, highest);
					for (int layer = 0; layer < 8 && layer / 8.0f < clearBelow - 1e-4f; ++layer)
						bits[sx * 8 + layer] &= uint8_t(~(1u << sz));
				}
			for (int c = 0; c < 16; ++c)
				if (b.ceiling[c] > 0.0f)
				{
					const int layer = std::clamp(int(b.ceiling[c] * 8.0f), 0, 7);
					for (int sx = (c >> 2) * 2; sx < (c >> 2) * 2 + 2; ++sx)
						bits[sx * 8 + layer] &= uint8_t(~(3u << ((c & 3) * 2)));
				}
			if (const auto dyn = g_dynBits.find(key); dyn != g_dynBits.end())
				for (int i = 0; i < 64; ++i)
					bits[i] |= dyn->second[i];
			int set = 0;
			for (int i = 0; i < 64; ++i)
				set += std::popcount(bits[i]);
			if (set == 512)
				return 1;
			auto heights = [](const float *v, int n, std::string &out) {
				out.clear();
				bool any = false;
				for (int i = 0; i < n; ++i)
				{
					const int h = int(std::lround(v[i] * 1000.0f));
					any |= h > 0;
					out += (i ? "," : "") + std::to_string(h);
				}
				if (!any)
					out.clear();
				return any;
			};
			const bool anyFloor = heights(level, 64, floor), anyCeiling = heights(b.ceiling, 16, ceiling);
			if (set == 0 && !anyFloor && !anyCeiling)
				return 0;
			static const char digits[] = "0123456789abcdef";
			hex.resize(128);
			for (int i = 0; i < 64; ++i)
			{
				hex[i * 2] = digits[bits[i] >> 4];
				hex[i * 2 + 1] = digits[bits[i] & 15];
			}
			return 2;
		}

		/// F11: what Minecraft gets around the player at chest height, 1/8 block per character ('#' solid, '.' air),
		/// for checking doorways (written next to the mod's DLL).
		void collision_map(const Vec3f &eye);

		/// Probe the world around the player (column rays for floors, boxes for walls and furniture) for what is
		/// unknown, or near and stale, nearest first, and send the blocks whose shape changed. `eye`: the player's eyes
		/// (cm); Steve's own body is never HN's world (HN's arms and held things). Returns whether everything within
		/// kDriveReadyRadius is known.
		bool sample_world(UObject *pawn, const Vec3f &eye)
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
			double ex, ey, ez;
			to_mc(eye, ex, ey, ez);
			const int px = int(std::floor(ex)), py = int(std::floor(ey - 1.62)), pz = int(std::floor(ez));
			const DWORD now = GetTickCount();
			int queries = 0;
			bool ready = true;
			// floors first: one column per (dx, dz), in the same nearest-first order
			for (size_t i = 0; i < g_voxOffsets.size() && queries < kVoxQueriesPerTick; i += 3)
			{
				const int dx = g_voxOffsets[i], dz = g_voxOffsets[i + 2];
				if (g_voxOffsets[i + 1] != 0)
					continue;
				const unsigned long long col = cell_key(px + dx, 0, pz + dz);
				auto it = g_columns.find(col);
				const int needLo = py - kVoxBelow, needHi = py + kVoxAbove + 1;
				const bool covered = it != g_columns.end() && it->second.lo <= needLo && it->second.hi >= needHi;
				const bool close = std::abs(dx) <= kVoxRefreshRadius && std::abs(dz) <= kVoxRefreshRadius;
				if (covered && !(close && now - it->second.probed > kVoxRefreshMs * 3))
					continue;
				if (!covered && std::abs(dx) <= kDriveReadyRadius && std::abs(dz) <= kDriveReadyRadius)
					ready = false;
				queries += probe_column(pawn, px + dx, pz + dz, needLo, needHi);
				// what is known now: these heights, joined to the earlier ones when they touch
				if (it != g_columns.end() && it->second.lo <= needHi && it->second.hi >= needLo)
					g_columns[col] = {now, std::min(it->second.lo, needLo), std::max(it->second.hi, needHi)};
				else
					g_columns[col] = {now, needLo, needHi};
				// the neighbouring columns' shapes depend on these floors too (steps across block borders)
				for (int nx = -1; nx <= 1; ++nx)
					for (int nz = -1; nz <= 1; ++nz)
						for (int y = py - kVoxBelow; y <= py + kVoxAbove; ++y)
							if ((nx || nz) && g_bits.count(cell_key(px + dx + nx, y, pz + dz + nz)))
								g_dirty.insert(cell_key(px + dx + nx, y, pz + dz + nz));
			}
			// then walls and furniture, block by block
			std::string hex;
			for (size_t i = 0; i < g_voxOffsets.size() && queries < kVoxQueriesPerTick; i += 3)
			{
				const int dx = g_voxOffsets[i], dy = g_voxOffsets[i + 1], dz = g_voxOffsets[i + 2];
				const int x = px + dx, y = py + dy, z = pz + dz;
				const unsigned long long key = cell_key(x, y, z);
				const auto known = g_bits.find(key);
				const bool boxKnown = known != g_bits.end() && known->second.boxKnown;
				const auto it = g_cells.find(key);
				const bool close = std::abs(dx) <= kVoxRefreshRadius && std::abs(dz) <= kVoxRefreshRadius;
				if (boxKnown && !(close && it != g_cells.end() && now - it->second.probed > kVoxRefreshMs))
					continue;
				if (!boxKnown && std::abs(dx) <= kDriveReadyRadius && std::abs(dz) <= kDriveReadyRadius)
					ready = false;
				BlockBits &b = g_bits[key];
				std::fill(std::begin(b.box), std::end(b.box), uint8_t(0));
				{
					hex.clear();
					const int kind = probe_block(pawn, x, y, z, hex, queries);
					if (kind == 1)
						std::fill(std::begin(b.box), std::end(b.box), uint8_t(0xFF));
					else if (kind == 2)
						for (int j = 0; j < 64; ++j)
							b.box[j] = uint8_t(std::stoi(hex.substr(j * 2, 2), nullptr, 16));
				}
				b.boxKnown = true;
				g_dirty.insert(key);
				auto &cell = g_cells[key];
				cell.probed = now;
				queries += 1;
			}
			// send the blocks whose shape changed
			std::string cells, masks, floors, ceilings, floor, ceiling;
			for (unsigned long long key : g_dirty)
			{
				const auto bit = g_bits.find(key);
				if (bit == g_bits.end())
					continue;
				const int kind = block_shape(key, bit->second, hex, floor, ceiling);
				auto [cell, added] = g_cells.try_emplace(key, Cell{~0ull, 0});
				const unsigned long long sig = kind == 2 ? ((std::hash<std::string>()(hex) ^ std::hash<std::string>()(floor) * 31 ^
															   std::hash<std::string>()(ceiling) * 131) | 3)
														 : unsigned(kind);
				if (!added && cell->second.signature == sig)
					continue;
				cell->second.signature = sig;
				++g_probeStats[kind];
				char entry[64];
				snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", cells.empty() ? "" : ",", cell_x(key), cell_y(key), cell_z(key), kind);
				cells += entry;
				if (kind == 2)
				{
					const bool first = masks.empty();
					masks += (first ? "\"" : ",\"") + hex + "\"";
					floors += (first ? "\"" : ",\"") + floor + "\"";
					ceilings += (first ? "\"" : ",\"") + ceiling + "\"";
				}
			}
			g_dirty.clear();
			if (!cells.empty())
				g_ws.send("{\"t\":\"shape\",\"c\":[" + cells + "],\"m\":[" + masks + "],\"f\":[" + floors + "],\"g\":[" + ceilings + "]}");
			static DWORD nextStats = 0;
			if (now > nextStats)
			{
				nextStats = now + 5000;
				if (g_probeStats[0] + g_probeStats[1] + g_probeStats[2] > 0)
					log(L"world: sent " + std::to_wstring(g_probeStats[0]) + L" empty, " + std::to_wstring(g_probeStats[1]) + L" full, " +
						std::to_wstring(g_probeStats[2]) + L" partial blocks (last 5 s)");
				g_probeStats[0] = g_probeStats[1] = g_probeStats[2] = 0;
			}
			return ready;
		}

		/// Put the ground under the player on a whole block, and start the barriers over.
		void relevel(UObject *pawn, const Vec3f &p)
		{
			float groundZ = 0.0f;
			if (!ground_below(pawn, p, groundZ))
				return;
			const double metres = groundZ / 100.0;
			// HN spans more height than Minecraft's world (-64..320): its nightmare rooms are ~140 m under the house, so
			// HN height 0 sits at Minecraft y ~140 (HN -200 m .. +180 m fit); the fraction puts the ground here on a
			// whole block, and the shift is constant so the yard and the nightmare rooms keep their relative heights
			g_yOffset = kYBase + std::round(metres) - metres;
			g_haveOffset = true;
			g_cells.clear();
			g_bits.clear();
			g_columns.clear();
			g_dirty.clear();
			g_driving = false;
			g_ws.send("{\"t\":\"clear\"}");
			// the blocks' colliders move with the new offset: rebuild them from Minecraft's blocks
			destroy_colliders(true);
			g_ws.send("{\"t\":\"blocksync\",\"r\":64}");
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

		/// The first `n` numbers of the array under `key`.
		bool json_numbers(const std::string &m, const char *key, double *v, int n)
		{
			size_t at = m.find(std::string("\"") + key + "\":[");
			if (at == std::string::npos)
				return false;
			const char *p = m.c_str() + at + std::strlen(key) + 4;
			for (int i = 0; i < n; ++i)
			{
				char *end = nullptr;
				v[i] = std::strtod(p, &end);
				if (end == p)
					return false;
				p = end + (*end == ',' ? 1 : 0);
			}
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

		/// All the integers of the array under `key` (appended to `out`).
		void json_ints(const std::string &m, const char *key, std::vector<int> &out)
		{
			const size_t at = m.find(std::string("\"") + key + "\":[");
			if (at == std::string::npos)
				return;
			const char *p = m.c_str() + at + std::strlen(key) + 4;
			while (*p && *p != ']')
			{
				char *end = nullptr;
				const long v = std::strtol(p, &end, 10);
				if (end == p)
					break;
				out.push_back(int(v));
				p = end + (*end == ',' ? 1 : 0);
			}
		}

		std::vector<int> g_pendingSet, g_pendingClear; // block changes from Minecraft, applied on the next tick
		struct Punch
		{
			Vec3f at, dir;
			float strength, damage;
			bool sprint;
		};
		std::vector<Punch> g_punches; // Minecraft's attacks that hit HN's world, applied on the next tick

		void handle(const std::string &m)
		{
			if (m.find("\"t\":\"ready\"") != std::string::npos)
			{
				// a (new) world: everything sent before it was dropped, so send HN's world again from scratch
				g_mcReady = true;
				g_cells.clear();
				g_bits.clear();
				g_columns.clear();
				g_dirty.clear();
				g_driving = false;
				log(L"Minecraft's world is ready");
			}
			else if (m.find("\"t\":\"blocks\"") != std::string::npos)
			{
				json_ints(m, "set", g_pendingSet);
				json_ints(m, "clear", g_pendingClear);
			}
			else if (m.find("\"t\":\"mcpos\"") != std::string::npos)
			{
				double eye[3], camera[3], rot[3], fov = 0;
				if (json_vec3(m, "eye", eye))
				{
					g_eye = to_ue(eye[0], eye[1], eye[2]);
					g_haveEye = true;
				}
				double feet[3], size[2];
				if (json_vec3(m, "feet", feet) && json_numbers(m, "size", size, 2))
				{
					g_feet = to_ue(feet[0], feet[1], feet[2]);
					g_steveWidth = float(size[0] * 100.0);
					g_steveHeight = float(size[1] * 100.0);
					g_haveFeet = true;
				}
				if (json_vec3(m, "cam", camera) && json_numbers(m, "rot", rot, 3) && json_number(m, "fov", fov))
				{
					g_mcCamera = to_ue(camera[0], camera[1], camera[2]);
					g_mcYaw = float(rot[0]);
					g_mcPitch = float(rot[1]);
					g_mcRoll = float(rot[2]);
					double look[2];
					if (json_numbers(m, "look", look, 2))
					{
						g_mcLookYaw = float(look[0]);
						g_mcLookPitch = float(look[1]);
					}
					g_mcFirstPerson = m.find("\"fp\":false") == std::string::npos;
					g_mcFov = float(fov);
					g_haveMcCamera = true;
				}
			}
			else if (m.find("\"t\":\"punch\"") != std::string::npos)
			{
				double at[3], dir[3], strength = 1.0, damage = 1.0;
				if (json_vec3(m, "p", at) && json_vec3(m, "d", dir))
				{
					json_number(m, "s", strength);
					json_number(m, "dmg", damage);
					// Minecraft x, y, z -> Unreal x, z, y (a direction: no offset)
					g_punches.push_back({to_ue(at[0], at[1], at[2]), Vec3f{float(dir[0]), float(dir[2]), float(dir[1])}, float(strength), float(damage),
						m.find("\"sprint\":true") != std::string::npos});
				}
			}
			else if (m.find("\"t\":\"screen\"") != std::string::npos)
				g_minecraftScreen = m.find("\"open\":true") != std::string::npos;
			else if (m.find("\"t\":\"hello\"") != std::string::npos)
			{
				double pid = 0;
				if (json_number(m, "pid", pid))
					g_mcPid = DWORD(pid);
				g_nextHide = 0;
				log(L"Minecraft says hello (pid " + std::to_wstring(g_mcPid) + L")");
			}
		}

		/// Actual gameplay: not HN's main menu (the Start map) and not paused. Minecraft shows only then.
		bool in_gameplay(UObject *pc)
		{
			static UObject *lastPc = nullptr;
			static bool menu = false;
			static UObject *pauseMenu = nullptr;
			static DWORD nextFind = 0;
			if (pc != lastPc)
			{
				lastPc = pc;
				menu = pc->GetFullName().find(L"/Maps/Start.") != StringType::npos;
				pauseMenu = nullptr; // a new level may come with a new pause menu: find it again
				nextFind = 0;
			}
			if (menu)
				return false;
			Call paused(pc, STR("IsPaused"));
			if (paused.invoke())
				if (const uint8_t *p = paused.param<uint8_t>(STR("ReturnValue")); p && *p)
					return false;
			// HN's own pause menu (BPW_MenuPause): HN doesn't use the engine's pause for it; paused while it shows
			if (!pauseMenu && GetTickCount() > nextFind)
			{
				nextFind = GetTickCount() + 3000;
				const std::vector<UObject *> menus = find_all(L"BPW_MenuPause_C");
				pauseMenu = menus.empty() ? nullptr : menus[0];
				if (pauseMenu)
					log(L"pause menu: " + pauseMenu->GetFullName());
			}
			if (pauseMenu)
			{
				Call shown(pauseMenu, STR("IsInViewport"));
				if (shown.invoke())
					if (const uint8_t *v = shown.param<uint8_t>(STR("ReturnValue")); v && *v)
						return false;
			}
			// HN's cutscenes (Act 1 opens with one): not gameplay. Minecraft counted it as play: its HUD and hand
			// showed over the cutscene, its starter kit filled the hotbar item by item there, and it took over moving
			// HN's player while the cutscene was moving it (the takeover started again when the cutscene put the
			// player down). HN's player says itself when one plays.
			bool cutscene = false;
			if (UObject *pawn = pawn_of(pc))
			{
				Call playing(pawn, STR("IsPlayCutScene"));
				if (playing.invoke())
					if (const uint8_t *v = playing.param<uint8_t>(STR("ReturnValue")); v && *v)
						cutscene = true;
			}
			static bool wasCutscene = false;
			if (cutscene != wasCutscene)
			{
				wasCutscene = cutscene;
				log(cutscene ? L"cutscene: Minecraft waits" : L"cutscene over: gameplay");
			}
			return !cutscene;
		}

		/// HN's first-person arms and body are hidden while Minecraft's hand is shown, and come back otherwise.
		void set_player_hidden(UObject *pawn, bool hidden)
		{
			static UObject *lastPawn = nullptr;
			static bool lastHidden = false;
			static DWORD next = 0;
			if (!pawn || (pawn == lastPawn && hidden == lastHidden && GetTickCount() < next))
				return;
			lastPawn = pawn;
			lastHidden = hidden;
			next = GetTickCount() + 1000; // HN may show it again: re-apply now and then
			Call(pawn, STR("SetActorHiddenInGame")).set_bool(STR("bNewHidden"), hidden).invoke();
		}

		void forward_key(int vk, const char *key)
		{
			if (const int e = edge(vk))
				sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", key, e > 0 ? "true" : "false");
		}

		// ---- Minecraft's controls inside HN's window ---------------------------------------------------------
		// HN has the focus, so its window procedure is subclassed: the keys Minecraft owns never reach HN (E is
		// Minecraft's inventory, Q drop, F swap hands, F5 perspective, 1-9 and the wheel the hotbar), the wheel is
		// counted for Minecraft, and the right mouse button is also HN's interact key (E): doors and objects open.
		// Minecraft reads the held keys with GetAsyncKeyState (the physical state), so swallowing them here is fine.

		HWND g_hnWindow = nullptr;
		WNDPROC g_hnProc = nullptr;

		bool minecraft_key(WPARAM vk)
		{
			return vk == 'E' || vk == 'Q' || vk == 'F' || vk == VK_F5 || (vk >= '1' && vk <= '9');
		}

		LRESULT CALLBACK hn_window_proc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
		{
			if (g_minecraftInput)
			{
				switch (msg)
				{
				case WM_INPUT:
				{
					RAWINPUT input;
					UINT size = sizeof(input);
					if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
						input.header.dwType == RIM_TYPEMOUSE && !(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
					{
						g_mouseDx += input.data.mouse.lLastX;
						g_mouseDy += input.data.mouse.lLastY;
						g_rawMouseSeen = true;
					}
					break;
				}
				case WM_MOUSEWHEEL:
					g_wheel += GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
					return 0;
				case WM_LBUTTONDOWN:
				case WM_LBUTTONUP:
					if (g_injectedClicks > 0)
					{
						--g_injectedClicks;
						break;
					}
					return 0;
				case WM_LBUTTONDBLCLK:
				case WM_MBUTTONDOWN:
				case WM_MBUTTONUP:
					return 0; // Minecraft's attack and pick block
				case WM_RBUTTONDOWN:
				case WM_RBUTTONUP:
					return 0; // Minecraft's use, or HN's interact on HN's objects (decided each tick: right_click)
				case WM_KEYDOWN:
				case WM_KEYUP:
					if (wparam == 'E' && g_injectedE > 0)
					{
						--g_injectedE;
						break;
					}
					if (minecraft_key(wparam) || (g_minecraftScreen && wparam == VK_ESCAPE))
						return 0;
					// while Minecraft walks the player, HN never sees its movement keys (no HN crouch, run or jump)
					if (g_minecraftDriving && (wparam == 'W' || wparam == 'A' || wparam == 'S' || wparam == 'D' || wparam == VK_SPACE ||
												  wparam == VK_SHIFT || wparam == VK_CONTROL || wparam == 'C'))
						return 0;
					break;
				default:
					break;
				}
			}
			return CallWindowProcW(g_hnProc, window, msg, wparam, lparam);
		}

		void hook_hn_window()
		{
			if (g_hnProc)
				return;
			struct Search
			{
				DWORD pid;
				HWND found;
			} s{GetCurrentProcessId(), nullptr};
			EnumWindows(
				[](HWND h, LPARAM p) -> BOOL {
					auto *s = reinterpret_cast<Search *>(p);
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
				reinterpret_cast<LPARAM>(&s));
			if (!s.found)
				return;
			g_hnWindow = s.found;
			g_hnProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hnWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hn_window_proc)));
			log(L"HN's window hooked for Minecraft's controls");
		}

		void unhook_hn_window()
		{
			if (g_hnProc && g_hnWindow)
				SetWindowLongPtrW(g_hnWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_hnProc));
			g_hnProc = nullptr;
		}

		std::vector<UObject *> g_interactiveActors; // this level's actors with HN's InteractiveComponent, and doors
		bool g_debugView = false;                   // F12: collision recorder view, and what the interaction ray finds

		bool class_contains(UObject *o, const wchar_t *part)
		{
			for (UStruct *s = o ? o->GetClassPrivate() : nullptr; s; s = s->GetSuperStruct())
				if (s->GetName().find(part) != StringType::npos)
					return true;
			return false;
		}

		/// One of HN's interactive actors: carries the InteractiveComponent, or is a door.
		bool interactive_actor(UObject *actor)
		{
			static UObject *interactive = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/HelloNeighbor.InteractiveComponent"));
			if (!actor)
				return false;
			if (interactive)
			{
				Call has(actor, STR("GetComponentByClass"));
				has.set<UObject *>(STR("ComponentClass"), interactive).invoke();
				if (const UObject *const *component = has.param<UObject *>(STR("ReturnValue")); component && *component)
					return true;
			}
			return class_contains(actor, L"Door");
		}

		bool has_component(UObject *actor, const TCHAR *path)
		{
			UObject *cls = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, path);
			if (!actor || !cls)
				return false;
			Call has(actor, STR("GetComponentByClass"));
			has.set<UObject *>(STR("ComponentClass"), cls).invoke();
			const UObject *const *component = has.param<UObject *>(STR("ReturnValue"));
			return component && *component;
		}

		/// HN's own answer for its FlatActors (cabinets, drawers, fridges, TVs, switches, panels, lamps; doors too):
		/// their blueprints implement OnIsActionComponent, which says whether a part (the component looked at) can be
		/// used with HN's action key (BP_Stand_tv compares it with its four drawer parts, a switch with its lever).
		/// 1 yes, 0 no, -1 the actor has no such function.
		int flat_actionable(UObject *actor, UObject *component)
		{
			Call c(actor, STR("OnIsActionComponent"));
			if (!c || !component)
				return -1;
			uint8_t *result = nullptr;
			bool given = false;
			for (FProperty *p : TFieldRange<FProperty>(c.function(), EFieldIterationFlags::IncludeDeprecated))
			{
				const bool out = p->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm);
				if (!given && !out && CastField<FObjectProperty>(p))
				{
					*reinterpret_cast<UObject **>(c.data() + p->GetOffset_Internal()) = component;
					given = true;
				}
				else if (!result && out && CastField<FBoolProperty>(p))
					result = c.data() + p->GetOffset_Internal();
			}
			if (!given || !result)
			{
				static int logged = 0;
				if (logged++ < 3)
					log(L"OnIsActionComponent on " + actor->GetClassPrivate()->GetName() + L": unexpected parameters");
				return -1;
			}
			c.invoke();
			return *result ? 1 : 0;
		}

		/// What the player looks at in HN (from Minecraft's camera, within reach), if it belongs to one of HN's
		/// interactive objects (doors, cabinets, TVs, switches, things to take: they carry HN's InteractiveComponent).
		/// The interaction ray (F12 shows it): Minecraft's own, from Steve's eyes along his look (not the camera's: in
		/// F5 that one looks back at him). It used to skip the probe's ignore list, which holds every loose physics
		/// object near Steve: the TV (a loose object) was never hit, the stand behind it was, and a loose object in
		/// front of a cabinet sent the click to whatever stood behind. Now only what Steve holds in HN is skipped; the
		/// nearest hit wins, and if it can't be used nothing happens (Minecraft's use).
		std::wstring g_lastTarget; // what the last interaction check found (F12 shows it)

		bool looking_at_interactive(UObject *pawn, bool quiet = false)
		{
			if (!g_haveEye)
				return false;
			const float d2r = 3.14159265f / 180.0f;
			// Minecraft's look (yaw 0 = +Z, pitch + = down) as an Unreal direction (Minecraft x -> x, z -> y, y -> z)
			const float yaw = g_mcLookYaw * d2r, pitch = g_mcLookPitch * d2r;
			const Vec3f dir{-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch)};
			const float reach = 300.0f;
			Call c(kismet(), STR("LineTraceSingle"));
			if (!c)
				return false;
			UObject *held = nullptr;
			{
				Call h(pawn, STR("GetHoldingItem"));
				if (h.invoke())
					if (UObject *const *item = h.param<UObject *>(STR("ReturnValue")))
						held = *item;
			}
			c.set<UObject *>(STR("WorldContextObject"), pawn)
				.set(STR("Start"), g_eye)
				.set(STR("End"), Vec3f{g_eye.x + dir.x * reach, g_eye.y + dir.y * reach, g_eye.z + dir.z * reach})
				.set<uint8_t>(STR("TraceChannel"), 0)
				.set(STR("ActorsToIgnore"), held ? ScriptArray{&held, 1, 1} : ScriptArray{nullptr, 0, 0})
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			FProperty *outHit = c.property(STR("OutHit"));
			g_lastTarget = L"nothing in reach";
			if (!ok || !*ok || !outHit)
				return false;
			// the hit actor, through GameplayStatics.BreakHitResult (FHitResult keeps it as a weak pointer)
			Call brk(gameplay_statics(), STR("BreakHitResult"));
			FProperty *hitParam = brk.property(STR("Hit"));
			if (!brk || !hitParam)
				return false;
			std::memcpy(brk.data() + hitParam->GetOffset_Internal(), c.data() + outHit->GetOffset_Internal(), size_t(outHit->GetSize()));
			brk.invoke();
			UObject *const *actor = brk.param<UObject *>(STR("HitActor"));
			UObject *const *hitComponent = brk.param<UObject *>(STR("HitComponent"));
			const Vec3f *point = brk.param<Vec3f>(STR("ImpactPoint"));
			if (!actor || !*actor || !point)
				return false;
			UObject *part = hitComponent ? *hitComponent : nullptr;
			// the hit actor, then what it is attached to
			UObject *match = nullptr;
			const wchar_t *how = L"";
			std::wstring flatInfo;
			for (UObject *a = *actor; a && !match; )
			{
				// HN's FlatActors decide themselves whether the part looked at can be used
				const int flat = flat_actionable(a, part);
				if (flat >= 0)
					flatInfo = L", OnIsActionComponent(" + (part ? part->GetName() : std::wstring(L"-")) + L") " + (flat ? L"yes" : L"no");
				if (flat == 1)
					match = a, how = L"its OnIsActionComponent";
				else if (interactive_actor(a)) // a door (leaf or frame), a thing to take: HN's action key works on it
					match = a, how = a == *actor ? L"hit" : L"attached";
				Call parent(a, STR("GetAttachParentActor"));
				parent.invoke();
				UObject *const *next = parent.param<UObject *>(STR("ReturnValue"));
				a = next && *next != a ? *next : nullptr;
			}
			// else a door whose bounds hold the hit point (the house's door frame was hit, around the leaf)
			float best = 1e9f;
			if (!match)
				for (UObject *a : g_interactiveActors)
				{
					if (!class_contains(a, L"Door"))
						continue;
					Vec3f o, e;
					Call b(a, STR("GetActorBounds"));
					b.set_bool(STR("bOnlyCollidingComponents"), false);
					if (!b.invoke())
						continue;
					if (const Vec3f *bo = b.param<Vec3f>(STR("Origin")), *be = b.param<Vec3f>(STR("BoxExtent")); bo && be)
						o = *bo, e = *be;
					else
						continue;
					if (e.x > 200.0f || e.y > 200.0f || e.z > 200.0f)
						continue; // not furniture: a whole room's worth
					const float m = 2.0f;
					if (std::abs(point->x - o.x) > e.x + m || std::abs(point->y - o.y) > e.y + m || std::abs(point->z - o.z) > e.z + m)
						continue;
					const float d = std::hypot(point->x - o.x, point->y - o.y, point->z - o.z);
					if (d < best)
						best = d, match = a, how = L"bounds";
				}
			const float distance = std::hypot(point->x - g_eye.x, point->y - g_eye.y, point->z - g_eye.z);
			wchar_t at[32];
			swprintf(at, 32, L" at %.2f m", distance / 100.0f);
			g_lastTarget = (*actor)->GetName() + L" / " + (part ? part->GetName() : std::wstring(L"-")) + at + flatInfo + L": " +
						   (match ? L"HN's action on " + match->GetName() + L" (" + how + L")" : std::wstring(L"not usable, Minecraft use"));
			static int logged = 0;
			if (!quiet && (logged < 60 || g_debugView))
			{
				++logged;
				log(L"right click: " + g_lastTarget);
			}
			return match != nullptr;
		}

		/// Right click: on one of HN's objects HN's interact (its E: doors open, things are taken) and Minecraft's hand
		/// swings, as when using a block (no block is placed against it, as vanilla never places on a door); anywhere else
		/// Minecraft's use (place a block, use the item).
		void right_click(UObject *pawn = nullptr);


		/// Minecraft's keybinds, sent every tick (held keys) or on change (presses).
		void minecraft_controls(UObject *pc)
		{
			// movement (Minecraft's: Shift sneaks, Ctrl sprints), every tick
			const int bits = (key_held('W') ? 1 : 0) | (key_held('S') ? 2 : 0) | (key_held('A') ? 4 : 0) | (key_held('D') ? 8 : 0) |
							 (key_held(VK_SPACE) ? 16 : 0) | (key_held(VK_SHIFT) ? 32 : 0) | (key_held(VK_CONTROL) ? 64 : 0);
			sendf("{\"t\":\"move\",\"m\":%d}", g_minecraftScreen ? 0 : bits);
			forward_key('E', "inventory");
			forward_key('Q', "drop");
			forward_key('F', "swap");
			forward_key(VK_F5, "perspective");
			const int wheel = g_wheel.exchange(0);
			if (g_minecraftScreen)
			{
				// the inventory: HN's cursor and buttons drive Minecraft's screen
				// the cursor as a fraction of HN's client area (Minecraft's window has its own pixel size and GUI scale)
				POINT cursor;
				RECT client;
				GetCursorPos(&cursor);
				ScreenToClient(g_hnWindow, &cursor);
				GetClientRect(g_hnWindow, &client);
				const double nx = double(cursor.x) / std::max<LONG>(1, client.right - client.left);
				const double ny = double(cursor.y) / std::max<LONG>(1, client.bottom - client.top);
				sendf("{\"t\":\"mouse\",\"x\":%.5f,\"y\":%.5f}", nx, ny);
				const int buttons[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
				for (int b = 0; b < 3; ++b)
					if (const int e = edge(buttons[b]))
						sendf("{\"t\":\"click\",\"b\":%d,\"down\":%s,\"x\":%.5f,\"y\":%.5f}", b, e > 0 ? "true" : "false", nx, ny);
				if (wheel)
					sendf("{\"t\":\"wheel\",\"d\":%d,\"x\":%.5f,\"y\":%.5f}", wheel, nx, ny);
				if (pressed(VK_ESCAPE))
					g_ws.send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}");
				return;
			}
			// holding one of HN's objects: left click is HN's own (throw it); otherwise Minecraft's attack, and a swing
			// also pushes the physics object it hits in HN
			bool holding = false;
			if (g_controlsPawn)
			{
				Call held(g_controlsPawn, STR("GetHoldingItem"));
				if (held.invoke())
					if (UObject *const *item = held.param<UObject *>(STR("ReturnValue")); item && *item)
						holding = true;
			}
			if (holding)
			{
				if (const int e = edge(VK_LBUTTON))
				{
					++g_injectedClicks;
					PostMessageW(g_hnWindow, e > 0 ? WM_LBUTTONDOWN : WM_LBUTTONUP, e > 0 ? MK_LBUTTON : 0, 0);
				}
			}
			else if (const int attack = edge(VK_LBUTTON))
			{
				sendf("{\"t\":\"key\",\"k\":\"attack\",\"down\":%s}", attack > 0 ? "true" : "false");
			}
			right_click();
			forward_key(VK_MBUTTON, "pick");
			for (int i = 0; i < 9; ++i)
				if (pressed('1' + i))
					sendf("{\"t\":\"slot\",\"n\":%d}", i);
			if (wheel)
				sendf("{\"t\":\"scroll\",\"d\":%d}", wheel);
		}

		bool g_rightClickHN = false; // the held right click went to HN (its E)

		void right_click(UObject *)
		{
			const int e = edge(VK_RBUTTON);
			if (e > 0)
			{
				g_rightClickHN = g_controlsPawn && looking_at_interactive(g_controlsPawn);
				if (g_rightClickHN)
				{
					++g_injectedE;
					PostMessageW(g_hnWindow, WM_KEYDOWN, 'E', 0x00120001);
					g_ws.send("{\"t\":\"swing\"}");
				}
				else
					g_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":true}");
			}
			else if (e < 0)
			{
				if (g_rightClickHN)
				{
					++g_injectedE;
					PostMessageW(g_hnWindow, WM_KEYUP, 'E', 0xC0120001);
				}
				else
					g_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":false}");
				g_rightClickHN = false;
			}
		}

		/// The cursor to the middle of HN's view (a menu opening: never left wherever the hidden game cursor was).
		void centre_cursor(UObject *pc)
		{
			RECT r;
			if (!g_hnWindow || !GetClientRect(g_hnWindow, &r))
				return;
			Call(pc, STR("SetMouseLocation")).set<int32>(STR("X"), (r.right - r.left) / 2).set<int32>(STR("Y"), (r.bottom - r.top) / 2).invoke();
		}

		/// While a Minecraft screen (the inventory) is open: HN's cursor shows and moves freely (game and UI input, not
		/// locked to the window), starting in the middle; HN's camera doesn't turn (the look is ours and is paused).
		/// Back to game-only input, cursor hidden, when it closes.
		void minecraft_screen_mode(UObject *pc, bool open)
		{
			static bool applied = false;
			if (applied == open)
				return;
			applied = open;
			static UObject *widgets = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/UMG.Default__WidgetBlueprintLibrary"));
			set_bool_property(pc, STR("bShowMouseCursor"), open);
			if (widgets)
			{
				if (open)
					Call(widgets, STR("SetInputMode_GameAndUIEx")).set<UObject *>(STR("PlayerController"), pc).set<UObject *>(STR("InWidgetToFocus"), nullptr)
						.set<uint8_t>(STR("InMouseLockMode"), 0).set_bool(STR("bHideCursorDuringCapture"), false).invoke();
				else
					Call(widgets, STR("SetInputMode_GameOnly")).set<UObject *>(STR("PlayerController"), pc).invoke();
			}
			if (open)
				centre_cursor(pc);
		}

		// ---- HN renders from Minecraft's camera ------------------------------------------------------------
		// A CameraActor of ours is HN's view target, placed every tick where Minecraft's camera is (first or third
		// person) with Minecraft's FOV (sprinting widens it): no HN head bob, and HN's picture is exactly Minecraft's
		// view. HN's own camera shakes are stopped.

		UObject *g_viewCamera = nullptr;
		UObject *g_viewCameraLevel = nullptr;

		UObject *view_camera(UObject *pc, UObject *pawn)
		{
			if (g_viewCamera && g_viewCameraLevel == pc)
				return g_viewCamera;
			g_viewCamera = nullptr;
			g_viewCameraLevel = pc;
			UObject *statics = gameplay_statics();
			UObject *cls = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.CameraActor"));
			if (!statics || !cls)
				return nullptr;
			Transform4 t;
			Call begin(statics, STR("BeginDeferredActorSpawnFromClass"));
			begin.set<UObject *>(STR("WorldContextObject"), pawn)
				.set<UObject *>(STR("ActorClass"), cls)
				.set(STR("SpawnTransform"), t)
				.set<uint8_t>(STR("CollisionHandlingOverride"), 1)
				.set<UObject *>(STR("Owner"), nullptr);
			if (!begin.invoke() || !*begin.param<UObject *>(STR("ReturnValue")))
				return nullptr;
			UObject *camera = *begin.param<UObject *>(STR("ReturnValue"));
			Call(statics, STR("FinishSpawningActor")).set<UObject *>(STR("Actor"), camera).set(STR("SpawnTransform"), t).invoke();
			if (UObject **component = camera->GetValuePtrByPropertyNameInChain<UObject *>(STR("CameraComponent")); component && *component)
			{
				set_bool_property(*component, STR("bConstrainAspectRatio"), false);
				set_bool_property(*component, STR("bUsePawnControlRotation"), false);
			}
			g_viewCamera = camera;
			log(L"view camera: " + camera->GetFullName());
			return camera;
		}

		/// Our camera as HN's view, placed at Minecraft's camera; `on` false gives the view back to HN's player.
		void minecraft_view(UObject *pc, UObject *pawn, bool on, double aspect)
		{
			UObject *camera = on ? view_camera(pc, pawn) : g_viewCamera;
			if (!camera || g_viewCameraLevel != pc)
				return;
			if (on && g_haveMcCamera)
			{
				// first person: the look as of now (mouse input reaches HN's camera this frame, not after a round trip through
				// Minecraft) plus Minecraft's view-bob offset from the look it rendered with; third person: Minecraft's camera
				float yaw = g_mcYaw, pitch = g_mcPitch;
				if (g_mcFirstPerson && g_lookOwned)
				{
					yaw = float(g_lookYaw) + (g_mcYaw - g_mcLookYaw);
					pitch = std::clamp(float(g_lookPitch) + (g_mcPitch - g_mcLookPitch), -90.0f, 90.0f);
				}
				struct
				{
					Vec3f loc;
					Rot3f rot;
				} pose{g_mcCamera, {-pitch, yaw + 90.0f, -g_mcRoll}};
				Call place(camera, STR("K2_SetActorLocationAndRotation"));
				place.set(STR("NewLocation"), pose.loc).set(STR("NewRotation"), pose.rot).set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
				const float hfov = float(2.0 * std::atan(std::tan(g_mcFov * 3.14159265358979 / 360.0) * aspect) * 180.0 / 3.14159265358979);
				// HN's next frame is rendered from exactly this pose: the compositor re-projects Minecraft's frame onto it
				// (reading HN's camera back on the game thread could be a frame ahead of the picture being presented)
				double mx, my, mz;
				to_mc(g_mcCamera, mx, my, mz);
				compositor::set_host_pose(yaw, pitch, g_mcRoll, g_mcFov, mx, my, mz);
				g_setHfov = hfov;
				if (UObject **component = camera->GetValuePtrByPropertyNameInChain<UObject *>(STR("CameraComponent")); component && *component)
					Call(*component, STR("SetFieldOfView")).set(STR("InFieldOfView"), hfov).invoke();
			}
			// the view target: ours while Minecraft drives (re-applied if HN takes it), the player's otherwise
			Call current(pc, STR("GetViewTarget"));
			current.invoke();
			UObject *const *target = current.param<UObject *>(STR("ReturnValue"));
			UObject *want = on ? camera : pawn;
			if (target && *target != want)
				Call(pc, STR("SetViewTargetWithBlend")).set<UObject *>(STR("NewViewTarget"), want).set(STR("BlendTime"), 0.0f).invoke();
			if (on)
				if (UObject **pcm = pc->GetValuePtrByPropertyNameInChain<UObject *>(STR("PlayerCameraManager")); pcm && *pcm)
				{
					Call(*pcm, STR("StopAllCameraShakes")).set_bool(STR("bImmediately"), true).invoke();
					// HN's camera modifiers (FOV, bob or zoom effects of its own) would stack on Minecraft's camera
					static DWORD nextModifiers = 0;
					if (GetTickCount() > nextModifiers)
					{
						nextModifiers = GetTickCount() + 1000;
						struct ObjectArray
						{
							UObject **data;
							int32 num, max;
						};
						if (auto *mods = (*pcm)->GetValuePtrByPropertyNameInChain<ObjectArray>(STR("ModifierList")); mods && mods->data)
							for (int32 i = 0; i < mods->num; ++i)
								if (mods->data[i])
									Call(mods->data[i], STR("DisableModifier")).set_bool(STR("bImmediate"), true).invoke();
					}
				}
		}

		/// HN's interaction circle (the player's cursor widget, m_pCursor) is hidden while Minecraft shows, and put back
		/// as it was otherwise. Only the widget's visibility: HN's interaction itself is untouched.
		void hide_hn_cursor(UObject *pawn, bool hide)
		{
			static UObject *cursor = nullptr;
			static uint8_t original = 0;
			static bool hidden = false;
			static DWORD next = 0;
			UObject *const *widget = pawn ? pawn->GetValuePtrByPropertyNameInChain<UObject *>(STR("m_pCursor")) : nullptr;
			UObject *now = widget ? *widget : nullptr;
			if (now != cursor)
			{
				cursor = now;
				hidden = false;
			}
			if (!cursor || (hide == hidden && GetTickCount() < next))
				return;
			next = GetTickCount() + 1000; // HN may show it again: re-apply now and then
			if (hide && !hidden)
			{
				Call get(cursor, STR("GetVisibility"));
				if (get.invoke())
					if (const uint8_t *v = get.param<uint8_t>(STR("ReturnValue")))
						original = *v;
			}
			Call(cursor, STR("SetVisibility")).set<uint8_t>(STR("InVisibility"), hide ? 2 /* Hidden */ : original).invoke();
			hidden = hide;
		}

		/// Measurements for the movement audit (UE4SS.log): HN's player dimensions once per level, and Steve's measured
		/// horizontal speed every 2 s while moving, with the keys held.
		void movement_report(UObject *pawn)
		{
			static UObject *reported = nullptr;
			if (pawn != reported)
			{
				reported = pawn;
				const float *eye = pawn->GetValuePtrByPropertyNameInChain<float>(STR("BaseEyeHeight"));
				UObject *const *capsule = pawn->GetValuePtrByPropertyNameInChain<UObject *>(STR("CapsuleComponent"));
				const float *radius = capsule && *capsule ? (*capsule)->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleRadius")) : nullptr;
				const float *half = capsule && *capsule ? (*capsule)->GetValuePtrByPropertyNameInChain<float>(STR("CapsuleHalfHeight")) : nullptr;
				log(L"HN player: eye height " + std::to_wstring(eye ? *eye : -1) + L" cm above the capsule centre, capsule radius " +
					std::to_wstring(radius ? *radius : -1) + L" cm, half height " + std::to_wstring(half ? *half : -1) + L" cm");
			}
			static DWORD start = 0;
			static Vec3f from;
			static double travelled = 0;
			static Vec3f last;
			static bool sprintSeen = false, anyKey = false;
			if (!g_haveEye)
				return;
			const DWORD now = GetTickCount();
			if (start == 0)
			{
				start = now;
				from = last = g_eye;
				travelled = 0;
				sprintSeen = anyKey = false;
				return;
			}
			travelled += std::hypot(g_eye.x - last.x, g_eye.y - last.y);
			last = g_eye;
			sprintSeen |= key_held(VK_CONTROL);
			anyKey |= key_held('W') || key_held('A') || key_held('S') || key_held('D');
			if (now - start >= 2000)
			{
				const double seconds = (now - start) / 1000.0;
				if (anyKey && travelled > 10.0)
					log(L"movement: " + std::to_wstring(travelled / 100.0 / seconds) + L" m/s over " + std::to_wstring(seconds) +
						L" s" + (sprintSeen ? L" (Ctrl held: sprint)" : L""));
				start = 0;
			}
		}

		void collision_map(const Vec3f &eye)
		{
			double ex, ey, ez;
			to_mc(eye, ex, ey, ez);
			const int bx = int(std::floor(ex)), bz = int(std::floor(ez));
			const double chest = ey - 1.62 + 1.0;
			const int by = int(std::floor(chest)), sy = std::clamp(int((chest - by) * 8.0), 0, 7);
			std::wofstream f(module_dir() + L"NeighborCraft_collision.txt");
			f << L"Minecraft collision at y " << chest << L" around (" << ex << L", " << ez << L"), 1/8 block per char, x right, z down, @ = player\n";
			for (int z = bz - 4; z <= bz + 4; ++z)
				for (int sz = 0; sz < 8; ++sz)
				{
					for (int x = bx - 4; x <= bx + 4; ++x)
					{
						const auto it = g_bits.find(cell_key(x, by, z));
						for (int sx = 0; sx < 8; ++sx)
						{
							const bool player = std::abs(x + (sx + 0.5) / 8.0 - ex) < 0.07 && std::abs(z + (sz + 0.5) / 8.0 - ez) < 0.07;
							const bool solid = it != g_bits.end() && (it->second.box[sx * 8 + sy] >> sz & 1);
							f << (player ? L'@' : solid ? L'#' : L'.');
						}
						f << L' ';
					}
					f << L"\n";
				}
			log(L"collision map written (F11)");
			notify("NeighborCraft: collision map written");
		}

		// ---- physics objects: Steve nudges what he walks into and pushes what he hits ---------------------------------
		// HN's loose objects simulate physics (PhysicsBody). Within reach of Steve they stay out of the world probe (so
		// Minecraft doesn't treat them as walls), and HN's physics moves them: walking into one gives it a velocity
		// change towards where Steve goes (less for heavier ones); a swing gives the object hit an impulse at the hit
		// point (its mass decides how far it goes), once per swing.

		struct ObjectArray
		{
			UObject **data;
			int32 num, max;
		};
		// loose objects are PhysicsBody or WorldDynamic (only those actually simulating physics are kept)
		const uint8_t kPhysicsTypes[] = {1, 3}; // ObjectTypeQuery2 = WorldDynamic, ObjectTypeQuery4 = PhysicsBody
		std::vector<UObject *> g_nearPhysics; // components simulating physics near Steve, this tick only

		std::vector<UObject *> physics_near(UObject *pawn, const Vec3f &at, float radius)
		{
			std::vector<UObject *> out;
			Call c(kismet(), STR("SphereOverlapComponents"));
			if (!c)
				return out;
			c.set<UObject *>(STR("WorldContextObject"), pawn)
				.set(STR("SpherePos"), at)
				.set(STR("SphereRadius"), radius)
				.set(STR("ObjectTypes"), ScriptArray{kPhysicsTypes, 2, 2})
				.set<UObject *>(STR("ComponentClassFilter"), nullptr)
				.set(STR("ActorsToIgnore"), ScriptArray{nullptr, 0, 0});
			c.invoke();
			if (const ObjectArray *found = c.param<ObjectArray>(STR("OutComponents")); found && found->data)
				for (int32 i = 0; i < found->num; ++i)
				{
					UObject *component = found->data[i];
					Call simulating(component, STR("IsSimulatingPhysics"));
					simulating.set(STR("BoneName"), NameParam{}).invoke();
					if (const uint8_t *yes = simulating.param<uint8_t>(STR("ReturnValue")); yes && *yes)
						out.push_back(component);
				}
			return out;
		}

		/// A part HN moves on its own (a door leaf, a cabinet door, a drawer): it simulates physics but belongs to a door
		/// or interactive actor and is not that actor's root (loose things to take are their own root).
		bool hinged_part(UObject *component)
		{
			Call c(component, STR("GetOwner"));
			c.invoke();
			UObject *const *owner = c.param<UObject *>(STR("ReturnValue"));
			if (!owner || !*owner)
				return false;
			if (class_contains(*owner, L"Door") || class_contains(component, L"MovingComponent"))
				return true;
			if (!interactive_actor(*owner) && !has_component(*owner, STR("/Script/HelloNeighbor.FlatComponent")))
				return false;
			Call root(*owner, STR("K2_GetRootComponent"));
			root.invoke();
			UObject *const *r = root.param<UObject *>(STR("ReturnValue"));
			return r && *r && *r != component;
		}

		float mass_of(UObject *component)
		{
			Call c(component, STR("GetMass"));
			c.invoke();
			const float *m = c.param<float>(STR("ReturnValue"));
			return m && *m > 0.0f ? *m : 10.0f;
		}

		Vec3f component_location(UObject *component)
		{
			Call c(component, STR("K2_GetComponentLocation"));
			c.invoke();
			const Vec3f *v = c.param<Vec3f>(STR("ReturnValue"));
			return v ? *v : Vec3f{};
		}

		UObject *owner_of(UObject *component); // (see the moving parts)

		/// The point of a component's collision nearest `to` (cm); its distance (0 inside), or -1.
		float closest_point(UObject *component, const Vec3f &to, Vec3f &out)
		{
			Call c(component, STR("GetClosestPointOnCollision"));
			c.set(STR("Point"), to).set(STR("BoneName"), NameParam{});
			if (!c.invoke())
				return -1.0f;
			const float *d = c.param<float>(STR("ReturnValue"));
			const Vec3f *p = c.param<Vec3f>(STR("OutPointOnBody"));
			if (!d || !p || *d < 0.0f)
				return -1.0f;
			out = *p;
			return *d;
		}

		Vec3f linear_velocity(UObject *component)
		{
			Call c(component, STR("GetPhysicsLinearVelocity"));
			c.set(STR("BoneName"), NameParam{}).invoke();
			const Vec3f *v = c.param<Vec3f>(STR("ReturnValue"));
			return v ? *v : Vec3f{};
		}

		/// The horizontal velocity the player asks for (cm/s): Minecraft's movement keys along Steve's look, at
		/// Minecraft's speeds (walk 4.317 m/s, sprint 5.612, sneak 1.295).
		Vec3f intended_velocity(bool &sprinting)
		{
			sprinting = false;
			const int f = (key_held('W') ? 1 : 0) - (key_held('S') ? 1 : 0), r = (key_held('D') ? 1 : 0) - (key_held('A') ? 1 : 0);
			if ((!f && !r) || g_minecraftScreen)
				return {};
			const float yaw = g_mcLookYaw * 3.14159265f / 180.0f;
			// Minecraft's forward (yaw 0 = +Z) and right, as Unreal x, y (Minecraft x -> x, z -> y)
			const float fx = -std::sin(yaw), fy = std::cos(yaw), rx = -std::cos(yaw), ry = -std::sin(yaw);
			float x = f * fx + r * rx, y = f * fy + r * ry;
			const float len = std::hypot(x, y);
			x /= len, y /= len;
			sprinting = f > 0 && key_held(VK_CONTROL) && !key_held(VK_SHIFT);
			const float speed = key_held(VK_SHIFT) ? 129.5f : sprinting ? 561.2f : 431.7f;
			return {x * speed, y * speed, 0.0f};
		}

		/// Every tick while Minecraft drives: the physics objects around Steve (kept out of this tick's probes; they
		/// are Minecraft collision from their own geometry, see the moving parts), and Steve's push on those he walks
		/// into.
		///
		/// Minecraft stops Steve at such an object (it is collision there), so his own speed says nothing about a push:
		/// it used to scale with it, and Steve blocked at a box (speed 0) moved nothing. The push follows what the
		/// player asks for (the movement keys along the look), as a contact: at the first touch an impact (Steve's
		/// 70 kg against the object's mass, along the push), then while he keeps walking into it a pushing force (a
		/// person's; more sprinting, less sneaking) that brings the object up to his speed and never beyond. A light box
		/// slides away at walking pace, a heavy cabinet barely moves (HN's friction holds it). Both act at the point of
		/// the object nearest Steve's waist: tall things tip, low ones slide.
		std::unordered_set<UObject *> g_touching; // what Steve pushed last tick (a new contact gets the impact)

		void physics_tick(UObject *pawn)
		{
			static Vec3f lastEye;
			static bool haveLast = false;
			static DWORD lastTime = 0;
			g_nearPhysics.clear();
			if (!g_haveEye)
			{
				g_touching.clear();
				return;
			}
			const Vec3f body{g_eye.x, g_eye.y, g_haveFeet ? g_feet.z + g_steveHeight * 0.55f : g_eye.z - 80.0f}; // waist
			const float feetZ = g_haveFeet ? g_feet.z : g_eye.z - 162.0f, height = g_haveFeet ? g_steveHeight : 180.0f;
			// within a Minecraft reach (punches) and well beyond a step (pushes)
			g_nearPhysics = physics_near(pawn, body, 400.0f);
			const DWORD now = GetTickCount();
			const float dt = haveLast ? std::clamp(float(now - lastTime) / 1000.0f, 0.001f, 0.05f) : 0.0f;
			const Vec3f actual = haveLast && dt > 0.0f ? Vec3f{(g_eye.x - lastEye.x) / dt, (g_eye.y - lastEye.y) / dt, 0.0f} : Vec3f{};
			lastEye = g_eye;
			lastTime = now;
			haveLast = true;
			static DWORD nextReport = 0;
			if (now > nextReport && !g_nearPhysics.empty())
			{
				nextReport = now + 5000;
				log(L"physics: " + std::to_wstring(g_nearPhysics.size()) + L" loose objects within 4 m");
			}
			bool sprinting = false;
			Vec3f v = intended_velocity(sprinting);
			if (std::hypot(actual.x, actual.y) > std::hypot(v.x, v.y) + 50.0f)
				v = actual; // carried faster than walking (a jump, a fall): that momentum
			const float speed = std::hypot(v.x, v.y);
			std::unordered_set<UObject *> touchingNow;
			if (speed < 30.0f || dt <= 0.0f)
			{
				g_touching.clear();
				return;
			}
			const Vec3f dir{v.x / speed, v.y / speed, 0.0f};
			const float reach = (height > 0 ? g_steveWidth * 0.5f : 30.0f) + 15.0f; // Steve's half width + Minecraft's margin
			for (UObject *component : physics_near(pawn, {body.x + dir.x * 25.0f, body.y + dir.y * 25.0f, body.z}, 60.0f))
			{
				if (hinged_part(component))
					continue; // HN moves its doors and drawers itself
				// in contact: the object's nearest point (to Steve's waist) at Steve's side, between his feet and head
				Vec3f cp;
				if (closest_point(component, body, cp) < 0.0f)
					continue;
				const float dx = cp.x - body.x, dy = cp.y - body.y, dh = std::hypot(dx, dy);
				if (dh > reach || cp.z < feetZ - 5.0f || cp.z > feetZ + height)
					continue;
				const float towards = dh < 1.0f ? 1.0f : (dx * dir.x + dy * dir.y) / dh;
				if (towards < 0.25f)
					continue;
				const float mass = mass_of(component);
				const Vec3f ov = linear_velocity(component);
				const float along = ov.x * dir.x + ov.y * dir.y;
				const float target = speed * towards;
				float impulse = 0.0f; // kg cm/s
				const bool first = !g_touching.count(component);
				if (first)
				{
					const float reduced = 70.0f * mass / (70.0f + mass);
					impulse += 1.2f * reduced * std::max(0.0f, target - along); // restitution 0.2
				}
				const float force = key_held(VK_SHIFT) ? 30000.0f : sprinting ? 150000.0f : 90000.0f; // kg cm/s2 (300-1500 N)
				impulse += std::min(mass * std::max(0.0f, target - along - impulse / mass), force * dt);
				touchingNow.insert(component);
				if (impulse <= 0.0f)
					continue;
				Call(component, STR("AddImpulseAtLocation")).set(STR("Impulse"), Vec3f{dir.x * impulse, dir.y * impulse, 0.0f})
					.set(STR("Location"), cp).set(STR("BoneName"), NameParam{}).invoke();
				static int logged = 0;
				if (first && logged++ < 40)
				{
					UObject *owner = owner_of(component);
					log(L"physics: Steve " + std::wstring(sprinting ? L"sprints" : L"walks") + L" into " + (owner ? owner->GetName() : component->GetName()) +
						L" (" + std::to_wstring(int(mass)) + L" kg, moving " + std::to_wstring(int(along)) + L" cm/s): impact " +
						std::to_wstring(int(impulse)) + L" kg cm/s at " + std::to_wstring(int(cp.z - feetZ)) + L" cm up");
				}
			}
			g_touching = std::move(touchingNow);
		}

		/// Minecraft's attack hit HN's world at `punch.at` (its own reach and ray: nothing through walls, once per
		/// attack): the loose object there, if any, gets an impulse at the hit point along the attack, scaled by the
		/// attack's charge (Minecraft's attack strength) and a sprint; heavy things barely move.
		void physics_punch(UObject *pawn, const Punch &punch)
		{
			UObject *best = nullptr;
			float bestDistance = 1e9f;
			Vec3f bestPoint{};
			for (UObject *component : physics_near(pawn, punch.at, 20.0f))
			{
				if (hinged_part(component))
					continue;
				Vec3f cp;
				const float d = closest_point(component, punch.at, cp);
				if (d >= 0.0f && d < bestDistance)
					best = component, bestDistance = d, bestPoint = cp;
			}
			static int logged = 0;
			if (!best)
			{
				if (logged++ < 40)
					log(L"physics: punch hit HN's world, no loose object there");
				return;
			}
			const float len = std::max(1e-3f, std::hypot(punch.dir.x, punch.dir.y, punch.dir.z));
			const Vec3f dir{punch.dir.x / len, punch.dir.y / len, punch.dir.z / len};
			// HN's objects weigh 150-400 kg (a vase 150, a chair 349, a TV 400: its physics' scale, not real weights), so
			// a punch sized for real masses moved them 3-19 cm/s, nothing visible. The punch sets a velocity change
			// instead, still less the heavier the object is on HN's own scale. At 350 / (1 + m / 200) a chair got under
			// 1 m/s and "barely budged" (HN's friction stops it at once); now a full fist sends a chair ~2.7 m/s and a
			// vase ~3.5, a sword (attack damage 7) half as much again, a sprint more. Quick repeated attacks (Minecraft's
			// uncharged swings) still push, at about two thirds.
			const float mass = mass_of(best);
			const float weapon = 0.7f + 0.06f * std::clamp(punch.damage, 1.0f, 12.0f);
			const float dv = 600.0f / (1.0f + mass / 500.0f) * weapon * (0.5f + 0.5f * std::clamp(punch.strength, 0.0f, 1.0f)) *
							 (punch.sprint ? 1.4f : 1.0f);
			const float impulse = mass * dv;
			Call(best, STR("AddImpulseAtLocation")).set(STR("Impulse"), Vec3f{dir.x * impulse, dir.y * impulse, dir.z * impulse + impulse * 0.15f})
				.set(STR("Location"), bestPoint).set(STR("BoneName"), NameParam{}).invoke();
			if (logged++ < 40)
			{
				UObject *owner = owner_of(best);
				log(L"physics: punch on " + (owner ? owner->GetName() : best->GetName()) + L" (" + std::to_wstring(int(mass)) + L" kg): " +
					std::to_wstring(int(impulse)) + L" kg cm/s, " + std::to_wstring(int(impulse / mass)) + L" cm/s");
			}
		}

		// ---- doors ----------------------------------------------------------------------------------------------------
		// A door that opens or closes changes the collision in its doorway at once, but the probe only refreshed nearby
		// blocks every 1.5 s (floors every 4.5 s): Minecraft kept the closed door as a wall for that long (the recorder
		// showed Steve blocked by a door-high box in an open doorway). HN's doors near Steve are watched every tick: when a
		// door's bounds change, every block they cover (before and after) is probed again right away, until it stops.

		struct DoorWatch
		{
			Vec3f origin, extent;
		};
		std::vector<UObject *> g_doors; // this level's doors (class Door and its subclasses)
		std::unordered_map<UObject *, DoorWatch> g_doorBounds;
		UObject *g_doorsLevel = nullptr;

		bool actor_bounds(UObject *actor, Vec3f &origin, Vec3f &extent)
		{
			Call c(actor, STR("GetActorBounds"));
			c.set_bool(STR("bOnlyCollidingComponents"), true);
			if (!c.invoke())
				return false;
			const Vec3f *o = c.param<Vec3f>(STR("Origin")), *e = c.param<Vec3f>(STR("BoxExtent"));
			if (!o || !e)
				return false;
			origin = *o;
			extent = *e;
			return true;
		}

		/// Forget what is known about the blocks a box (Unreal cm) covers: they are probed again, nearest first.
		void invalidate_box(const Vec3f &lo, const Vec3f &hi)
		{
			double x0, y0, z0, x1, y1, z1;
			to_mc(lo, x0, y0, z0);
			to_mc(hi, x1, y1, z1);
			for (int x = int(std::floor(std::min(x0, x1))); x <= int(std::floor(std::max(x0, x1))); ++x)
				for (int z = int(std::floor(std::min(z0, z1))); z <= int(std::floor(std::max(z0, z1))); ++z)
				{
					g_columns.erase(cell_key(x, 0, z));
					for (int y = int(std::floor(std::min(y0, y1))); y <= int(std::floor(std::max(y0, y1))); ++y)
					{
						const auto it = g_bits.find(cell_key(x, y, z));
						if (it != g_bits.end())
							it->second.boxKnown = false;
					}
				}
		}

		void doors_tick(UObject *pc, UObject *pawn)
		{
			if (pc != g_doorsLevel)
			{
				g_doorsLevel = pc;
				g_doors = find_all(L"Door");
				g_doorBounds.clear();
				log(L"doors: " + std::to_wstring(g_doors.size()) + L" in this level");
			}
			if (!g_haveEye)
				return;
			for (UObject *door : g_doors)
			{
				Vec3f origin, extent;
				if (!actor_bounds(door, origin, extent))
					continue;
				if (std::hypot(origin.x - g_eye.x, origin.y - g_eye.y) > 800.0f)
					continue;
				auto [it, added] = g_doorBounds.try_emplace(door, DoorWatch{origin, extent});
				DoorWatch &last = it->second;
				const bool moved = std::abs(origin.x - last.origin.x) + std::abs(origin.y - last.origin.y) + std::abs(origin.z - last.origin.z) +
									   std::abs(extent.x - last.extent.x) + std::abs(extent.y - last.extent.y) + std::abs(extent.z - last.extent.z) >
								   0.5f;
				if (added || !moved)
					continue;
				// the space before and after this tick's movement
				invalidate_box({std::min(origin.x - extent.x, last.origin.x - last.extent.x), std::min(origin.y - extent.y, last.origin.y - last.extent.y),
								   std::min(origin.z - extent.z, last.origin.z - last.extent.z)},
					{std::max(origin.x + extent.x, last.origin.x + last.extent.x), std::max(origin.y + extent.y, last.origin.y + last.extent.y),
						std::max(origin.z + extent.z, last.origin.z + last.extent.z)});
				last = {origin, extent};
			}
		}

		// ---- moving parts as collision, from their real geometry ------------------------------------------------------
		// The world probe asks HN what blocks a player. Moving parts don't fit that: HN makes an open door leaf
		// non-blocking for the player (Steve walked through open doors once the probe caught up), and loose objects
		// move all the time. So every tick, the moving components near Steve are turned into Minecraft collision from
		// their own geometry: the mesh's local bounding box at the component's current world transform, voxelised at
		// 1/8 block. Doors, drawers and cabinet parts join once they have moved (their frames stay in the world probe);
		// loose objects simulating physics always. No collision queries: just transforms, so it keeps up every tick.

		struct Watched
		{
			UObject *component;
			Transform4 first;   // where it was when first seen (a part that has moved since is a moving part)
			bool moving;
		};
		std::vector<Watched> g_watched;           // the parts of HN's interactive actors (this level)
		UObject *g_watchedLevel = nullptr;

		bool component_transform(UObject *component, Transform4 &t)
		{
			Call c(component, STR("K2_GetComponentToWorld"));
			if (!c.invoke())
				return false;
			const Transform4 *v = c.param<Transform4>(STR("ReturnValue"));
			if (!v)
				return false;
			t = *v;
			return true;
		}

		bool local_bounds(UObject *component, Vec3f &lo, Vec3f &hi)
		{
			Call c(component, STR("GetLocalBounds"));
			if (!c.invoke())
				return false;
			const Vec3f *a = c.param<Vec3f>(STR("Min")), *b = c.param<Vec3f>(STR("Max"));
			if (!a || !b)
				return false;
			lo = *a;
			hi = *b;
			return hi.x > lo.x && hi.y > lo.y && hi.z > lo.z;
		}

		/// v rotated by the unit quaternion q (x, y, z, w).
		Vec3f rotate(const float q[4], const Vec3f &v)
		{
			const float x = q[0], y = q[1], z = q[2], w = q[3];
			const float tx = 2.0f * (y * v.z - z * v.y), ty = 2.0f * (z * v.x - x * v.z), tz = 2.0f * (x * v.y - y * v.x);
			return {v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx)};
		}

		/// Mark the sub-cells (1/8 block) whose centres lie in a component's oriented box (local bounds at its transform),
		/// grown by half a sub-cell so thin parts (a door leaf) are never missed.
		void voxelise(UObject *component, std::unordered_map<unsigned long long, std::array<uint8_t, 64>> &out)
		{
			Transform4 t;
			Vec3f lo, hi;
			if (!component_transform(component, t) || !local_bounds(component, lo, hi))
				return;
			const float inv[4] = {-t.rot[0], -t.rot[1], -t.rot[2], t.rot[3]};
			const Vec3f pos{t.pos[0], t.pos[1], t.pos[2]};
			const float grow = 6.25f; // half a sub-cell, cm
			// the box's world corners -> the Minecraft blocks to look at
			double bx0 = 1e18, by0 = 1e18, bz0 = 1e18, bx1 = -1e18, by1 = -1e18, bz1 = -1e18;
			for (int i = 0; i < 8; ++i)
			{
				const Vec3f local{(i & 1 ? hi.x : lo.x) * t.scale[0], (i & 2 ? hi.y : lo.y) * t.scale[1], (i & 4 ? hi.z : lo.z) * t.scale[2]};
				const Vec3f r = rotate(t.rot, local);
				double mx, my, mz;
				to_mc({pos.x + r.x, pos.y + r.y, pos.z + r.z}, mx, my, mz);
				bx0 = std::min(bx0, mx), by0 = std::min(by0, my), bz0 = std::min(bz0, mz);
				bx1 = std::max(bx1, mx), by1 = std::max(by1, my), bz1 = std::max(bz1, mz);
			}
			if (bx1 - bx0 > 12 || by1 - by0 > 12 || bz1 - bz0 > 12)
				return; // not a small moving part (a whole building): leave it to the world probe
			for (int x = int(std::floor(bx0)); x <= int(std::floor(bx1)); ++x)
				for (int y = int(std::floor(by0)); y <= int(std::floor(by1)); ++y)
					for (int z = int(std::floor(bz0)); z <= int(std::floor(bz1)); ++z)
					{
						std::array<uint8_t, 64> *bits = nullptr;
						for (int sx = 0; sx < 8; ++sx)
							for (int sy = 0; sy < 8; ++sy)
								for (int sz = 0; sz < 8; ++sz)
								{
									const Vec3f c = to_ue(x + (sx + 0.5) / 8.0, y + (sy + 0.5) / 8.0, z + (sz + 0.5) / 8.0);
									const Vec3f l = rotate(inv, {c.x - pos.x, c.y - pos.y, c.z - pos.z});
									const float lx = l.x / t.scale[0], ly = l.y / t.scale[1], lz = l.z / t.scale[2];
									if (lx < lo.x - grow || lx > hi.x + grow || ly < lo.y - grow || ly > hi.y + grow || lz < lo.z - grow || lz > hi.z + grow)
										continue;
									if (!bits)
										bits = &out[cell_key(x, y, z)];
									(*bits)[sx * 8 + sy] |= uint8_t(1u << sz);
								}
					}
		}

		/// The parts of HN's interactive actors (doors, drawers, cabinets ...): their static mesh components, once per
		/// level (the arrays the engine returns are kept: one small allocation per actor and level).
		void watch_interactives(UObject *pc)
		{
			if (pc == g_watchedLevel)
				return;
			g_watchedLevel = pc;
			g_watched.clear();
			static UObject *meshClass = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.StaticMeshComponent"));
			std::unordered_set<UObject *> actors;
			for (UObject *door : g_doors)
				actors.insert(door);
			for (UObject *component : find_all(L"MovingComponent"))
			{
				Call owner(component, STR("GetOwner"));
				owner.invoke();
				if (UObject *const *actor = owner.param<UObject *>(STR("ReturnValue")); actor && *actor)
					actors.insert(*actor);
			}
			for (UObject *component : find_all(L"InteractiveComponent"))
			{
				Call owner(component, STR("GetOwner"));
				owner.invoke();
				if (UObject *const *actor = owner.param<UObject *>(STR("ReturnValue")); actor && *actor)
					actors.insert(*actor);
			}
			g_interactiveActors.assign(actors.begin(), actors.end());
			for (UObject *actor : actors)
			{
				Call parts(actor, STR("GetComponentsByClass"));
				parts.set<UObject *>(STR("ComponentClass"), meshClass).invoke();
				const ObjectArray *list = parts.param<ObjectArray>(STR("ReturnValue"));
				for (int32 i = 0; list && list->data && i < list->num; ++i)
				{
					Transform4 t;
					if (list->data[i] && component_transform(list->data[i], t))
						g_watched.push_back({list->data[i], t, false});
				}
			}
			log(L"moving parts: watching " + std::to_wstring(g_watched.size()) + L" parts of " + std::to_wstring(actors.size()) +
				L" interactive objects");
		}

		/// Every tick: the moving parts near Steve and the loose objects around him, as collision; blocks whose moving
		/// collision changed are sent again.
		UObject *owner_of(UObject *component)
		{
			Call c(component, STR("GetOwner"));
			c.invoke();
			UObject *const *a = c.param<UObject *>(STR("ReturnValue"));
			return a ? *a : nullptr;
		}

		void dynamic_tick(UObject *pc)
		{
			watch_interactives(pc);
			if (!g_haveEye)
				return;
			// what the player holds in HN (attached in front of the camera) is never collision
			UObject *held = nullptr;
			if (g_controlsPawn)
			{
				Call h(g_controlsPawn, STR("GetHoldingItem"));
				if (h.invoke())
					if (UObject *const *item = h.param<UObject *>(STR("ReturnValue")))
						held = *item;
			}
			std::unordered_map<unsigned long long, std::array<uint8_t, 64>> now;
			for (Watched &w : g_watched)
			{
				if (held && owner_of(w.component) == held)
					continue;
				Transform4 t;
				if (!component_transform(w.component, t))
					continue;
				if (std::hypot(t.pos[0] - g_eye.x, t.pos[1] - g_eye.y) > 800.0f)
					continue;
				if (!w.moving)
				{
					float d = 0.0f;
					for (int i = 0; i < 4; ++i)
						d += std::abs(t.rot[i] - w.first.rot[i]);
					for (int i = 0; i < 3; ++i)
						d += std::abs(t.pos[i] - w.first.pos[i]) * 0.01f;
					if (d < 0.01f)
						continue;
					w.moving = true; // it has moved: from now on it is a moving part
					static int logged = 0;
					if (logged++ < 20)
						log(L"moving part: " + w.component->GetFullName());
				}
				voxelise(w.component, now);
			}
			for (UObject *component : g_nearPhysics)
				if (!held || owner_of(component) != held)
					voxelise(component, now);
			// Not where Steve is. A door leaf swinging into him or an object falling on his head became collision inside
			// his hitbox: Minecraft can't push him out of it, finds no room to stand, and forces him to crawl (the
			// recorder: boxes appearing in his head the tick before POSE_FORCED SWIMMING, at a door). HN's own physics
			// has the part meet his capsule (his size) instead; Minecraft's collision is kept only outside him.
			if (g_haveFeet)
			{
				double fx, fy, fz;
				to_mc(g_feet, fx, fy, fz);
				const double hw = g_steveWidth / 200.0 + 0.01, h = g_steveHeight / 100.0 + 0.01;
				const double x0 = fx - hw, x1 = fx + hw, y0 = fy - 0.01, y1 = fy + h, z0 = fz - hw, z1 = fz + hw;
				for (int x = int(std::floor(x0)); x <= int(std::floor(x1)); ++x)
					for (int y = int(std::floor(y0)); y <= int(std::floor(y1)); ++y)
						for (int z = int(std::floor(z0)); z <= int(std::floor(z1)); ++z)
						{
							const auto it = now.find(cell_key(x, y, z));
							if (it == now.end())
								continue;
							for (int sx = 0; sx < 8; ++sx)
								for (int sy = 0; sy < 8; ++sy)
									for (int sz = 0; sz < 8; ++sz)
									{
										const double cx0 = x + sx / 8.0, cy0 = y + sy / 8.0, cz0 = z + sz / 8.0;
										if (cx0 + 0.125 > x0 && cx0 < x1 && cy0 + 0.125 > y0 && cy0 < y1 && cz0 + 0.125 > z0 && cz0 < z1)
											it->second[sx * 8 + sy] &= uint8_t(~(1u << sz));
									}
						}
			}
			// what changed since last tick goes to Minecraft (with the block's static shape)
			for (auto &[key, bits] : now)
			{
				const auto old = g_dynBits.find(key);
				if (old == g_dynBits.end() || old->second != bits)
				{
					g_bits[key];
					g_dirty.insert(key);
				}
			}
			for (auto &[key, bits] : g_dynBits)
				if (!now.count(key))
				{
					g_bits[key];
					g_dirty.insert(key);
				}
			g_dynBits = std::move(now);
		}

		// ---- HN's surface material under Steve, for Minecraft's particles and footsteps -----------------------------

		/// HN's physical material name (PM_Grass, Wood, Tile ...) as the Minecraft block that looks and sounds like it.
		const char *block_for_material(const std::wstring &name)
		{
			std::wstring n = name;
			std::transform(n.begin(), n.end(), n.begin(), ::towlower);
			struct Pair
			{
				const wchar_t *key;
				const char *block;
			};
			static const Pair map[] = {
				{L"grass", "minecraft:grass_block"}, {L"earth", "minecraft:dirt"}, {L"dirt", "minecraft:dirt"}, {L"ground", "minecraft:dirt"},
				{L"sand", "minecraft:sand"}, {L"wood", "minecraft:oak_planks"}, {L"furniture", "minecraft:oak_planks"}, {L"parquet", "minecraft:oak_planks"},
				{L"tile", "minecraft:smooth_stone"}, {L"road", "minecraft:gray_concrete"}, {L"asphalt", "minecraft:gray_concrete"},
				{L"concrete", "minecraft:light_gray_concrete"}, {L"stone", "minecraft:stone"}, {L"carpet", "minecraft:white_wool"},
				{L"metal", "minecraft:iron_block"}, {L"rail", "minecraft:iron_block"}, {L"ice", "minecraft:ice"}, {L"water", "minecraft:water"},
				{L"plastic", "minecraft:white_concrete"}, {L"flour", "minecraft:white_concrete_powder"}, {L"glue", "minecraft:slime_block"},
				{L"paper", "minecraft:white_wool"}, {L"glass", "minecraft:glass"},
			};
			for (const Pair &p : map)
				if (n.find(p.key) != std::wstring::npos)
					return p.block;
			return "minecraft:stone";
		}

		void surface_tick(UObject *pawn)
		{
			static DWORD next = 0;
			static std::string sent;
			if (!g_haveEye || GetTickCount() < next)
				return;
			next = GetTickCount() + 250;
			Call c(kismet(), STR("LineTraceSingle"));
			if (!c)
				return;
			c.set<UObject *>(STR("WorldContextObject"), pawn)
				.set(STR("Start"), g_eye)
				.set(STR("End"), Vec3f{g_eye.x, g_eye.y, g_eye.z - 250.0f})
				.set<uint8_t>(STR("TraceChannel"), 0)
				.set(STR("ActorsToIgnore"), ignore_list())
				.set_bool(STR("bTraceComplex"), true)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			FProperty *outHit = c.property(STR("OutHit"));
			if (!ok || !*ok || !outHit)
				return;
			Call brk(gameplay_statics(), STR("BreakHitResult"));
			FProperty *hitParam = brk.property(STR("Hit"));
			if (!brk || !hitParam)
				return;
			std::memcpy(brk.data() + hitParam->GetOffset_Internal(), c.data() + outHit->GetOffset_Internal(), size_t(outHit->GetSize()));
			brk.invoke();
			UObject *const *material = brk.param<UObject *>(STR("PhysMat"));
			const std::string block = material && *material ? block_for_material((*material)->GetName()) : "minecraft:stone";
			if (block != sent)
			{
				sent = block;
				sendf("{\"t\":\"surface\",\"b\":\"%s\"}", block.c_str());
				static int logged = 0;
				if (logged++ < 30)
					log(L"surface: " + (material && *material ? (*material)->GetName() : StringType(L"(none)")) + L" -> " +
						std::wstring(block.begin(), block.end()));
			}
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

	// ---- watchdog ----------------------------------------------------------------------------------------------------
	// HN froze once right as a level started (no crash, nothing in the log); its dump at exit held only the exiting
	// thread. If the game thread stops ticking for 5 s while Minecraft drives, a full dump of every thread is written
	// next to the mod (NeighborCraft_freeze_*.dmp) while it is still stuck: the stuck line is in it.
	std::atomic<unsigned long long> g_heartbeat{0};
	std::atomic<bool> g_watchArmed{false};

	void watchdog()
	{
		int written = 0;
		for (;;)
		{
			Sleep(1000);
			const unsigned long long last = g_heartbeat.load();
			if (!g_watchArmed.load() || last == 0 || GetTickCount64() - last < 5000 || written >= 2)
				continue;
			const std::wstring file = module_dir() + L"NeighborCraft_freeze_" + std::to_wstring(GetTickCount64()) + L".dmp";
			HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h != INVALID_HANDLE_VALUE)
			{
				MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
					MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory), nullptr, nullptr, nullptr);
				CloseHandle(h);
				++written;
			}
			// wait for the game to tick again before another one
			while (g_heartbeat.load() == last)
				Sleep(500);
		}
	}

	void start(void *module)
	{
		g_module = module;
		std::thread(watchdog).detach();
		g_ws.start("127.0.0.1", kPort);
		g_launchAt = GetTickCount() + 3000; // Minecraft may be running already: give the link a moment first
		compositor::set_host_planes(kHostNear, 1.0e7f);
		compositor::set_pose_lag(0);
		log(L"started; Minecraft link on 127.0.0.1:" + std::to_wstring(kPort));
	}

	void stop()
	{
		unhook_hn_window();
		compositor::set_active(false);
		compositor::unregister(g_module);
		g_ws.stop();
	}

	void tick(void *engine_object, float)
	{
		compositor::try_register(g_module);
		hook_hn_window();
		++g_frame;
		g_heartbeat = GetTickCount64();
		g_watchArmed = g_driving;

		// Minecraft: started from here if it isn't running, and kept out of sight
		if (!g_launched && !g_ws.connected() && GetTickCount() > g_launchAt)
			launch_minecraft();
		if (g_ws.connected() && GetTickCount() > g_nextHide)
		{
			g_nextHide = GetTickCount() + 2000;
			hide_minecraft_window();
		}

		if (pressed(VK_F12))
		{
			g_debugView = !g_debugView;
			const bool debug = g_debugView;
			sendf("{\"t\":\"debug\",\"on\":%s}", debug ? "true" : "false");
			log(debug ? L"collision recorder + debug view on (F12)" : L"collision recorder off");
			notify(debug ? "Collision recorder ON (F12 to stop)" : "Collision recorder off");
		}
		if (pressed(VK_F7))
		{
			g_enabled = !g_enabled;
			log(g_enabled ? L"passthrough on" : L"passthrough off");
			notify(g_enabled ? "NeighborCraft on" : "NeighborCraft off");
		}

		UObject *pc = player_controller(static_cast<UObject *>(engine_object));
		UObject *pawn = pc ? pawn_of(pc) : nullptr;
		if (pressed(VK_F9))
			dump(pc, pawn);

		if (!g_ws.connected())
			g_mcReady = false; // a new Minecraft says "ready" again once its world is loaded
		std::string message;
		while (g_ws.poll(message))
			handle(message);

		Camera cam;
		const bool gameplay = pawn && in_gameplay(pc);
		static bool wasGameplay = false;
		if (wasGameplay && !gameplay && pc)
			centre_cursor(pc);
		wasGameplay = gameplay;
		// Minecraft pauses whenever HN isn't in gameplay (pause menu, main menu) or the passthrough is off
		static bool mcPaused = false;
		if (!g_ws.connected())
			mcPaused = false; // a restarted Minecraft starts unpaused
		else if ((gameplay && g_enabled) == mcPaused)
		{
			mcPaused = !mcPaused;
			sendf("{\"t\":\"pause\",\"on\":%s}", mcPaused ? "true" : "false");
		}
		const bool live = g_enabled && g_ws.connected() && gameplay && read_camera(pc, cam);
		if (live && g_driving && g_setHfov > 0.0f && std::abs(cam.hfov - g_setHfov) > 0.2f)
		{
			static DWORD nextFovLog = 0;
			if (GetTickCount() > nextFovLog)
			{
				nextFovLog = GetTickCount() + 3000;
				log(L"FOV: HN rendered " + std::to_wstring(cam.hfov) + L" degrees, it was given " + std::to_wstring(g_setHfov) +
					L" (something in HN changes the camera's FOV)");
			}
		}
		compositor::set_active(live);
		set_player_hidden(pawn, live);
		g_minecraftInput = live;
		if (pc)
			minecraft_screen_mode(pc, live && g_minecraftScreen);
		if (!live)
		{
			if (pawn)
				hide_hn_cursor(pawn, false);
			if (pc && pawn)
				minecraft_view(pc, pawn, false, 1.0);
			if (pawn)
				minecraft_movement(pawn, false);
			if (pawn && g_driving)
				hn_movement(pawn, true);
			g_driving = false;
			return;
		}

		// a new level (or the same one reloaded after being caught): its colliders went with the old one
		if (pc != g_colliderLevel)
		{
			destroy_colliders(false);
			g_cells.clear();
			g_bits.clear();
			g_columns.clear();
			g_dirty.clear();
			g_colliderLevel = pc;
			if (g_haveOffset)
				g_ws.send("{\"t\":\"blocksync\",\"r\":64}");
			static bool dumped = false;
			if (!dumped)
			{
				dumped = true;
				dump(pc, pawn);
			}
		}
		if (!g_pendingSet.empty() || !g_pendingClear.empty())
		{
			apply_blocks(pawn, g_pendingSet, g_pendingClear);
			g_pendingSet.clear();
			g_pendingClear.clear();
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
		// Unreal's roll is clockwise-positive, Minecraft's camera roll counter-clockwise-positive (driving: the pose
		// Minecraft's camera gives HN, set where HN's camera is placed)
		if (!g_driving)
			compositor::set_host_pose(cam.rot.yaw - 90.0f, -cam.rot.pitch, -cam.rot.roll, vfov, cx, cy, cz);
		// Unreal renders a frame while the game thread is already on the next one: no overlap, so the frame presented is
		// the one the pose above was set for
		static UObject *lagOffFor = nullptr;
		if (lagOffFor != pc)
		{
			lagOffFor = pc;
			// no render-thread overlap; no motion blur (HN's picture smeared when turning while Minecraft's stays sharp)
			for (const TCHAR *command : {STR("r.OneFrameThreadLag 0"), STR("r.MotionBlurQuality 0")})
			{
				Call(kismet(), STR("ExecuteConsoleCommand")).set<UObject *>(STR("WorldContextObject"), pawn).set(STR("Command"), FString(command)).invoke();
				log(L"console: " + std::wstring(command));
			}
		}
		// HN's camera for the next frame is placed after this frame's scene, and the UI (where Minecraft is composited)
		// is drawn after this tick: while driving, the frame on screen was rendered with the pose set one tick earlier
		compositor::set_pose_lag(g_driving ? 1 : 0);
		// where the player looks (HN's mouse look: the control rotation; the camera itself follows Minecraft when it
		// drives), and HN's own base FOV for Minecraft to apply its FOV effects to
		Rot3f look = cam.rot;
		{
			Call control(pc, STR("GetControlRotation"));
			if (control.invoke())
				if (const Rot3f *r = control.param<Rot3f>(STR("ReturnValue")))
					look = *r;
		}
		float baseHfov = cam.hfov;
		if (UObject **pcm = pc->GetValuePtrByPropertyNameInChain<UObject *>(STR("PlayerCameraManager")); pcm && *pcm)
			if (const float *f = (*pcm)->GetValuePtrByPropertyNameInChain<float>(STR("DefaultFOV")); f && *f > 1.0f)
				baseHfov = *f;
		const float baseVfov = float(2.0 * std::atan(std::tan(baseHfov * 3.14159265358979 / 360.0) / aspect) * 180.0 / 3.14159265358979);
		float yaw = look.yaw - 90.0f, pitch = -look.pitch;
		const float roll = 0.0f;
		if (g_driving)
		{
			if (!g_lookOwned)
			{
				// Minecraft takes the look over from where HN's was; HN's own mouse look stops
				g_lookOwned = true;
				g_lookYaw = yaw;
				g_lookPitch = pitch;
				Call(pc, STR("SetIgnoreLookInput")).set_bool(STR("bNewLookInput"), true).invoke();
			}
			const long dx = g_mouseDx.exchange(0), dy = g_mouseDy.exchange(0);
			if (!g_minecraftScreen)
			{
				if (g_rawMouseSeen)
				{
					// Minecraft's MouseHandler: f = sensitivity * 0.6 + 0.2, turn = counts * f^3 * 8 * 0.15 degrees
					const double f = 0.5 * 0.6 + 0.2, k = f * f * f * 8.0 * 0.15;
					g_lookYaw += dx * k;
					g_lookPitch = std::clamp(g_lookPitch + dy * k, -90.0, 90.0);
				}
				else
				{
					// no raw input in this window: follow HN's control rotation (unwrapped)
					double d = std::fmod(yaw - g_lookYaw, 360.0);
					d += d > 180.0 ? -360.0 : d < -180.0 ? 360.0 : 0.0;
					g_lookYaw += d;
					g_lookPitch = std::clamp(double(pitch), -90.0, 90.0);
				}
			}
			yaw = float(g_lookYaw);
			pitch = float(g_lookPitch);
			// HN aims where Steve looks (its interact traces use the control rotation)
			Call(pc, STR("SetControlRotation")).set(STR("NewRotation"), Rot3f{-pitch, yaw + 90.0f, 0.0f}).invoke();
		}
		else if (g_lookOwned)
		{
			g_lookOwned = false;
			Call(pc, STR("ResetIgnoreLookInput")).invoke();
		}
		g_minecraftDriving = g_driving;

		// F10: Minecraft's own physics against the voxels (blocky collision) instead of HN's exact collision
		if (pressed(VK_F10))
		{
			g_gridPhysics = !g_gridPhysics;
			notify(g_gridPhysics ? "Minecraft physics: blocky collision" : "HN collision, Minecraft movement");
		}
		refresh_probe_ignore(pawn);
		doors_tick(pc, pawn);
		if (g_driving)
		{
			physics_tick(pawn);
			for (const Punch &punch : g_punches)
				physics_punch(pawn, punch);
		}
		else
			g_nearPhysics.clear();
		g_punches.clear();
		if (g_driving && g_mcReady)
			dynamic_tick(pc);
		// The loose objects near Steve are moving collision (see the moving parts), so the world probe looks past them:
		// past their actor, as traces ignore whole actors. Only when the object is its actor (its root component): a
		// loose part of a bigger actor (a street's bins, a yard's props) used to make the probe ignore that whole actor,
		// and on Act 1's road the floor under Steve stopped being found (the recorder: the road's blocks emptying
		// around him, then a fall).
		g_probeIgnoreNow = g_probeIgnore;
		for (UObject *component : g_nearPhysics)
		{
			Call owner(component, STR("GetOwner"));
			owner.invoke();
			UObject *const *actor = owner.param<UObject *>(STR("ReturnValue"));
			if (!actor || !*actor)
				continue;
			Call root(*actor, STR("K2_GetRootComponent"));
			root.invoke();
			UObject *const *r = root.param<UObject *>(STR("ReturnValue"));
			const bool standalone = r && *r == component;
			static std::unordered_set<UObject *> reported;
			if (reported.size() < 200 && reported.insert(*actor).second)
				log(L"physics: " + component->GetName() + L" of " + (*actor)->GetName() + L" (" + (*actor)->GetClassPrivate()->GetName() + L"): " +
					(standalone ? L"loose object, the probe looks past it" : L"part of a bigger actor, the probe keeps the actor"));
			if (standalone)
				g_probeIgnoreNow.push_back(*actor);
		}
		const bool ready = g_mcReady && sample_world(pawn, g_driving && g_haveEye ? g_eye : cam.loc);
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
			// never under HN's world: if there is no ground under Steve any more (fell through a gap in the probed world,
			// or Minecraft respawned him somewhere), put him back where there was; HN's player isn't moved there
			static DWORD nextGroundCheck = 0;
			static bool haveGood = false;
			static Vec3f lastGood;
			static bool underWorld = false; // until Steve is back on HN's ground, HN's player stays where it is
			if (g_haveEye && GetTickCount() > nextGroundCheck)
			{
				nextGroundCheck = GetTickCount() + 250;
				float groundZ = 0.0f;
				const bool ground = ground_below(pawn, g_eye, groundZ);
				if (ground && g_eye.z - groundZ < 400.0f)
				{
					lastGood = {g_eye.x, g_eye.y, groundZ + 10.0f};
					haveGood = true;
					underWorld = false;
				}
				else if (!ground)
				{
					if (!underWorld)
					{
						log(L"Steve is under HN's world: back to the last ground");
						// what the probe knew where he fell: the column's rays and the floors found in it
						const Vec3f from = haveGood ? lastGood : g_eye;
						double fx, fy, fz;
						to_mc(from, fx, fy, fz);
						const int cx = int(std::floor(fx)), cz = int(std::floor(fz)), cy = int(std::floor(fy));
						std::wstring info = L"  last ground at block " + std::to_wstring(cx) + L" " + std::to_wstring(cy) + L" " + std::to_wstring(cz);
						if (const auto col = g_columns.find(cell_key(cx, 0, cz)); col != g_columns.end())
							info += L", column rays for y " + std::to_wstring(col->second.lo) + L".." + std::to_wstring(col->second.hi - 1) +
									L", " + std::to_wstring(GetTickCount() - col->second.probed) + L" ms ago";
						else
							info += L", column never probed";
						for (int y = cy - 3; y <= cy + 1; ++y)
						{
							int floors = 0;
							if (const auto b = g_bits.find(cell_key(cx, y, cz)); b != g_bits.end())
								for (float f : b->second.floor)
									floors += f > 0.0f;
							info += L"; y " + std::to_wstring(y) + L": " + std::to_wstring(floors) + L" floors";
						}
						log(info);
					}
					underWorld = true;
					teleport_minecraft(pawn, haveGood ? lastGood : p);
					nextGroundCheck = GetTickCount() + 1000; // give the teleport time to land
				}
			}
			// HN's player stands where Steve does (its eyes at his), and HN sees through Minecraft's camera
			if (g_haveEye && !underWorld)
			{
				const float *eyeHeight = pawn->GetValuePtrByPropertyNameInChain<float>(STR("BaseEyeHeight"));
				Vec3f target{g_eye.x, g_eye.y, g_eye.z - (eyeHeight ? *eyeHeight : 64.0f)};
				if (g_haveFeet)
				{
					// Steve's hitbox exactly: the capsule his size, standing on his feet
					steve_capsule(pawn);
					target = {g_feet.x, g_feet.y, g_feet.z + std::max(g_steveHeight, g_steveWidth) * 0.5f};
				}
				set_actor_location(pawn, target);
				// HN's own camera component (where HN's action key looks from) at Steve's eyes, so HN uses the object
				// the interaction ray picked, not a neighbour (it sat 8 cm lower, and without Minecraft's look offset)
				if (UObject *const *camera = pawn->GetValuePtrByPropertyNameInChain<UObject *>(STR("Camera")); camera && *camera)
					Call(*camera, STR("K2_SetWorldLocation")).set(STR("NewLocation"), g_eye).set_bool(STR("bSweep"), false)
						.set_bool(STR("bTeleport"), true).invoke();
				g_lastPlaced = target;
				g_placed = true;
			}
			minecraft_view(pc, pawn, !underWorld, aspect);
			sendf("{\"t\":\"cam\",\"drive\":true,\"f\":%lld,\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f}", g_frame, yaw, pitch, roll, baseVfov);
		}
		else
		{
			// HN moves the player (with Minecraft's speeds and jump) and collides it exactly; Minecraft follows, and
			// knows when the player is in the air (falling makes Minecraft's hits critical)
			UObject *cmc = movement_component(pawn);
			const uint8_t *mode = cmc ? cmc->GetValuePtrByPropertyNameInChain<uint8_t>(STR("MovementMode")) : nullptr;
			const bool air = mode && *mode == 3; // MOVE_Falling
			minecraft_view(pc, pawn, false, aspect);
			sendf("{\"t\":\"cam\",\"f\":%lld,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":true,\"air\":%s}",
				g_frame, cx, cy, cz, yaw, pitch, roll, vfov, air ? "true" : "false");
		}

		g_controlsPawn = pawn;
		if (g_debugView && g_driving && !g_minecraftScreen)
		{
			// F12: what a right click would do, under the crosshair, twice a second
			static DWORD nextTarget = 0;
			if (GetTickCount() > nextTarget)
			{
				nextTarget = GetTickCount() + 500;
				looking_at_interactive(pawn, true);
				std::string text;
				for (wchar_t ch : g_lastTarget)
					text += ch < 128 && ch != '"' && ch != '\\' ? char(ch) : '?';
				notify(text);
			}
		}
		minecraft_controls(pc);
		if (g_driving)
			surface_tick(pawn);
		hide_hn_cursor(pawn, true);
		movement_report(pawn);
		if (pressed(VK_F11) && g_haveEye)
			collision_map(g_eye);
	}
}
