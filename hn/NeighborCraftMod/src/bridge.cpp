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
// Keys: Minecraft's own (see minecraft_controls), F7 passthrough on/off, F8 re-level, F9 reflection dump,
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

		// Minecraft's controls and camera
		std::atomic<bool> g_minecraftInput{false};  // swallow HN's copies of Minecraft's keys
		std::atomic<bool> g_minecraftScreen{false}; // a Minecraft screen is open: the mouse is Minecraft's too
		std::atomic<int> g_wheel{0};                // wheel notches since the last tick
		std::atomic<int> g_injectedE{0};            // E messages we posted ourselves (let through)
		std::atomic<bool> g_minecraftDriving{false}; // HN's movement keys are Minecraft's too (swallowed)
		std::atomic<long> g_mouseDx{0}, g_mouseDy{0};  // raw mouse counts since the last tick
		std::atomic<bool> g_rawMouseSeen{false};
		double g_lookYaw = 0, g_lookPitch = 0;         // the look while Minecraft drives (Minecraft degrees, yaw unwrapped)
		bool g_lookOwned = false;
		bool g_haveMcCamera = false;
		Vec3f g_mcCamera;
		float g_mcYaw = 0, g_mcPitch = 0, g_mcRoll = 0, g_mcFov = 70; // Minecraft's camera (Minecraft degrees, vertical FOV)

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

		ScriptArray ignore_list()
		{
			return {g_probeIgnore.data(), int32(g_probeIgnore.size()), int32(g_probeIgnore.size())};
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

		/// A line trace against what blocks a player in HN (the "Pawn" profile); the impact point on a hit.
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
				.set_bool(STR("bTraceComplex"), false)
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
			float floor[16] = {};     // from the column rays: surface height in the block (0..1], per 1/4 x 1/4 column
			bool boxKnown = false;
		};
		std::unordered_map<unsigned long long, BlockBits> g_bits;
		std::unordered_map<unsigned long long, DWORD> g_columns; // column (x, z) -> when its rays were cast
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

		/// Cast the rays of column (x, z) between Minecraft heights yLo and yHi; rewrites the floor layers of its blocks.
		int probe_column(UObject *pawn, int x, int z, int yLo, int yHi)
		{
			for (int y = yLo; y < yHi; ++y)
			{
				const unsigned long long key = cell_key(x, y, z);
				BlockBits &b = g_bits[key];
				std::fill(std::begin(b.floor), std::end(b.floor), 0.0f);
				g_dirty.insert(key);
			}
			int queries = 0;
			for (int i = 0; i < 4; ++i)
				for (int k = 0; k < 4; ++k)
				{
					const double mx = x + (i + 0.5) / 4.0, mz = z + (k + 0.5) / 4.0;
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
						// the surface's exact height, in the block it lies in (a surface on a block boundary belongs to the
						// block below: its top face)
						const int by = int(std::floor(hy - 1e-4));
						if (by >= yLo && by < yHi)
						{
							float &top = g_bits[cell_key(x, by, z)].floor[i * 4 + k];
							top = std::max(top, float(hy - by));
						}
						from = {hit.x, hit.y, hit.z - 3.0f}; // on below this surface: the storey under it
						if (from.z <= to.z)
							break;
					}
				}
			return queries;
		}

		/// The block's shape: what the box probe and the floor rays found. 0 empty, 1 full, 2 partial (mask in `hex`,
		/// floor heights in `floor`: 16 thousandths of a block, 0 for none).
		int block_shape(const BlockBits &b, std::string &hex, std::string &floor)
		{
			const uint8_t *bits = b.box;
			int set = 0;
			for (int i = 0; i < 64; ++i)
				set += std::popcount(bits[i]);
			if (set == 512)
				return 1;
			floor.clear();
			bool anyFloor = false;
			for (int i = 0; i < 16; ++i)
			{
				const int h = int(std::lround(b.floor[i] * 1000.0f));
				anyFloor |= h > 0;
				floor += (i ? "," : "") + std::to_string(h);
			}
			if (set == 0 && !anyFloor)
				return 0;
			if (!anyFloor)
				floor.clear();
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
				const bool close = std::abs(dx) <= kVoxRefreshRadius && std::abs(dz) <= kVoxRefreshRadius;
				if (it != g_columns.end() && !(close && now - it->second > kVoxRefreshMs * 3))
					continue;
				if (it == g_columns.end() && std::abs(dx) <= kDriveReadyRadius && std::abs(dz) <= kDriveReadyRadius)
					ready = false;
				queries += probe_column(pawn, px + dx, pz + dz, py - kVoxBelow, py + kVoxAbove + 1);
				g_columns[col] = now;
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
				const bool inPlayer = x + 1 > ex - 0.35 && x < ex + 0.35 && z + 1 > ez - 0.35 && z < ez + 0.35 && y + 1 > ey - 1.5 &&
									  y < ey + 0.25;
				BlockBits &b = g_bits[key];
				std::fill(std::begin(b.box), std::end(b.box), uint8_t(0));
				if (!inPlayer)
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
			std::string cells, masks, floors, floor;
			for (unsigned long long key : g_dirty)
			{
				const auto bit = g_bits.find(key);
				if (bit == g_bits.end())
					continue;
				const int kind = block_shape(bit->second, hex, floor);
				auto [cell, added] = g_cells.try_emplace(key, Cell{~0ull, 0});
				const unsigned long long sig = kind == 2 ? ((std::hash<std::string>()(hex) ^ std::hash<std::string>()(floor) * 31) | 3) : unsigned(kind);
				if (!added && cell->second.signature == sig)
					continue;
				cell->second.signature = sig;
				++g_probeStats[kind];
				char entry[64];
				snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", cells.empty() ? "" : ",", cell_x(key), cell_y(key), cell_z(key), kind);
				cells += entry;
				if (kind == 2)
				{
					masks += (masks.empty() ? "\"" : ",\"") + hex + "\"";
					floors += (floors.empty() && masks.size() == hex.size() + 2 ? "\"" : ",\"") + floor + "\"";
				}
			}
			g_dirty.clear();
			if (!cells.empty())
				g_ws.send("{\"t\":\"shape\",\"c\":[" + cells + "],\"m\":[" + masks + "],\"f\":[" + floors + "]}");
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
			g_yOffset = std::round(metres) - metres;
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
				if (json_vec3(m, "cam", camera) && json_numbers(m, "rot", rot, 3) && json_number(m, "fov", fov))
				{
					g_mcCamera = to_ue(camera[0], camera[1], camera[2]);
					g_mcYaw = float(rot[0]);
					g_mcPitch = float(rot[1]);
					g_mcRoll = float(rot[2]);
					g_mcFov = float(fov);
					g_haveMcCamera = true;
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
			return true;
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

		/// What the player looks at in HN (from Minecraft's camera, within reach), if it is one of HN's interactive
		/// objects (doors, switches, things to take: they carry HN's InteractiveComponent).
		bool looking_at_interactive(UObject *pawn)
		{
			if (!g_haveMcCamera)
				return false;
			const float d2r = 3.14159265f / 180.0f;
			// Minecraft's look (yaw 0 = +Z, pitch + = down) as an Unreal direction (Minecraft x -> x, z -> y, y -> z)
			const float yaw = g_mcYaw * d2r, pitch = g_mcPitch * d2r;
			const Vec3f dir{-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch)};
			const float reach = 300.0f;
			Call c(kismet(), STR("LineTraceSingle"));
			if (!c)
				return false;
			c.set<UObject *>(STR("WorldContextObject"), pawn)
				.set(STR("Start"), g_mcCamera)
				.set(STR("End"), Vec3f{g_mcCamera.x + dir.x * reach, g_mcCamera.y + dir.y * reach, g_mcCamera.z + dir.z * reach})
				.set<uint8_t>(STR("TraceChannel"), 0)
				.set(STR("ActorsToIgnore"), ignore_list())
				.set_bool(STR("bTraceComplex"), false)
				.set<uint8_t>(STR("DrawDebugType"), 0)
				.set_bool(STR("bIgnoreSelf"), true);
			c.invoke();
			const uint8_t *ok = c.param<uint8_t>(STR("ReturnValue"));
			FProperty *outHit = c.property(STR("OutHit"));
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
			if (!actor || !*actor)
				return false;
			static UObject *interactive = UObjectGlobals::StaticFindObject<UObject *>(nullptr, nullptr, STR("/Script/HelloNeighbor.InteractiveComponent"));
			bool yes = false;
			if (interactive)
			{
				Call has(*actor, STR("GetComponentByClass"));
				has.set<UObject *>(STR("ComponentClass"), interactive).invoke();
				const UObject *const *component = has.param<UObject *>(STR("ReturnValue"));
				yes = component && *component;
			}
			if (!yes)
				for (UStruct *s = (*actor)->GetClassPrivate(); s && !yes; s = s->GetSuperStruct())
					yes = s->GetName().find(L"Door") != StringType::npos;
			static int logged = 0;
			if (logged < 20)
			{
				++logged;
				log(L"right click on " + (*actor)->GetClassPrivate()->GetName() + (yes ? L": HN interact" : L": Minecraft use") +
					(interactive ? L"" : L" (no InteractiveComponent class)"));
			}
			return yes;
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
				POINT cursor;
				GetCursorPos(&cursor);
				ScreenToClient(g_hnWindow, &cursor);
				sendf("{\"t\":\"mouse\",\"x\":%d,\"y\":%d}", cursor.x, cursor.y);
				const int buttons[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
				for (int b = 0; b < 3; ++b)
					if (const int e = edge(buttons[b]))
						sendf("{\"t\":\"click\",\"b\":%d,\"down\":%s,\"x\":%d,\"y\":%d}", b, e > 0 ? "true" : "false", cursor.x, cursor.y);
				if (wheel)
					sendf("{\"t\":\"wheel\",\"d\":%d,\"x\":%d,\"y\":%d}", wheel, cursor.x, cursor.y);
				if (pressed(VK_ESCAPE))
					g_ws.send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}");
				return;
			}
			forward_key(VK_LBUTTON, "attack");
			right_click();
			forward_key(VK_MBUTTON, "pick");
			for (int i = 0; i < 9; ++i)
				if (pressed('1' + i))
					sendf("{\"t\":\"slot\",\"n\":%d}", i);
			if (wheel)
				sendf("{\"t\":\"scroll\",\"d\":%d}", wheel);
		}

		bool g_rightClickHN = false; // the held right click went to HN (its E)
		UObject *g_controlsPawn = nullptr;

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

		/// While a Minecraft screen is open HN shows its cursor and stops turning the camera.
		void minecraft_screen_mode(UObject *pc, bool open)
		{
			static bool applied = false;
			if (applied == open)
				return;
			applied = open;
			set_bool_property(pc, STR("bShowMouseCursor"), open);
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
				struct
				{
					Vec3f loc;
					Rot3f rot;
				} pose{g_mcCamera, {-g_mcPitch, g_mcYaw + 90.0f, -g_mcRoll}};
				Call place(camera, STR("K2_SetActorLocationAndRotation"));
				place.set(STR("NewLocation"), pose.loc).set(STR("NewRotation"), pose.rot).set_bool(STR("bSweep"), false).set_bool(STR("bTeleport"), true).invoke();
				const float hfov = float(2.0 * std::atan(std::tan(g_mcFov * 3.14159265358979 / 360.0) * aspect) * 180.0 / 3.14159265358979);
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
					Call(*pcm, STR("StopAllCameraShakes")).set_bool(STR("bImmediately"), true).invoke();
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
		// Unreal's roll is clockwise-positive, Minecraft's camera roll counter-clockwise-positive
		compositor::set_host_pose(cam.rot.yaw - 90.0f, -cam.rot.pitch, -cam.rot.roll, vfov, cx, cy, cz);
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
						log(L"Steve is under HN's world: back to the last ground");
					underWorld = true;
					teleport_minecraft(pawn, haveGood ? lastGood : p);
					nextGroundCheck = GetTickCount() + 1000; // give the teleport time to land
				}
			}
			// HN's player stands where Steve does (its eyes at his), and HN sees through Minecraft's camera
			if (g_haveEye && !underWorld)
			{
				const float *eyeHeight = pawn->GetValuePtrByPropertyNameInChain<float>(STR("BaseEyeHeight"));
				const Vec3f target{g_eye.x, g_eye.y, g_eye.z - (eyeHeight ? *eyeHeight : 64.0f)};
				set_actor_location(pawn, target);
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
		minecraft_controls(pc);
		hide_hn_cursor(pawn, true);
		movement_report(pawn);
		if (pressed(VK_F11) && g_haveEye)
			collision_map(g_eye);
	}
}
