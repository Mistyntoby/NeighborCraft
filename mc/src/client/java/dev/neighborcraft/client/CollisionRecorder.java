package dev.neighborcraft.client;

import dev.neighborcraft.Passthrough;
import java.io.IOException;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Pose;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;

/**
 * Collision diagnostics, always recording (F12 in the host adds the debug view and records every kind of event; without
 * it only movement problems are written: blocked, failed steps, lost speed or sprint): every client tick of the player's own movement is recorded
 * (attempted and resolved movement, velocity, inputs, ground and collision flags, pose, hitbox, and the collision
 * boxes around the player), with labels for what the evidence shows. The last 10 s are kept in memory; when an event
 * worth looking at happens, the 2 s before and 1 s after it go to run/neighborcraft-collisions/session-*.jsonl,
 * rate limited. tools/collision_report.py turns a session into a readable report with pictures.
 *
 * <p>Labels (only when the tick's numbers show it):
 * BLOCKED_BY_WALL: a horizontal collision took more than half the attempted horizontal movement;
 * MINOR_COLLISION: Minecraft's own "minor" horizontal collision (a graze);
 * STEP_UP: the movement was lifted onto something (Minecraft's step-up);
 * STEP_UP_FAILED: blocked on the ground by something no higher than the step height (0.6), with the reason
 * (HEADROOM: something above stopped the lift);
 * VELOCITY_RESET: horizontal speed fell under 30% in one tick;
 * SPRINT_LOST: sprinting ended; POSE_FORCED: crouching or crawling without the sneak key;
 * GROUND_CHECK_MISMATCH: on the ground with no collision surface under the feet, or the reverse;
 * MIDAIR_STOP: falling in the air, the fall cut to under half with no vertical collision (something held him).
 */
public final class CollisionRecorder {
	private static final int KEEP = 200;   // ticks in memory (10 s)
	private static final int BEFORE = 40;  // ticks written before an event (2 s)
	private static final int AFTER = 20;   // and after (1 s)
	private static final long MIN_GAP_NANOS = 500_000_000L;
	private static final int MAX_DUMPS = 400;

	private static volatile boolean on;          // the debug view (F12)
	private static final boolean RECORDING = true; // the recording itself: always, so a sticking spot is caught when it happens
	private static final ArrayDeque<String> ring = new ArrayDeque<>();
	private static Vec3 posBefore;
	private static Vec3 attempted;
	private static boolean groundBefore;
	private static double lastSpeed;
	private static boolean lastSprint;
	private static long tick;
	private static int pendingAfter = -1;
	private static String pendingEvent;
	private static long lastDump;
	private static int dumps;
	private static Path file;

	private CollisionRecorder() {
	}

	public static boolean on() {
		return on;
	}

	static void setOn(final boolean value) {
		on = value;
		ring.clear();
		pendingAfter = -1;
		Passthrough.LOG.info("collision recorder {}", value ? "on" : "off");
	}

	/** Entity.move HEAD, the local player's own movement. */
	public static void beforeMove(final LocalPlayer player, final Vec3 movement) {
		if (!RECORDING) {
			return;
		}

		posBefore = player.position();
		attempted = movement;
		groundBefore = player.onGround();
	}

