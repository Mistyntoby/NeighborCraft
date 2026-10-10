package dev.neighborcraft.client.mixin;

import dev.neighborcraft.Passthrough;
import dev.neighborcraft.WorldBridge;
import dev.neighborcraft.client.CollisionRecorder;
import net.minecraft.core.BlockPos;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * The local player's own movement (what it tried, what collision let it do) for the collision recorder, and Minecraft
 * movement over Hello Neighbor's floors without losing speed on slopes, seams and stairs.
 *
 * <p>Micro-ledges. HN's gentle slopes and seams are, at 1/8 block, staircases of tiny risers: the recorder showed a
 * ramp after a curb rising 0.8 cm per column, and sprints there cut to 0.13 blocks a tick (tools/drivetest.py
 * reproduces it: 5.6 m/s on the flat, 2.3-2.7 on such a ramp). Minecraft's step-up takes the lowest step height that
 * helps, one column per tick; a lift alone didn't help either, because the tick's gravity (resolved before the
 * horizontal movement) dropped Steve back onto the column he stood on. So on the ground, before the move, Steve is
 * lifted onto the highest ledge of at most 1/16 block in his path (if there is room above him), that tick moves
 * horizontally only, and he stays on the ground (he stands on that ledge at the end of the move). Real steps stay
 * Minecraft's step-up.
 *
 * <p>Stairs. A horizontal collision makes Minecraft zero the velocity along its axis. On HN's stairs a sprinting tick
 * steps up and grazes the next riser: the recorder showed the step succeed with most of the movement through and the
 * zeroed axis dropping the speed to half, at every step. When the player stepped up and got through at least half of
 * the movement, or only grazed something (85% through), the speed from before the collision is kept; a wall (most of
 * the movement stopped) still stops him as in Minecraft.
 */
@Mixin(Entity.class)
abstract class EntityMoveMixin {
	@Unique
	private Vec3 passthrough$velocityBefore;

	@Unique
	private Vec3 passthrough$positionBefore;

	@Unique
	private boolean passthrough$groundBefore;

	@Unique
	private boolean passthrough$lifted;

	@Inject(method = "move", at = @At("HEAD"))
	private void passthrough$beforeMove(final MoverType type, final Vec3 movement, final CallbackInfo ci) {
		if (type == MoverType.SELF && (Object) this instanceof LocalPlayer player) {
			CollisionRecorder.beforeMove(player, movement);
		}
	}

	@Unique
	private static long passthrough$waitingSince;

	/**
	 * Whether HN's ground under the player is in the client's world. HN's world reaches Minecraft as blocks the
	 * integrated server places, and the client has them only once the server has sent them. After HN moves its player
	 * far (a level's start: Act 1's road, ~130 blocks from Minecraft's spawn) the server falls seconds behind ("Can't
	 * keep up! ... 2963ms / 12590ms behind" both times Steve fell there) and hundreds of chunks are queued for the
	 * client: Steve, moved by the client, ran onto ground that wasn't there yet and fell through the road while HN had
	 * measured it ("standing on air - force-sending blocks below"). Minecraft waits for the world at a join; here it
	 * waits, holding still, while the chunks under him or HN's collision for the ground at his feet are still on their
	 * way (the blocks at and below his feet: walls arriving late don't drop him, and moving parts beside him update
	 * all the time).
	 */
	@Unique
	private static boolean passthrough$worldAround(final LocalPlayer player) {
		AABB box = player.getBoundingBox().inflate(1.0, 0.0, 1.0);
		String missing = null;
		int x0 = ((int) Math.floor(box.minX)) >> 4, x1 = ((int) Math.floor(box.maxX)) >> 4;
		int z0 = ((int) Math.floor(box.minZ)) >> 4, z1 = ((int) Math.floor(box.maxZ)) >> 4;
		for (int cx = x0; cx <= x1 && missing == null; cx++) {
			for (int cz = z0; cz <= z1 && missing == null; cz++) {
				if (!player.level().getChunkSource().hasChunk(cx, cz)) {
					missing = "chunk " + cx + " " + cz;
				}
			}
		}

		BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		int feet = (int) Math.floor(box.minY + 1.0E-3);
		for (int x = (int) Math.floor(box.minX); x <= (int) Math.floor(box.maxX) && missing == null; x++) {
			for (int z = (int) Math.floor(box.minZ); z <= (int) Math.floor(box.maxZ) && missing == null; z++) {
				for (int y = feet - 1; y <= feet && missing == null; y++) {
					if (!WorldBridge.arrived(player.level(), pos.set(x, y, z))) {
						missing = "HN's ground at " + x + " " + y + " " + z;
					}
				}
			}
		}

		long now = System.nanoTime();
		if (missing != null) {
			if (passthrough$waitingSince == 0L) {
				passthrough$waitingSince = now;
				Passthrough.LOG.info("waiting for HN's world: {} not on the client yet", missing);
			}

			// never stuck for good (something that never arrives): after 5 s Steve moves anyway, and it is logged
			if (now - passthrough$waitingSince < 5_000_000_000L) {
				return false;
			}

			if (passthrough$waitingSince > 0L) {
				Passthrough.LOG.warn("HN's world still not here after 5 s ({}): moving on", missing);
				passthrough$waitingSince = -1L;
			}

			return true;
		}

		if (passthrough$waitingSince > 0L) {
			Passthrough.LOG.info("HN's world arrived after {} ms", (now - passthrough$waitingSince) / 1_000_000L);
		}

		passthrough$waitingSince = 0L;
		return true;
	}

