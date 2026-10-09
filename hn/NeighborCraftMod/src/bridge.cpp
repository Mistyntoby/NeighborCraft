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
		// EObjectTypeQuery: ObjectTypeQuery1 = WorldStatic, ObjectTypeQuery2 = WorldDynamic (not pawns, not physics props)
		const uint8_t kSolidTypes[] = {0, 1};
		/// Actors the world probes don't see: our own colliders for Minecraft's blocks (they aren't HN's world).
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
				.set(STR("ActorsToIgnore"), ignore_list())
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
				.set(STR("ActorsToIgnore"), ignore_list())
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

		void rebuild_probe_ignore()
		{
			g_probeIgnore.clear();
			for (auto &[key, actor] : g_colliders)
				g_probeIgnore.push_back(actor);
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
				// cells the player's own capsule overlaps are never solid: what HN probes there is the player's arms and
				// held items (Minecraft then outlined invisible blocks around the face)
				const Vec3f lo = to_ue(x, y, z), hi = to_ue(x + 1, y + 1, z + 1);
				const bool inPlayer = lo.x < feet.x + 40.0f && hi.x > feet.x - 40.0f && lo.y < feet.y + 40.0f && hi.y > feet.y - 40.0f &&
									  lo.z < feet.z + 95.0f && hi.z > feet.z - 95.0f;
				queries += inPlayer ? 0 : 5;
				const bool s = !inPlayer && probe_cell(pawn, x, y, z);
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
			if (m.find("\"t\":\"blocks\"") != std::string::npos)
			{
				json_ints(m, "set", g_pendingSet);
				json_ints(m, "clear", g_pendingClear);
			}
			else if (m.find("\"t\":\"mcpos\"") != std::string::npos)
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

		/// Actual gameplay: not HN's main menu (the Start map) and not paused. Minecraft shows only then.
		bool in_gameplay(UObject *pc)
		{
			static UObject *lastPc = nullptr;
			static bool menu = false;
			if (pc != lastPc)
			{
				lastPc = pc;
				menu = pc->GetFullName().find(L"/Maps/Start.") != StringType::npos;
			}
			if (menu)
				return false;
			Call paused(pc, STR("IsPaused"));
			if (paused.invoke())
				if (const uint8_t *p = paused.param<uint8_t>(STR("ReturnValue")); p && *p)
					return false;
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
		const bool live = g_enabled && g_ws.connected() && pawn && in_gameplay(pc) && read_camera(pc, cam);
		compositor::set_active(live);
		set_player_hidden(pawn, live);
		if (!live)
		{
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
