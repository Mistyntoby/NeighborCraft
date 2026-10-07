package dev.neighborcraft;

import java.util.function.Consumer;
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

	@Override
	public void onInitialize() {
		ServerLifecycleEvents.SERVER_STARTED.register(WorldBridge::attach);
		ServerLifecycleEvents.SERVER_STOPPING.register(server -> WorldBridge.detach());
		ServerTickEvents.END_SERVER_TICK.register(WorldBridge::tick);
		LOG.info("neighborcraft loaded");
	}
}
