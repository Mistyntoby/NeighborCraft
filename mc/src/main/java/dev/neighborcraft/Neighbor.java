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

	public Neighbor(final EntityType<? extends Neighbor> type, final Level level) {
		super(type, level);
		this.setNoAi(true);
		this.setNoGravity(true);
		this.setInvisible(true);
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

	@Override
	public EntityDimensions getDefaultDimensions(final Pose pose) {
		double[] t = target;
		return t != null ? EntityDimensions.scalable((float) t[4], (float) t[5]) : super.getDefaultDimensions(pose);
	}

	@Override
	public boolean hurtServer(final ServerLevel level, final DamageSource source, final float amount) {
		if (source.getEntity() instanceof Player player) {
			hitDamage = Math.max(hitDamage, amount);
			hitSprint = player.isSprinting();
			return true; // hit (Minecraft's swing, sound and particles), but no health lost and no knockback here
		}

		return false; // nothing else hurts him in Minecraft (fall, fire, suffocation in HN's world blocks)
	}

	@Override
	public void knockback(final double strength, final double x, final double z, final DamageSource source, final float f, final boolean b) {
		// HN moves him
	}

	@Override
	public boolean isPushable() {
		return false;
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
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"nhit\",\"dmg\":%.2f,\"crit\":%b,\"sprint\":%b}", hitDamage, hitCrit, hitSprint));
			Passthrough.LOG.info("neighbor: hit for {} ({}{})", hitDamage, hitCrit ? "critical" : "normal", hitSprint ? ", sprinting" : "");
			hitDamage = -1.0F;
			hitCrit = false;
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

		entity.setPos(t[0], t[1], t[2]);
		entity.setYRot((float) t[3]);
		entity.setYHeadRot((float) t[3]);
		entity.setYBodyRot((float) t[3]);
		entity.setHealth(entity.getMaxHealth());
	}
}
