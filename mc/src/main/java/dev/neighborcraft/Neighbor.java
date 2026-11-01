package dev.neighborcraft;

import java.util.Locale;
import net.fabricmc.fabric.api.object.builder.v1.entity.FabricDefaultAttributeRegistry;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.MobCategory;
import net.minecraft.world.entity.Pose;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.level.Level;

/**
 * Hello Neighbor's Neighbor as a Minecraft entity, so that Minecraft's combat works on him: Steve's reach, swings, damage
 * and criticals. HN draws and moves him: this mob has no AI, no gravity and isn't drawn (invisible, a no-op renderer). It
 * stands where HN says each server tick, at HN's capsule size. A hit goes to HN ("nhit": damage, whether a critical), which
 * plays the Neighbor's own reaction; his health here never drops (HN decides what a hit does).
 */
public class Neighbor extends Mob {
	public static EntityType<Neighbor> TYPE;

	/** HN's Neighbor (link thread writes, server thread reads): feet, yaw, capsule width and height (blocks). */
	private static volatile double[] target;
	/** The one Neighbor entity (server thread). */
	private static Neighbor entity;
	/** A hit this tick (server thread): its damage; and whether Minecraft made it a critical (Player.crit, after it). */
	private static float hitDamage = -1.0F;
	private static boolean hitCrit;
	private static boolean hitSprint;
	/** Its knockback: direction (x, z, unit) and Minecraft's strength; whether an explosion; whether Steve's. */
	private static double hitKx, hitKz, hitK;
	private static boolean hitBoom, hitPlayer, hitBurn;
	/** The game time he last took fire damage as a hit (fire hurts every tick or so: once a second is enough). */
	private static long lastBurn = -100;

	public Neighbor(final EntityType<? extends Neighbor> type, final Level level) {
		super(type, level);
		this.setNoAi(true);
		this.setNoGravity(true);
		// not invisible: mobs barely see an invisible target (0.07 of their range) and ignored him (the user, 2026-11-01);
		// he still isn't drawn (NoopRenderer) nor exported as a shadow box (ShadowExport)
		this.setPersistenceRequired();
		this.setSilent(true);
	}

	public static void register() {
		ResourceKey<EntityType<?>> key = ResourceKey.create(Registries.ENTITY_TYPE, Identifier.fromNamespaceAndPath(Passthrough.ID, "neighbor"));
		TYPE = Registry.register(BuiltInRegistries.ENTITY_TYPE, key,
			EntityType.Builder.<Neighbor>of(Neighbor::new, MobCategory.MISC).sized(0.9F, 2.2F).noSummon().build(key));
		FabricDefaultAttributeRegistry.register(TYPE, Mob.createMobAttributes().add(Attributes.MAX_HEALTH, 1000.0));
	}

	/** From HN ("nbr"): where its Neighbor stands; null: there is none (another level, a cutscene). */
	public static void target(final double[] t) {
		target = t;
	}

	/** Client, each frame: the entity exactly at HN's Neighbor (old positions too: no easing between frames). */
	public static void snap(final Entity e) {
		double[] t = target;
		if (t == null || e.isPassenger()) {
			return; // riding: where Minecraft seats him
		}

		e.setPos(t[0], t[1], t[2]);
		e.xo = e.xOld = t[0];
		e.yo = e.yOld = t[1];
		e.zo = e.zOld = t[2];
		e.setYRot((float) t[3]);
		e.yRotO = (float) t[3];
	}

	@Override
	public EntityDimensions getDefaultDimensions(final Pose pose) {
		double[] t = target;
		return t != null ? EntityDimensions.scalable((float) t[4], (float) t[5]) : super.getDefaultDimensions(pose);
	}

