package dev.neighborcraft.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import dev.neighborcraft.Passthrough;

/** Hello Neighbor's latest camera and player pose, already in Minecraft coordinates (the host converts). */
public final class HostState {
	/**
	 * @param hostFrame the host's frame number for this pose (echoed in the exported frame)
	 * @param x camera position
	 * @param yaw Minecraft yaw (0 = facing +Z), pitch (positive = down) and roll, in degrees
	 * @param fov vertical field of view, degrees
	 * @param firstPerson whether the host camera is first person (third person shows the player model)
	 * @param px the player's feet (third person)
	 * @param bodyYaw the player's body yaw (third person)
	 * @param air the host's player is airborne (jumping or falling): Minecraft's hits are critical while falling
	 * @param drive Minecraft moves the player (its own physics) and the host follows; only the look (yaw, pitch,
	 *        fov) comes from the host, and the position fields are unused
	 */
	public record Pose(
		long hostFrame, double x, double y, double z, float yaw, float pitch, float roll, float fov,
		boolean firstPerson, double px, double py, double pz, float bodyYaw, long receivedNanos, boolean air, boolean drive
	) {
	}

	private static final long TIMEOUT_NANOS = 2_000_000_000L;
	private static volatile Pose latest;
	/** The pose this frame renders with, taken once per frame so every hook agrees. Render thread only. */
	private static Pose frame;
	/** The host frame the last Minecraft frame was rendered for. Render thread only. */
	private static long renderedHostFrame = -1;

	private HostState() {
	}

	/**
	 * {"t":"cam","f":frame,"p":[x,y,z],"r":[yaw,pitch,roll],"fov":deg,"fp":bool,"pl":[x,y,z],"h":bodyYaw}, or
	 * {"t":"cam","drive":true,"f":frame,"r":[yaw,pitch,roll],"fov":deg} when Minecraft moves the player.
	 */
	static void update(final JsonObject m) {
		boolean drive = m.has("drive") && m.get("drive").getAsBoolean();
		JsonArray p = drive && !m.has("p") ? null : m.getAsJsonArray("p");
		JsonArray r = m.getAsJsonArray("r");
		JsonArray pl = m.has("pl") ? m.getAsJsonArray("pl") : p;
		float yaw = r.get(0).getAsFloat();
		latest = new Pose(
			m.has("f") ? m.get("f").getAsLong() : 0L,
			p == null ? 0.0 : p.get(0).getAsDouble(), p == null ? 0.0 : p.get(1).getAsDouble(), p == null ? 0.0 : p.get(2).getAsDouble(),
			yaw, r.get(1).getAsFloat(), r.size() > 2 ? r.get(2).getAsFloat() : 0.0F,
			m.has("fov") ? m.get("fov").getAsFloat() : 70.0F,
			!m.has("fp") || m.get("fp").getAsBoolean(),
			pl == null ? 0.0 : pl.get(0).getAsDouble(), pl == null ? 0.0 : pl.get(1).getAsDouble(), pl == null ? 0.0 : pl.get(2).getAsDouble(),
			m.has("h") ? m.get("h").getAsFloat() : yaw,
			System.nanoTime(),
			m.has("air") && m.get("air").getAsBoolean(),
			drive
		);
	}

	/** The latest pose if the host is still sending, else null. */
	static Pose live() {
		Pose p = latest;
		return p != null && System.nanoTime() - p.receivedNanos() < TIMEOUT_NANOS ? p : null;
	}

	/** The pose for the frame being rendered, or null when no host is attached. */
	public static Pose frame() {
		return frame;
	}

	/**
	 * Lock-step with the host: each Minecraft frame waits (up to 25 ms) for the host's next camera, so both games make
	 * exactly one frame per host frame. Running freely, Minecraft's 120 fps against the host's own rate meant the host
	 * picked up Minecraft poses of uneven age: judder, worst while falling.
	 */
	public static void beginFrame() {
		Pose before = latest;
		if (before != null && System.nanoTime() - before.receivedNanos() < TIMEOUT_NANOS && before.hostFrame() == renderedHostFrame) {
			long until = System.nanoTime() + 25_000_000L;
			while (latest == before && System.nanoTime() < until) {
				java.util.concurrent.locks.LockSupport.parkNanos(100_000L);
			}
		}

		frame = live();
		if (frame != null) {
			renderedHostFrame = frame.hostFrame();
		}

		Passthrough.active = frame != null;
		Passthrough.driving = frame != null && frame.drive();
	}
}
