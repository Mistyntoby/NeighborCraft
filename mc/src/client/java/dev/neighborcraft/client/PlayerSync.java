package dev.neighborcraft.client;

import dev.neighborcraft.Passthrough;
import java.util.Locale;
import net.minecraft.client.CameraType;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.player.Abilities;
import net.minecraft.world.phys.Vec3;

/** Keeps the Minecraft player on the host's player: it stands where they stand and looks where the host camera looks. */
public final class PlayerSync {
	private static final double TELEPORT_SQ = 64.0 * 64.0;
	/** How far the host's player moved over the last client tick (drives the walk animation). */
	private static float tickDistance;
	/** Minecraft's own view-bob inputs, from the host's movement (the synced player never walks by itself). */
	private static float walkDist, walkDistO, bob, bobO;
	private static double lastX = Double.NaN, lastZ;

	private PlayerSync() {
	}

	public static float tickDistance() {
		return tickDistance;
	}

	/** The hand's walk bob, as vanilla computes it for a walking player: {walk distance (negated), bob}. */
	public static float[] handBob(final float partialTick) {
		float walk = walkDist + (walkDist - walkDistO) * partialTick;
		return new float[] {-walk, bobO + (bob - bobO) * partialTick};
	}

	/** Every frame, before the camera update: position, rotation, and first/third person to match the host. */
	public static void frame(final float partialTick) {
		HostState.Pose p = HostState.frame();
		Minecraft minecraft = Minecraft.getInstance();
		LocalPlayer player = minecraft.player;
		if (p == null || player == null) {
			return;
		}

		if (p.drive()) {
			// Minecraft moves the player: only the look comes from the host, and the host is told where the eyes are
			player.setYRot(p.yaw());
			player.setXRot(p.pitch());
			player.yRotO = p.yaw();
			player.xRotO = p.pitch();
			player.yHeadRot = player.yHeadRotO = p.yaw();
			if (minecraft.options.getCameraType() != CameraType.FIRST_PERSON) {
				minecraft.options.setCameraType(CameraType.FIRST_PERSON);
			}

			Vec3 eye = player.getEyePosition(partialTick);
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"mcpos\",\"eye\":[%.4f,%.4f,%.4f],\"feet\":%.4f,\"ground\":%b,\"tn\":%d}",
				eye.x, eye.y, eye.z, player.getPosition(partialTick).y, player.onGround(), System.nanoTime()));
			return;
		}

		player.setYRot(p.yaw());
		player.setXRot(p.pitch());
		player.yRotO = p.yaw();
		player.xRotO = p.pitch();
		player.yHeadRot = player.yHeadRotO = p.yaw();
		player.yBodyRot = player.yBodyRotO = p.firstPerson() ? p.yaw() : p.bodyYaw();
		// the model stands exactly where the host's player is this frame (not a tick behind, interpolating)
		double x = p.firstPerson() ? p.x() : p.px();
		double y = p.firstPerson() ? p.y() - player.getEyeHeight() : p.py();
		double z = p.firstPerson() ? p.z() : p.pz();
		player.setPos(x, y, z);
		player.xo = player.xOld = x;
		player.yo = player.yOld = y;
		player.zo = player.zOld = z;
		CameraType cameraType = p.firstPerson() ? CameraType.FIRST_PERSON : CameraType.THIRD_PERSON_BACK;
		if (minecraft.options.getCameraType() != cameraType) {
			minecraft.options.setCameraType(cameraType);
		}
	}

	/**
	 * Just before the player's movement goes to the server: on the ground or not as the host's player is. The server
	 * counts the fall from there, so a hit while falling after a jump is critical, as in Minecraft.
	 */
	public static void beforeSend(final LocalPlayer player) {
		HostState.Pose p = HostState.live();
		if (p != null && !p.drive()) {
			player.setOnGround(!p.air());
		}
	}

	/**
	 * Every client tick, at the start of the player's tick (the old position is already saved, so the model
	 * interpolates and walks): in first person the player's eyes are at the host camera, in third person
	 * their feet are at the host player's.
	 */
	public static void tick(final LocalPlayer player) {
		HostState.Pose p = HostState.live();
		if (p == null) {
			return;
		}

		if (p.drive()) {
			// walking, not creative flight: jumps, falls and critical hits work as in survival
			Abilities abilities = player.getAbilities();
			if (abilities.flying || abilities.mayfly) {
				abilities.flying = false;
				abilities.mayfly = false;
				player.onUpdateAbilities();
			}

			return;
		}

		double x = p.firstPerson() ? p.x() : p.px();
		double y = p.firstPerson() ? p.y() - player.getEyeHeight() : p.py();
		double z = p.firstPerson() ? p.z() : p.pz();
		tickDistance = Double.isNaN(lastX) ? 0.0F : (float) Math.min(Math.hypot(x - lastX, z - lastZ), 1.0);
		walkDistO = walkDist;
		walkDist += tickDistance * 0.6F;
		bobO = bob;
		bob += ((p.air() ? 0.0F : Math.min(0.1F, tickDistance)) - bob) * 0.4F;
		lastX = x;
		lastZ = z;
		boolean teleport = player.distanceToSqr(x, y, z) > TELEPORT_SQ;
		player.setPos(x, y, z);
		if (teleport) {
			player.xo = player.xOld = x;
			player.yo = player.yOld = y;
			player.zo = player.zOld = z;
		}

		player.setDeltaMovement(Vec3.ZERO);
		Abilities abilities = player.getAbilities();
		if (abilities.mayfly && !abilities.flying) {
			abilities.flying = true;
			player.onUpdateAbilities();
		}
	}
}
