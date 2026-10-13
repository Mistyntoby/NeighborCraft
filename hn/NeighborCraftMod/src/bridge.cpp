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
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/FMemory.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include <windows.h>
#include <algorithm>
#include <chrono>
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
#include <sstream>
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
		constexpr int kColumnMargin = 4;       // blocks probed beyond what is needed below (half that above)
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

		/// An engine object that may be destroyed (a streamed level's light, a stand-in): the pointer and its slot in the
		/// global object array; Get() gives it only while that slot still holds this very object. In game UE4SS's
		/// FWeakObjectPtr gave objects calls failed on (the capture prototype's test cube, 2026-10-13: "position couldn't
		/// be read"), so this checks the slot itself (the object array is what UE4SS's FindAllOf walks, and that works).
		struct WeakRef
		{
			UObject *ptr = nullptr;
			int32_t index = -1;
			WeakRef() = default;
			WeakRef(UObject *o) : ptr(o), index(o ? o->GetInternalIndex() : -1) {}
			UObject *Get() const
			{
				if (!ptr || index < 0)
					return nullptr;
				FUObjectItem *item = UObjectArray::IndexToObject(index);
				if (!item || item->GetUObject() != ptr || item->IsUnreachable())
					return nullptr;
				return ptr;
			}
		};

		/// What the per-level caches (doors, interactive parts, the lent light, the sun) are keyed by: the controller AND the
		/// levels the world has loaded. A nightmare is a streamed level: it comes and goes under the same controller, and
		/// caches keyed by the controller alone kept its objects after it unloaded: the game crashed calling a destroyed
		/// light (2026-10-13 18:34, bridge.cpp lend_shadows_tick, after a nightmare ended). Only ever compared, never
		/// dereferenced: a new value whenever either changes.
		UObject *level_key(UObject *pc)
		{
			struct RawArray
			{
				UObject **data;
				int32_t num, max;
			};
			UObject *level = pc ? pc->GetOuterPrivate() : nullptr;
			UObject *world = level ? level->GetOuterPrivate() : nullptr;
			const RawArray *levels = world ? world->GetValuePtrByPropertyNameInChain<RawArray>(STR("Levels")) : nullptr;
			uint64_t sig = reinterpret_cast<uintptr_t>(pc);
			if (levels && levels->num >= 0 && levels->num < 4096)
			{
				sig = sig * 1099511628211ull ^ uint64_t(levels->num);
				for (int32_t i = 0; i < levels->num; ++i)
					sig = sig * 1099511628211ull ^ reinterpret_cast<uintptr_t>(levels->data[i]);
			}
			static uint64_t last = 0;
			static uintptr_t key = 0;
			if (sig != last)
			{
				last = sig;
				++key;
				log(L"levels: " + (levels ? std::to_wstring(levels->num) + L" loaded" : std::wstring(L"the world's level list unreadable")) +
					L": per-level caches start over");
			}
			return reinterpret_cast<UObject *>(key);
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

		/// Pickup progress or throw charge (0..1) in Minecraft's experience bar's place (CarryBar), each its own kind;
		/// a negative value ends it. Minecraft lets a value it stops getting run out by itself (400 ms).
		void progress(const char *kind, float value)
		{
			sendf("{\"t\":\"bar\",\"k\":\"%s\",\"v\":%.3f}", kind, value);
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
		// Minecraft's drawn things as boxes (ShadowExport.java): per box its centre and three half-axes (blocks, world)
		std::vector<double> g_shadowBoxes;
		std::vector<int> g_shadowColours;   // per box: its colour (0xRRGGBB, a model cube's skin average) or -1 (grey)
		std::vector<int> g_shadowKinds;     // per box: 1 a model's real surface (drawn by HN: receives its light)
		DWORD g_shadowBoxesAt = 0;
		double g_hand3p[12] = {};      // Steve's main-hand item frame: origin, then unit x, y, z axes (Minecraft world)
		double g_heldBoxHalf = 0.0;    // the held HN object's half size in Steve's hand (blocks), 0: none shown
		bool g_heldCasting = false;    // the held HN object's own mesh casts (third person)
		DWORD g_hand3pAt = 0;
		/// The recent "shadows" messages by Minecraft's frame number: the stand-ins are posed from the one of the frame the
		/// compositor shows, not the newest. The picture reaches HN a frame or more after its shadow message (a GPU
		/// readback, more when Minecraft stutters), so the newest boxes ran ahead of the picture: Steve's stand-in showed
		/// beside him while running (the user, 2026-10-13).
		struct ShadowFrame
		{
			long long frame = -1;
			std::vector<double> boxes;
			std::vector<int> kinds;
			std::vector<int> colours;
			double hand[12] = {};
			bool hasHand = false;
			DWORD at = 0;
		};
		std::deque<ShadowFrame> g_shadowFrames;

		/// Puts the matching message's boxes in g_shadowBoxes / g_shadowKinds / g_hand3p (game thread, every tick).
		void select_shadow_frame()
		{
			if (g_shadowFrames.empty())
				return;
			// the frame the picture will show at the next present: the one shown last plus Minecraft's frames per HN frame.
			// The first attempt took the one shown last, which with Minecraft at 120 fps against HN's 60 was two of its
			// frames old by the present (the user: "a clone of me in white copying my exact moves"); the newest instead
			// ran 2-3 frames ahead of the picture (log: "most ahead 3"; Steve's trail on turns)
			const ShadowFrame *pick = &g_shadowFrames.back();
			static DWORD nextLog = 0;
			static long long most = 0, lastNewest = -1;
			static double perHnFrame = 1.0;
			const long long shown = compositor::shown_frame();
			if (pick->frame >= 0)
			{
				if (lastNewest >= 0 && pick->frame >= lastNewest && pick->frame - lastNewest < 10)
					perHnFrame += (double(pick->frame - lastNewest) - perHnFrame) * 0.05;
				lastNewest = pick->frame;
			}
			if (shown >= 0 && pick->frame >= 0)
			{
				most = std::max(most, pick->frame - shown);
				const long long target = shown + std::max(1LL, (long long)std::llround(perHnFrame));
				for (auto it = g_shadowFrames.rbegin(); it != g_shadowFrames.rend(); ++it)
				{
					pick = &*it;
					if (it->frame <= target)
						break;
				}
			}
			if (GetTickCount() > nextLog)
			{
				nextLog = GetTickCount() + 5000;
				log(L"shadows: posed from Minecraft frame " + std::to_wstring(pick->frame) + L", newest " + std::to_wstring(g_shadowFrames.back().frame) +
					L", the picture shown is frame " + std::to_wstring(shown) + L" (most ahead in 5 s: " + std::to_wstring(most) + L"; " +
					std::to_wstring(perHnFrame).substr(0, 4) + L" Minecraft frames per HN frame)");
				most = 0;
			}
			g_shadowBoxes = pick->boxes;
			g_shadowKinds = pick->kinds;
			g_shadowColours = pick->colours;
			g_shadowBoxesAt = pick->at;
			if (pick->hasHand)
			{
				std::copy(pick->hand, pick->hand + 12, g_hand3p);
				g_hand3pAt = pick->at;
			}
		}

		bool shadow_boxes_fresh()
		{
			return GetTickCount() - g_shadowBoxesAt < 500;
		}
		bool hand3p_fresh()
		{
			return GetTickCount() - g_hand3pAt < 500;
		}

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

		void quat_of(const Rot3f &r, float q[4]);
		void place_actor_rot(UObject *actor, const Vec3f &at, const Rot3f &rot);
		void set_actor_scale(UObject *actor, const Vec3f &scale);

		// ---- shadows of Minecraft's blocks in HN's own lighting -----------------------------------------------------
		// Minecraft's blocks are a composited picture: HN's lights never saw them, so a placed block cast no shadow on HN's
		// floor and walls. Each full opaque block gets an invisible stand-in in HN's world: Unreal's own 1 m cube
		// (/Engine/BasicShapes/Cube, shipped with HN's content), hidden but casting shadows (bCastHiddenShadow), without
		// collision. HN's lights, shadow maps and shadow filtering then shadow it like any HN object: direction, softness
		// and intensity are HN's. (SkyCraft, chasmlol/SkyCraft MIT, gets there by drawing Minecraft's blocks into Skyrim's
		// sun shadow map from a hook, skse/src/WorldRender.cpp DepthVS/SunShadowRenderHook; Unreal does it for any
		// primitive in the scene.) Glass, slabs, torches (not full opaque cubes) get none.

		std::unordered_map<unsigned long long, UObject *> g_shadows; // block -> its shadow stand-in
		bool g_proxiesShown = false; // F6: the stand-ins drawn in the picture as well (to see where they are)
		/// A stand-in seen by the camera or not: hidden (shadow only) normally; with F6 also drawn, to check its place and
		/// size. Hidden in game, its shadow stays (bCastHiddenShadow).
		void proxy_main_pass(UObject *actor, bool shown)
		{
			if (UObject **mesh = actor ? actor->GetValuePtrByPropertyNameInChain<UObject *>(STR("StaticMeshComponent")) : nullptr; mesh && *mesh)
				Call(*mesh, STR("SetHiddenInGame")).set_bool(STR("NewHidden"), !shown).set_bool(STR("bPropagateToChildren"), false).invoke();
		}
		constexpr size_t kMaxShadows = 1500;

		/// Unreal's 1 m cube: loaded already, else through the asset registry (Blueprint-callable, shipped).
		UObject *shadow_cube()
		{
			if (UObject *cube = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Engine/BasicShapes/Cube.Cube")))
				return cube;
			static bool failed = false;
			if (failed)
				return nullptr;
			UObject *helpers = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/AssetRegistry.Default__AssetRegistryHelpers"));
			auto fail = [&](const wchar_t *why) -> UObject * {
				failed = true;
				log(std::wstring(L"shadows: no cube mesh (") + why + L"): Minecraft's blocks cast no shadows in HN");
				return nullptr;
			};
			if (!helpers)
				return fail(L"no AssetRegistryHelpers");
			Call reg(helpers, STR("GetAssetRegistry"));
			UObject *const *registry = reg.invoke() ? reg.param<UObject *>(STR("ReturnValue")) : nullptr; // TScriptInterface: object first
			if (!registry || !*registry)
				return fail(L"no asset registry");
			Call find(*registry, STR("GetAssetByObjectPath"));
			find.set(STR("ObjectPath"), name_param(STR("/Engine/BasicShapes/Cube.Cube"))).set_bool(STR("bIncludeOnlyOnDiskAssets"), false);
			FProperty *data = find.property(STR("ReturnValue"));
			if (!data || !find.invoke())
				return fail(L"GetAssetByObjectPath");
			Call get(helpers, STR("GetAsset"));
			FProperty *in = get.property(STR("InAssetData"));
			if (!in || in->GetSize() != data->GetSize())
				return fail(L"GetAsset's parameters");
			std::memcpy(get.data() + in->GetOffset_Internal(), find.data() + data->GetOffset_Internal(), size_t(data->GetSize()));
			UObject *const *cube = get.invoke() ? get.param<UObject *>(STR("ReturnValue")) : nullptr;
			if (!cube || !*cube)
				return fail(L"the registry has no such asset");
			log(L"shadows: loaded " + (*cube)->GetFullName());
			return *cube;
		}

		bool get_bool_property(UObject *o, const TCHAR *name, bool &value)
		{
			auto *p = static_cast<FBoolProperty *>(o ? o->GetPropertyByNameInChain(name) : nullptr);
			if (!p)
				return false;
			value = p->GetPropertyValue(p->ContainerPtrToValuePtr<void>(o));
			return true;
		}

		/// A box that only casts shadows: Unreal's 1 m cube at `centre`, `size` (m per axis) and `rot`, movable (dynamic
		/// shadows), no collision, hidden from the camera (its component bHiddenInGame) and casting all the same
		/// (bCastHiddenShadow: Unreal's own invisible shadow caster). History, with the evidence:
		///  - 12:00: the actor hidden + bCastHiddenShadow, but the stand-in was static (SetMobility(2) hadn't taken,
		///    readback "mobility 0"): a static primitive made at runtime has no baked shadow, so none was seen;
		///  - 13:22: movable, SetRenderInMainPass(false) instead of hidden: still none. The 17:4x screenshots with
		///    ReShade off (stand-ins only, Minecraft's picture gone) settled it: with the stand-ins drawn (F6) the sun
		///    cast Steve's and the blocks' shadows on the road and curb; with them out of the main pass, no shadow at
		///    all. In this build (UE 4.20) a primitive out of the main pass is out of the sun's shadow too.
		/// The stand-ins' material where HN draws them (receivers): HN's own plain opaque material (base_m, its
		/// Diffuse texture) with a flat mid-grey texture the mod writes and imports, so HN lights them exactly as its
		/// own surfaces (its sun, its shadows, its ambient) and the compositor reads that light off the picture
		/// (NeighborCraft.fx: Minecraft's colour times it): a roof's shadow edge crosses a block or Steve as it crosses
		/// the ground (the user). Once per level; nullptr if any step fails (the stand-ins stay shadow-only).
		std::wstring module_dir();
		UObject *g_receiverMaterial = nullptr; // this level's (shadow_extras_reset forgets it)
		UObject *g_receiverTexture = nullptr;
		bool g_receiverTried = false;
		UObject *spawn_shadow_box(UObject *context, const Vec3f &centre, const Vec3f &size, const Rot3f &rot);
		/// HN's base_m with a texture of one colour (a PNG the mod writes, imported), kept alive by a keeper stand-in
		/// (hidden, far below) that always uses it: nothing else of the engine's held such a material, and the garbage
		/// collector freed it; the next SetMaterial read a freed material and the game crashed (19:11 and 19:2x on
		/// 2026-10-11, on F5, when Steve's boxes start being drawn). UE4SS's SetRootSet didn't keep it in this build. The
		/// keeper dies with its level; the pool's level change makes new materials (shadow_boxes_tick).
		UObject *new_receiver_material(UObject *context, const std::wstring &file, const std::string &png)
		{
			if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
				if (FILE *f = _wfopen(file.c_str(), L"wb"))
				{
					fwrite(png.data(), 1, png.size(), f);
					fclose(f);
				}
			static UObject *rendering = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
			static UObject *matLib = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetMaterialLibrary"));
			UObject *parent = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Game/Objects/Materials/Base/base_m.base_m"));
			if (!rendering || !matLib || !parent)
			{
				log(L"receivers: no material (base_m " + std::wstring(parent ? L"found" : L"missing") + L"): stand-ins stay shadow-only");
				return nullptr;
			}
			Call import(rendering, STR("ImportFileAsTexture2D"));
			import.set<UObject *>(STR("WorldContextObject"), context).set(STR("Filename"), FString(file.c_str()));
			UObject *const *texture = import.invoke() ? import.param<UObject *>(STR("ReturnValue")) : nullptr;
			Call make(matLib, STR("CreateDynamicMaterialInstance"));
			// its outer: the material library's default object (permanent), not a level's actor (it would keep its outer,
			// and with it an old level, alive)
			make.set<UObject *>(STR("WorldContextObject"), matLib).set<UObject *>(STR("Parent"), parent).set(STR("OptionalName"), NameParam{});
			UObject *const *mid = make.invoke() ? make.param<UObject *>(STR("ReturnValue")) : nullptr;
			if (!texture || !*texture || !mid || !*mid)
			{
				log(L"receivers: the texture or the material couldn't be made (" + file + L")");
				return nullptr;
			}
			Call(*mid, STR("SetTextureParameterValue")).set(STR("ParameterName"), name_param(STR("Diffuse"))).set<UObject *>(STR("Value"), *texture).invoke();
			if (UObject *keeper = spawn_shadow_box(context, Vec3f{g_eye.x, g_eye.y, g_eye.z - 100000.0f}, Vec3f{0.01f, 0.01f, 0.01f}, Rot3f{}))
				if (UObject **km = keeper->GetValuePtrByPropertyNameInChain<UObject *>(STR("StaticMeshComponent")); km && *km)
				{
					Call(*km, STR("SetMaterial")).set<int32_t>(STR("ElementIndex"), 0).set<UObject *>(STR("Material"), *mid).invoke();
					Call(*km, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), false).invoke();
				}
			return *mid;
		}

		/// A 4x4 PNG of one colour (stored deflate blocks: no compressor needed).
		std::string solid_png(uint8_t r, uint8_t g, uint8_t b)
		{
			static uint32_t table[256];
			if (!table[1])
				for (uint32_t n = 0; n < 256; ++n)
				{
					uint32_t c = n;
					for (int k = 0; k < 8; ++k)
						c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
					table[n] = c;
				}
			auto crc = [&](const std::string &d) {
				uint32_t c = 0xFFFFFFFFu;
				for (unsigned char ch : d)
					c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
				return c ^ 0xFFFFFFFFu;
			};
			auto be32 = [](std::string &o, uint32_t v) {
				for (int k = 3; k >= 0; --k)
					o += char((v >> (k * 8)) & 0xFF);
			};
			auto chunk = [&](std::string &o, const char *type, const std::string &data) {
				be32(o, uint32_t(data.size()));
				const std::string body = std::string(type, 4) + data;
				o += body;
				be32(o, crc(body));
			};
			std::string raw;
			for (int y = 0; y < 4; ++y)
			{
				raw += char(0);
				for (int x = 0; x < 4; ++x)
					raw += char(r), raw += char(g), raw += char(b);
			}
			uint32_t a = 1, bsum = 0;
			for (unsigned char ch : raw)
				a = (a + ch) % 65521, bsum = (bsum + a) % 65521;
			const uint16_t len = uint16_t(raw.size());
			std::string z;
			z += char(0x78), z += char(0x01), z += char(0x01); // zlib header, one final stored block
			z += char(len & 0xFF), z += char(len >> 8), z += char(~len & 0xFF), z += char((~len >> 8) & 0xFF);
			z += raw;
			be32(z, (bsum << 16) | a);
			std::string ihdr;
			be32(ihdr, 4), be32(ihdr, 4);
			ihdr += char(8), ihdr += char(2), ihdr += char(0), ihdr += char(0), ihdr += char(0); // 8-bit RGB
			std::string png("\x89PNG\r\n\x1a\n", 8);
			chunk(png, "IHDR", ihdr);
			chunk(png, "IDAT", z);
			chunk(png, "IEND", std::string());
			return png;
		}

		/// The grey receiver (Steve's boxes, blocks without a colour).
		UObject *receiver_material(UObject *context)
		{
			if (g_receiverTried)
				return g_receiverMaterial;
			g_receiverTried = true;
			g_receiverMaterial = new_receiver_material(context, module_dir() + L"neighborcraft_grey.png", solid_png(128, 128, 128));
			if (g_receiverMaterial)
				log(L"receivers: HN's base_m with a flat grey texture: stand-ins of blocks and models are drawn by HN to light them");
			return g_receiverMaterial;
		}

		/// A block's colour for its receiver: the block's hue at the grey's brightness. The compositor reads the receiver's
		/// brightness as the light on the block (NeighborCraft.fx; ReceiverReference is the grey's), so the colour is scaled
		/// until its luma (as the compositor measures it, on sRGB values) is the grey's; what peeks past Minecraft's picture
		/// at an edge then has the block's colour, not a pale grey line (the user: "make them look very similar").
		uint32_t receiver_colour(uint32_t rgb)
		{
			auto lin = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
			auto enc = [](double c) { c = std::clamp(c, 0.0, 1.0); return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; };
			const double l[3] = {lin(((rgb >> 16) & 0xFF) / 255.0), lin(((rgb >> 8) & 0xFF) / 255.0), lin((rgb & 0xFF) / 255.0)};
			const double target = 128.0 / 255.0, grey = lin(target);
			auto luma_at = [&](double k, double mix, double out[3]) {
				for (int i = 0; i < 3; ++i)
					out[i] = enc((l[i] * (1.0 - mix) + grey * mix) * k);
				return 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2];
			};
			double out[3];
			// towards the grey until the colour can reach the grey's brightness without clipping (a very dark or very
			// saturated colour), then the scale that matches it
			for (double mix = 0.0; mix <= 1.0; mix += 0.125)
			{
				if (luma_at(64.0, mix, out) < target)
					continue;
				double lo = 0.0, hi = 64.0;
				for (int it = 0; it < 40; ++it)
				{
					const double k = (lo + hi) * 0.5;
					(luma_at(k, mix, out) < target ? lo : hi) = k;
				}
				luma_at(hi, mix, out);
				auto byte = [](double c) { return uint32_t(std::lround(c * 255.0)); };
				return byte(out[0]) << 16 | byte(out[1]) << 8 | byte(out[2]);
			}
			return 0x808080;
		}

		std::unordered_map<uint32_t, UObject *> g_colourMaterials; // this level's, by colour (shadow_extras_reset forgets them)

		/// The receiver material in a block's colour (0xRRGGBB, Minecraft's map colour; anything above 24 bits: grey).
		UObject *receiver_material(UObject *context, uint32_t rgb)
		{
			if (rgb > 0xFFFFFF || !receiver_material(context))
				return receiver_material(context);
			const uint32_t c = receiver_colour(rgb);
			if (auto it = g_colourMaterials.find(c); it != g_colourMaterials.end())
				return it->second ? it->second : g_receiverMaterial;
			wchar_t name[64];
			swprintf(name, 64, L"neighborcraft_rgb_%06x.png", c);
			UObject *m = new_receiver_material(context, module_dir() + name, solid_png(uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)));
			g_colourMaterials[c] = m;
			if (m)
				log(L"receivers: a material in " + std::wstring(name) + L" (Minecraft's colour " + std::to_wstring(rgb) + L")");
			return m ? m : g_receiverMaterial;
		}

		/// A stand-in drawn by HN (receiver: its grey material, visible) or only casting (hidden).
		void stand_in_receives(UObject *actor, bool receive, uint32_t rgb = 0xFFFFFFFFu)
		{
			UObject **mesh = actor ? actor->GetValuePtrByPropertyNameInChain<UObject *>(STR("StaticMeshComponent")) : nullptr;
			if (!mesh || !*mesh)
				return;
			if (receive)
				Call(*mesh, STR("SetMaterial")).set<int32_t>(STR("ElementIndex"), 0).set<UObject *>(STR("Material"), receiver_material(actor, rgb)).invoke();
			Call(*mesh, STR("SetHiddenInGame")).set_bool(STR("NewHidden"), !receive && !g_proxiesShown).set_bool(STR("bPropagateToChildren"), false).invoke();
		}

		UObject *spawn_shadow_box(UObject *context, const Vec3f &centre, const Vec3f &size, const Rot3f &rot)
		{
			static UObject *meshActor = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.StaticMeshActor"));
			UObject *statics = gameplay_statics(), *cube = shadow_cube();
			if (!meshActor || !statics || !cube)
				return nullptr;
			Transform4 t;
			t.pos[0] = centre.x, t.pos[1] = centre.y, t.pos[2] = centre.z;
			quat_of(rot, t.rot);
			t.scale[0] = size.x, t.scale[1] = size.y, t.scale[2] = size.z;
			Call begin(statics, STR("BeginDeferredActorSpawnFromClass"));
			begin.set<UObject *>(STR("WorldContextObject"), context).set<UObject *>(STR("ActorClass"), meshActor).set(STR("SpawnTransform"), t)
				.set<uint8_t>(STR("CollisionHandlingOverride"), 1).set<UObject *>(STR("Owner"), nullptr);
			UObject *const *actor = begin.invoke() ? begin.param<UObject *>(STR("ReturnValue")) : nullptr;
			if (!actor || !*actor)
				return nullptr;
			UObject **mesh = (*actor)->GetValuePtrByPropertyNameInChain<UObject *>(STR("StaticMeshComponent"));
			if (mesh && *mesh)
			{
				// movable: a static primitive gets its shadows only from baked lighting, which a stand-in made now has
				// none of: the 12:42 session's readback said "mobility 0" after SetMobility(2) (the call didn't take), and
				// no shadow was seen. The property is written before the actor is finished (as its constructor would),
				// then SetMobility again, and the readback below says what the engine has.
				if (uint8_t *mobility = (*mesh)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mobility")))
					*mobility = 2;
				Call(*mesh, STR("SetMobility")).set<uint8_t>(STR("NewMobility"), 2).invoke();
				Call(*mesh, STR("SetStaticMesh")).set<UObject *>(STR("NewMesh"), cube).invoke();
				// no collision, three ways (a call that doesn't take is silent: SetMobility didn't): the profile, every channel
				// ignored, collision off. With collision the world probe found them as ground: HN's "ground" inside
				// Minecraft's own blocks, which Minecraft waited for and held Steve 5 s (the user: "freezing randomly")
				Call(*mesh, STR("SetCollisionProfileName")).set(STR("InCollisionProfileName"), name_param(STR("NoCollision"))).invoke();
				Call(*mesh, STR("SetCollisionResponseToAllChannels")).set<uint8_t>(STR("NewResponse"), 0).invoke(); // ECR_Ignore
				Call(*mesh, STR("SetCollisionEnabled")).set<uint8_t>(STR("NewType"), 0).invoke(); // no collision
				Call(*mesh, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), true).invoke();
				set_bool_property(*mesh, STR("bCastDynamicShadow"), true);
				set_bool_property(*mesh, STR("bCastHiddenShadow"), true);
				set_bool_property(*mesh, STR("bAffectDynamicIndirectLighting"), false);
				Call(*mesh, STR("SetHiddenInGame")).set_bool(STR("NewHidden"), !g_proxiesShown).set_bool(STR("bPropagateToChildren"), false).invoke();
			}
			Call(statics, STR("FinishSpawningActor")).set<UObject *>(STR("Actor"), *actor).set(STR("SpawnTransform"), t).invoke();
			Call(*actor, STR("SetActorEnableCollision")).set_bool(STR("bNewActorEnableCollision"), false).invoke();
			if (mesh && *mesh)
			{
				Call(*mesh, STR("SetMobility")).set<uint8_t>(STR("NewMobility"), 2).invoke();
				if (uint8_t *mobility = (*mesh)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mobility")); mobility && *mobility != 2)
				{
					*mobility = 2; // as a last resort, and the render state rebuilt with it
					Call(*mesh, STR("MarkRenderStateDirty")).invoke();
				}
			}
			static bool reported = false;
			if (!reported && mesh && *mesh)
			{
				reported = true;
				bool cast = false, dynamic = false, mainPass = true, hidden = false;
				get_bool_property(*mesh, STR("CastShadow"), cast);
				get_bool_property(*mesh, STR("bCastDynamicShadow"), dynamic);
				get_bool_property(*mesh, STR("bRenderInMainPass"), mainPass);
				get_bool_property(*mesh, STR("bHiddenInGame"), hidden);
				Call ce(*mesh, STR("GetCollisionEnabled"));
				const uint8_t *collision = ce.invoke() ? ce.param<uint8_t>(STR("ReturnValue")) : nullptr;
				log(L"shadows: a stand-in's collision as the engine has it: " + (collision ? std::to_wstring(*collision) : std::wstring(L"?")) + L" (0: none)");
				const uint8_t *mobility = (*mesh)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mobility"));
				log(L"shadows: a stand-in as the engine has it: CastShadow " + std::to_wstring(cast) + L", bCastDynamicShadow " +
					std::to_wstring(dynamic) + L", bRenderInMainPass " + std::to_wstring(mainPass) + L", hidden " + std::to_wstring(hidden) +
					L", mobility " + (mobility ? std::to_wstring(*mobility) : L"?"));
			}
			return *actor;
		}

		struct PendingReceiver
		{
			UObject *actor;
			DWORD at;
			uint32_t rgb;
		};
		std::vector<PendingReceiver> g_pendingReceivers;

		/// New block stand-ins become receivers when their time comes (still there: not removed meanwhile).
		void pending_receivers_tick()
		{
			const DWORD now = GetTickCount();
			for (auto it = g_pendingReceivers.begin(); it != g_pendingReceivers.end();)
			{
				if (now < it->at)
				{
					++it;
					continue;
				}
				for (auto &[key, actor] : g_shadows)
					if (actor == it->actor)
					{
						stand_in_receives(actor, true, it->rgb);
						break;
					}
				it = g_pendingReceivers.erase(it);
			}
		}

		void capture_test_cube(UObject *a); // the scene-capture prototype's test cube (below)
		UObject *spawn_shadow(UObject *context, const Vec3f &centre, uint32_t rgb)
		{
			// exactly a block: flush with its neighbours (a 0.99 stand-in left 1 cm gaps, through which the compositor read
			// the ground's light: seams across walls, the user)
			UObject *a = spawn_shadow_box(context, centre, Vec3f{1.0f, 1.0f, 1.0f}, Rot3f{});
			// drawn (a receiver) only once Minecraft's picture has the block: a new block reaches Minecraft's picture a few
			// frames after HN hears of it, and the grey stand-in showed in between (the user)
			if (a && receiver_material(context))
				g_pendingReceivers.push_back({a, GetTickCount() + 100, rgb});
			if (a)
				capture_test_cube(a);
			return a;
		}

		// ---- scene-capture receiver prototype (2026-10-13) -------------------------------------------------------
		// The receivers (stand-ins HN draws so the compositor can read HN's light on Minecraft's surfaces) show wherever
		// Minecraft's picture doesn't cover them. The experiment: hide ONE receiver from the player's camera only
		// (APlayerController.HiddenActors) and let a scene capture that follows the camera see it, lit by HN with its
		// shadows. Off unless dlls/neighborcraft_capture_test.txt exists ("hide" in it: the test cube hidden from the
		// player; else shown, for comparing the capture with the screen). Rollback: delete the file (or git tag
		// receivers-visible-baseline).
		struct CaptureTest
		{
			bool checked = false, on = false, hide = false;
			UObject *level = nullptr;   // level_key
			WeakRef cube;        // the test receiver (the first block stand-in placed while on)
			WeakRef actor;       // the SceneCapture2D
			WeakRef component;   // its CaptureComponent2D
			WeakRef target;      // its TextureRenderTarget2D
			bool hidden = false;        // the cube is in the player controller's HiddenActors
			DWORD nextLog = 0, nextExport = 0, nextRead = 0;
		} g_cap;
		constexpr int kCapW = 480, kCapH = 270; // a size nothing else in HN uses: the compositor could find it by it

		/// A field of the capture component: by name, else at its offset in this build (CXXHeaderDump, Engine.hpp
		/// USceneCaptureComponent(2D)): the name lookup came back empty for FOVAngle in game, and writing through it
		/// crashed HN (19:20:57, bridge.cpp capture_test_tick).
		template <typename T>
		T *capture_field(UObject *component, const TCHAR *name, size_t offset)
		{
			if (!component)
				return nullptr;
			if (T *p = component->GetValuePtrByPropertyNameInChain<T>(name))
				return p;
			if (!is_a(component, L"SceneCaptureComponent2D"))
				return nullptr;
			static std::unordered_set<std::wstring> logged;
			if (logged.insert(name).second)
				log(std::wstring(L"capture test: ") + name + L" not found by name: at its offset " + std::to_wstring(offset));
			return reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(component) + offset);
		}

		bool capture_test_on()
		{
			// read again every 5 s: "show" and "hide" switch live (A/B in one session)
			if (g_cap.checked && g_cap.on && GetTickCount() > g_cap.nextRead)
			{
				g_cap.nextRead = GetTickCount() + 5000;
				std::ifstream f(module_dir() + L"neighborcraft_capture_test.txt");
				std::string word;
				if (f)
					f >> word;
				if ((word == "hide") != g_cap.hide)
				{
					g_cap.hide = word == "hide";
					log(std::wstring(L"capture test: now ") + (g_cap.hide ? L"hiding" : L"showing") + L" the test cube");
				}
			}
			if (!g_cap.checked)
			{
				g_cap.checked = true;
				std::ifstream f(module_dir() + L"neighborcraft_capture_test.txt");
				g_cap.on = bool(f);
				std::string word;
				if (f)
					f >> word;
				g_cap.hide = word == "hide";
				if (g_cap.on)
					log(std::wstring(L"capture test: on, the test cube ") + (g_cap.hide ? L"hidden from the player's camera" : L"shown (comparison)"));
			}
			return g_cap.on;
		}

		/// Adds `actor` to the controller's HiddenActors (an engine-owned TArray<AActor*>: grown with the engine's own
		/// allocator, as the engine frees it with its own on a level change).
		bool hide_from_player(UObject *pc, UObject *actor)
		{
			struct RawArray
			{
				UObject **data;
				int32_t num, max;
			};
			auto *a = pc->GetValuePtrByPropertyNameInChain<RawArray>(STR("HiddenActors"));
			if (!a || !GMalloc || !*GMalloc)
				return false;
			for (int32_t i = 0; i < a->num; ++i)
				if (a->data[i] == actor)
					return true;
			if (a->num == a->max)
			{
				const int32_t max = a->max * 2 + 4;
				void *grown = (*GMalloc)->Realloc(a->data, size_t(max) * sizeof(UObject *));
				if (!grown)
					return false;
				a->data = static_cast<UObject **>(grown);
				a->max = max;
			}
			a->data[a->num++] = actor;
			return true;
		}

		void capture_test_cube(UObject *a)
		{
			// the newest block placed (the first one spawned was an old block synced in at load, far off)
			if (capture_test_on())
			{
				g_cap.cube = a;
				g_cap.hidden = false;
				// evidence for the weak pointers: does UE4SS's agree with the object array's slot?
				const FWeakObjectPtr theirs(a);
				log(L"capture test: the test cube is " + a->GetName() + L" (UE4SS's weak pointer: " +
					(theirs.Get() == a ? L"the same object" : theirs.Get() ? L"A DIFFERENT OBJECT" : L"null") + L"; the slot check: " +
					(g_cap.cube.Get() == a ? L"the same object" : L"null") + L")");
			}
		}

		/// Takes `actor` out of the controller's HiddenActors again.
		void unhide_from_player(UObject *pc, UObject *actor)
		{
			struct RawArray
			{
				UObject **data;
				int32_t num, max;
			};
			auto *a = pc->GetValuePtrByPropertyNameInChain<RawArray>(STR("HiddenActors"));
			if (!a)
				return;
			for (int32_t i = 0; i < a->num; ++i)
				if (a->data[i] == actor)
				{
					a->data[i] = a->data[--a->num];
					return;
				}
		}

		void capture_test_tick(UObject *pc, UObject *pawn)
		{
			if (!capture_test_on())
				return;
			if (g_cap.level != level_key(pc))
			{
				const WeakRef cube = g_cap.cube; // a block placed before the first tick here is still the test cube
				g_cap = CaptureTest{true, true, g_cap.hide};
				g_cap.level = level_key(pc);
				g_cap.cube = cube;
			}
			UObject *cube = g_cap.cube.Get();
			if (!cube)
				return; // waiting for the first block placed
			if (!g_cap.actor.Get())
			{
				static UObject *captureClass = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.SceneCapture2D"));
				static UObject *rendering = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
				UObject *statics = gameplay_statics();
				if (!captureClass || !rendering || !statics)
				{
					log(L"capture test: no SceneCapture2D class / rendering library");
					g_cap.on = false;
					return;
				}
				Transform4 t;
				Call begin(statics, STR("BeginDeferredActorSpawnFromClass"));
				begin.set<UObject *>(STR("WorldContextObject"), pawn).set<UObject *>(STR("ActorClass"), captureClass).set(STR("SpawnTransform"), t)
					.set<uint8_t>(STR("CollisionHandlingOverride"), 1).set<UObject *>(STR("Owner"), nullptr);
				UObject *const *actor = begin.invoke() ? begin.param<UObject *>(STR("ReturnValue")) : nullptr;
				if (!actor || !*actor)
				{
					log(L"capture test: the SceneCapture2D couldn't be spawned");
					g_cap.on = false;
					return;
				}
				Call(statics, STR("FinishSpawningActor")).set<UObject *>(STR("Actor"), *actor).set(STR("SpawnTransform"), t).invoke();
				UObject **component = (*actor)->GetValuePtrByPropertyNameInChain<UObject *>(STR("CaptureComponent2D"));
				Call make(rendering, STR("CreateRenderTarget2D"));
				make.set<UObject *>(STR("WorldContextObject"), pawn).set<int32_t>(STR("Width"), kCapW).set<int32_t>(STR("Height"), kCapH).set<uint8_t>(STR("Format"), 5); // RTF_RGBA16f (4.20 exports float targets only)
				UObject *const *target = make.invoke() ? make.param<UObject *>(STR("ReturnValue")) : nullptr;
				if (!component || !*component || !target || !*target)
				{
					log(L"capture test: no capture component or render target");
					g_cap.on = false;
					return;
				}
				UObject **texture = capture_field<UObject *>(*component, STR("TextureTarget"), 0x2E0);
				uint8_t *source = capture_field<uint8_t>(*component, STR("CaptureSource"), 0x2E8);
				bool *everyFrame = capture_field<bool>(*component, STR("bCaptureEveryFrame"), 0x280);
				if (!texture || !source || !everyFrame)
				{
					log(L"capture test: the capture component's fields weren't found: off");
					g_cap.on = false;
					return;
				}
				*texture = *target;
				*source = 2; // SCS_FinalColorLDR: as the screen
				*everyFrame = true;
				g_cap.actor = *actor;
				g_cap.component = *component;
				g_cap.target = *target;
				log(L"capture test: " + (*actor)->GetFullName() + L" -> " + (*target)->GetFullName() + L" (" + std::to_wstring(kCapW) + L"x" +
					std::to_wstring(kCapH) + L", final colour)");
			}
			UObject *actor = g_cap.actor.Get(), *component = g_cap.component.Get(), *target = g_cap.target.Get();
			if (!actor || !component || !target)
				return;
			// the test cube: hidden from the player's camera only (the capture has its own hidden list)
			if (g_cap.hide && !g_cap.hidden)
			{
				g_cap.hidden = hide_from_player(pc, cube);
				log(std::wstring(L"capture test: the test cube ") + (g_cap.hidden ? L"is in the player controller's HiddenActors" : L"couldn't be hidden"));
			}
			else if (!g_cap.hide && g_cap.hidden)
			{
				unhide_from_player(pc, cube);
				g_cap.hidden = false;
				log(L"capture test: the test cube is shown to the player again");
			}
			// the capture follows the camera HN renders with
			Camera cam;
			if (!read_camera(pc, cam))
				return;
			Call(actor, STR("K2_SetActorLocationAndRotation")).set(STR("NewLocation"), cam.loc).set(STR("NewRotation"), cam.rot)
				.set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
			if (float *fov = capture_field<float>(component, STR("FOVAngle"), 0x2D4))
				*fov = cam.hfov;
			// diagnostics: the capture's pixel at the cube's centre, every 2 s; the whole capture as a file every 30 s
			if (GetTickCount() > g_cap.nextLog)
			{
				g_cap.nextLog = GetTickCount() + 2000;
				Vec3f at;
				const Vec3f *c = actor_location(cube, at) ? &at : nullptr;
				if (!c)
					log(L"capture test: the test cube's position couldn't be read");
				if (c)
				{
					const float d2r = 3.14159265f / 180.0f, cp = std::cos(cam.rot.pitch * d2r), sp = std::sin(cam.rot.pitch * d2r);
					const float cy = std::cos(cam.rot.yaw * d2r), sy = std::sin(cam.rot.yaw * d2r);
					const Vec3f f{cp * cy, cp * sy, sp}, r{-sy, cy, 0.0f}, u{-sp * cy, -sp * sy, cp};
					const Vec3f d{c->x - cam.loc.x, c->y - cam.loc.y, c->z - cam.loc.z};
					const float df = d.x * f.x + d.y * f.y + d.z * f.z, dr = d.x * r.x + d.y * r.y + d.z * r.z, du = d.x * u.x + d.y * u.y + d.z * u.z;
					const float t = std::tan(cam.hfov * 0.5f * d2r);
					const float nx = df > 0.0f ? dr / df / t : 9.0f, ny = df > 0.0f ? du / df / (t * kCapH / kCapW) : 9.0f;
					if (std::abs(nx) < 1.0f && std::abs(ny) < 1.0f)
					{
						static UObject *rendering = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
						const int px = int((nx + 1.0f) * 0.5f * kCapW), py = int((1.0f - ny) * 0.5f * kCapH);
						Call read(rendering, STR("ReadRenderTargetRawPixel"));
						read.set<UObject *>(STR("WorldContextObject"), pawn).set<UObject *>(STR("TextureRenderTarget"), target).set<int32_t>(STR("X"), px).set<int32_t>(STR("Y"), py);
						const uint8_t *bgra = read.invoke() ? read.param<uint8_t>(STR("ReturnValue")) : nullptr;
						if (bgra)
							log(L"capture test: the cube's centre at capture pixel " + std::to_wstring(px) + L"," + std::to_wstring(py) + L" (" +
								std::to_wstring(int(df / 100.0f)) + L" m): RGB " + std::to_wstring(bgra[2]) + L" " + std::to_wstring(bgra[1]) + L" " +
								std::to_wstring(bgra[0]) + (g_cap.hidden ? L" (hidden from the player)" : L" (shown to the player)"));
					}
					else
						log(L"capture test: the cube is off the capture");
				}
			}
			if (GetTickCount() > g_cap.nextExport)
			{
				g_cap.nextExport = GetTickCount() + 30000;
				static UObject *rendering = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
				static int n = 0;
				const std::wstring name = L"capture_" + std::to_wstring(n++) + L".hdr";
				Call(rendering, STR("ExportRenderTarget")).set<UObject *>(STR("WorldContextObject"), pawn).set<UObject *>(STR("TextureRenderTarget"), target)
					.set(STR("FilePath"), FString(module_dir().c_str())).set(STR("FileName"), FString(name.c_str())).invoke(); // "FileName", as the icon export
				log(L"capture test: exported " + module_dir() + name);
			}
		}

		/// HN's lights as the engine has them, once per level: which can cast a dynamic shadow at all (a static light's
		/// shadows are baked: nothing movable, HN's own props included, gets one from it).
		void light_census(UObject *pawn)
		{
			static UObject *censusFor = nullptr;
			if (censusFor == pawn)
				return;
			censusFor = pawn;
			static const wchar_t *kinds[] = {L"PointLightComponent", L"SpotLightComponent", L"DirectionalLightComponent", L"RectLightComponent"};
			int byMobility[3] = {}, shadowing = 0, total = 0;
			struct Near
			{
				float d;
				std::wstring line;
			};
			std::vector<Near> nearest;
			for (const wchar_t *kind : kinds)
				for (UObject *l : find_all(kind))
				{
					++total;
					const uint8_t *mobility = l->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mobility"));
					bool cast = false, dynamic = false, visible = true;
					get_bool_property(l, STR("CastShadows"), cast);
					get_bool_property(l, STR("CastDynamicShadows"), dynamic);
					get_bool_property(l, STR("bVisible"), visible);
					if (mobility && *mobility < 3)
						++byMobility[*mobility];
					const bool dynamicShadow = cast && dynamic && mobility && *mobility >= 1 && visible;
					shadowing += dynamicShadow;
					if (dynamicShadow)
						log(L"shadows: a light that casts dynamic shadows: " + std::wstring(kind) + L" " + l->GetFullName() + L", mobility " +
							std::to_wstring(*mobility));
					Call w(l, STR("K2_GetComponentLocation"));
					const Vec3f *at = w.invoke() ? w.param<Vec3f>(STR("ReturnValue")) : nullptr;
					const float d = at ? std::sqrt((at->x - g_eye.x) * (at->x - g_eye.x) + (at->y - g_eye.y) * (at->y - g_eye.y) + (at->z - g_eye.z) * (at->z - g_eye.z)) : 1e9f;
					nearest.push_back({d, std::wstring(kind) + L" " + l->GetName() + L" at " + std::to_wstring(int(d / 100.0f)) + L" m: mobility " +
						(mobility ? std::to_wstring(*mobility) : L"?") + L", CastShadows " + std::to_wstring(cast) + L", CastDynamicShadows " +
						std::to_wstring(dynamic) + L", visible " + std::to_wstring(visible)});
				}
			log(L"shadows: HN's lights: " + std::to_wstring(total) + L" (static " + std::to_wstring(byMobility[0]) + L", stationary " +
				std::to_wstring(byMobility[1]) + L", movable " + std::to_wstring(byMobility[2]) + L"), " + std::to_wstring(shadowing) +
				L" able to cast dynamic shadows (movable objects get shadows only from those)");
			// the sun: a stationary directional light gives movable objects its sharp cascaded shadows only within its
			// DynamicShadowDistanceStationaryLight; beyond (or with 0) each movable primitive gets a small per-object
			// shadow, soft and blurred (the 18:12 screenshots: Steve's shadow a blob, the house's sharp). Within 40 m of
			// the camera the cascades take them all, at least 3 cascades.
			for (UObject *l : find_all(L"DirectionalLightComponent"))
			{
				const uint8_t *mobility = l->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mobility"));
				float *distance = l->GetValuePtrByPropertyNameInChain<float>(STR("DynamicShadowDistanceStationaryLight"));
				int32 *cascades = l->GetValuePtrByPropertyNameInChain<int32>(STR("DynamicShadowCascades"));
				bool cast = false;
				get_bool_property(l, STR("CastShadows"), cast);
				if (!mobility || *mobility != 1 || !distance || !cascades || !cast)
					continue;
				const float before = *distance;
				const int32 cascadesBefore = *cascades;
				if (*distance < 4000.0f)
					Call(l, STR("SetDynamicShadowDistanceStationaryLight")).set(STR("NewValue"), 4000.0f).invoke();
				if (*cascades < 3)
					Call(l, STR("SetDynamicShadowCascades")).set<int32>(STR("NewValue"), 3).invoke();
				log(L"shadows: the sun " + l->GetFullName() + L": stationary, dynamic shadow distance " + std::to_wstring(int(before)) + L" cm -> " +
					std::to_wstring(int(*distance)) + L", cascades " + std::to_wstring(cascadesBefore) + L" -> " + std::to_wstring(*cascades));
			}
			std::sort(nearest.begin(), nearest.end(), [](const Near &a, const Near &b) { return a.d < b.d; });
			for (size_t i = 0; i < nearest.size() && i < 6; ++i)
				log(L"shadows: near light: " + nearest[i].line);
		}

		/// Shadows indoors. HN's house lights are movable point and spot lights with shadows off (the 12:42 census: 120
		/// movable lights, 2 able to cast dynamic shadows, both outdoor): indoors nothing casts a dynamic shadow, HN's own
		/// props included, so neither can Steve or a block. The light that lights Steve most (in range, on, visible) is
		/// given shadows (LightComponent::SetCastShadows(true)) while he is near it, and given back when another light
		/// lights him more (by 30%) or the level changes: HN's own light then shadows everything near it in HN's way,
		/// Steve's and the blocks' stand-ins included. One light at a time (a point light's shadow is six views).
		struct LentLight
		{
			UObject *level = nullptr;                // level_key
			std::vector<WeakRef> lights;      // this level's point and spot lights (weak: a nightmare's go with it)
			WeakRef lent;                     // the light given shadows (it had none)
			float score = 0.0f;
			DWORD next = 0;
		} g_lent;
		constexpr bool kLendShadows = true;

		void lend_shadows_tick(UObject *pc)
		{
			LentLight &L = g_lent;
			if (!kLendShadows || !g_haveFeet)
				return;
			if (L.level != level_key(pc))
			{
				if (UObject *was = L.lent.Get())
					Call(was, STR("SetCastShadows")).set_bool(STR("bNewValue"), false).invoke();
				L = LentLight{};
				L.level = level_key(pc);
				for (const wchar_t *kind : {L"PointLightComponent", L"SpotLightComponent"})
					for (UObject *l : find_all(kind))
						L.lights.emplace_back(l);
			}
			if (GetTickCount() < L.next)
				return;
			L.next = GetTickCount() + 500;
			const Vec3f at{g_feet.x, g_feet.y, g_feet.z + g_steveHeight * 0.5f};
			UObject *best = nullptr;
			float bestScore = 0.0f, bestDist = 0.0f;
			UObject *const lent = L.lent.Get(); // nullptr once destroyed
			for (const WeakRef &weak : L.lights)
			{
				UObject *l = weak.Get();
				if (!l)
					continue; // destroyed (its level unloaded)
				bool visible = false, cast = false;
				get_bool_property(l, STR("bVisible"), visible);
				const float *intensity = l->GetValuePtrByPropertyNameInChain<float>(STR("Intensity"));
				const float *radius = l->GetValuePtrByPropertyNameInChain<float>(STR("AttenuationRadius"));
				if (!visible || !intensity || *intensity <= 0.0f || !radius || *radius <= 0.0f)
					continue;
				get_bool_property(l, STR("CastShadows"), cast);
				if (cast && l != lent)
					continue; // it casts already (HN's own): nothing to lend
				Call w(l, STR("K2_GetComponentLocation"));
				const Vec3f *p = w.invoke() ? w.param<Vec3f>(STR("ReturnValue")) : nullptr;
				if (!p)
					continue;
				const float d = std::hypot(p->x - at.x, p->y - at.y, p->z - at.z);
				if (d >= *radius)
					continue;
				const float falloff = 1.0f - d / *radius;
				const float score = *intensity * falloff * falloff;
				if (score > bestScore)
					best = l, bestScore = score, bestDist = d;
			}
			if (best == lent)
			{
				L.score = bestScore;
				return;
			}
			if (lent && best && bestScore < L.score * 1.3f)
				return; // not clearly brighter: no flicker between two lights
			if (lent)
			{
				Call(lent, STR("SetCastShadows")).set_bool(STR("bNewValue"), false).invoke();
				log(L"shadows: " + lent->GetName() + L" has no shadows again (as HN made it)");
			}
			L.lent = best;
			L.score = bestScore;
			if (best)
			{
				set_bool_property(best, STR("CastDynamicShadows"), true);
				Call(best, STR("SetCastShadows")).set_bool(STR("bNewValue"), true).invoke();
				bool now = false;
				get_bool_property(best, STR("CastShadows"), now);
				const uint8_t *mobility = best->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mobility"));
				log(L"shadows: HN's light " + best->GetFullName() + L" (mobility " + (mobility ? std::to_wstring(*mobility) : L"?") + L", " +
					std::to_wstring(int(bestDist)) + L" cm from Steve) casts shadows now: CastShadows " + std::to_wstring(now));
			}
		}

		/// Minecraft's drawn things, each a hidden shadow-casting cube of HN's (spawn_shadow_box): Steve's model as
		/// Minecraft poses it this frame, the items in hands, dropped items, other entities, particles (ShadowExport.java
		/// sends them every frame). A pool, grown as needed (24 a tick at most), each placed by one SetActorTransform;
		/// the ones not needed this frame stop casting. Plus, in first person, the held HN object's own box at Steve's
		/// hand (its mesh is on the camera there; in third person the object itself is in his hand and casts).
		struct ShadowPool
		{
			UObject *level = nullptr;
			std::vector<UObject *> boxes;
			std::vector<bool> casting;
			std::vector<int> receiving; // -1 unknown, 0 hidden, else drawn: 1 + its colour (0x1000000: grey)
			size_t used = 0;
		} g_pool;
		constexpr size_t kMaxPool = 420;

		void pool_cast(size_t i, bool on)
		{
			if (g_pool.casting[i] == on)
				return;
			g_pool.casting[i] = on;
			if (UObject **mesh = g_pool.boxes[i]->GetValuePtrByPropertyNameInChain<UObject *>(STR("StaticMeshComponent")); mesh && *mesh)
				Call(*mesh, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), on).invoke();
		}

		/// A box from Minecraft's centre and half-axes (blocks) as an Unreal transform of the 1 m cube: Unreal's axes
		/// are Minecraft's swapped (to_ue: x, z, y), which mirrors; the box is symmetric, so its third axis is taken as
		/// Z x X (a proper rotation) with the length of the one Minecraft gave.
		bool box_transform(const double *b, Transform4 &t)
		{
			const Vec3f o = to_ue(0.0, 0.0, 0.0), c = to_ue(b[0], b[1], b[2]);
			auto dir = [&](const double *v) {
				const Vec3f w = to_ue(v[0], v[1], v[2]);
				return Vec3f{w.x - o.x, w.y - o.y, w.z - o.z};
			};
			Vec3f X = dir(b + 3), Y = dir(b + 6), Z = dir(b + 9);
			auto len = [](const Vec3f &v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
			const float lx = len(X), ly = len(Y), lz = len(Z);
			if (lx < 0.01f || ly < 0.01f || lz < 0.01f)
				return false;
			X = {X.x / lx, X.y / lx, X.z / lx};
			// Z made perpendicular to X, then Y = Z x X
			const float d = Z.x * X.x + Z.y * X.y + Z.z * X.z;
			Z = {Z.x - X.x * d, Z.y - X.y * d, Z.z - X.z * d};
			const float lz2 = len(Z);
			if (lz2 < 1e-4f)
				return false;
			Z = {Z.x / lz2, Z.y / lz2, Z.z / lz2};
			const Vec3f Yn{Z.y * X.z - Z.z * X.y, Z.z * X.x - Z.x * X.z, Z.x * X.y - Z.y * X.x};
			// rotation matrix (columns X, Yn, Z) to a quaternion
			const float m00 = X.x, m10 = X.y, m20 = X.z, m01 = Yn.x, m11 = Yn.y, m21 = Yn.z, m02 = Z.x, m12 = Z.y, m22 = Z.z;
			float qw, qx, qy, qz;
			const float tr = m00 + m11 + m22;
			if (tr > 0.0f)
			{
				const float s2 = std::sqrt(tr + 1.0f) * 2.0f;
				qw = 0.25f * s2, qx = (m21 - m12) / s2, qy = (m02 - m20) / s2, qz = (m10 - m01) / s2;
			}
			else if (m00 > m11 && m00 > m22)
			{
				const float s2 = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
				qw = (m21 - m12) / s2, qx = 0.25f * s2, qy = (m01 + m10) / s2, qz = (m02 + m20) / s2;
			}
			else if (m11 > m22)
			{
				const float s2 = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
				qw = (m02 - m20) / s2, qx = (m01 + m10) / s2, qy = 0.25f * s2, qz = (m12 + m21) / s2;
			}
			else
			{
				const float s2 = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
				qw = (m10 - m01) / s2, qx = (m02 + m20) / s2, qy = (m12 + m21) / s2, qz = 0.25f * s2;
			}
			t.rot[0] = qx, t.rot[1] = qy, t.rot[2] = qz, t.rot[3] = qw;
			t.pos[0] = c.x, t.pos[1] = c.y, t.pos[2] = c.z;
			// the cube is 1 m: its scale is the box's full size in m (half-axis length in cm * 2 / 100)
			t.scale[0] = lx / 50.0f, t.scale[1] = ly / 50.0f, t.scale[2] = lz / 50.0f;
			return true;
		}

		void shadow_boxes_tick(UObject *pc, UObject *pawn)
		{
			ShadowPool &P = g_pool;
			if (P.level != pc)
			{
				P = ShadowPool{}; // the old level's boxes went with it
				P.level = pc;
				// a new level: a new material and keeper (the old ones went with the old level)
				g_receiverMaterial = g_receiverTexture = nullptr;
				g_receiverTried = false;
				g_colourMaterials.clear();
				receiver_material(pawn); // made now, not at the first block placed (an import and a material: a hitch)
			}
			std::vector<double> boxes = shadow_boxes_fresh() ? g_shadowBoxes : std::vector<double>{};
			// the held HN object in first person: its box at Steve's hand (g_heldBoxHalf, set by carry_tick)
			if (!boxes.empty() && g_mcFirstPerson && hand3p_fresh() && g_heldBoxHalf > 0.0)
			{
				const double h = g_heldBoxHalf, *f = g_hand3p;
				const double box[12] = {f[0], f[1], f[2], f[3] * h, f[4] * h, f[5] * h, f[6] * h, f[7] * h, f[8] * h, f[9] * h, f[10] * h, f[11] * h};
				boxes.insert(boxes.end(), box, box + 12);
			}
			const size_t want = std::min(boxes.size() / 12, kMaxPool);
			// grown ahead of need, 4 a tick up to 240 boxes, so that the first burst of particles (breaking a block)
			// doesn't spawn dozens of actors in one frame (the user: a stutter the first time); beyond, as needed
			const size_t ahead = std::min<size_t>(240, kMaxPool);
			const int perTick = P.boxes.size() < want ? 24 : 4;
			for (int grown = 0; P.boxes.size() < std::max(want, ahead) && grown < perTick; ++grown)
			{
				UObject *a = spawn_shadow_box(pawn, Vec3f{g_eye.x, g_eye.y, g_eye.z - 5000.0f}, Vec3f{0.1f, 0.1f, 0.1f}, Rot3f{});
				if (!a)
					break;
				P.boxes.push_back(a);
				P.casting.push_back(true);
				P.receiving.push_back(-1);
				pool_cast(P.boxes.size() - 1, false); // not used yet
			}
			size_t used = 0;
			for (size_t i = 0; i < want && i < P.boxes.size(); ++i)
			{
				Transform4 t;
				if (!box_transform(&boxes[i * 12], t))
					continue;
				Call(P.boxes[used], STR("K2_SetActorTransform")).set(STR("NewTransform"), t).set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
				pool_cast(used, true);
				// a model's surface is drawn (lit by HN for the compositor); the local player's only in third person: in first
				// person Minecraft doesn't draw him, and HN's grey boxes would be what one sees looking down
				const int kind = i < g_shadowKinds.size() ? g_shadowKinds[i] : 0;
				const int colour = i < g_shadowColours.size() ? g_shadowColours[i] : -1;
				// -2: a cube the skin leaves transparent (an empty outer layer): Minecraft draws nothing there, so HN
				// doesn't draw its stand-in either (it still casts)
				const bool draw = (kind == 1 || (kind == 2 && !g_mcFirstPerson)) && colour != -2 && receiver_material(pawn);
				const int receive = draw ? 1 + (colour >= 0 ? (colour & 0xFFFFFF) : 0x1000000) : 0;
				if (P.receiving[used] != receive)
				{
					P.receiving[used] = receive;
					// in the model's skin colour (at the grey's brightness): a stand-in a frame off Steve looks like him
					stand_in_receives(P.boxes[used], draw, colour >= 0 ? uint32_t(colour) : 0xFFFFFFFFu);
				}
				++used;
			}
			for (size_t i = used; i < P.boxes.size(); ++i)
			{
				pool_cast(i, false);
				if (P.receiving[i] > 0)
				{
					P.receiving[i] = 0;
					stand_in_receives(P.boxes[i], false);
				}
			}
			static size_t reported = size_t(-1);
			if ((used == 0) != (reported == 0) || (reported != size_t(-1) && used > reported + 50))
			{
				reported = used;
				log(L"shadows: " + std::to_wstring(used) + L" boxes of Minecraft's drawn things (Steve as posed, items, particles) cast HN shadows (" +
					std::to_wstring(P.boxes.size()) + L" in the pool)");
			}
			P.used = used;
		}

		/// The colour of HN's sun (its directional light's LightColor, following HN's day and night), once a second, to
		/// the compositor: Minecraft's surfaces are tinted by it. The compositor used to take the light's colour from
		/// the picture, which is mostly the surfaces' own colour: a cobblestone wall went green while the screen
		/// showed grass and beige while it showed sky and houses (the user: "bipolar"). The sun's colour doesn't
		/// change with where one looks.
		void light_colour_tick(UObject *pc)
		{
			static UObject *level = nullptr, *sun = nullptr;
			static DWORD next = 0;
			if (level != level_key(pc)) // the sun is in a streamed level (Act1_Day)
			{
				level = level_key(pc);
				sun = nullptr;
				for (UObject *l : find_all(L"DirectionalLightComponent"))
				{
					bool visible = false;
					get_bool_property(l, STR("bVisible"), visible);
					if (visible)
					{
						sun = l;
						break;
					}
				}
				log(sun ? L"light: Minecraft's tint from HN's sun " + sun->GetFullName() : std::wstring(L"light: no sun in this level: Minecraft untinted"));
			}
			if (GetTickCount() < next)
				return;
			next = GetTickCount() + 1000;
			float r = 1.0f, g = 1.0f, b = 1.0f;
			if (sun)
				if (const uint8_t *c = sun->GetValuePtrByPropertyNameInChain<uint8_t>(STR("LightColor"))) // FColor: B, G, R, A
					b = c[0] / 255.0f, g = c[1] / 255.0f, r = c[2] / 255.0f;
			compositor::set_light_colour(r, g, b);
			// and where it shines from: the light travels along its forward vector; towards the sun is the opposite, in
			// Minecraft's axes (Unreal x, y, z = Minecraft x, z, y)
			float strength = 0.0f;
			Vec3f toward{0.0f, 0.0f, 1.0f};
			if (sun)
			{
				Call f(sun, STR("GetForwardVector"));
				if (const Vec3f *fw = f.invoke() ? f.param<Vec3f>(STR("ReturnValue")) : nullptr)
					toward = {-fw->x, -fw->y, -fw->z};
				const float *intensity = sun->GetValuePtrByPropertyNameInChain<float>(STR("Intensity"));
				strength = intensity && *intensity > 0.0f && toward.z > 0.0f ? 1.0f : 0.0f; // below the horizon: no sun
			}
			compositor::set_sun(toward.x, toward.z, toward.y, strength);
			static int logged = 0;
			if (logged++ < 1 || (logged % 120) == 0)
			{
				char line[160];
				snprintf(line, sizeof(line), "light: towards HN's sun (Minecraft axes) %.2f %.2f %.2f, strength %.0f", toward.x, toward.z, toward.y, strength);
				log(std::wstring(line, line + strlen(line)));
			}
			static float lr = -1.0f, lg = -1.0f, lb = -1.0f;
			if (std::abs(r - lr) + std::abs(g - lg) + std::abs(b - lb) > 0.02f)
			{
				lr = r, lg = g, lb = b;
				char line[120];
				snprintf(line, sizeof(line), "light: HN's sun colour %.2f %.2f %.2f", r, g, b);
				log(std::wstring(line, line + strlen(line)));
			}
		}

		/// Steve's shadow: his model's boxes (Minecraft's player model: head 8 px, body 8x12x4, arms and legs 4x12x4; 32 px =
		/// his height) as shadow-only boxes at his feet, facing his way, legs and arms swinging with his walk. HN's player
		/// is a puppet without a shadow; Minecraft's Steve is a picture HN's lights never see.
		struct SteveShadow
		{
			UObject *level = nullptr;
			UObject *parts[6] = {};
			Vec3f lastFeet{};
			float phase = 0.0f, swing = 0.0f;
		} g_steveShadow;

		void steve_shadow_tick(UObject *pc, UObject *pawn)
		{
			SteveShadow &s = g_steveShadow;
			if (!g_haveFeet)
				return;
			// Minecraft's own poses arrive (shadow_boxes_tick): these approximate boxes stand aside (no shadow), and
			// come back if they stop
			const bool replaced = shadow_boxes_fresh();
			static bool wasReplaced = false;
			if (replaced != wasReplaced && s.level == pc)
			{
				wasReplaced = replaced;
				for (UObject *part : s.parts)
					if (part)
						if (UObject **mesh = part->GetValuePtrByPropertyNameInChain<UObject *>(STR("StaticMeshComponent")); mesh && *mesh)
							Call(*mesh, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), !replaced).invoke();
				log(replaced ? L"shadows: Steve's shadow from Minecraft's own pose now" : L"shadows: no poses from Minecraft: Steve's approximate shadow again");
			}
			if (replaced && s.level == pc)
				return;
			// part: centre (px: right, forward, up), size (px: right, forward, up), pivot height (px) and swing sign
			struct Part
			{
				float cx, cy, cz, sx, sy, sz, pivot, swing;
			};
			static const Part parts[6] = {
				{-2, 0, 6, 4, 4, 12, 12, 1},   // left leg
				{2, 0, 6, 4, 4, 12, 12, -1},   // right leg
				{0, 0, 18, 8, 4, 12, 0, 0},    // body
				{-6, 0, 18, 4, 4, 12, 22, -1}, // left arm
				{6, 0, 18, 4, 4, 12, 22, 1},   // right arm
				{0, 0, 28, 8, 8, 8, 0, 0},     // head
			};
			if (s.level != pc)
			{
				s = SteveShadow{};
				s.level = pc;
				for (int i = 0; i < 6; ++i)
					s.parts[i] = spawn_shadow_box(pawn, g_feet, Vec3f{0.04f, 0.04f, 0.04f}, Rot3f{});
				log(L"shadows: Steve's shadow made of " + std::to_wstring(std::count_if(std::begin(s.parts), std::end(s.parts), [](UObject *a) { return a != nullptr; })) + L" boxes");
			}
			// the walk: the swing grows with his speed and its phase with the distance walked (as Minecraft's limb swing)
			const float dx = g_feet.x - s.lastFeet.x, dy = g_feet.y - s.lastFeet.y;
			const float step = std::sqrt(dx * dx + dy * dy);
			s.lastFeet = g_feet;
			if (step < 200.0f) // not a teleport
			{
				s.phase += step * 0.045f;
				s.swing += ((step > 0.5f ? 32.0f : 0.0f) - s.swing) * 0.2f;
			}
			const bool crawling = g_steveHeight < 120.0f;
			const float px = std::max(g_steveHeight, 120.0f) / 32.0f; // cm per model pixel (5.6 standing)
			const float yaw = g_mcLookYaw + 90.0f, d2r = 3.14159265f / 180.0f;
			const Vec3f fwd{std::cos(yaw * d2r), std::sin(yaw * d2r), 0.0f}, right{-std::sin(yaw * d2r), std::cos(yaw * d2r), 0.0f};
			for (int i = 0; i < 6; ++i)
			{
				if (!s.parts[i])
					continue;
				const Part &p = parts[i];
				const float a = p.swing * s.swing * std::sin(s.phase) * d2r;
				// a limb turns about its top (hip or shoulder): its middle moves forward/back and up
				float cy = p.cy, cz = p.cz;
				if (p.swing != 0.0f)
				{
					const float half = p.sz * 0.5f;
					cy = std::sin(a) * half;
					cz = p.pivot - std::cos(a) * half;
				}
				const Vec3f at{g_feet.x + (right.x * p.cx + fwd.x * cy) * px, g_feet.y + (right.y * p.cx + fwd.y * cy) * px, g_feet.z + cz * px};
				place_actor_rot(s.parts[i], at, Rot3f{a / d2r, yaw, 0.0f});
				set_actor_scale(s.parts[i], Vec3f{p.sy * px / 100.0f, p.sx * px / 100.0f, crawling ? 0.0001f : p.sz * px / 100.0f});
			}
		}

		/// Steve's shadow boxes and the light given shadows go with the blocks' stand-ins: on a level change (they die
		/// with it: only forgotten) and when NeighborCraft stops driving (destroyed, the light as HN made it).
		void shadow_extras_reset(bool alive)
		{
			if (alive)
			{
				for (UObject *part : g_steveShadow.parts)
					if (part)
						Call(part, STR("K2_DestroyActor")).invoke();
				if (UObject *lent = g_lent.lent.Get())
					Call(lent, STR("SetCastShadows")).set_bool(STR("bNewValue"), false).invoke();
			}
			if (alive)
				for (UObject *box : g_pool.boxes)
					Call(box, STR("K2_DestroyActor")).invoke();
			g_pool = ShadowPool{};
			g_steveShadow = SteveShadow{};
			g_lent = LentLight{};
			g_receiverMaterial = g_receiverTexture = nullptr;
			g_receiverTried = false;
			g_colourMaterials.clear();
			g_pendingReceivers.clear();
		}

		void destroy_colliders(bool alive)
		{
			if (alive)
			{
				for (auto &[key, actor] : g_colliders)
					Call(actor, STR("K2_DestroyActor")).invoke();
				for (auto &[key, actor] : g_shadows)
					Call(actor, STR("K2_DestroyActor")).invoke();
			}
			g_colliders.clear();
			g_shadows.clear();
			g_probeIgnore.clear();
			shadow_extras_reset(alive);
		}

		/// {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]} from Minecraft: solid blocks placed and removed.
		void apply_blocks(UObject *context, const std::vector<int> &set, const std::vector<int> &clear, const std::vector<int> &opaque, const std::vector<int> &colours)
		{
			// shadows: gone with the block, or when it is no longer a full opaque cube; made for the opaque ones (drawn in
			// their colour: one per opaque block)
			std::unordered_map<unsigned long long, uint32_t> isOpaque;
			for (size_t i = 0; i + 2 < opaque.size(); i += 3)
				isOpaque[cell_key(opaque[i], opaque[i + 1], opaque[i + 2])] = i / 3 < colours.size() ? uint32_t(colours[i / 3]) : 0xFFFFFFFFu;
			auto dropShadow = [&](unsigned long long key) {
				if (auto it = g_shadows.find(key); it != g_shadows.end())
				{
					Call(it->second, STR("K2_DestroyActor")).invoke();
					g_shadows.erase(it);
				}
			};
			for (size_t i = 0; i + 2 < clear.size(); i += 3)
				dropShadow(cell_key(clear[i], clear[i + 1], clear[i + 2]));
			for (size_t i = 0; i + 2 < set.size(); i += 3)
			{
				const unsigned long long key = cell_key(set[i], set[i + 1], set[i + 2]);
				if (!isOpaque.count(key))
					dropShadow(key);
				else if (!g_shadows.count(key) && g_shadows.size() < kMaxShadows)
					if (UObject *a = spawn_shadow(context, to_ue(set[i] + 0.5, set[i + 1] + 0.5, set[i + 2] + 0.5), isOpaque[key]))
						g_shadows[key] = a;
			}
			static size_t lastCount = size_t(-1);
			if (g_shadows.size() != lastCount)
			{
				lastCount = g_shadows.size();
				log(L"shadows: " + std::to_wstring(g_shadows.size()) + L" of Minecraft's blocks cast HN shadows");
			}
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
			// a hair inside, so a surface exactly on the edge isn't counted twice; a single sub-cell is solid only when
			// HN's geometry reaches its middle 6.5 cm (a wall grazing its edge made a whole 12.5 cm cell solid: walls,
			// door frames and furniture up to 12.5 cm bigger than they are, too strict for the user). Floors keep their
			// exact height from the floor rays.
			const float h = n == 1 ? 3.25f : n * s * 0.5f - 0.5f;
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
		std::unordered_map<unsigned long long, std::array<uint8_t, 64>> g_dynBits; // moving parts' sub-cells, per block (sent; kept out of Steve)
		/// Where the moving parts are, for the static probe to leave alone: all of them (never cleared around Steve) and
		/// half a sub-cell wider. The probe used to mask with g_dynBits, which has the cells inside Steve removed: a door
		/// leaf swinging into him was re-probed every tick of its swing, found in exactly those cells, and baked into
		/// the static world there, inside his standing box (the 20:44 session: boxes in his head the tick before every
		/// forced crawl at Door_inward_2092, 4 times). Wider, because a static sub-cell counts geometry anywhere in its
		/// middle 6.5 cm while a moving one counts its centre only.
		std::unordered_map<unsigned long long, std::array<uint8_t, 64>> g_dynMask;

		/// Whether a point (Minecraft coordinates) lies in a moving part's sub-cells this tick (a door leaf, a cabinet
		/// door, a loose object). The static probe leaves those to the moving collision: a door re-probed mid-swing
		/// left pieces of its leaf and handle behind in the static world (the recorder: thin slabs on the closed
		/// door's plane, a 1/8 cell floating a metre up at a ray-measured, off-grid height, Steve blocked by them).
		bool in_moving_part(double mx, double my, double mz)
		{
			const int bx = int(std::floor(mx)), by = int(std::floor(my)), bz = int(std::floor(mz));
			const auto it = g_dynMask.find(cell_key(bx, by, bz));
			if (it == g_dynMask.end())
				return false;
			const int sx = std::clamp(int((mx - bx) * 8.0), 0, 7), sy = std::clamp(int((my - by) * 8.0), 0, 7), sz = std::clamp(int((mz - bz) * 8.0), 0, 7);
			return (it->second[sx * 8 + sy] >> sz & 1) != 0;
		}
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
						// a moving part's top (a door leaf's edge) is not a floor: on below it
						if (in_moving_part(hx, hy - 0.01, hz))
						{
							from = {hit.x, hit.y, hit.z - 3.0f};
							if (from.z <= to.z)
								break;
							continue;
						}
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
						// a moving part's underside (a door handle) is not a ceiling: on above it
						if (in_moving_part(hx, hy + 0.01, hz))
						{
							from = {hit.x, hit.y, hit.z + 3.0f};
							continue;
						}
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
			float ceil[16];
			std::memcpy(ceil, b.ceiling, sizeof(ceil));
			// Never collision inside Steve's current hitbox: HN's fixed geometry can't be there (Minecraft keeps him out
			// of it), so whatever is found there moved into him, and Minecraft, finding no room to stand, forces the
			// crawl. The 21:30 session still had 9 forced crawls (the door leaf 0.17-0.4 m above his feet at tick
			// 2441, a chair's cells at his waist at 1337) after the moving parts' own clearance: whatever the source,
			// it is kept out when sent. His current size, not his standing one: crouched under something low, that
			// stays (it is above him). From 0.15 above his feet (HN knows his feet a frame late: the ground under him
			// on a slope is never touched), 3 cm to spare around.
			if (g_haveFeet)
			{
				double fx, fy, fz;
				to_mc(g_feet, fx, fy, fz);
				const double hw = g_steveWidth / 200.0 + 0.03;
				const double x0 = fx - hw, x1 = fx + hw, z0 = fz - hw, z1 = fz + hw, y0 = fy + 0.15, y1 = fy + g_steveHeight / 100.0;
				if (bx < x1 && bx + 1 > x0 && bz < z1 && bz + 1 > z0 && by < y1 && by + 1 > y0)
				{
					int removed = 0;
					for (int sx = 0; sx < 8; ++sx)
					{
						const double cx0 = bx + sx / 8.0;
						if (cx0 + 0.125 <= x0 || cx0 >= x1)
							continue;
						for (int sz = 0; sz < 8; ++sz)
						{
							const double cz0 = bz + sz / 8.0;
							if (cz0 + 0.125 <= z0 || cz0 >= z1)
								continue;
							for (int sy = 0; sy < 8; ++sy)
							{
								const double cy0 = by + sy / 8.0;
								if (cy0 >= y0 && cy0 < y1 && (bits[sx * 8 + sy] >> sz & 1u))
									bits[sx * 8 + sy] &= uint8_t(~(1u << sz)), ++removed;
							}
							const double top = by + level[sx * 8 + sz];
							if (level[sx * 8 + sz] > 0.0f && top > y0 && top < y1)
								level[sx * 8 + sz] = 0.0f, ++removed;
						}
					}
					for (int c = 0; c < 16; ++c)
					{
						const double cx0 = bx + (c >> 2) * 0.25, cz0 = bz + (c & 3) * 0.25, under = by + ceil[c];
						if (ceil[c] > 0.0f && cx0 + 0.25 > x0 && cx0 < x1 && cz0 + 0.25 > z0 && cz0 < z1 && under > y0 && under < y1)
							ceil[c] = 0.0f, ++removed;
					}
					static DWORD nextLog = 0;
					if (removed && GetTickCount() > nextLog)
					{
						nextLog = GetTickCount() + 2000;
						log(L"collision: " + std::to_wstring(removed) + L" pieces inside Steve kept out of block " + std::to_wstring(bx) + L" " +
							std::to_wstring(by) + L" " + std::to_wstring(bz) + (g_dynBits.count(key) ? L" (a moving part is there)" : L" (static)"));
					}
				}
			}
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
			const bool anyFloor = heights(level, 64, floor), anyCeiling = heights(ceil, 16, ceiling);
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
				// probed with a margin (a ray costs about the same whatever its length): going down a hill, every
				// column within reach was cast again each block Steve descended (~80 rays each, the probe's budget
				// used up frame after frame); now every 4 blocks
				const int probeLo = py - kVoxBelow - kColumnMargin, probeHi = py + kVoxAbove + 1 + kColumnMargin / 2;
				const bool close = std::abs(dx) <= kVoxRefreshRadius && std::abs(dz) <= kVoxRefreshRadius;
				if (covered && !(close && now - it->second.probed > kVoxRefreshMs * 3))
					continue;
				if (!covered && std::abs(dx) <= kDriveReadyRadius && std::abs(dz) <= kDriveReadyRadius)
					ready = false;
				queries += probe_column(pawn, px + dx, pz + dz, probeLo, probeHi);
				// what is known now: these heights, joined to the earlier ones when they touch
				if (it != g_columns.end() && it->second.lo <= probeHi && it->second.hi >= probeLo)
					g_columns[col] = {now, std::min(it->second.lo, probeLo), std::max(it->second.hi, probeHi)};
				else
					g_columns[col] = {now, probeLo, probeHi};
				// the neighbouring columns' shapes depend on these floors too (steps across block borders)
				for (int nx = -1; nx <= 1; ++nx)
					for (int nz = -1; nz <= 1; ++nz)
						for (int y = probeLo; y < probeHi; ++y)
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
					// the moving parts here now are theirs, not the static world's
					if (const auto dyn = g_dynMask.find(key); dyn != g_dynMask.end())
						for (int j = 0; j < 64; ++j)
							b.box[j] &= uint8_t(~dyn->second[j]);
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

		/// All the numbers of the array under `key` (into `out`, replacing it).
		void json_doubles(const std::string &m, const char *key, std::vector<double> &out)
		{
			out.clear();
			const size_t at = m.find(std::string("\"") + key + "\":[");
			if (at == std::string::npos)
				return;
			const char *p = m.c_str() + at + std::strlen(key) + 4;
			while (*p && *p != ']')
			{
				char *end = nullptr;
				const double v = std::strtod(p, &end);
				if (end == p)
					break;
				out.push_back(v);
				p = end + (*end == ',' ? 1 : 0);
			}
		}

		std::vector<int> g_pendingSet, g_pendingClear, g_pendingOpaque, g_pendingColours; // block changes from Minecraft, applied on the next tick
		std::vector<std::string> g_throwRequests;      // tokens Minecraft dropped: HN throws the objects
		std::string g_heldId, g_heldCls;               // the HN object in Minecraft's selected slot ("" none)
		bool g_handEmpty = true;                       // Minecraft's main hand holds nothing (only then HN's objects are picked up)
		Vec3f g_handFist, g_handUp;                    // where Minecraft draws the carrying fist (Unreal cm), and the camera's up
		float g_handDist = 1.35f;                      // how far that is from the camera (blocks)
		DWORD g_handAt = 0;                            // when that came (it comes every frame while an object is held)
		std::string g_handId;                          // the token whose arm that fist is (Minecraft draws one per frame)
		/// Minecraft drew the carrying fist for this object within the last 250 ms: only then is it placed on it.
		bool fist_for(const std::string &id)
		{
			return !id.empty() && g_handId == id && GetTickCount() - g_handAt < 250;
		}
		Vec3f g_handLocal{0.66f, -0.28f, 1.15f};       // the fist in the camera's frame (blocks: right, up, forward)
		void carry_hand_follow(UObject *camera, const Vec3f &cameraLoc, const Rot3f &cameraRot);
		Vec3f rotate(const float q[4], const Vec3f &v);
		bool local_bounds(UObject *component, Vec3f &lo, Vec3f &hi);
		void carry_answer(const std::string &id, bool ok, const std::string &why);
		bool carry_place(UObject *pc, UObject *pawn);
		bool carry_throw(UObject *pc, UObject *pawn, const std::string &id, const std::string &cls, float charge = 0.5f);
		struct ThrowCharge
		{
			std::string id;
			DWORD start = 0;
			bool active = false;
		} g_throwCharge; // Q held with an HN object in the hand
		void carry_start_pickup(UObject *actor, UObject *body);
		bool item_use(UObject *pc, UObject *pawn);
		UObject *pickup_target(UObject *pawn, UObject **bodyOut, const wchar_t **whyOut = nullptr, UObject **hitOut = nullptr);
		bool electric(UObject *actor);
		void hn_action_tap();
		void release_hn_action();
		void release_hn_action();
		/// The refusal of a loose physics object that is neither ours nor HN's to pick up (a TV, a lamp): its HN action is
		/// HN's grab, so it gets none.
		const wchar_t *const kNotCarriedNotGrabbed = L"a TV, switch or lamp: neither carried nor grabbed";
		void carry_cancel_pickup();
		bool actor_bounds(UObject *actor, Vec3f &origin, Vec3f &extent);
		UObject *g_controlsPc = nullptr;

		/// The string value under `key` (no escapes: ids and paths are plain).
		std::string json_string(const std::string &m, const char *key)
		{
			const size_t at = m.find(std::string("\"") + key + "\":\"");
			if (at == std::string::npos)
				return {};
			const size_t from = at + std::strlen(key) + 4, to = m.find('"', from);
			return to == std::string::npos ? std::string{} : m.substr(from, to - from);
		}
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
			else if (m.find("\"t\":\"unreceive\"") != std::string::npos)
			{
				// broken on the client (the server's change comes a tick later): its stand-in stops being drawn now
				double p3[3];
				if (json_vec3(m, "p", p3))
					if (auto it = g_shadows.find(cell_key(int(p3[0]), int(p3[1]), int(p3[2]))); it != g_shadows.end())
						stand_in_receives(it->second, false);
			}
			else if (m.find("\"t\":\"shadows\"") != std::string::npos)
			{
				ShadowFrame f;
				json_doubles(m, "b", f.boxes);
				json_ints(m, "k", f.kinds);
				json_ints(m, "c", f.colours);
				f.hasHand = json_numbers(m, "hand", f.hand, 12);
				f.at = GetTickCount();
				double frame = -1.0;
				if (json_number(m, "f", frame))
					f.frame = (long long)frame;
				// the first message of a frame is the one sent with its picture (later ones of the same number are
				// frames Minecraft didn't export)
				if (f.frame < 0 || g_shadowFrames.empty() || g_shadowFrames.back().frame != f.frame)
					g_shadowFrames.push_back(std::move(f));
				while (g_shadowFrames.size() > 12)
					g_shadowFrames.pop_front();
			}
			else if (m.find("\"t\":\"blocks\"") != std::string::npos)
			{
				json_ints(m, "set", g_pendingSet);
				json_ints(m, "clear", g_pendingClear);
				std::vector<int> opaque;
				json_ints(m, "opaque", opaque);
				g_pendingOpaque.insert(g_pendingOpaque.end(), opaque.begin(), opaque.end());
				std::vector<int> colours;
				json_ints(m, "col", colours);
				colours.resize(opaque.size() / 3, -1); // none sent: grey
				g_pendingColours.insert(g_pendingColours.end(), colours.begin(), colours.end());
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
			else if (m.find("\"t\":\"held\"") != std::string::npos)
			{
				g_heldId = json_string(m, "id");
				g_heldCls = json_string(m, "cls");
				g_handEmpty = m.find("\"empty\":false") == std::string::npos;
			}
			else if (m.find("\"t\":\"hand\"") != std::string::npos)
			{
				double p[3], u[3];
				if (json_vec3(m, "p", p) && json_vec3(m, "u", u))
				{
					g_handFist = to_ue(p[0], p[1], p[2]);
					const Vec3f o = to_ue(0.0, 0.0, 0.0), up = to_ue(u[0], u[1], u[2]);
					g_handUp = {up.x - o.x, up.y - o.y, up.z - o.z};
					const float n = std::sqrt(g_handUp.x * g_handUp.x + g_handUp.y * g_handUp.y + g_handUp.z * g_handUp.z);
					if (n > 1.0e-3f)
						g_handUp = {g_handUp.x / n, g_handUp.y / n, g_handUp.z / n};
					g_handAt = GetTickCount();
					g_handId = json_string(m, "id");
					double l[3];
					if (json_vec3(m, "l", l))
						g_handLocal = {float(l[0]), float(l[1]), float(l[2])};
					double d = 0.0;
					if (json_number(m, "d", d) && d > 0.2)
						g_handDist = float(d);
				}
			}
			else if (m.find("\"t\":\"picked\"") != std::string::npos)
				carry_answer(json_string(m, "id"), m.find("\"ok\":true") != std::string::npos, json_string(m, "why"));
			else if (m.find("\"t\":\"throwreq\"") != std::string::npos)
				g_throwRequests.push_back(json_string(m, "id"));
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
		bool g_inPauseMenu = false; // HN's pause menu (or the engine's pause) is up in a level (in_gameplay)

		bool in_gameplay(UObject *pc)
		{
			g_inPauseMenu = false;
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
					return !(g_inPauseMenu = true);
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
						return !(g_inPauseMenu = true);
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
			// Q throws a held HN object, charged like HN's own throw: held, the throw gathers strength for 1.2 s, and letting
			// go throws it (a tap: a short throw); Ctrl+Q the same (one object per slot). Otherwise Minecraft's drop, with
			// Ctrl the whole stack (Minecraft can't see Ctrl itself: its window never has the keys).
			if (!g_heldId.empty() && g_controlsPawn)
			{
				const int q = edge('Q');
				if (q > 0)
					g_throwCharge = {g_heldId, GetTickCount(), true};
				if (g_throwCharge.active)
				{
					const float charge = std::clamp((GetTickCount() - g_throwCharge.start) / 1200.0f, 0.0f, 1.0f);
					if (g_throwCharge.id != g_heldId || g_minecraftScreen)
					{
						g_throwCharge.active = false; // the object left the hand, or a screen opened: no throw
						progress("throw", -1.0f);
						log(L"carry: throw charge cancelled (" + std::wstring(g_minecraftScreen ? L"a screen opened" : L"another slot") + L")");
					}
					else if (q < 0)
					{
						g_throwCharge.active = false;
						progress("throw", -1.0f);
						carry_throw(g_controlsPc, g_controlsPawn, g_heldId, g_heldCls, charge);
					}
					else
					{
						static DWORD nextBar = 0;
						if (GetTickCount() > nextBar)
						{
							nextBar = GetTickCount() + 50;
							progress("throw", charge);
						}
					}
				}
			}
			else if (g_throwCharge.active)
			{
				g_throwCharge.active = false; // nothing in the hand any more
				progress("throw", -1.0f);
			}
			else if (const int q = edge('Q'))
				sendf("{\"t\":\"key\",\"k\":\"drop\",\"down\":%s,\"stack\":%s}", q > 0 ? "true" : "false", key_held(VK_CONTROL) ? "true" : "false");
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
				// a usable item in the hand (a key): the left button uses it on what the crosshair is on, if that is one of
				// HN's objects; only then is the click ours (no mining, no punch). Else, and with anything else in the hand,
				// Minecraft's attack as always (mining with an empty hand, or with a non-usable object)
				static bool used = false;
				if (attack > 0)
					used = !g_heldId.empty() && g_controlsPawn && item_use(g_controlsPc, g_controlsPawn);
				if (!used)
					sendf("{\"t\":\"key\",\"k\":\"attack\",\"down\":%s}", attack > 0 ? "true" : "false");
				else if (attack < 0)
					used = false;
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
		DWORD g_hnActionAt = 0;      // when it did: 0.6 s later, what HN holds is logged (a prop its action took)
		bool g_rightClickCarry = false; // the held right click is a pickup or a placement


		void right_click(UObject *)
		{
			release_hn_action();
			const int e = edge(VK_RBUTTON);
			// carrying: a held HN object is put down; a loose object under the crosshair starts a pickup (hold). A
			// carryable object never goes on to HN's action key, which is HN's own grab for its loose props: with
			// something in the hand it is refused (one thing in the hand at a time), not grabbed the HN way.
			if (e > 0 && g_controlsPawn)
			{
				if (!g_heldId.empty())
				{
					carry_place(g_controlsPc, g_controlsPawn);
					g_rightClickCarry = true;
					return;
				}
				UObject *body = nullptr, *hit = nullptr;
				const wchar_t *why = nullptr;
				UObject *target = pickup_target(g_controlsPawn, &body, &why, &hit);
				if (!target && why && std::wcscmp(why, L"not a physics object") != 0 && std::wcscmp(why, L"no root component") != 0)
					log(std::wstring(L"carry: what the look meets isn't carried: ") + why);
				// HN's electric devices (TV, radio, lamp, switch) are switched with HN's action, as a tap: a TV (never
				// carried) at once; a radio (carryable) on letting go before the pickup completes, held it is carried
				if (!target && electric(hit))
				{
					log(L"right click: " + hit->GetName() + L" is one of HN's electric devices: HN's action as a tap (switch it)");
					hn_action_tap();
					g_rightClickCarry = true;
					return;
				}
				if (!target && why == kNotCarriedNotGrabbed)
				{
					g_rightClickCarry = true;
					return;
				}
				if (target)
				{
					if (g_handEmpty)
						carry_start_pickup(target, body);
					else if (electric(target))
					{
						log(L"right click: " + target->GetName() + L" with something in the hand: HN's action as a tap (switch it)");
						hn_action_tap();
					}
					else
					{
						notify("Empty your hand first");
						log(L"carry: " + target->GetName() + L" not picked up: the hand isn't empty");
					}
					g_rightClickCarry = true;
					return;
				}
			}
			if (e < 0 && g_rightClickCarry)
			{
				g_rightClickCarry = false;
				carry_cancel_pickup();
				return;
			}
			if (e > 0)
			{
				g_rightClickHN = g_controlsPawn && looking_at_interactive(g_controlsPawn);
				if (g_rightClickHN)
					hn_action_tap();
				else
					g_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":true}");
			}
			else if (e < 0)
			{
				if (!g_rightClickHN)
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
				// the held object in the hand, from this very camera pose (placed before it, it was a frame behind)
				carry_hand_follow(camera, pose.loc, pose.rot);
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
			// 1 m/s and "barely budged" (HN's friction stops it at once), at three times that still too little; now a full
			// fist sends a chair ~8 m/s and a vase ~10, a sword (attack damage 7) half as much again, a sprint more.
			// Quick repeated attacks (Minecraft's uncharged swings) still push, at about two thirds.
			const float mass = mass_of(best);
			const float weapon = 0.7f + 0.06f * std::clamp(punch.damage, 1.0f, 12.0f);
			const float dv = 1800.0f / (1.0f + mass / 500.0f) * weapon * (0.5f + 0.5f * std::clamp(punch.strength, 0.0f, 1.0f)) *
							 (punch.sprint ? 1.4f : 1.0f);
			const float impulse = mass * dv;
			Call(best, STR("AddImpulseAtLocation")).set(STR("Impulse"), Vec3f{dir.x * impulse, dir.y * impulse, dir.z * impulse + impulse * 0.15f})
				.set(STR("Location"), bestPoint).set(STR("BoneName"), NameParam{}).invoke();
			// Minecraft plays its attack sound for a hit that moved something
			sendf("{\"t\":\"punched\",\"s\":%.3f,\"sprint\":%s}", punch.strength, punch.sprint ? "true" : "false");
			if (logged++ < 40)
			{
				UObject *owner = owner_of(best);
				log(L"physics: punch on " + (owner ? owner->GetName() : best->GetName()) + L" (" + std::to_wstring(int(mass)) + L" kg): " +
					std::to_wstring(int(impulse)) + L" kg cm/s, " + std::to_wstring(int(impulse / mass)) + L" cm/s");
			}
		}

		// ---- carrying HN's objects in Minecraft's inventory ----------------------------------------------------------
		// Hold right click on a loose object (simulating physics, its own actor, not a door or drawer, not one of HN's
		// story items with an InteractiveComponent, which keep HN's own pickup): after kPickupHold it leaves HN's world
		// for Minecraft's inventory. The object is never destroyed or copied: it stays in the level hidden, without
		// collision or physics, while Minecraft holds a token for it (Carry.java). Selected in the hotbar, HN shows that
		// very object where Minecraft's hand is (real mesh, HN's lighting). Right click with it puts it down where Steve
		// looks, Q throws it. Each step is confirmed by the other side before anything is removed.

		constexpr DWORD kPickupHold = 350;    // ms of holding right click to pick up
		constexpr float kPickupReach = 300.0f; // cm

		struct Carried
		{
			UObject *actor = nullptr;
			UObject *body = nullptr;   // the simulating component (the actor's root)
			std::wstring cls;          // class path, to make a new one when the object is gone (another level)
			Vec3f scale{1, 1, 1};
			float extent = 20.0f;      // half size (cm, largest axis) at its own scale
			float fit = 1.0f;          // its size in the hand (times its own scale), before the near placement
			UObject *attachedTo = nullptr; // the view camera it is attached to while shown in the hand
			bool swept = false;        // the 360-degree check ran for it
			float mass = 10.0f;
			bool confirmed = false;    // Minecraft has its token
			UObject *level = nullptr;  // the player controller of its level (gone with it)
			// its life with us: picked (hidden, waiting for Minecraft's slot), held (shown in the hand), stored (another
			// slot: hidden, and its own activity stopped), and back in the world (put down or thrown: as it was)
			enum class Life
			{
				Picked,
				Held,
				Stored,
			} life = Life::Picked;
			DWORD selectedAt = 0;      // when it became the held one (shown once its fist is known)
			bool shown = false;        // drawn in the hand
			std::vector<UObject *> paused; // its sounds and effects stopped while stored (resumed with it)
			bool tickWas = true;       // its tick before it was stored
		};
		std::unordered_map<std::string, Carried> g_carried;
		void suspend_activity(Carried &c);
		void resume_activity(Carried &c);
		std::string g_shownId;           // what is shown in the hand now
		int g_carryCounter = 0;
		void carry_detach(Carried &c);
		void carry_sweep(Carried &c, UObject *camera, const Vec3f &loc, const Rot3f &rot);

		struct PickupTry
		{
			UObject *actor = nullptr;
			UObject *body = nullptr;
			DWORD start = 0;
			bool active = false;
			bool tapActivates = false; // an electric device (the radio): let go early = HN's action (switch it), held = carry
		} g_pickup;

		/// HN's action key E as a tap: pressed now, released ~60 ms later (release_hn_action, every tick). It used to be held
		/// for as long as the right button was, and holding the right button is what our hold-to-pick-up asks for: a held E
		/// on HN's props is HN's own take; a tap is its action (switch the TV or the radio on, open a door).
		DWORD g_hnActionUpAt = 0;
		void hn_action_tap()
		{
			if (g_hnActionUpAt)
				return; // one at a time: never two activations from one click
			g_hnActionAt = GetTickCount(); // what HN ends up holding is checked (carry_tick)
			++g_injectedE;
			PostMessageW(g_hnWindow, WM_KEYDOWN, 'E', 0x00120001);
			g_hnActionUpAt = GetTickCount() + 60;
			g_ws.send("{\"t\":\"swing\"}");
		}

		void release_hn_action()
		{
			if (g_hnActionUpAt && GetTickCount() >= g_hnActionUpAt)
			{
				g_hnActionUpAt = 0;
				++g_injectedE;
				PostMessageW(g_hnWindow, WM_KEYUP, 'E', 0xC0120001);
			}
		}

		/// One of HN's electric devices (TVs, radios, lamps, switches: class chain ... < Tumbler < Electric < FlatActor).
		bool electric(UObject *actor)
		{
			return actor && (class_contains(actor, L"Electric") || class_contains(actor, L"Tumbler"));
		}

		std::string narrow(const std::wstring &w);
		bool get_bool_property(UObject *o, const TCHAR *name, bool &value);
		std::string narrow(const std::wstring &w)
		{
			std::string s;
			for (wchar_t c : w)
				s += c < 128 && c != '"' && c != '\\' ? char(c) : '_';
			return s;
		}

		/// A readable name from the class: BP_Dining_chair_C -> "Dining chair", BP_Mug_apartments_C -> "Mug".
		std::string object_name(UObject *actor)
		{
			std::wstring cls = actor->GetClassPrivate()->GetName();
			if (cls.starts_with(L"BP_"))
				cls = cls.substr(3);
			if (cls.ends_with(L"_C"))
				cls = cls.substr(0, cls.size() - 2);
			std::string out;
			size_t at = 0;
			while (at <= cls.size())
			{
				size_t next = cls.find(L'_', at);
				if (next == std::wstring::npos)
					next = cls.size();
				std::wstring part = cls.substr(at, next - at);
				while (!part.empty() && iswdigit(part.back()))
					part.pop_back();
				if (!part.empty() && part != L"apartments" && part != L"house" && part != L"new" && part != L"NEW")
					out += (out.empty() ? "" : " ") + narrow(part);
				at = next + 1;
			}
			return out.empty() ? "Object" : out;
		}

		/// An FString property's text (empty if there is none).
		std::wstring string_property(UObject *o, const TCHAR *name)
		{
			struct Str
			{
				const wchar_t *data;
				int32 num, max;
			};
			const Str *v = o ? o->GetValuePtrByPropertyNameInChain<Str>(name) : nullptr;
			return v && v->data && v->num > 1 ? std::wstring(v->data, size_t(v->num - 1)) : std::wstring();
		}

		/// One of HN's usable items: it has an ApplyComponent (keys and lockpicks, bulbs, the crowbar's kin, chairs,
		/// buckets...), the component HN's own "apply" (its left mouse) works through.
		UObject *apply_component(UObject *actor)
		{
			UObject **ac = actor ? actor->GetValuePtrByPropertyNameInChain<UObject *>(STR("ApplyComponent")) : nullptr;
			return ac ? *ac : nullptr;
		}

		/// The item's name for Minecraft: HN's keys by what they are (their class: HN has no names for them), with
		/// the key's code, which is what decides the lock it opens (a lock opens for the keys whose Code is among its
		/// m_Codes); the rest from the class (object_name).
		std::string item_name(UObject *actor);
		std::wstring item_name_w(UObject *actor)
		{
			const std::string n = item_name(actor);
			return std::wstring(n.begin(), n.end());
		}

		std::string item_name(UObject *actor)
		{
			static const std::pair<const wchar_t *, const char *> named[] = {
				{L"KeyCar", "Car Key"}, {L"DollHouseKey", "Dollhouse Key"}, {L"Key_Card", "Key Card"}, {L"Key_Universal", "Key"},
				{L"Tutorial_Key", "Key"}, {L"lockpick", "Lockpick"}, {L"Lock_Red", "Red Lock"}, {L"Lock_Blue", "Blue Lock"},
				{L"Lock_Green", "Green Lock"}, {L"Lock_Yellow", "Yellow Lock"}};
			const std::wstring cls = actor->GetClassPrivate()->GetName();
			std::string name;
			for (const auto &[match, readable] : named)
				if (cls.find(match) != std::wstring::npos)
				{
					name = readable;
					break;
				}
			if (name.empty())
				name = object_name(actor);
			if (class_contains(actor, L"Lockpick"))
				if (const std::wstring code = string_property(actor, STR("Code")); !code.empty())
					name += " (" + narrow(code) + ")";
			return name;
		}

		Vec3f look_dir()
		{
			const float d2r = 3.14159265f / 180.0f;
			const float yaw = g_mcLookYaw * d2r, pitch = g_mcLookPitch * d2r;
			return {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch)};
		}

		/// What a prop is made of: whether its root may move, how many primitive components it has, its half size (cm).
		struct PropShape
		{
			bool movable = false;
			int parts = 0;
			float extent = 0.0f;
		};

		PropShape prop_shape(UObject *actor, UObject *root)
		{
			PropShape out;
			if (const uint8_t *mobility = root->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mobility")))
				out.movable = *mobility == 2; // EComponentMobility::Movable
			static UObject *primitive = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.PrimitiveComponent"));
			Call list(actor, STR("GetComponentsByClass"));
			list.set<UObject *>(STR("ComponentClass"), primitive);
			struct ObjectArray
			{
				UObject **data;
				int32 num, max;
			};
			if (const ObjectArray *a = list.invoke() ? list.param<ObjectArray>(STR("ReturnValue")) : nullptr)
				out.parts = a->num;
			Vec3f o, e;
			if (actor_bounds(actor, o, e))
				out.extent = std::max({e.x, e.y, e.z});
			return out;
		}

		/// The loose object under the crosshair, if it can be carried (else nullptr).
		UObject *pickup_target(UObject *pawn, UObject **bodyOut, const wchar_t **whyOut, UObject **hitOut)
		{
			if (!g_haveEye)
				return nullptr;
			const Vec3f dir = look_dir();
			Call c(kismet(), STR("LineTraceSingle"));
			if (!c)
				return nullptr;
			c.set<UObject *>(STR("WorldContextObject"), pawn)
				.set(STR("Start"), g_eye)
				.set(STR("End"), Vec3f{g_eye.x + dir.x * kPickupReach, g_eye.y + dir.y * kPickupReach, g_eye.z + dir.z * kPickupReach})
				.set<uint8_t>(STR("TraceChannel"), 0)
				.set(STR("ActorsToIgnore"), ScriptArray{nullptr, 0, 0})
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			FProperty *outHit = c.property(STR("OutHit"));
			if (!ok || !*ok || !outHit)
				return nullptr;
			Call brk(gameplay_statics(), STR("BreakHitResult"));
			FProperty *hitParam = brk.property(STR("Hit"));
			if (!brk || !hitParam)
				return nullptr;
			std::memcpy(brk.data() + hitParam->GetOffset_Internal(), c.data() + outHit->GetOffset_Internal(), size_t(outHit->GetSize()));
			brk.invoke();
			UObject *const *actor = brk.param<UObject *>(STR("HitActor"));
			UObject *const *component = brk.param<UObject *>(STR("HitComponent"));
			if (!actor || !*actor || !component || !*component)
				return nullptr;
			if (hitOut)
				*hitOut = *actor;
			// The object is the actor; its body is its root, which simulates physics. Any part of it may be what the look
			// meets (a mesh or collider under the root): it used to have to be the root itself, and a prop hit on another
			// part was refused without a word and went on to HN's action, which is HN's own grab for its props.
			auto refuse = [&](const wchar_t *why) -> UObject * {
				if (whyOut)
					*whyOut = why;
				return nullptr;
			};
			Call root(*actor, STR("K2_GetRootComponent"));
			root.invoke();
			UObject *const *r = root.param<UObject *>(STR("ReturnValue"));
			if (!r || !*r)
				return refuse(L"no root component");
			// The rule, in this order (each step names why in the log):
			//  1. a part of a fixed object (a door leaf, a drawer, a switch's lever): HN's action;
			//  2. an electric device (TV, radio, lamp): ours if HN would let it be taken (m_bPickUp) or it is loose; a
		//     tap switches it either way; one of HN's usable items (an ApplyComponent: keys...): ours;
			//  3. one of HN's inventory items (InteractiveComponent + a class chain naming an item or key): HN's pickup;
			//  4. a loose physics body: ours;
			//  5. a prop HN would take into its own hotbar: ours. HN keeps many props asleep, not simulating, until they
			//     are touched, and the TV, BP_Picture_set and BP_Trashcan were refused as "not a physics object" without a
			//     word, so HN's action took them into HN's hotbar (the 22:57-23:01 session). Such a prop: HN says its
			//     action applies to what the look meets (not a part of a fixed thing: rule 1), its root is movable, and it
			//     is at most 90 cm in half size (a wardrobe is bigger; its doors are parts);
			//  6. anything else: HN's action.
			if (hinged_part(*r) || hinged_part(*component))
				return refuse(L"a door or drawer");
			Call simulatingRoot(*r, STR("IsSimulatingPhysics"));
			simulatingRoot.set(STR("BoneName"), NameParam{}).invoke();
			const uint8_t *rootLoose = simulatingRoot.param<uint8_t>(STR("ReturnValue"));
			// HN's own word for "can be taken": FlatActor::m_bPickUp (what HN's E pickup takes into its hands/hotbar)
			bool hnPickUp = false;
			get_bool_property(*actor, STR("m_bPickUp"), hnPickUp);
			const PropShape shape = prop_shape(*actor, *r);
			// HN's own word or a loose body only: a wall switch may well be movable and small (never carried)
			const bool takeable = hnPickUp || (rootLoose && *rootLoose);
			// an electric device (TV, radio, lamp): carried if HN lets it be taken (a tap on it still switches it: the
			// pickup only completes on a hold); a fixed one (a wall switch, a ceiling lamp) only switches. The TV used to
			// be refused outright (2026-10-11: it only ever switched on, never came along)
			if (electric(*actor) || class_contains(*actor, L"TV") || class_contains(*actor, L"Lamp"))
			{
				if (!takeable)
				{
					static std::unordered_set<UObject *> told;
					if (told.insert(*actor).second)
						log(L"carry: " + (*actor)->GetName() + L" only switches (HN's m_bPickUp " + std::to_wstring(hnPickUp) + L", loose " +
							std::to_wstring(rootLoose && *rootLoose) + L", movable " + std::to_wstring(shape.movable) + L", " + std::to_wstring(int(shape.extent)) + L" cm)");
					return refuse(kNotCarriedNotGrabbed);
				}
				if (bodyOut)
					*bodyOut = *r;
				return *actor;
			}
			// one of HN's usable items (keys, lockpicks...): ours, used with the left button on what it is for (item_use).
			// They went to HN's own pickup and HN's hotbar, where Minecraft's hand could never use them
			if (apply_component(*actor) && takeable)
			{
				if (bodyOut)
					*bodyOut = *r;
				return *actor;
			}
			if (interactive_actor(*actor))
			{
				static std::unordered_set<UObject *> logged;
				if (logged.insert((*actor)->GetClassPrivate()).second)
				{
					std::wstring chain;
					for (UStruct *k = (*actor)->GetClassPrivate(); k; k = k->GetSuperStruct())
						chain += (chain.empty() ? L"" : L" < ") + k->GetName();
					log(L"carry: class chain of " + (*actor)->GetName() + L": " + chain);
				}
				if (class_contains(*actor, L"Item") || class_contains(*actor, L"Pickup") || class_contains(*actor, L"PickUp") ||
					class_contains(*actor, L"Key"))
					return refuse(L"one of HN's inventory items (its class chain names an item or key): HN's pickup");
			}
			if (!rootLoose || !*rootLoose)
			{
				// a whole movable prop HN acts on (not one of its parts: those were refused above), up to 90 cm in half size,
				// whatever it is made of: requiring one mesh left chairs (seat, legs, back) to HN's take into its hotbar
				if (!(shape.movable && shape.extent > 0.0f && shape.extent <= 90.0f && flat_actionable(*actor, *component) == 1))
					return refuse(L"not a physics object");
			}
			if (bodyOut)
				*bodyOut = *r;
			return *actor;
		}

		void set_carried_state(UObject *actor, UObject *body, bool inWorld)
		{
			Call(actor, STR("SetActorHiddenInGame")).set_bool(STR("bNewHidden"), !inWorld).invoke();
			Call(actor, STR("SetActorEnableCollision")).set_bool(STR("bNewActorEnableCollision"), inWorld).invoke();
			if (body)
				Call(body, STR("SetSimulatePhysics")).set_bool(STR("bSimulate"), inWorld).invoke();
		}

		Vec3f actor_scale(UObject *actor)
		{
			Call c(actor, STR("GetActorScale3D"));
			c.invoke();
			const Vec3f *v = c.param<Vec3f>(STR("ReturnValue"));
			return v ? *v : Vec3f{1, 1, 1};
		}

		void set_actor_scale(UObject *actor, const Vec3f &scale)
		{
			Call(actor, STR("SetActorScale3D")).set(STR("NewScale3D"), scale).invoke();
		}

		void place_actor_rot(UObject *actor, const Vec3f &at, const Rot3f &rot)
		{
			Call c(actor, STR("K2_SetActorLocationAndRotation"));
			c.set(STR("NewLocation"), at).set(STR("NewRotation"), rot).set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
		}

		void place_actor(UObject *actor, const Vec3f &at, float yaw)
		{
			Call c(actor, STR("K2_SetActorLocationAndRotation"));
			c.set(STR("NewLocation"), at).set(STR("NewRotation"), Rot3f{0.0f, yaw, 0.0f}).set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
		}

		/// Pickup completes: out of HN's world (hidden, no collision, no physics), and Minecraft is asked for a slot.
		void quat_of(const Rot3f &r, float q[4]);

		std::unordered_set<std::wstring> g_iconsMade; // classes with a picture made this session (or tried)

		/// The object's picture for Minecraft's hotbar and inventory, once per class: a scene capture of the object alone
		/// (its components only, from a three-quarter view above, 64 px), exported by UE 4.20's ExportRenderTarget into
		/// icons/ next to the mod; Minecraft loads it ({"t":"icon"}, CarryIcons). Taken at the pickup, before the
		/// object is hidden. Any step failing is logged and Minecraft keeps its fallback icon for the class.
		void capture_icon(UObject *pawn, UObject *object, const std::wstring &cls)
		{
			if (!g_iconsMade.insert(cls).second)
				return;
			static UObject *rendering = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetRenderingLibrary"));
			static UObject *captureClass = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.SceneCapture2D"));
			UObject *statics = gameplay_statics();
			auto fail = [&](const wchar_t *why) { log(L"carry: no icon for " + cls + L": " + why); };
			if (!rendering || !captureClass || !statics)
				return fail(L"the engine's capture or rendering library isn't there");
			Call rt(rendering, STR("CreateRenderTarget2D"));
			rt.set<UObject *>(STR("WorldContextObject"), pawn).set<int32_t>(STR("Width"), 128).set<int32_t>(STR("Height"), 128).set<uint8_t>(STR("Format"), 2); // RTF_RGBA8
			UObject *const *target = rt.invoke() ? rt.param<UObject *>(STR("ReturnValue")) : nullptr;
			if (!target || !*target)
				return fail(L"no render target");
			// where to look from: what is drawn of the object (its visible meshes' bounds, not the actor's, which count
			// trigger boxes and volumes: a chair's icon was a speck, 23% of the picture), seen from front-left above, at
			// the distance where its box's corners just fit the view (a sphere around the box wasted a third of it)
			Vec3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
			int meshes = 0;
			{
				static UObject *meshClass = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.MeshComponent"));
				struct ObjectArray
				{
					UObject **data;
					int32 num, max;
				};
				Call list(object, STR("GetComponentsByClass"));
				list.set<UObject *>(STR("ComponentClass"), meshClass);
				const ObjectArray *a = meshClass && list.invoke() ? list.param<ObjectArray>(STR("ReturnValue")) : nullptr;
				for (int i = 0; a && i < a->num; ++i)
				{
					UObject *m = a->data[i];
					bool visible = false, hiddenInGame = true;
					if (!m || !get_bool_property(m, STR("bVisible"), visible) || !visible)
						continue;
					get_bool_property(m, STR("bHiddenInGame"), hiddenInGame);
					if (hiddenInGame)
						continue;
					Call cb(kismet(), STR("GetComponentBounds"));
					cb.set<UObject *>(STR("Component"), m);
					const Vec3f *o = cb.invoke() ? cb.param<Vec3f>(STR("Origin")) : nullptr, *e = cb.param<Vec3f>(STR("BoxExtent"));
					if (!o || !e || e->x + e->y + e->z <= 0.0f)
						continue;
					lo = {std::min(lo.x, o->x - e->x), std::min(lo.y, o->y - e->y), std::min(lo.z, o->z - e->z)};
					hi = {std::max(hi.x, o->x + e->x), std::max(hi.y, o->y + e->y), std::max(hi.z, o->z + e->z)};
					++meshes;
				}
			}
			Vec3f boV, beV;
			if (meshes)
				boV = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f}, beV = {(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
			else
			{
				Call b(object, STR("GetActorBounds"));
				b.set_bool(STR("bOnlyCollidingComponents"), false);
				const Vec3f *bo = b.invoke() ? b.param<Vec3f>(STR("Origin")) : nullptr, *be = b.param<Vec3f>(STR("BoxExtent"));
				if (!bo || !be)
					return fail(L"no bounds");
				boV = *bo, beV = *be;
			}
			const Vec3f *bo = &boV, *be = &beV;
			const float fov = 30.0f, tanHalf = std::tan(fov * 0.5f * 3.14159265f / 180.0f);
			const Vec3f dir{-0.62f, -0.62f, 0.48f}; // unit: from the object to the camera
			// the camera's right and up for that direction; the farthest a box corner reaches across the view, and
			// towards the camera (it is nearer, so it looks bigger), give the distance where all eight fit
			const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
			const Vec3f f{-dir.x / dl, -dir.y / dl, -dir.z / dl};
			const float rl = std::hypot(f.y, f.x);
			const Vec3f right{-f.y / rl, f.x / rl, 0.0f};
			const Vec3f up{right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z, right.x * f.y - right.y * f.x};
			float dist = 5.0f;
			for (int corner = 0; corner < 8; ++corner)
			{
				const Vec3f v{(corner & 1 ? 1 : -1) * be->x, (corner & 2 ? 1 : -1) * be->y, (corner & 4 ? 1 : -1) * be->z};
				const float across = std::max(std::abs(v.x * right.x + v.y * right.y + v.z * right.z), std::abs(v.x * up.x + v.y * up.y + v.z * up.z));
				const float toward = -(v.x * f.x + v.y * f.y + v.z * f.z); // how much nearer the camera than the middle
				dist = std::max(dist, across / tanHalf + toward);
			}
			dist *= 1.06f;
			log(L"carry: icon of " + cls + L" framed on " + std::to_wstring(meshes) + L" visible meshes, " + std::to_wstring(int(be->x)) + L"x" +
				std::to_wstring(int(be->y)) + L"x" + std::to_wstring(int(be->z)) + L" cm half size, from " + std::to_wstring(int(dist)) + L" cm");
			Transform4 t;
			t.pos[0] = bo->x + dir.x * dist, t.pos[1] = bo->y + dir.y * dist, t.pos[2] = bo->z + dir.z * dist;
			quat_of(Rot3f{-std::asin(dir.z) * 180.0f / 3.14159265f, std::atan2(-dir.y, -dir.x) * 180.0f / 3.14159265f, 0.0f}, t.rot);
			Call begin(statics, STR("BeginDeferredActorSpawnFromClass"));
			begin.set<UObject *>(STR("WorldContextObject"), pawn).set<UObject *>(STR("ActorClass"), captureClass).set(STR("SpawnTransform"), t)
				.set<uint8_t>(STR("CollisionHandlingOverride"), 1).set<UObject *>(STR("Owner"), nullptr);
			UObject *const *camera = begin.invoke() ? begin.param<UObject *>(STR("ReturnValue")) : nullptr;
			if (!camera || !*camera)
				return fail(L"no scene capture");
			Call(statics, STR("FinishSpawningActor")).set<UObject *>(STR("Actor"), *camera).set(STR("SpawnTransform"), t).invoke();
			UObject **component = (*camera)->GetValuePtrByPropertyNameInChain<UObject *>(STR("CaptureComponent2D"));
			bool exported = false;
			const std::wstring dirPath = module_dir() + L"icons\\";
			std::wstring name = cls.substr(cls.find_last_of(L"./") + 1);
			for (wchar_t &ch : name)
				if (!iswalnum(ch) && ch != L'_')
					ch = L'_';
			name += L".hdr";
			if (component && *component)
			{
				if (UObject **tt = (*component)->GetValuePtrByPropertyNameInChain<UObject *>(STR("TextureTarget")))
					*tt = *target;
				if (uint8_t *mode = (*component)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("PrimitiveRenderMode")))
					*mode = 2; // PRM_UseShowOnlyList
				if (uint8_t *source = (*component)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("CaptureSource")))
					*source = 0; // SCS_SceneColorHDR: lit, no tone mapping or eye adaptation
				if (float *angle = (*component)->GetValuePtrByPropertyNameInChain<float>(STR("FOVAngle")))
					*angle = fov;
				set_bool_property(*component, STR("bCaptureEveryFrame"), false);
				Call(*component, STR("ShowOnlyActorComponents")).set<UObject *>(STR("InActor"), object).invoke();
				Call(*component, STR("CaptureScene")).invoke();
				CreateDirectoryW(dirPath.c_str(), nullptr);
				Call ex(rendering, STR("ExportRenderTarget"));
				ex.set<UObject *>(STR("WorldContextObject"), pawn).set<UObject *>(STR("TextureRenderTarget"), *target)
					.set(STR("FilePath"), FString(dirPath.c_str())).set(STR("FileName"), FString(name.c_str()));
				exported = ex.invoke();
			}
			Call((*camera), STR("K2_DestroyActor")).invoke();
			const std::wstring file = dirPath + name;
			if (!exported || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
				return fail(component && *component ? L"the export wrote nothing" : L"no capture component");
			// slashes before narrow(), which makes every backslash '_' (the 22:20 session: Minecraft got
			// "C:_Program Files (x86)_Steam_..." and found nothing)
			std::wstring forward = file;
			std::replace(forward.begin(), forward.end(), L'\\', L'/');
			const std::string path = narrow(forward);
			sendf("{\"t\":\"icon\",\"key\":\"%s\",\"file\":\"%s\"}", narrow(cls).c_str(), path.c_str());
			log(L"carry: icon for " + cls + L" in " + file);
		}

		/// The first stage of drawing Minecraft's blocks as HN geometry (as SkyCraft draws Minecraft's block meshes in
		/// Skyrim's frame, chasmlol/SkyCraft, MIT: protocol/skycraft_protocol.h's render ring, WorldExporter.java): what
		/// this build of HN offers for it, once per level, logged (`mesh path: ...`). Found in the shipping binary:
		/// ProceduralMeshComponent (CreateMeshSection_LinearColor, UpdateMeshSection), ImportFileAsTexture2D,
		/// CreateDynamicMaterialInstance, SetTextureParameterValue. Unknown until now, and needed: a material whose
		/// texture can be set (a shipped game can't make new ones), ideally masked (cutout) and lit. The probe makes a
		/// dynamic instance of loaded materials and asks it for a texture under the usual parameter names; it draws
		/// nothing and changes nothing.
		void mesh_path_probe(UObject *pawn)
		{
			static UObject *probedFor = nullptr;
			if (probedFor == pawn)
				return;
			probedFor = pawn;
			UObject *procMesh = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/ProceduralMeshComponent.ProceduralMeshComponent"));
			static UObject *matLib = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.Default__KismetMaterialLibrary"));
			log(std::wstring(L"mesh path: ProceduralMeshComponent class ") + (procMesh ? L"present" : L"MISSING") + L", material library " +
				(matLib ? L"present" : L"MISSING"));
			if (!matLib)
				return;
			static const wchar_t *names[] = {L"BaseColor", L"Texture", L"Diffuse", L"Albedo", L"BaseTexture", L"MainTexture", L"Tex", L"T_Diffuse",
				L"DiffuseTexture", L"Color", L"BaseColorTexture", L"Base Color"};
			int tried = 0, found = 0;
			for (UObject *m : find_all(L"Material"))
			{
				if (tried >= 400 || found >= 15)
					break;
				++tried;
				Call make(matLib, STR("CreateDynamicMaterialInstance"));
				make.set<UObject *>(STR("WorldContextObject"), pawn).set<UObject *>(STR("Parent"), m).set(STR("OptionalName"), NameParam{});
				UObject *const *mid = make.invoke() ? make.param<UObject *>(STR("ReturnValue")) : nullptr;
				if (!mid || !*mid)
					continue;
				std::wstring params;
				for (const wchar_t *n : names)
				{
					Call get(*mid, STR("K2_GetTextureParameterValue"));
					get.set(STR("ParameterName"), name_param(n));
					if (UObject *const *t = get.invoke() ? get.param<UObject *>(STR("ReturnValue")) : nullptr; t && *t)
						params += (params.empty() ? L"" : L", ") + std::wstring(n);
				}
				if (params.empty())
					continue;
				++found;
				const uint8_t *blend = m->GetValuePtrByPropertyNameInChain<uint8_t>(STR("BlendMode"));
				const uint8_t *shading = m->GetValuePtrByPropertyNameInChain<uint8_t>(STR("ShadingModel"));
				static const wchar_t *blends[] = {L"opaque", L"masked", L"translucent", L"additive", L"modulate", L"alpha composite"};
				log(L"mesh path: material " + m->GetFullName() + L": texture parameters " + params + L"; blend " +
					(blend && *blend < 6 ? blends[*blend] : L"?") + L", shading model " + (shading ? std::to_wstring(*shading) : L"?"));
			}
			log(L"mesh path: " + std::to_wstring(found) + L" materials with a settable texture among " + std::to_wstring(tried) + L" tried");
		}

		void complete_pickup(UObject *pc, UObject *actor, UObject *body)
		{
			Vec3f origin, extent;
			Carried c;
			c.actor = actor;
			c.body = body;
			c.cls = actor->GetClassPrivate()->GetFullName();
			if (const size_t space = c.cls.find(L' '); space != std::wstring::npos)
				c.cls = c.cls.substr(space + 1); // "BlueprintGeneratedClass /Game/...": the path
			c.scale = actor_scale(actor);
			if (actor_bounds(actor, origin, extent))
				c.extent = std::max({extent.x, extent.y, extent.z, 5.0f});
			c.mass = mass_of(body);
			c.level = pc;
			const std::string id = "hn" + std::to_string(++g_carryCounter) + "-" + narrow(actor->GetName());
			capture_icon(actor, actor, c.cls); // before it is hidden
			set_carried_state(actor, body, false);
			g_carried[id] = c;
			const std::string name = item_name(actor);
			sendf("{\"t\":\"pickup\",\"id\":\"%s\",\"name\":\"%s\",\"cls\":\"%s\",\"kg\":%d}", id.c_str(), name.c_str(), narrow(c.cls).c_str(), int(c.mass));
			log(L"carry: picked up " + actor->GetName() + L" (" + std::wstring(name.begin(), name.end()) + L", " + std::to_wstring(int(c.mass)) +
				L" kg, " + std::to_wstring(int(c.extent)) + L" cm) as " + std::wstring(id.begin(), id.end()) + L", waiting for Minecraft's inventory");
		}

		/// Minecraft's answer to a pickup: the token is in (the object stays out of HN's world) or not (it comes back).
		void carry_answer(const std::string &id, bool ok, const std::string &why)
		{
			auto it = g_carried.find(id);
			if (it == g_carried.end())
				return;
			if (ok)
			{
				it->second.confirmed = true;
				return;
			}
			set_carried_state(it->second.actor, it->second.body, true);
			log(L"carry: Minecraft didn't take " + std::wstring(id.begin(), id.end()) + L" (" + std::wstring(why.begin(), why.end()) + L"): back in HN's world");
			notify(why == "hand not empty" ? "Empty your hand first" : why == "inventory full" ? "Inventory full" : "Couldn't pick that up");
			g_carried.erase(it);
		}

		/// Whether something solid is in the way of an object of half size `extent` at `at`: what blocks a player (the
		/// "Pawn" profile), not Steve's own capsule, not the object itself. It was an overlap by object types, and HN's
		/// levels are full of trigger volumes (overlap only, never blocking): every place and throw of the 20:31 session
		/// found "no room". `blocker` names what was in the way.
		bool overlaps_world(UObject *pawn, const Vec3f &at, float extent, UObject *ignore, std::wstring *blocker = nullptr)
		{
			Call c(kismet(), STR("BoxTraceSingleByProfile"));
			if (!c)
				return false;
			static const NameParam pawnProfile = name_param(STR("Pawn"));
			UObject *ignored[1] = {ignore};
			const float e = extent * 0.8f;
			c.set<UObject *>(STR("WorldContextObject"), pawn)
				.set(STR("Start"), Vec3f{at.x, at.y, at.z + 0.5f})
				.set(STR("End"), Vec3f{at.x, at.y, at.z - 0.5f})
				.set(STR("HalfSize"), Vec3f{e, e, e})
				.set(STR("Orientation"), Rot3f{})
				.set(STR("ProfileName"), pawnProfile)
				.set(STR("ActorsToIgnore"), ScriptArray{ignored, ignore ? 1 : 0, ignore ? 1 : 0})
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *hit = c.param<uint8_t>(STR("ReturnValue"));
			if (!hit || !*hit)
				return false;
			if (blocker)
			{
				*blocker = L"?";
				FProperty *outHit = c.property(STR("OutHit"));
				Call brk(gameplay_statics(), STR("BreakHitResult"));
				FProperty *hitParam = brk ? brk.property(STR("Hit")) : nullptr;
				if (outHit && hitParam)
				{
					std::memcpy(brk.data() + hitParam->GetOffset_Internal(), c.data() + outHit->GetOffset_Internal(), size_t(outHit->GetSize()));
					brk.invoke();
					UObject *const *actor = brk.param<UObject *>(STR("HitActor"));
					UObject *const *component = brk.param<UObject *>(STR("HitComponent"));
					*blocker = (actor && *actor ? (*actor)->GetName() : std::wstring(L"-")) + L" / " +
						(component && *component ? (*component)->GetName() : std::wstring(L"-"));
				}
			}
			return true;
		}

		/// The carried object for an id: the hidden one, or a new one of its class (the old one went with its level).
		Carried *carried_for(UObject *pc, UObject *pawn, const std::string &id, const std::string &cls)
		{
			if (auto it = g_carried.find(id); it != g_carried.end() && it->second.level == pc)
				return &it->second;
			g_carried.erase(id);
			UObject *klass = cls.empty() ? nullptr : UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, std::wstring(cls.begin(), cls.end()).c_str());
			UObject *statics = gameplay_statics();
			if (!klass || !statics)
			{
				log(L"carry: " + std::wstring(id.begin(), id.end()) + L" can't be made again in this level (class not loaded)");
				return nullptr;
			}
			Transform4 t;
			t.pos[0] = g_eye.x, t.pos[1] = g_eye.y, t.pos[2] = g_eye.z - 1000.0f;
			Call begin(statics, STR("BeginDeferredActorSpawnFromClass"));
			begin.set<UObject *>(STR("WorldContextObject"), pawn).set<UObject *>(STR("ActorClass"), klass).set(STR("SpawnTransform"), t)
				.set<uint8_t>(STR("CollisionHandlingOverride"), 1).set<UObject *>(STR("Owner"), nullptr);
			if (!begin.invoke())
				return nullptr;
			UObject **actor = begin.param<UObject *>(STR("ReturnValue"));
			if (!actor || !*actor)
				return nullptr;
			Call(statics, STR("FinishSpawningActor")).set<UObject *>(STR("Actor"), *actor).set(STR("SpawnTransform"), t).invoke();
			Call root(*actor, STR("K2_GetRootComponent"));
			root.invoke();
			UObject *const *r = root.param<UObject *>(STR("ReturnValue"));
			Carried c;
			c.actor = *actor;
			c.body = r ? *r : nullptr;
			c.cls = std::wstring(cls.begin(), cls.end());
			c.scale = actor_scale(*actor);
			Vec3f origin, extent;
			if (actor_bounds(*actor, origin, extent))
				c.extent = std::max({extent.x, extent.y, extent.z, 5.0f});
			c.mass = c.body ? mass_of(c.body) : 10.0f;
			c.confirmed = true;
			c.level = pc;
			set_carried_state(c.actor, c.body, false);
			log(L"carry: made a new " + (*actor)->GetName() + L" for " + std::wstring(id.begin(), id.end()));
			return &(g_carried[id] = c);
		}

		/// What an object does by itself in HN's world, stopped while it is stored in another slot: its playing sounds
		/// (a radio's music), its active particle effects, its tick. A stored object is hidden, without collision and
		/// physics, and left where it was last held: the radio went on playing there (the user: "it stayed there as I
		/// walked away"). Each one stopped is remembered and started again when the object is held or put back.
		/// Nothing else is touched (other objects' sounds, HN's audio).
		void suspend_activity(Carried &c)
		{
			if (!c.actor || !c.paused.empty() || c.life == Carried::Life::Stored)
				return; // stored already: its tick state stays the one it had before (read again, it would be "off")
			static UObject *audio = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.AudioComponent"));
			static UObject *particles = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.ParticleSystemComponent"));
			struct ObjectArray
			{
				UObject **data;
				int32 num, max;
			};
			int sounds = 0, effects = 0;
			for (UObject *kind : {audio, particles})
			{
				if (!kind)
					continue;
				Call list(c.actor, STR("GetComponentsByClass"));
				list.set<UObject *>(STR("ComponentClass"), kind);
				const ObjectArray *a = list.invoke() ? list.param<ObjectArray>(STR("ReturnValue")) : nullptr;
				for (int i = 0; a && i < a->num; ++i)
				{
					UObject *comp = a->data[i];
					if (!comp)
						continue;
					if (kind == audio)
					{
						Call playing(comp, STR("IsPlaying"));
						const uint8_t *on = playing.invoke() ? playing.param<uint8_t>(STR("ReturnValue")) : nullptr;
						if (on && *on)
						{
							Call(comp, STR("SetPaused")).set_bool(STR("bPause"), true).invoke();
							c.paused.push_back(comp);
							++sounds;
						}
					}
					else
					{
						Call active(comp, STR("IsActive"));
						const uint8_t *on = active.invoke() ? active.param<uint8_t>(STR("ReturnValue")) : nullptr;
						if (on && *on)
						{
							Call(comp, STR("Deactivate")).invoke();
							c.paused.push_back(comp);
							++effects;
						}
					}
				}
			}
			Call ticking(c.actor, STR("IsActorTickEnabled"));
			const uint8_t *t = ticking.invoke() ? ticking.param<uint8_t>(STR("ReturnValue")) : nullptr;
			c.tickWas = !t || *t;
			Call(c.actor, STR("SetActorTickEnabled")).set_bool(STR("bEnabled"), false).invoke();
			log(L"carry: stored " + c.actor->GetName() + L": " + std::to_wstring(sounds) + L" sounds paused, " + std::to_wstring(effects) +
				L" effects stopped, its tick off");
		}

		void resume_activity(Carried &c)
		{
			if (!c.actor)
				return;
			if (c.life == Carried::Life::Stored)
				Call(c.actor, STR("SetActorTickEnabled")).set_bool(STR("bEnabled"), c.tickWas).invoke();
			if (c.paused.empty())
				return;
			static UObject *audio = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/Engine.AudioComponent"));
			for (UObject *comp : c.paused)
			{
				if (audio && comp->IsA(static_cast<UClass *>(audio)))
					Call(comp, STR("SetPaused")).set_bool(STR("bPause"), false).invoke();
				else
					Call(comp, STR("Activate")).set_bool(STR("bReset"), false).invoke();
			}
			log(L"carry: " + c.actor->GetName() + L" active again (" + std::to_wstring(c.paused.size()) + L" sounds/effects resumed)");
			c.paused.clear();
		}

		/// Back into HN's world at `at`, as itself (scale, collision, physics), moving with `velocity`; Minecraft's
		/// token goes after.
		void release(const std::string &id, Carried &c, const Vec3f &at, const Vec3f &velocity)
		{
			resume_activity(c);
			c.life = Carried::Life::Picked;
			carry_detach(c);
			set_actor_scale(c.actor, c.scale);
			place_actor(c.actor, at, g_mcLookYaw + 90.0f);
			if (c.body)
				Call(c.body, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), true).invoke();
			set_carried_state(c.actor, c.body, true);
			if (c.body)
				Call(c.body, STR("SetPhysicsLinearVelocity")).set(STR("NewVel"), velocity).set_bool(STR("bAddToCurrent"), false)
					.set(STR("BoneName"), NameParam{}).invoke();
			sendf("{\"t\":\"consume\",\"id\":\"%s\"}", id.c_str());
			if (g_shownId == id)
				g_shownId.clear();
			g_carried.erase(id);
		}

		/// Right click holding a carried object: put it down where Steve looks (on what the look meets, out of walls).
		bool carry_place(UObject *pc, UObject *pawn)
		{
			Carried *c = carried_for(pc, pawn, g_heldId, g_heldCls);
			if (!c)
				return false;
			const std::string id = g_heldId;
			const Vec3f dir = look_dir();
			const float reach = 250.0f;
			Vec3f at{g_eye.x + dir.x * 120.0f, g_eye.y + dir.y * 120.0f, g_eye.z + dir.z * 120.0f};
			Call trace(kismet(), STR("LineTraceSingle"));
			UObject *ignored[1] = {c->actor};
			trace.set<UObject *>(STR("WorldContextObject"), pawn).set(STR("Start"), g_eye)
				.set(STR("End"), Vec3f{g_eye.x + dir.x * reach, g_eye.y + dir.y * reach, g_eye.z + dir.z * reach})
				.set<uint8_t>(STR("TraceChannel"), 0).set(STR("ActorsToIgnore"), ScriptArray{ignored, 1, 1})
				.set_bool(STR("bTraceComplex"), false).set<uint8_t>(STR("DrawDebugType"), 0).set_bool(STR("bIgnoreSelf"), true);
			trace.invoke();
			if (const uint8_t *hit = trace.param<uint8_t>(STR("ReturnValue")); hit && *hit)
			{
				Vec3f p;
				if (impact_point(trace, p))
				{
					// back off the surface along the look by the object's half size
					const float back = c->extent + 3.0f;
					at = {p.x - dir.x * back, p.y - dir.y * back, p.z - dir.z * back + (dir.z < -0.3f ? c->extent : 0.0f)};
				}
			}
			const float fromEye = std::hypot(at.x - g_eye.x, at.y - g_eye.y);
			bool free = false;
			std::wstring blocker;
			for (int lift = 0; lift < 4 && !free; ++lift)
			{
				const Vec3f tryAt{at.x, at.y, at.z + lift * 10.0f};
				if (!overlaps_world(pawn, tryAt, c->extent, c->actor, &blocker) && fromEye > 20.0f)
				{
					at = tryAt;
					free = true;
				}
			}
			if (!free)
			{
				notify("No room to put it there");
				log(L"carry: no room to place " + std::wstring(id.begin(), id.end()) + L" (" + std::to_wstring(int(c->extent)) + L" cm, " +
					(fromEye <= 20.0f ? std::wstring(L"too close") : L"in the way: " + blocker) + L")");
				return false;
			}
			log(L"carry: placed " + std::wstring(id.begin(), id.end()));
			release(id, *c, at, Vec3f{});
			return true;
		}

		/// Q holding a carried object: thrown along the look, a little upwards, from just in front of Steve.
		bool carry_throw(UObject *pc, UObject *pawn, const std::string &id, const std::string &cls, float charge)
		{
			Carried *c = carried_for(pc, pawn, id, cls);
			if (!c)
				return false;
			const Vec3f dir = look_dir();
			const float out = g_steveWidth * 0.5f + c->extent + 10.0f;
			const Vec3f at{g_eye.x + dir.x * out, g_eye.y + dir.y * out, g_eye.z - 20.0f + dir.z * out};
			if (std::wstring blocker; overlaps_world(pawn, at, c->extent, c->actor, &blocker))
			{
				notify("No room to throw it");
				log(L"carry: no room to throw " + std::wstring(id.begin(), id.end()) + L" (in the way: " + blocker + L")");
				return false;
			}
			// lighter objects fly further; on HN's mass scale (props 30-400 kg) 6-11 m/s at the middle of the charge; a tap
			// 0.45 of it, the whole 1.2 s charge 1.45 of it (the cap: holding Q longer adds nothing)
			const float base = std::clamp(1100.0f / (1.0f + c->mass / 600.0f), 600.0f, 1100.0f);
			const float speed = base * (0.45f + std::clamp(charge, 0.0f, 1.0f));
			const Vec3f v{dir.x * speed, dir.y * speed, dir.z * speed + 200.0f};
			log(L"carry: threw " + std::wstring(id.begin(), id.end()) + L" at " + std::to_wstring(int(speed)) + L" cm/s (charge " +
				std::to_wstring(int(charge * 100.0f)) + L"%)");
			release(id, *c, at, v);
			g_ws.send("{\"t\":\"swing\"}");
			return true;
		}

		/// What the crosshair is on in HN (Minecraft's eye and look, as the right click and the pickup trace), up to
		/// `reach` cm, not `ignore`: the actor, the component, and the point.
		bool look_hit(UObject *pawn, UObject *ignore, float reach, UObject *&actor, UObject *&component, Vec3f &point)
		{
			actor = component = nullptr;
			if (!g_haveEye)
				return false;
			const Vec3f dir = look_dir();
			Call c(kismet(), STR("LineTraceSingle"));
			if (!c)
				return false;
			UObject *ignored[1] = {ignore};
			c.set<UObject *>(STR("WorldContextObject"), pawn).set(STR("Start"), g_eye)
				.set(STR("End"), Vec3f{g_eye.x + dir.x * reach, g_eye.y + dir.y * reach, g_eye.z + dir.z * reach})
				.set<uint8_t>(STR("TraceChannel"), 0).set(STR("ActorsToIgnore"), ScriptArray{ignored, ignore ? 1 : 0, ignore ? 1 : 0})
				.set_bool(STR("bTraceComplex"), false).set<uint8_t>(STR("DrawDebugType"), 0).set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			FProperty *outHit = c.property(STR("OutHit"));
			if (!ok || !*ok || !outHit)
				return false;
			impact_point(c, point);
			Call brk(gameplay_statics(), STR("BreakHitResult"));
			FProperty *hitParam = brk ? brk.property(STR("Hit")) : nullptr;
			if (!hitParam)
				return false;
			std::memcpy(brk.data() + hitParam->GetOffset_Internal(), c.data() + outHit->GetOffset_Internal(), size_t(outHit->GetSize()));
			brk.invoke();
			UObject *const *a = brk.param<UObject *>(STR("HitActor"));
			UObject *const *k = brk.param<UObject *>(STR("HitComponent"));
			actor = a ? *a : nullptr;
			component = k ? *k : nullptr;
			return actor != nullptr;
		}

		struct FStringArray
		{
			struct Str
			{
				const wchar_t *data;
				int32 num, max;
			} *data;
			int32 num, max;
		};

		/// A lock's state for the log: open or not, and the codes it opens for.
		std::wstring lock_state(UObject *lock, bool *openOut = nullptr)
		{
			Call open(lock, STR("IsOpen"));
			const uint8_t *o = open.invoke() ? open.param<uint8_t>(STR("ReturnValue")) : nullptr;
			if (openOut)
				*openOut = o && *o;
			std::wstring codes;
			if (const FStringArray *a = lock->GetValuePtrByPropertyNameInChain<FStringArray>(STR("m_Codes")))
				for (int i = 0; i < a->num; ++i)
					codes += (codes.empty() ? L"" : L", ") + (a->data[i].data && a->data[i].num > 1 ? std::wstring(a->data[i].data, size_t(a->data[i].num - 1)) : L"\"\"");
			return std::wstring(o ? (*o ? L"open" : L"locked") : L"?") + L" (codes: " + (codes.empty() ? L"none" : codes) + L")";
		}

		/// Whether a lock opens for this key, by HN's own rule (the key's Code is one of the lock's m_Codes).
		bool key_fits(UObject *key, UObject *lock)
		{
			const std::wstring code = string_property(key, STR("Code"));
			const FStringArray *a = lock->GetValuePtrByPropertyNameInChain<FStringArray>(STR("m_Codes"));
			for (int i = 0; a && !code.empty() && i < a->num; ++i)
				if (a->data[i].data && a->data[i].num > 1 && _wcsicmp(std::wstring(a->data[i].data, size_t(a->data[i].num - 1)).c_str(), code.c_str()) == 0)
					return true; // as HN compares them (case-insensitive)
			return false;
		}

		/// HN's own apply of a usable item, the code HN runs when its left mouse uses the item in its hands: each usable
		/// class binds a native method (not a UFUNCTION: nothing reflection can name) into its ApplyComponent, in the
		/// component's unreflected bytes at 0xF0 (an FDelegateBase: the bound instance, then its size), signature
		/// bool (ACharacter *, AActor *target). Found in the game's code (HelloNeighbor-Win64-Shipping.exe): ALockpick's
		/// constructor at 0x14050c1c0 binds 0x14051fea0, which checks the key's Code against each of the lock's m_Codes
		/// (case-insensitive), and on a match tells the character, frees the lock from its door, sets it open (byte
		/// 0x440, what IsOpen reads), calls OnChange(true), plays the lock's OpenSound, and deletes the key only if it is
		/// bOneOff (FlatComponent mode FM_Delete); no match: the character's "doesn't fit". The blueprint event
		/// OnApplyToMe that the first version called has no native code and the locks don't implement it (nothing
		/// happened), and the lock's OnChange(true) it fell back to is only the lock's look: no sound, HN's state
		/// left locked (the 13:28 session).
		/// The bound instance is checked before it is called: its object is this very item (the weak pointer's index is
		/// the item's), its vtable and method are in the game's own image. False if it isn't there or doesn't check out.
		bool native_apply(UObject *item, UObject *character, UObject *target, bool &result, std::wstring &why)
		{
			UObject *component = apply_component(item);
			if (!component)
				return why = L"no ApplyComponent", false;
			static const uint8_t *image = reinterpret_cast<const uint8_t *>(GetModuleHandleW(nullptr));
			static size_t imageSize = [] {
				const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
				const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
				return size_t(nt->OptionalHeader.SizeOfImage);
			}();
			auto inImage = [](const void *ptr) { return ptr >= image && ptr < image + imageSize; };
			const uint8_t *instance = *reinterpret_cast<const uint8_t *const *>(reinterpret_cast<const uint8_t *>(component) + 0xF0);
			if (!instance)
				return why = L"its ApplyComponent has nothing bound (HN gives this item no apply)", false;
			const void *vtable = *reinterpret_cast<const void *const *>(instance);
			const int32 objectIndex = *reinterpret_cast<const int32 *>(instance + 8);
			const int32 itemIndex = *reinterpret_cast<const int32 *>(reinterpret_cast<const uint8_t *>(item) + 0xC); // UObjectBase::InternalIndex
			using Apply = bool (*)(UObject *self, UObject *character, UObject *target);
			const Apply method = *reinterpret_cast<const Apply *>(instance + 0x10);
			if (!inImage(vtable) || !inImage(reinterpret_cast<const void *>(method)) || objectIndex != itemIndex)
			{
				wchar_t text[200];
				swprintf(text, 200, L"the bound apply doesn't check out (vtable %p, method %p, object %d, item %d)", vtable,
					reinterpret_cast<const void *>(method), objectIndex, itemIndex);
				return why = text, false;
			}
			wchar_t text[96];
			swprintf(text, 96, L"HN's native apply (image+0x%llx)", static_cast<unsigned long long>(reinterpret_cast<const uint8_t *>(method) - image));
			why = text;
			// its answer: true only when the item is used up (the lockpick's: a fitting bOneOff key); a reusable key that
			// opened the lock answers false, as one that didn't fit (the lock's state says which)
			result = method(item, character, target);
			return true;
		}

		/// The FlatComponent's mode (EFlatMode: 0 free, 1 delete, 2 fixed, 4 in the human's hand, 7 inventory...).
		int flat_mode(UObject *actor)
		{
			UObject **flat = actor ? actor->GetValuePtrByPropertyNameInChain<UObject *>(STR("FlatComponent")) : nullptr;
			const uint8_t *mode = flat && *flat ? (*flat)->GetValuePtrByPropertyNameInChain<uint8_t>(STR("Mode")) : nullptr;
			return mode ? *mode : -1;
		}

		/// Left button with one of HN's usable items in the hand (a key): HN's own apply of it (native_apply) on what the
		/// crosshair is on; for a door, its lock nearest the crosshair (the locks hang on the door). One call per click.
		/// Logged with the lock's state before and after, HN's answer, and the item's fate, which is HN's: an item HN
		/// deletes (a one-off lockpick) or takes (its mode changed) leaves Minecraft's inventory; anything else stays,
		/// as it is, in the hand (HN's keys are kept: only bOneOff ones go). The first version took the key's token
		/// away whenever the key wasn't hidden, and a held key is never hidden: the key left the inventory and its
		/// actor stayed on the camera, no longer ours (the user: "the key remains in my hand ... leaves my inventory").
		/// False: no usable item, or nothing of HN's in reach (the click stays Minecraft's attack).
		bool item_use(UObject *pc, UObject *pawn)
		{
			auto it = g_carried.find(g_heldId);
			if (it == g_carried.end() || !it->second.confirmed || !it->second.actor || it->second.level != pc)
				return false;
			Carried &c = it->second;
			if (!apply_component(c.actor))
				return false;
			UObject *actor = nullptr, *component = nullptr;
			Vec3f point{};
			if (!look_hit(pawn, c.actor, 300.0f, actor, component, point) || !class_contains(actor, L"FlatActor"))
				return false;
			UObject *target = actor;
			if (!class_contains(actor, L"Lock") && class_contains(actor, L"Door"))
			{
				struct ObjectArray
				{
					UObject **data;
					int32 num, max;
				};
				float best = 80.0f;
				if (const ObjectArray *locks = actor->GetValuePtrByPropertyNameInChain<ObjectArray>(STR("m_Locks")))
					for (int i = 0; i < locks->num; ++i)
					{
						Vec3f at;
						if (UObject *lock = locks->data[i]; lock && actor_location(lock, at))
						{
							const float d = std::hypot(at.x - point.x, at.y - point.y, at.z - point.z);
							if (d < best)
								best = d, target = lock;
						}
					}
			}
			const bool isLock = class_contains(target, L"Lock") && !class_contains(target, L"Lockpick");
			bool openBefore = false, openAfter = false;
			const std::wstring before = isLock ? lock_state(target, &openBefore) : L"-";
			const int modeBefore = flat_mode(c.actor);
			bool result = false;
			std::wstring how;
			const bool applied = native_apply(c.actor, pawn, target, result, how);
			const std::wstring after = isLock ? lock_state(target, &openAfter) : L"-";
			log(L"use: " + item_name_w(c.actor) + L" on " + target->GetName() + (target != actor ? L" (on " + actor->GetName() + L")" : L"") + L": " + how +
				(applied ? std::wstring(result ? L", HN used the item up (a one-off)" : L", the item is kept") : L"") +
				(isLock ? L"; lock " + before + L" -> " + after + (openAfter && !openBefore ? L": UNLOCKED" : L"") +
					(key_fits(c.actor, target) ? L" (the key's code is the lock's)" : L" (the key's code isn't the lock's)") : L""));
			if (!applied)
				return false; // not HN's to apply: the click stays Minecraft's
			g_ws.send("{\"t\":\"swing\"}");
			// the item's fate is HN's: deleted (gone, or its mode FM_Delete) or taken (its mode changed) -> the token goes;
			// else it stays ours, in the hand
			Call valid(kismet(), STR("IsValid"));
			const uint8_t *alive = valid.set<UObject *>(STR("Object"), c.actor).invoke() ? valid.param<uint8_t>(STR("ReturnValue")) : nullptr;
			const int modeAfter = alive && *alive ? flat_mode(c.actor) : -1;
			if (alive && *alive && modeAfter == modeBefore)
				return true;
			const bool deleted = !alive || !*alive || modeAfter == 1;
			log(L"use: HN " + std::wstring(deleted ? L"used up " : L"took ") + c.actor->GetName() + L" (its mode " + std::to_wstring(modeBefore) + L" -> " +
				std::to_wstring(modeAfter) + L"): its token goes");
			if (!deleted)
			{
				// HN has it now: off our camera only if it is still on it, its own size back; HN places it
				carry_detach(c);
				set_actor_scale(c.actor, c.scale);
			}
			c.attachedTo = nullptr;
			sendf("{\"t\":\"consume\",\"id\":\"%s\"}", g_heldId.c_str());
			if (g_shownId == g_heldId)
				g_shownId.clear();
			g_carried.erase(it);
			return true;
		}

		void carry_start_pickup(UObject *actor, UObject *body)
		{
			g_pickup = {actor, body, GetTickCount(), true, electric(actor)};
			if (g_throwCharge.active)
			{
				g_throwCharge.active = false; // never both bars
				progress("throw", -1.0f);
			}
			progress("pickup", 0.0f);
			notify("Picking up " + item_name(actor));
			log(L"carry: pickup started on " + actor->GetName() + L" (hold " + std::to_wstring(kPickupHold) + L" ms)");
		}

		void carry_cancel_pickup()
		{
			if (g_pickup.active)
			{
				notify("");
				progress("pickup", -1.0f);
				if (g_pickup.tapActivates)
				{
					// a tap on the radio: HN switches it (the same thing under the crosshair: HN's E traces from the eyes)
					log(L"right click: a tap on " + g_pickup.actor->GetName() + L": HN's action (switch it)");
					hn_action_tap();
				}
				else
					log(L"carry: pickup cancelled (released after " + std::to_wstring(GetTickCount() - g_pickup.start) + L" ms)");
			}
			g_pickup.active = false;
		}

		/// Every tick while Minecraft drives: the pickup in progress, and the carried object in the hand.
		float grip_scale(UObject *actor);

		void carry_tick(UObject *pc, UObject *pawn)
		{
			// HN's action, 0.6 s on: if HN holds something now, its action took an object into HN's hands or hotbar, which
			// is what NeighborCraft's pickup should have done: logged with what the eligibility rule looks at
			if (g_hnActionAt && GetTickCount() - g_hnActionAt > 600)
			{
				g_hnActionAt = 0;
				Call h(pawn, STR("GetHoldingItem"));
				if (UObject *const *item = h.invoke() ? h.param<UObject *>(STR("ReturnValue")) : nullptr; item && *item)
				{
					Call rr(*item, STR("K2_GetRootComponent"));
					rr.invoke();
					UObject *const *root = rr.param<UObject *>(STR("ReturnValue"));
					const PropShape shape = root && *root ? prop_shape(*item, *root) : PropShape{};
					log(L"carry: HN took " + (*item)->GetName() + L" into its hands after its action (movable " + std::to_wstring(shape.movable) +
						L", " + std::to_wstring(shape.parts) + L" parts, " + std::to_wstring(int(shape.extent)) + L" cm): NeighborCraft didn't take it");
				}
			}
			// a level change took the hidden objects with it
			for (auto it = g_carried.begin(); it != g_carried.end();)
				it = it->second.level != pc ? g_carried.erase(it) : std::next(it);
			if (g_pickup.active)
			{
				UObject *body = nullptr;
				UObject *now = pickup_target(pawn, &body);
				const DWORD held = GetTickCount() - g_pickup.start;
				if (now != g_pickup.actor)
				{
					g_pickup.active = false;
					notify("");
					progress("pickup", -1.0f);
					log(L"carry: pickup cancelled (" + std::wstring(now ? L"looking at another object" : L"target lost or out of reach") + L")");
				}
				else if (held >= kPickupHold)
				{
					g_pickup.active = false;
					notify("");
					progress("pickup", -1.0f);
					complete_pickup(pc, g_pickup.actor, g_pickup.body);
				}
				else
				{
					static DWORD nextBar = 0;
					if (GetTickCount() > nextBar)
					{
						nextBar = GetTickCount() + 50;
						progress("pickup", float(held) / float(kPickupHold));
					}
				}
			}
			// the object in the selected slot, where Minecraft's hand is: right of and below the view, its size
			// fitted (at most ~35 cm across), turned with the look
			if (g_shownId != g_heldId)
			{
				if (auto it = g_carried.find(g_shownId); it != g_carried.end())
				{
					carry_detach(it->second);
					set_actor_scale(it->second.actor, it->second.scale);
					set_carried_state(it->second.actor, nullptr, false);
					Call(it->second.actor, STR("SetActorHiddenInGame")).set_bool(STR("bNewHidden"), true).invoke();
					it->second.shown = false;
					suspend_activity(it->second);
					it->second.life = Carried::Life::Stored;
				}
				g_shownId.clear();
			}
			g_heldBoxHalf = 0.0;
			if (g_heldId.empty())
				return;
			if (auto held = g_carried.find(g_heldId); held != g_carried.end() && held->second.shown)
				g_heldBoxHalf = std::clamp(held->second.extent * std::clamp(18.0f / std::max(held->second.extent, 1.0f), 0.05f, 3.0f), 3.0f, 40.0f) / 100.0;
			Carried *c = nullptr;
			const auto found = g_carried.find(g_heldId);
			if (found != g_carried.end() && found->second.confirmed)
				c = &found->second;
			static std::string reported;
			if (!c)
			{
				if (reported != g_heldId)
				{
					reported = g_heldId;
					log(L"carry: holding " + std::wstring(g_heldId.begin(), g_heldId.end()) +
						(found == g_carried.end() ? L" but HN has no such object here" : L" but Minecraft hasn't confirmed it yet"));
				}
				return;
			}
			const bool first = g_shownId != g_heldId;
			g_shownId = g_heldId;
			if (first)
			{
				c->selectedAt = GetTickCount();
				c->shown = false;
				if (c->life == Carried::Life::Stored)
					resume_activity(*c);
				c->life = Carried::Life::Held;
			}
			if (first && c->body) // held at the eye, small: no shadow of it on the room (back with release())
				Call(c->body, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), false).invoke();
			// Its size in the hand: every object brought to one visual size, about twice Minecraft's fist (26 cm in half at
			// the fist's 1.35 m: 19.3% of its distance), times its grip's scale. It was only ever shrunk (to 18%), never
			// grown: a mug stayed its own size and looked smaller than the fist (the user). Small things grow up to 3x.
			// Only the copy in the hand: put down or thrown, the object is its own size again (release()).
			const bool fistKnown = fist_for(g_heldId);
			constexpr float kHeldHalf = 0.193f;
			const float target = (fistKnown ? kHeldHalf * 100.0f * g_handDist : 26.0f) * grip_scale(c->actor);
			const float fit = std::clamp(target / std::max(c->extent, 1.0f), 0.05f, 3.0f);
			c->fit = fit;
			// Shown only once Minecraft has drawn the carrying fist for this very object: until then it stays hidden. The
			// first frame after a pickup used to be shown on a fallback fist in front of the camera (Minecraft hadn't
			// drawn the arm yet), then jumped onto Minecraft's and rose with its equip motion (the user: "in the wrong
			// location for a split second"; the 12:43 log: every pickup's first "hand ok" line had "fist default").
			// Without a fist for 600 ms (third person, or the arm not drawn) it is shown in front after all.
			const DWORD waited = GetTickCount() - c->selectedAt;
			const bool inHand3p = !g_mcFirstPerson && hand3p_fresh(); // third person: Steve's hand is where it goes
			if (!fistKnown && !inHand3p && waited < 600)
			{
				if (c->shown)
				{
					Call(c->actor, STR("SetActorHiddenInGame")).set_bool(STR("bNewHidden"), true).invoke();
					c->shown = false;
				}
				return;
			}
			if (!c->shown)
			{
				c->shown = true;
				Call(c->actor, STR("SetActorHiddenInGame")).set_bool(STR("bNewHidden"), false).invoke();
				log(L"carry: in the hand: " + c->actor->GetName() + L", scale " + std::to_wstring(fit) + L", shown " + std::to_wstring(waited) +
					(fistKnown ? L" ms after it was selected, on Minecraft's fist for it (hidden until then)" : L" ms after it was selected: no fist from Minecraft, in front"));
			}
		}

		/// How an object sits in the hand: offsets from the fist (cm, the camera's forward/right/up) and a turn from the
		/// camera's own rotation (degrees, Unreal pitch/yaw/roll), matched by class name; the first match wins, "" for the
		/// rest. Read from NeighborCraft_grips.txt next to the mod if it is there ("match fwd right up pitch yaw roll").
		struct Grip
		{
			std::wstring match;
			float fwd = 0.0f, right = 0.0f, up = 0.0f, pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
			float scale = 1.0f; // times the common held size
		};

		const std::vector<Grip> &grips()
		{
			static std::vector<Grip> list;
			static bool loaded = false;
			if (!loaded)
			{
				loaded = true;
				std::wifstream f(module_dir() + L"NeighborCraft_grips.txt");
				std::wstring line;
				while (std::getline(f, line))
				{
					if (line.empty() || line[0] == L'#')
						continue;
					std::wistringstream in(line);
					Grip g;
					if (in >> g.match >> g.fwd >> g.right >> g.up >> g.pitch >> g.yaw >> g.roll)
					{
						if (float sc; in >> sc && sc > 0.05f)
							g.scale = sc;
						if (g.match == L"*")
							g.match.clear();
						list.push_back(g);
					}
				}
				log(L"carry: " + std::to_wstring(list.size()) + L" grips from NeighborCraft_grips.txt");
				if (std::none_of(list.begin(), list.end(), [](const Grip &g) { return g.match.empty(); }))
					list.push_back(Grip{});
			}
			return list;
		}

		const Grip &grip_for(UObject *actor)
		{
			for (const Grip &g : grips())
				if (g.match.empty() || class_contains(actor, g.match.c_str()))
					return g;
			return grips().back();
		}

		float grip_scale(UObject *actor)
		{
			return grip_for(actor).scale;
		}

		// Unreal's rotator <-> quaternion (x, y, z, w), degrees
		void quat_of(const Rot3f &r, float q[4])
		{
			const float d = 3.14159265f / 360.0f;
			const float sp = std::sin(r.pitch * d), cp = std::cos(r.pitch * d), sy = std::sin(r.yaw * d), cy = std::cos(r.yaw * d),
						sr = std::sin(r.roll * d), cr = std::cos(r.roll * d);
			q[0] = cr * sp * sy - sr * cp * cy;
			q[1] = -cr * sp * cy - sr * cp * sy;
			q[2] = cr * cp * sy - sr * sp * cy;
			q[3] = cr * cp * cy + sr * sp * sy;
		}

		Rot3f rot_of(const float q[4])
		{
			const float x = q[0], y = q[1], z = q[2], w = q[3], r2d = 180.0f / 3.14159265f;
			const float sing = z * x - w * y, yawY = 2.0f * (w * z + x * y), yawX = 1.0f - 2.0f * (y * y + z * z);
			Rot3f r{};
			r.yaw = std::atan2(yawY, yawX) * r2d;
			if (sing < -0.4999995f)
				r.pitch = -90.0f, r.roll = -r.yaw - 2.0f * std::atan2(x, w) * r2d;
			else if (sing > 0.4999995f)
				r.pitch = 90.0f, r.roll = r.yaw - 2.0f * std::atan2(x, w) * r2d;
			else
				r.pitch = std::asin(2.0f * sing) * r2d, r.roll = std::atan2(-2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y)) * r2d;
			return r;
		}

		void quat_mul(const float a[4], const float b[4], float out[4])
		{
			out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
			out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
			out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
			out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
		}

		/// The held object, rigid in the camera's frame: where Minecraft draws the fist (sent in the camera's frame:
		/// HN's camera of this frame places it, not Minecraft's of an older one), turned with the camera's yaw, pitch and
		/// roll plus the object's grip, its grip point (its mesh's bottom middle) on the fist. It used to be placed in
		/// world space with the look's yaw only and before HN's camera moved: looking up or down it stayed upright and
		/// slid up and down against the hand, a frame behind (the user: "moves vertically with the camera").
		/// The held object in the camera's frame as the engine has it: (its location - the camera's) turned back by the
		/// camera's rotation, and its rotation relative to the camera's (forward and up axes).
		void held_in_camera(Carried &c, const Vec3f &loc, const Rot3f &camRot, Vec3f &pos, Vec3f &fwd, Vec3f &up)
		{
			Call w(c.actor, STR("K2_GetActorLocation"));
			w.invoke();
			Call r(c.actor, STR("K2_GetActorRotation"));
			r.invoke();
			const Vec3f *wl = w.param<Vec3f>(STR("ReturnValue"));
			const Rot3f *wr = r.param<Rot3f>(STR("ReturnValue"));
			float qc[4], qcInv[4], qo[4];
			quat_of(camRot, qc);
			qcInv[0] = -qc[0], qcInv[1] = -qc[1], qcInv[2] = -qc[2], qcInv[3] = qc[3];
			pos = wl ? rotate(qcInv, Vec3f{wl->x - loc.x, wl->y - loc.y, wl->z - loc.z}) : Vec3f{};
			quat_of(wr ? *wr : Rot3f{}, qo);
			fwd = rotate(qcInv, rotate(qo, Vec3f{1, 0, 0}));
			up = rotate(qcInv, rotate(qo, Vec3f{0, 0, 1}));
		}

		/// The full turn, in the engine, once per pickup: the view camera turned through 360 degrees of yaw (every 15,
		/// at pitch -60, 0 and 60) within this tick, and the held object read back at each pose in the camera's frame:
		/// it must not move there. The camera is put back before the frame is drawn. Logged: the largest change of the
		/// object's place (cm) and turn (its axes) relative to the camera, and the pose where it happened.
		void carry_sweep(Carried &c, UObject *camera, const Vec3f &loc, const Rot3f &rot)
		{
			auto pose = [&](const Rot3f &r) {
				Call(camera, STR("K2_SetActorLocationAndRotation")).set(STR("NewLocation"), loc).set(STR("NewRotation"), r)
					.set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
			};
			Vec3f p0, f0, u0;
			held_in_camera(c, loc, rot, p0, f0, u0);
			float worstPos = 0.0f, worstTurn = 0.0f;
			Rot3f worstAt = rot;
			int poses = 0;
			for (float pitch : {-60.0f, 0.0f, 60.0f})
				for (int step = 0; step < 24; ++step)
				{
					const Rot3f r{pitch, rot.yaw + step * 15.0f, rot.roll};
					pose(r);
					Vec3f p, f, u;
					held_in_camera(c, loc, r, p, f, u);
					++poses;
					const float dp = std::sqrt((p.x - p0.x) * (p.x - p0.x) + (p.y - p0.y) * (p.y - p0.y) + (p.z - p0.z) * (p.z - p0.z));
					const float dt = std::max(std::sqrt((f.x - f0.x) * (f.x - f0.x) + (f.y - f0.y) * (f.y - f0.y) + (f.z - f0.z) * (f.z - f0.z)),
						std::sqrt((u.x - u0.x) * (u.x - u0.x) + (u.y - u0.y) * (u.y - u0.y) + (u.z - u0.z) * (u.z - u0.z)));
					if (dp > worstPos || dt > worstTurn)
						worstAt = r;
					worstPos = std::max(worstPos, dp), worstTurn = std::max(worstTurn, dt);
				}
			pose(rot);
			char line[320];
			snprintf(line, sizeof(line), "carry: 360 check of %s: %d camera poses, the object in the camera's frame at %.1f %.1f %.1f cm moved at most %.2f cm and turned at most %.4f (axis change) - worst at yaw %.0f pitch %.0f: %s",
				narrow(c.actor->GetName()).c_str(), poses, p0.x, p0.y, p0.z, worstPos, worstTurn, worstAt.yaw, worstAt.pitch,
				worstPos < 0.5f && worstTurn < 0.01f ? "continuous" : "DISCONTINUOUS");
			log(std::wstring(line, line + strlen(line)));
		}

		/// Off the camera (put down, thrown, another slot): back to world space where it is.
		void carry_detach(Carried &c)
		{
			if (!c.attachedTo || !c.actor)
				return;
			Call(c.actor, STR("K2_DetachFromActor")).set<uint8_t>(STR("LocationRule"), 1).set<uint8_t>(STR("RotationRule"), 1)
				.set<uint8_t>(STR("ScaleRule"), 1).invoke(); // KeepWorld
			c.attachedTo = nullptr;
		}

		/// The held object in the first-person hand: attached to HN's view camera and placed by a transform relative to
		/// it only (the fist in the camera's frame from Minecraft, the grip's offset and turn, its grip point), so the
		/// engine's hierarchy makes it follow the camera, in the same update, whatever way Steve faces. It used to be
		/// placed in world space from the camera's pose each frame; it broke on one half of the turn (the user: turning
		/// east it ended up far away). No world-space step is left on our side, and the world position it really has
		/// is checked and logged (`carry: hand ...`).
		/// Third person: the held object in Steve's hand (Minecraft's main-hand item frame, hand3p), upright, facing his
		/// way, about the size Minecraft gives a held item, casting its own shadow. It was left on the camera, in front
		/// of the view (the user: "in third person the hn items just show up covering the screen").
		void carry_hand_third_person(Carried &c)
		{
			if (c.attachedTo)
			{
				carry_detach(c);
				log(L"carry: " + c.actor->GetName() + L" in Steve's hand (third person)");
			}
			const float size = std::clamp(18.0f * grip_scale(c.actor) / std::max(c.extent, 1.0f), 0.05f, 3.0f);
			set_actor_scale(c.actor, Vec3f{c.scale.x * size, c.scale.y * size, c.scale.z * size});
			const Vec3f at = to_ue(g_hand3p[0], g_hand3p[1], g_hand3p[2]);
			place_actor(c.actor, at, g_mcLookYaw + 90.0f);
			// its middle on the hand (its pivot may be anywhere): by its drawn bounds. actor_bounds counts colliding
			// components only, and a carried object has collision off: its bounds were empty, origin 0, and "at + (at -
			// 0)" put it at twice the hand's coordinates, 60 m away (the 18:16 log: hand -5073 2299 175, object -10147
			// 4598 350): never seen in third person
			Call b(c.actor, STR("GetActorBounds"));
			b.set_bool(STR("bOnlyCollidingComponents"), false);
			const Vec3f *o = b.invoke() ? b.param<Vec3f>(STR("Origin")) : nullptr, *e = b.param<Vec3f>(STR("BoxExtent"));
			if (o && e && e->x + e->y + e->z > 0.5f && std::hypot(o->x - at.x, o->y - at.y, o->z - at.z) < 150.0f)
				place_actor(c.actor, Vec3f{at.x + (at.x - o->x), at.y + (at.y - o->y), at.z + (at.z - o->z)}, g_mcLookYaw + 90.0f);
			// where it really is, once a second (the 18:0x session: placed, but not seen)
			static DWORD nextLog = 0;
			if (GetTickCount() > nextLog)
			{
				nextLog = GetTickCount() + 1000;
				Vec3f w{};
				actor_location(c.actor, w);
				bool hidden = false;
				get_bool_property(c.actor, STR("bHidden"), hidden);
				const Vec3f sc = actor_scale(c.actor);
				char line[360];
				snprintf(line, sizeof(line), "carry: third person: %s at %.0f %.0f %.0f, %.0f cm from the camera (hand %.0f %.0f %.0f), hidden %d, scale %.2f, extent %.0f cm",
					narrow(c.actor->GetName()).c_str(), w.x, w.y, w.z,
					std::sqrt((w.x - g_mcCamera.x) * (w.x - g_mcCamera.x) + (w.y - g_mcCamera.y) * (w.y - g_mcCamera.y) + (w.z - g_mcCamera.z) * (w.z - g_mcCamera.z)),
					at.x, at.y, at.z, int(hidden), sc.x, c.extent);
				log(std::wstring(line, line + strlen(line)));
			}
			if (c.body && !g_heldCasting)
			{
				g_heldCasting = true;
				Call(c.body, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), true).invoke();
			}
		}

		void carry_hand_follow(UObject *camera, const Vec3f &loc, const Rot3f &rot)
		{
			const auto it = g_carried.find(g_shownId);
			if (g_shownId.empty() || it == g_carried.end() || !it->second.actor || !camera)
				return;
			Carried &c = it->second;
			if (!g_mcFirstPerson && hand3p_fresh())
			{
				carry_hand_third_person(c);
				return;
			}
			if (c.body && g_heldCasting) // back to first person: on the camera, its own shadow off (its box casts)
			{
				g_heldCasting = false;
				Call(c.body, STR("SetCastShadow")).set_bool(STR("NewCastShadow"), false).invoke();
			}
			if (c.attachedTo != camera)
			{
				carry_detach(c);
				Call attach(c.actor, STR("K2_AttachToActor"));
				attach.set<UObject *>(STR("ParentActor"), camera).set(STR("SocketName"), NameParam{})
					.set<uint8_t>(STR("LocationRule"), 0).set<uint8_t>(STR("RotationRule"), 0).set<uint8_t>(STR("ScaleRule"), 0) // KeepRelative
					.set_bool(STR("bWeldSimulatedBodies"), false);
				attach.invoke();
				c.attachedTo = camera;
				log(L"carry: " + c.actor->GetName() + L" attached to the view camera");
			}
			const Grip &g = grip_for(c.actor);
			// the fist (Minecraft's, while it draws the carrying arm; else low right in front), plus the grip's offset,
			// in the camera's frame (Unreal: x forward, y right, z up; cm)
			const bool onFist = fist_for(g_shownId);
			const Vec3f fist = onFist ? Vec3f{g_handLocal.z * 100.0f, g_handLocal.x * 100.0f, g_handLocal.y * 100.0f} : Vec3f{60.0f, 25.0f, -25.0f};
			// Near the eye, the same picture: everything scaled towards the camera to kNear. At the fist's own distance
			// (~1.35 m) the object went into walls in front of Steve and vanished; at 25 cm it is inside Steve's own
			// collision radius (30 cm), where nothing of the world can be. HN's near plane is ~10 cm.
			constexpr float kNear = 25.0f;
			const float reach = std::sqrt(fist.x * fist.x + fist.y * fist.y + fist.z * fist.z);
			const float k = reach > kNear ? kNear / reach : 1.0f;
			const Vec3f at{(fist.x + g.fwd) * k, (fist.y + g.right) * k, (fist.z + g.up) * k};
			const float size = c.fit * k;
			set_actor_scale(c.actor, Vec3f{c.scale.x * size, c.scale.y * size, c.scale.z * size});
			float qg[4];
			quat_of(Rot3f{g.pitch, g.yaw, g.roll}, qg);
			// its grip point (its mesh's bottom middle) at its size now, turned by the grip only: the camera turns the rest
			Vec3f lo{}, hi{}, grip{};
			if (c.body && local_bounds(c.body, lo, hi))
				grip = rotate(qg, Vec3f{(lo.x + hi.x) * 0.5f * c.scale.x * size, (lo.y + hi.y) * 0.5f * c.scale.y * size, lo.z * c.scale.z * size});
			const Vec3f rel{at.x - grip.x, at.y - grip.y, at.z - grip.z};
			UObject *root = c.body;
			if (!root)
			{
				Call r(c.actor, STR("K2_GetRootComponent"));
				r.invoke();
				if (UObject *const *rr = r.param<UObject *>(STR("ReturnValue")))
					root = *rr;
			}
			if (root)
				Call(root, STR("K2_SetRelativeLocationAndRotation")).set(STR("NewLocation"), rel).set(STR("NewRotation"), Rot3f{g.pitch, g.yaw, g.roll})
					.set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
			if (!c.swept && root)
			{
				c.swept = true;
				carry_sweep(c, camera, loc, rot);
			}
			// where it really is: logged every second while held, and at once if it is ever far from the camera
			static DWORD nextLog = 0, nextWarn = 0;
			Call where(c.actor, STR("K2_GetActorLocation"));
			where.invoke();
			if (const Vec3f *w = where.param<Vec3f>(STR("ReturnValue")))
			{
				const float dist = std::sqrt((w->x - loc.x) * (w->x - loc.x) + (w->y - loc.y) * (w->y - loc.y) + (w->z - loc.z) * (w->z - loc.z));
				const DWORD now = GetTickCount();
				if (now > nextLog || (dist > 100.0f && now > nextWarn))
				{
					nextLog = now + 1000;
					if (dist > 100.0f)
						nextWarn = now + 250;
					char line[400];
					snprintf(line, sizeof(line), "carry: hand %s: camera yaw %.1f pitch %.1f roll %.1f at %.0f %.0f %.0f; fist %s (%.0f %.0f %.0f cm); relative %.1f %.1f %.1f; object at %.0f %.0f %.0f = %.0f cm from the camera",
						dist > 100.0f ? "FAR" : "ok", rot.yaw, rot.pitch, rot.roll, loc.x, loc.y, loc.z, onFist ? "Minecraft's" : "default", fist.x, fist.y, fist.z,
						rel.x, rel.y, rel.z, w->x, w->y, w->z, dist);
					log(std::wstring(line, line + strlen(line)));
				}
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
			if (level_key(pc) != g_doorsLevel)
			{
				g_doorsLevel = level_key(pc);
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
		using CellMap = std::unordered_map<unsigned long long, std::array<uint8_t, 64>>;
		void voxelise_at(UObject *component, const Transform4 &t, CellMap &out, float grow = 0.0f);

		/// A part's collision cells (`out`), and the same half a sub-cell wider (`mask`, for the static probe).
		void voxelise(UObject *component, CellMap &out, CellMap &mask)
		{
			Transform4 t;
			if (!component_transform(component, t))
				return;
			voxelise_at(component, t, out);
			voxelise_at(component, t, mask, 6.25f);
		}

		/// The sub-cells whose centres a part's box covers, at transform `t`. Every part used to grow by half a sub-cell
		/// all round (up to 12.5 cm more than the part: too tight to walk past, the user said); now a part is its own
		/// size, and only a part thinner than a sub-cell along an axis (a door leaf, 4-5 cm) is widened to one, so a
		/// row of centres always falls inside it and it never has gaps.
		void voxelise_at(UObject *component, const Transform4 &t, CellMap &out, float grow)
		{
			Vec3f lo, hi;
			if (!local_bounds(component, lo, hi))
				return;
			const float inv[4] = {-t.rot[0], -t.rot[1], -t.rot[2], t.rot[3]};
			const Vec3f pos{t.pos[0], t.pos[1], t.pos[2]};
			auto widen = [](float &a, float &b, float scale) {
				const float minSize = 13.0f / std::max(std::abs(scale), 1.0e-3f); // a sub-cell (12.5 cm) and a bit, local units
				if (b - a < minSize)
				{
					const float mid = (a + b) * 0.5f;
					a = mid - minSize * 0.5f;
					b = mid + minSize * 0.5f;
				}
			};
			widen(lo.x, hi.x, t.scale[0]);
			widen(lo.y, hi.y, t.scale[1]);
			widen(lo.z, hi.z, t.scale[2]);
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
			if (level_key(pc) == g_watchedLevel)
				return;
			g_watchedLevel = level_key(pc);
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
			CellMap now, mask;
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
					// Where it stood until now was probed as part of the static world (a closed door is a wall until it
					// first moves): that stayed collision, a door-shaped remnant in the open doorway, until the blocks
					// were probed again (the user, 20:31). Its sub-cells at rest leave the static world now.
					std::unordered_map<unsigned long long, std::array<uint8_t, 64>> rest;
					voxelise_at(w.component, w.first, rest, 6.25f);
					for (auto &[key, bits] : rest)
					{
						const auto b = g_bits.find(key);
						if (b == g_bits.end())
							continue;
						for (int j = 0; j < 64; ++j)
							b->second.box[j] &= uint8_t(~bits[j]);
						// floor heights measured on its top (a drawer's, a cabinet door's)
						for (int sx = 0; sx < 8; ++sx)
							for (int sz = 0; sz < 8; ++sz)
							{
								float &h = b->second.floor[sx * 8 + sz];
								const int sy = std::clamp(int(std::ceil(h * 8.0f)) - 1, 0, 7);
								if (h > 0.0f && (bits[sx * 8 + sy] >> sz & 1u))
									h = 0.0f;
							}
						b->second.boxKnown = false; // and probed again, without it
						g_dirty.insert(key);
					}
				}
				voxelise(w.component, now, mask);
			}
			for (UObject *component : g_nearPhysics)
				if (!held || owner_of(component) != held)
					voxelise(component, now, mask);
			// Not where Steve is. A door leaf swinging into him or an object falling on his head became collision inside
			// his hitbox: Minecraft can't push him out of it, finds no room to stand, and forces him to crawl (the
			// recorder: boxes appearing in his head the tick before POSE_FORCED SWIMMING, at a door). HN's own physics
			// has the part meet his capsule (his size) instead; Minecraft's collision is kept only outside him.
			// His standing body, whatever his pose, with a sub-cell to spare: the 20:03 session had a kitchen stand's
			// part opening 2.6 cm into him (he had moved on since the feet HN knew), Minecraft forced him to crawl, and
			// the clearance then shrank to the crawling hitbox (0.6) and let the part come down to his waist.
			if (g_haveFeet)
			{
				double fx, fy, fz;
				to_mc(g_feet, fx, fy, fz);
				const double hw = g_steveWidth / 200.0 + 0.125, h = std::max(g_steveHeight / 100.0, 1.8) + 0.05;
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
			// blocks whose moving collision changed: sent again, and their static part probed again (a probe taken
			// while the part was there may hold pieces of it; the moving collision has it now)
			auto restale = [](unsigned long long key) {
				BlockBits &b = g_bits[key];
				b.boxKnown = false;
				g_columns.erase(cell_key(cell_x(key), 0, cell_z(key)));
				g_dirty.insert(key);
			};
			for (auto &[key, bits] : now)
			{
				const auto old = g_dynBits.find(key);
				if (old == g_dynBits.end() || old->second != bits)
					restale(key);
			}
			for (auto &[key, bits] : g_dynBits)
				if (!now.count(key))
					restale(key);
			// restale also when only the mask changed (a part moving within the cells it already had)
			for (auto &[key, bits] : mask)
				if (const auto old = g_dynMask.find(key); old == g_dynMask.end() || old->second != bits)
					restale(key);
			g_dynBits = std::move(now);
			g_dynMask = std::move(mask);
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

	// HN's frame times while Minecraft drives, and the world probe's share (every 5 s in the log): the evidence for
	// stutter (a hill's re-probing used the probe's whole budget frame after frame)
	struct FrameStats
	{
		double frameSum = 0, frameMax = 0, probeSum = 0, probeMax = 0;
		int frames = 0, hitches = 0;
		std::chrono::steady_clock::time_point last{}, next{};
	} g_frameStats;

	void frame_stats(double probeMs)
	{
		FrameStats &f = g_frameStats;
		const auto now = std::chrono::steady_clock::now();
		if (f.last.time_since_epoch().count() != 0 && g_driving)
		{
			const double ms = std::chrono::duration<double, std::milli>(now - f.last).count();
			f.frameSum += ms;
			f.frameMax = std::max(f.frameMax, ms);
			f.hitches += ms > 50.0;
			f.probeSum += probeMs;
			f.probeMax = std::max(f.probeMax, probeMs);
			++f.frames;
		}
		f.last = now;
		if (now > f.next)
		{
			if (f.frames > 0)
			{
				wchar_t text[256];
				swprintf(text, 256, L"frames: %d in 5 s, average %.1f ms, worst %.1f ms, %d over 50 ms; world probe average %.2f ms, worst %.1f ms",
					f.frames, f.frameSum / f.frames, f.frameMax, f.hitches, f.probeSum / f.frames, f.probeMax);
				log(text);
				// Minecraft renders at HN's rate (PassthroughClient): at 120 against HN's 60 it drew two frames per HN frame,
				// and the picture HN showed was 2-3 of its frames behind the newest stand-in poses (Steve's trail on turns)
				sendf("{\"t\":\"hostfps\",\"v\":%.1f}", 1000.0 / (f.frameSum / f.frames));
			}
			const auto next = now + std::chrono::seconds(5);
			f = FrameStats{};
			f.last = now;
			f.next = next;
		}
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
		// alive, once a second from the game thread, in menus too: a crashed (or frozen) HN keeps the connection open
		// while Unreal's crash reporter holds its process, so only silence tells Minecraft it is gone (the user,
		// 2026-10-13: HN crashed, Minecraft played on and Steve fell into the void)
		static DWORD nextBeat = 0;
		if (g_ws.connected() && GetTickCount() > nextBeat)
		{
			nextBeat = GetTickCount() + 1000;
			sendf("{\"t\":\"beat\"}");
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
		if (pressed(VK_F6))
		{
			g_proxiesShown = !g_proxiesShown;
			int n = 0;
			for (auto &[key, actor] : g_shadows)
				proxy_main_pass(actor, g_proxiesShown || receiver_material(actor) != nullptr), ++n;
			for (UObject *part : g_steveShadow.parts)
				if (part)
					proxy_main_pass(part, g_proxiesShown), ++n;
			for (UObject *box : g_pool.boxes)
				proxy_main_pass(box, g_proxiesShown), ++n;
			log(L"shadows: F6: " + std::to_wstring(n) + L" stand-ins " + (g_proxiesShown ? L"drawn in the picture too (where they are)" : L"shadow-only again"));
			notify(g_proxiesShown ? "Shadow stand-ins visible (F6)" : "Shadow stand-ins hidden");
		}

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
		// under HN's pause menu Minecraft's last picture stays (the camera doesn't move): without it the stand-ins showed
		// bare behind the menu, plain coloured boxes (the user took them for Steve and the blocks with wrong textures);
		// Minecraft's own pause screen and hand are left out meanwhile. Not in cutscenes: their camera moves.
		static bool shownBefore = false;
		const bool shown = live || (shownBefore && g_inPauseMenu && g_enabled && g_ws.connected());
		shownBefore = shown;
		compositor::set_active(shown);
		compositor::set_paused(shown && !live);
		set_player_hidden(pawn, shown);
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
			apply_blocks(pawn, g_pendingSet, g_pendingClear, g_pendingOpaque, g_pendingColours);
			g_pendingSet.clear();
			g_pendingClear.clear();
			g_pendingOpaque.clear();
			g_pendingColours.clear();
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
		select_shadow_frame();
		doors_tick(pc, pawn);
		if (g_driving)
		{
			physics_tick(pawn);
			for (const Punch &punch : g_punches)
				physics_punch(pawn, punch);
			g_controlsPc = pc;
			carry_tick(pc, pawn);
			mesh_path_probe(pawn);
			light_census(pawn);
			steve_shadow_tick(pc, pawn);
			shadow_boxes_tick(pc, pawn);
			pending_receivers_tick();
			capture_test_tick(pc, pawn);
			lend_shadows_tick(pc);
			light_colour_tick(pc);
			for (const std::string &id : g_throwRequests)
				carry_throw(pc, pawn, id, g_heldId == id ? g_heldCls : std::string{});
		}
		else
			g_nearPhysics.clear();
		g_punches.clear();
		g_throwRequests.clear();
		if (g_driving && g_mcReady)
			dynamic_tick(pc);
		// The loose objects near Steve are moving collision (see the moving parts), so the world probe looks past them:
		// past their actor, as traces ignore whole actors. Only when the object is its actor (its root component): a
		// loose part of a bigger actor (a street's bins, a yard's props) used to make the probe ignore that whole actor,
		// and on Act 1's road the floor under Steve stopped being found (the recorder: the road's blocks emptying
		// around him, then a fall).
		g_probeIgnoreNow = g_probeIgnore;
		// the shadow stand-ins and receivers are ours, not HN's world (whatever their collision says)
		for (auto &[key, actor] : g_shadows)
			g_probeIgnoreNow.push_back(actor);
		for (size_t i = 0; i < g_pool.used && i < g_pool.boxes.size(); ++i)
			g_probeIgnoreNow.push_back(g_pool.boxes[i]);
		for (UObject *part : g_steveShadow.parts)
			if (part)
				g_probeIgnoreNow.push_back(part);
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
		const auto probeStart = std::chrono::steady_clock::now();
		const bool ready = g_mcReady && sample_world(pawn, g_driving && g_haveEye ? g_eye : cam.loc);
		frame_stats(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probeStart).count());
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
