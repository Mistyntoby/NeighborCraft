package dev.neighborcraft;

import java.util.function.Consumer;
import net.fabricmc.fabric.api.event.player.PlayerBlockBreakEvents;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.item.Item;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.fabricmc.api.ModInitializer;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerLifecycleEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class Passthrough implements ModInitializer {
	public static final String ID = "neighborcraft";
	public static final Logger LOG = LoggerFactory.getLogger(ID);
	/** True while Hello Neighbor is driving the camera. The integrated server shares this JVM, so both sides read it. */
	public static volatile boolean active;
	/** True while Minecraft moves the player (its physics, against the host's world as barriers) and the host follows. */
	public static volatile boolean driving;
	/** Where events for the host go (JSON lines); the client's HostLink sets it. */
	public static volatile Consumer<String> events = message -> {};
	/** Hello Neighbor's world: invisible blocks shaped like HN's geometry (see HostBlock). */
	public static Block HOST;
	/** A Hello Neighbor object carried in Minecraft's inventory (see Carry). */
	public static Item HN_OBJECT;
	/** The block standing for HN's surface material under the player (grass, wood, tiles ...), from the host. */
	public static volatile BlockState surface;

	/** For a player's movement effects (particles, footsteps): HN's surface instead of the invisible host block. */
	public static BlockState surface(final Entity entity, final BlockState state) {
		BlockState s = surface;
		return s != null && entity instanceof Player && state.is(HOST) ? s : state;
	}

	@Override
	public void onInitialize() {
		ResourceKey<Block> key = ResourceKey.create(Registries.BLOCK, Identifier.fromNamespaceAndPath(ID, "host"));
		HOST = Registry.register(BuiltInRegistries.BLOCK, key, new HostBlock(BlockBehaviour.Properties.of()
			.setId(key).strength(-1.0F, 3600000.0F).noLootTable().noOcclusion().dynamicShape()
			.isSuffocating((state, level, pos) -> false)));
		ResourceKey<Item> itemKey = ResourceKey.create(Registries.ITEM, Identifier.fromNamespaceAndPath(ID, "hn_object"));
		HN_OBJECT = Registry.register(BuiltInRegistries.ITEM, itemKey, new Item(new Item.Properties().setId(itemKey).stacksTo(1)));
		Neighbor.register();
		ServerTickEvents.END_SERVER_TICK.register(Carry::tick);
		ServerTickEvents.END_SERVER_TICK.register(Neighbor::tick);
		// HN's world can't be broken from Minecraft (creative would otherwise break it instantly)
		PlayerBlockBreakEvents.BEFORE.register((level, player, pos, state, entity) -> !state.is(HOST));
		ServerLifecycleEvents.SERVER_STARTED.register(WorldBridge::attach);
		ServerLifecycleEvents.SERVER_STOPPING.register(server -> WorldBridge.detach());
		ServerTickEvents.END_SERVER_TICK.register(WorldBridge::tick);
		LOG.info("neighborcraft loaded");
	}
}