	/** Entity.move RETURN. */
	public static void afterMove(final LocalPlayer player) {
		if (!RECORDING || posBefore == null) {
			return;
		}

		tick++;
		Vec3 resolved = player.position().subtract(posBefore);
		AABB bb = player.getBoundingBox();
		List<AABB> boxes = new ArrayList<>();
		// around him up to standing height whatever his pose (crawling, his box is 0.6: what forced it was out of sight)
		AABB around = new AABB(bb.minX, bb.minY, bb.minZ, bb.maxX, Math.max(bb.maxY, bb.minY + 1.8), bb.maxZ);
		for (VoxelShape shape : player.level().getBlockCollisions(player, around.inflate(0.8, 0.4, 0.8).expandTowards(attempted))) {
			boxes.addAll(shape.toAabbs());
		}

		List<String> labels = new ArrayList<>();
		double hAttempt = Math.hypot(attempted.x, attempted.z), hResolved = Math.hypot(resolved.x, resolved.z);
		double feet = posBefore.y;
		if (player.horizontalCollision && hAttempt > 0.01 && hResolved < hAttempt * 0.5 && !player.minorHorizontalCollision) {
			labels.add("BLOCKED_BY_WALL");
		}

		if (player.minorHorizontalCollision) {
			labels.add("MINOR_COLLISION");
		}

		if (groundBefore && resolved.y > attempted.y + 0.01 && resolved.y > 0.01) {
			labels.add(String.format(Locale.ROOT, "STEP_UP %.3f", resolved.y));
		}

		if (player.horizontalCollision && groundBefore && hAttempt > 0.01) {
			// what blocked the attempted horizontal path, and could Minecraft's step-up (0.6) have lifted over it?
			AABB moved = player.getBoundingBox().move(attempted.x, 0.0, attempted.z);
			double top = Double.NEGATIVE_INFINITY;
			for (AABB box : boxes) {
				if (box.intersects(moved) && !box.intersects(bb)) {
					top = Math.max(top, box.maxY);
				}
			}

			double height = top - player.getY();
			if (top > Double.NEGATIVE_INFINITY && height > 0.0 && height <= player.maxUpStep() + 1.0E-3) {
				AABB lifted = moved.move(0.0, height + 1.0E-3, 0.0);
				boolean headroom = boxes.stream().anyMatch(box -> box.intersects(lifted));
				labels.add(String.format(Locale.ROOT, "STEP_UP_FAILED %.3f%s", height, headroom ? " HEADROOM" : ""));
			}
		}

		// in the air, falling, and the fall cut short with nothing under him (no vertical collision): a hang in mid-air
		if (!groundBefore && attempted.y < -0.01 && resolved.y > attempted.y * 0.5 + 1.0E-4 && !player.verticalCollision) {
			labels.add(String.format(Locale.ROOT, "MIDAIR_STOP %.3f of %.3f", resolved.y, attempted.y));
		}

		double speed = Math.hypot(player.getDeltaMovement().x, player.getDeltaMovement().z);
		if (lastSpeed > 0.1 && speed < lastSpeed * 0.3) {
			labels.add("VELOCITY_RESET");
		}

		if (lastSprint && !player.isSprinting()) {
			labels.add("SPRINT_LOST");
		}

		Pose pose = player.getPose();
		boolean sneakKey = Minecraft.getInstance().options.keyShift.isDown();
		if ((pose == Pose.CROUCHING && !sneakKey) || pose == Pose.SWIMMING) {
			labels.add("POSE_FORCED " + pose);
		}

		boolean surfaceUnder = boxes.stream().anyMatch(box -> Math.abs(box.maxY - player.getY()) < 0.02
			&& box.maxX > bb.minX && box.minX < bb.maxX && box.maxZ > bb.minZ && box.minZ < bb.maxZ);
		if (player.onGround() != surfaceUnder && Math.abs(player.getDeltaMovement().y) < 0.2) {
			labels.add(player.onGround() ? "GROUND_CHECK_MISMATCH on ground, no surface under the feet" : "GROUND_CHECK_MISMATCH surface under the feet, not on ground");
		}

		lastSpeed = speed;
		lastSprint = player.isSprinting();

		StringBuilder b = new StringBuilder(512);
		b.append(String.format(Locale.ROOT,
			"{\"tick\":%d,\"ns\":%d,\"pos\":[%.4f,%.4f,%.4f],\"attempt\":[%.4f,%.4f,%.4f],\"resolved\":[%.4f,%.4f,%.4f],\"vel\":[%.4f,%.4f,%.4f],"
				+ "\"ground\":%b,\"groundBefore\":%b,\"hColl\":%b,\"minor\":%b,\"vColl\":%b,\"pose\":\"%s\",\"sprint\":%b,\"sneakKey\":%b,"
				+ "\"input\":[%.2f,%.2f],\"jump\":%b,\"bb\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],\"labels\":[",
			tick, System.nanoTime(), player.getX(), player.getY(), player.getZ(), attempted.x, attempted.y, attempted.z, resolved.x, resolved.y, resolved.z,
			player.getDeltaMovement().x, player.getDeltaMovement().y, player.getDeltaMovement().z, player.onGround(), groundBefore,
			player.horizontalCollision, player.minorHorizontalCollision, player.verticalCollision, pose, player.isSprinting(), sneakKey,
			player.input.getMoveVector().x, player.input.getMoveVector().y, player.input.keyPresses.jump(), bb.minX, bb.minY, bb.minZ, bb.maxX, bb.maxY, bb.maxZ));
		for (int i = 0; i < labels.size(); i++) {
			b.append(i == 0 ? "\"" : ",\"").append(labels.get(i)).append('"');
		}

		b.append("],\"boxes\":[");
		for (int i = 0; i < boxes.size() && i < 400; i++) {
			AABB box = boxes.get(i);
			b.append(i == 0 ? "" : ",").append(String.format(Locale.ROOT, "[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f]",
				box.minX, box.minY, box.minZ, box.maxX, box.maxY, box.maxZ));
		}

		b.append("]}");
		ring.addLast(b.toString());
		while (ring.size() > KEEP) {
			ring.removeFirst();
		}

		// an event worth a look (a step that worked is normal, and is only recorded around other events)
		boolean event = labels.stream().anyMatch(l -> !l.startsWith("STEP_UP ") && !l.startsWith("MINOR")
			&& (on || !l.startsWith("GROUND_CHECK") && !l.startsWith("POSE_FORCED")));
		if (event && pendingAfter < 0 && System.nanoTime() - lastDump > MIN_GAP_NANOS && dumps < MAX_DUMPS) {
			pendingAfter = AFTER;
			pendingEvent = String.join(" | ", labels);
		}

		if (pendingAfter >= 0 && pendingAfter-- == 0) {
			dump();
		}
	}

	private static void dump() {
		lastDump = System.nanoTime();
		dumps++;
		try {
			if (file == null) {
				Path dir = Minecraft.getInstance().gameDirectory.toPath().resolve("neighborcraft-collisions");
				Files.createDirectories(dir);
				file = dir.resolve("session-" + LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss")) + ".jsonl");
			}

			List<String> window = new ArrayList<>(ring);
			int from = Math.max(0, window.size() - BEFORE - AFTER - 1);
			try (Writer w = Files.newBufferedWriter(file, StandardCharsets.UTF_8, java.nio.file.StandardOpenOption.CREATE, java.nio.file.StandardOpenOption.APPEND)) {
				w.write("{\"event\":\"" + pendingEvent + "\",\"ticks\":[" + String.join(",", window.subList(from, window.size())) + "]}\n");
			}
		} catch (IOException e) {
			Passthrough.LOG.warn("collision recorder: couldn't write", e);
		}
	}
}