	/** After the recorder saw the attempt: the lift over micro-ledges (the tick then moves horizontally only). */
	@ModifyVariable(method = "move", at = @At("HEAD"), argsOnly = true)
	private Vec3 passthrough$overMicroLedges(final Vec3 movement, final MoverType type, final Vec3 original) {
		this.passthrough$lifted = false;
		if (type != MoverType.SELF || !((Object) this instanceof LocalPlayer player)) {
			return movement;
		}

		this.passthrough$velocityBefore = player.getDeltaMovement();
		this.passthrough$groundBefore = player.onGround();
		if (Passthrough.driving && !passthrough$worldAround(player)) {
			// HN's world isn't here yet: hold still (no fall) until it is
			player.setDeltaMovement(Vec3.ZERO);
			this.passthrough$positionBefore = player.position();
			return Vec3.ZERO;
		}

		Vec3 result = movement;
		if (Passthrough.driving && player.onGround() && movement.y <= 0.0 && Math.hypot(movement.x, movement.z) > 1.0E-4) {
			final double micro = 0.0625;
			AABB box = player.getBoundingBox();
			AABB path = box.expandTowards(movement.x, 0.0, movement.z).setMinY(box.minY + 1.0E-5).setMaxY(box.minY + micro);
			double top = box.minY;
			for (VoxelShape shape : player.level().getBlockCollisions(player, path)) {
				for (AABB b : shape.toAabbs()) {
					if (b.intersects(path) && b.maxY > top && b.maxY <= box.minY + micro) {
						top = b.maxY;
					}
				}
			}

			double lift = top - box.minY;
			if (lift > 1.0E-5 && player.level().noCollision(player, box.move(0.0, lift, 0.0))) {
				player.setPos(player.getX(), player.getY() + lift, player.getZ());
				result = new Vec3(movement.x, 0.0, movement.z);
				this.passthrough$lifted = true;
			}
		}

		this.passthrough$positionBefore = player.position();
		return result;
	}

	@Inject(method = "move", at = @At("RETURN"))
	private void passthrough$afterMove(final MoverType type, final Vec3 movement, final CallbackInfo ci) {
		if (type == MoverType.SELF && (Object) this instanceof LocalPlayer player) {
			if (this.passthrough$lifted) {
				// on the ledge he was lifted onto: grounded, no fall built up
				player.setOnGround(true);
				Vec3 v = player.getDeltaMovement();
				player.setDeltaMovement(v.x, 0.0, v.z);
			}

			if (Passthrough.driving && player.horizontalCollision && this.passthrough$groundBefore && this.passthrough$positionBefore != null) {
				Vec3 moved = player.position().subtract(this.passthrough$positionBefore);
				double attempted = Math.hypot(movement.x, movement.z), through = Math.hypot(moved.x, moved.z);
				boolean stepped = moved.y > 1.0E-4 && through >= 0.5 * attempted;
				boolean grazed = through >= 0.85 * attempted;
				if (attempted > 1.0E-4 && (stepped || grazed)) {
					Vec3 now = player.getDeltaMovement();
					player.setDeltaMovement(this.passthrough$velocityBefore.x, now.y, this.passthrough$velocityBefore.z);
				}
			}

			CollisionRecorder.afterMove(player);
		}
	}
}
