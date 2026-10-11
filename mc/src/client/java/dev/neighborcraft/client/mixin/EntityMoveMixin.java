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
	private boolean passthrough$holding;

	@Unique
	private static long passthrough$waitingSince;

	/** How long Steve keeps moving (never down) while his ground is on its way, before he is held still. */
	@Unique
	private static final long BRIEF_WAIT_NANOS = 250_000_000L;

	/** How far down Steve follows the ground he walks on (Minecraft's step-up height, the other way). */
	@Unique
	private static final double STEP_DOWN = 0.6;

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
	/** The blocks of HN's ground found missing by the last check (and whether a whole chunk was). */
	@Unique
	private static final java.util.List<BlockPos> passthrough$missing = new java.util.ArrayList<>();

	@Unique
	private static boolean passthrough$chunkMissing;

	@Unique
	private static boolean passthrough$worldAround(final LocalPlayer player, final double down) {
		AABB box = player.getBoundingBox().inflate(1.0, 0.0, 1.0);
		String missing = null;
		passthrough$missing.clear();
		passthrough$chunkMissing = false;
		int x0 = ((int) Math.floor(box.minX)) >> 4, x1 = ((int) Math.floor(box.maxX)) >> 4;
		int z0 = ((int) Math.floor(box.minZ)) >> 4, z1 = ((int) Math.floor(box.maxZ)) >> 4;
		for (int cx = x0; cx <= x1 && missing == null; cx++) {
			for (int cz = z0; cz <= z1 && missing == null; cz++) {
				if (!player.level().getChunkSource().hasChunk(cx, cz)) {
					missing = "chunk " + cx + " " + cz;
					passthrough$chunkMissing = true;
				}
			}
		}

		// every missing block at and below the feet, down to where this tick's fall can reach
		BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
		int feet = (int) Math.floor(box.minY + 1.0E-3);
		int lowest = Math.min(feet - 1, (int) Math.floor(box.minY + Math.min(0.0, down)) - 1);
		for (int x = (int) Math.floor(box.minX); x <= (int) Math.floor(box.maxX) && !passthrough$chunkMissing; x++) {
			for (int z = (int) Math.floor(box.minZ); z <= (int) Math.floor(box.maxZ); z++) {
				for (int y = lowest; y <= feet; y++) {
					if (!WorldBridge.arrived(player.level(), pos.set(x, y, z))) {
						passthrough$missing.add(pos.immutable());
						if (missing == null) {
							missing = "HN's ground at " + x + " " + y + " " + z;
						}
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

	/**
	 * A move while some of HN's ground around Steve is on its way: whole, unless it would take him into one of the
	 * missing blocks; then it goes down no further than that block's top (the block's own floor inside it is unknown:
	 * from inside it, not down at all). The speed is kept. The first version forbade every move down and zeroed the
	 * fall: jumping down Act 1's road, Steve hung in mid-air for a tick at every wait (the 21:30 recorder session,
	 * ticks 1846, 1862, 1895, 1942: falling 0.15-0.45 a tick, then 0, the fall starting over), though the missing block
	 * lay a block below him, out of that tick's reach. A whole chunk missing: no move down at all, as before.
	 */
	@Unique
	private static Vec3 passthrough$aboveMissing(final LocalPlayer player, final Vec3 movement) {
		if (movement.y >= 0.0) {
			return movement;
		}

		AABB box = player.getBoundingBox();
		if (passthrough$chunkMissing) {
			return new Vec3(movement.x, 0.0, movement.z);
		}

		AABB path = box.expandTowards(movement.x, 0.0, movement.z);
		double dy = movement.y;
		for (BlockPos p : passthrough$missing) {
			if (p.getX() + 1 <= path.minX || p.getX() >= path.maxX || p.getZ() + 1 <= path.minZ || p.getZ() >= path.maxZ) {
				continue;
			}

			double top = p.getY() + 1.0;
			dy = Math.max(dy, top <= box.minY + 1.0E-7 ? top - box.minY : 0.0);
		}

		return dy == movement.y ? movement : new Vec3(movement.x, dy, movement.z);
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
		this.passthrough$holding = false;
		if (Passthrough.driving && !passthrough$worldAround(player, movement.y)) {
			this.passthrough$positionBefore = player.position();
			if (System.nanoTime() - passthrough$waitingSince < BRIEF_WAIT_NANOS) {
				// HN's ground a tick late (running downhill, every wait in the 20:03 session was 47-61 ms): freezing
				// Steve and zeroing his speed stopped every sprint and sprint-jump there. He keeps going; only a move
				// into ground that isn't here yet is cut (see passthrough$aboveMissing).
				Vec3 kept = passthrough$aboveMissing(player, movement);
				this.passthrough$holding = kept != movement;
				return kept;
			}

			// HN's world isn't here yet (a level's start: seconds): hold still (no fall) until it is
			player.setDeltaMovement(Vec3.ZERO);
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

			if (this.passthrough$holding && this.passthrough$groundBefore) {
				// moving on while his ground is on its way: still standing (he can jump, no fall builds up)
				player.setOnGround(true);
				player.resetFallDistance();
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

			if (Passthrough.driving && this.passthrough$groundBefore && !player.onGround() && !this.passthrough$holding && movement.y <= 0.0) {
				this.passthrough$stepDown(player, movement);
			}

			CollisionRecorder.afterMove(player);
		}
	}

	/**
	 * Down a slope. HN's slopes are 1/8-block staircases; walking down one, Steve left the ground at every micro-step
	 * and fell onto the next (the 20:03 recorder session: on ground, then airborne with falls of up to 0.5 a tick
	 * the whole way down Act 1's road), a jolt each time, and a jump pressed in those ticks did nothing. Walking (not
	 * jumping) off ground he stood on, onto ground at most STEP_DOWN below, he is put on it as Minecraft's step-up
	 * does going up; a real drop is still a fall.
	 */
	@Unique
	private void passthrough$stepDown(final LocalPlayer player, final Vec3 movement) {
		if (Math.hypot(movement.x, movement.z) < 1.0E-4 || player.getAbilities().flying || player.isInWater() || player.isInLava()
			|| player.onClimbable() || player.isPassenger() || player.getDeltaMovement().y > 0.0) {
			return;
		}

		AABB box = player.getBoundingBox();
		AABB below = new AABB(box.minX, box.minY - STEP_DOWN, box.minZ, box.maxX, box.minY, box.maxZ);
		double top = Double.NEGATIVE_INFINITY;
		for (VoxelShape shape : player.level().getBlockCollisions(player, below)) {
			for (AABB b : shape.toAabbs()) {
				if (b.intersects(below) && b.maxY <= box.minY + 1.0E-7 && b.maxY > top) {
					top = b.maxY;
				}
			}
		}

		if (top == Double.NEGATIVE_INFINITY || !player.level().noCollision(player, box.move(0.0, top - box.minY, 0.0))) {
			return;
		}

		player.setPos(player.getX(), player.getY() + (top - box.minY), player.getZ());
		player.setOnGround(true);
		player.resetFallDistance();
		Vec3 v = player.getDeltaMovement();
		player.setDeltaMovement(v.x, 0.0, v.z);
	}
}