	@Override
	public boolean hurtServer(final ServerLevel level, final DamageSource source, final float amount) {
		boolean boom = source.is(net.minecraft.tags.DamageTypeTags.IS_EXPLOSION);
		boolean player = source.getEntity() instanceof Player;
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE) && source.getEntity() == null && source.getDirectEntity() == null) {
			// burning (fire, lava, a flame's ticks): a hit with no push, at most once a second (the user: "fire burns him")
			if (level.getGameTime() - lastBurn >= 20) {
				lastBurn = level.getGameTime();
				hitDamage = Math.max(hitDamage, amount);
				hitPlayer = false;
				hitBurn = true;
			}

			return true;
		}

		if (!player && !boom && !(source.getEntity() instanceof Mob) && source.getDirectEntity() == null) {
			return false; // nothing else hurts him in Minecraft (falls, suffocation in HN's world blocks)
		}

		// a hit (Minecraft's swing, sound and particles), no health lost here; HN plays it and pushes him (knockback as
		// Minecraft's: 0.4, more for a sprinting player or a mob's knockback; an explosion by its damage). The user:
		// "explosions should knock him down, mobs give him knockback" (2026-11-01)
		double strength = 0.4;
		if (source.getEntity() instanceof Player p) {
			hitSprint = p.isSprinting();
			strength += hitSprint ? 0.5 : 0.0;
		} else if (source.getEntity() instanceof Mob m && source.getDirectEntity() == m) {
			strength += m.getAttributeValue(Attributes.ATTACK_KNOCKBACK);
		}

		if (boom) {
			strength = Math.min(1.6, 0.3 + amount / 8.0);
		}

		net.minecraft.world.phys.Vec3 from = source.getSourcePosition();
		double dx = from != null ? this.getX() - from.x : 0.0, dz = from != null ? this.getZ() - from.z : 0.0, d = Math.hypot(dx, dz);
		if (amount >= hitDamage || boom) {
			hitKx = d > 1.0E-4 ? dx / d : 0.0;
			hitKz = d > 1.0E-4 ? dz / d : 0.0;
			hitK = d > 1.0E-4 ? strength : 0.0;
			hitBoom |= boom;
			hitPlayer = player;
		}

		hitDamage = Math.max(hitDamage, amount);
		return true;
	}

	@Override
	public void knockback(final double strength, final double x, final double z, final DamageSource source, final float f, final boolean b) {
		// HN moves him
	}

	/** Pushable, so a boat takes him aboard (it looks only among pushable entities)... */
	@Override
	public boolean isPushable() {
		return true;
	}

	/** ...but other entities don't shove him (that would reach HN as a push): HN's own bodies meet his. */
	@Override
	public void push(final Entity other) {
	}

	@Override
	public boolean removeWhenFarAway(final double distance) {
		return false;
	}

	/** Player.canCriticalAttack said yes for a charged swing: the hit that follows is a critical. */
	public static void critical(final Entity target) {
		if (target instanceof Neighbor) {
			hitCrit = true;
		}
	}

	/** Server thread, end of each tick: the entity follows HN's Neighbor; a hit this tick goes to HN. */
	public static void tick(final MinecraftServer server) {
		if (hitDamage >= 0.0F) {
			Passthrough.events.accept(String.format(Locale.ROOT,
				"{\"t\":\"nhit\",\"dmg\":%.2f,\"crit\":%b,\"sprint\":%b,\"kx\":%.3f,\"kz\":%.3f,\"k\":%.3f,\"boom\":%b,\"player\":%b,\"burn\":%b}",
				hitDamage, hitCrit, hitSprint, hitKx, hitKz, hitK, hitBoom, hitPlayer, hitBurn));
			Passthrough.LOG.info("neighbor: hit for {} ({}{}{}), knockback {}", hitDamage, hitCrit ? "critical" : "normal", hitSprint ? ", sprinting" : "",
				hitBoom ? ", explosion" : hitPlayer ? "" : ", not the player", String.format(Locale.ROOT, "%.2f", hitK));
			hitDamage = -1.0F;
			hitCrit = false;
			hitSprint = false;
			hitBoom = false;
			hitBurn = false;
			hitK = 0.0;
		}

		double[] t = target;
		ServerLevel level = server.overworld();
		if (t == null || !Passthrough.driving) {
			if (entity != null) {
				entity.discard();
				entity = null;
			}

			return;
		}

		if (entity == null || entity.isRemoved()) {
			entity = TYPE.create(level, net.minecraft.world.entity.EntitySpawnReason.COMMAND);
			if (entity == null) {
				return;
			}

			entity.snapTo(t[0], t[1], t[2], (float) t[3], 0.0F);
			level.addFreshEntity(entity);
			Passthrough.LOG.info("neighbor: in Minecraft at {} {} {}", t[0], t[1], t[2]);
		}

		if (Math.abs(entity.getBbWidth() - t[4]) > 0.01 || Math.abs(entity.getBbHeight() - t[5]) > 0.01) {
			entity.refreshDimensions();
		}

		// hostile mobs go for him (theirs only hunt players): each second, the free ones within 24 blocks
		if (server.getTickCount() % 20 == 0) {
			for (net.minecraft.world.entity.monster.Monster m : level.getEntitiesOfClass(net.minecraft.world.entity.monster.Monster.class,
				entity.getBoundingBox().inflate(24.0))) {
				if (m.getTarget() == null || !m.getTarget().isAlive()) {
					m.setTarget(entity);
				}
			}
		}

		entity.setHealth(entity.getMaxHealth());
		// what Minecraft did to move him (a fishing rod's pull, a wind charge, an explosion's push) goes to HN as a launch:
		// here HN places him (the user: "the fishing rod should pull him")
		net.minecraft.world.phys.Vec3 v = entity.getDeltaMovement();
		if (v.lengthSqr() > 0.0025) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"npush\",\"v\":[%.4f,%.4f,%.4f]}", v.x, v.y, v.z));
			Passthrough.LOG.info("neighbor: pushed {} {} {} blocks a tick", String.format(Locale.ROOT, "%.2f", v.x), String.format(Locale.ROOT, "%.2f", v.y),
				String.format(Locale.ROOT, "%.2f", v.z));
		}

		entity.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
		// in a boat (or a minecart): Minecraft places him on its seat and HN holds him there (stuck, as a mob in a boat)
		if (entity.isPassenger()) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"nride\",\"p\":[%.4f,%.4f,%.4f],\"yaw\":%.2f}", entity.getX(), entity.getY(),
				entity.getZ(), entity.getYRot()));
			if (!riding) {
				riding = true;
				Passthrough.LOG.info("neighbor: riding {}", entity.getVehicle().getType().toShortString());
			}

			return;
		}

		if (riding) {
			riding = false;
			Passthrough.events.accept("{\"t\":\"nride\"}");
			Passthrough.LOG.info("neighbor: out again");
		}

		entity.setPos(t[0], t[1], t[2]);
		entity.setYRot((float) t[3]);
		entity.setYHeadRot((float) t[3]);
		entity.setYBodyRot((float) t[3]);
	}

	/** He is riding something (server thread): HN was told so. */
	private static boolean riding;
}
