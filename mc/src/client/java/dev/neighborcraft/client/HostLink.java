package dev.neighborcraft.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.WorldBridge;
import java.net.InetSocketAddress;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import org.java_websocket.WebSocket;
import org.java_websocket.handshake.ClientHandshake;
import org.java_websocket.server.WebSocketServer;

/**
 * Hello Neighbor's connection: a WebSocket server on 127.0.0.1 (port 25620, or -Dneighborcraft.port).
 *
 * <p>Host to Minecraft (JSON, already in Minecraft coordinates; the UE4SS mod converts):
 * <ul>
 * <li>{"t":"cam","f":frame,"p":[x,y,z],"r":[yaw,pitch,roll],"fov":vertical degrees,"fp":first person,"pl":[feet x,y,z],"h":body yaw}</li>
 * <li>{"t":"ground","c":[x,z,yBottom,yTop, ...]}: solid columns (barriers)</li>
 * <li>{"t":"clear"}: remove the barriers placed so far</li>
 * <li>{"t":"blocksync","r":radius}: report every solid block near the player (the host rebuilds its colliders)</li>
 * <li>{"t":"cmd","c":"time set noon"}: a server command</li>
 * <li>{"t":"key","k":"use|attack|pick|inventory|drop|swap|escape","down":bool}</li>
 * <li>{"t":"slot","n":0-8}, {"t":"scroll","d":+-1}, {"t":"hud","hidden":bool}, {"t":"view","w":px,"h":px}</li>
 * </ul>
 * Minecraft to host: {"t":"hello",...} on connect, {"t":"blocks","set":[...],"clear":[...]}, {"t":"explosion",...},
 * {"t":"pteleport","pos":[x,y,z]}. Messages of type "dev" are relayed unchanged to the other clients (test tools).
 */
public final class HostLink extends WebSocketServer {
	public static final int DEFAULT_PORT = 25620;
	private static HostLink instance;

	private HostLink(final int port) {
		super(new InetSocketAddress("127.0.0.1", port));
		this.setReuseAddr(true);
		this.setDaemon(true);
	}

	static void launch() {
		int port = Integer.getInteger("neighborcraft.port", DEFAULT_PORT);
		instance = new HostLink(port);
		instance.start();
		Passthrough.events = message -> instance.broadcast(message);
	}

	@Override
	public void onStart() {
		Passthrough.LOG.info("host link listening on 127.0.0.1:{}", this.getPort());
	}

	@Override
	public void onOpen(final WebSocket conn, final ClientHandshake handshake) {
		Passthrough.LOG.info("host connected from {}", conn.getRemoteSocketAddress());
		conn.send(String.format(Locale.ROOT, "{\"t\":\"hello\",\"v\":1,\"shm\":\"%s\",\"pid\":%d}", FrameExporter.NAME.replace("\\", "\\\\"), ProcessHandle.current().pid()));
	}

	@Override
	public void onClose(final WebSocket conn, final int code, final String reason, final boolean remote) {
		Passthrough.LOG.info("host disconnected ({} {})", code, reason);
	}

	@Override
	public void onMessage(final WebSocket conn, final String message) {
		try {
			JsonObject m = JsonParser.parseString(message).getAsJsonObject();
			switch (m.get("t").getAsString()) {
				case "cam" -> HostState.update(m);
				case "ground" -> WorldBridge.solid(ints(m.getAsJsonArray("c")));
				case "clear" -> WorldBridge.clearSolid();
				case "cmd" -> WorldBridge.command(m.get("c").getAsString());
				case "blocksync" -> WorldBridge.sync(m.has("r") ? m.get("r").getAsInt() : 48);
				case "dev" -> this.relay(conn, message);
				default -> {
					Minecraft minecraft = Minecraft.getInstance();
					minecraft.execute(() -> ClientInput.handle(minecraft, m));
				}
			}
		} catch (RuntimeException e) {
			Passthrough.LOG.warn("bad host message {}: {}", message.length() > 200 ? message.substring(0, 200) : message, e.toString());
		}
	}

	/** Test tools talk to the host plugin through Minecraft's link: pass their messages to every other client. */
	private void relay(final WebSocket from, final String message) {
		for (WebSocket c : this.getConnections()) {
			if (c != from && c.isOpen()) {
				c.send(message);
			}
		}
	}

	@Override
	public void onError(final WebSocket conn, final Exception e) {
		Passthrough.LOG.warn("host link error", e);
	}

	private static int[] ints(final JsonArray a) {
		int[] out = new int[a.size()];
		for (int i = 0; i < out.length; i++) {
			out[i] = a.get(i).getAsInt();
		}

		return out;
	}
}
