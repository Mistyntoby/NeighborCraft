package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.FrameExporter;
import dev.neighborcraft.client.HostState;
import dev.neighborcraft.client.PlayerSync;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.player.AbstractClientPlayer;
import dev.neighborcraft.Passthrough;
import java.util.Locale;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.phys.Vec3;
import org.joml.Quaternionf;
import org.joml.Vector3f;
import org.joml.Vector3fc;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The host's camera replaces the player's: position, rotation (with roll), field of view and first/third person. */
@Mixin(Camera.class)
abstract class CameraMixin {
	private static final float DEG = (float) (Math.PI / 180.0);
	@Shadow @Final private static Vector3fc FORWARDS;
	@Shadow @Final private static Vector3fc UP;
	@Shadow @Final private static Vector3fc LEFT;
	@Shadow @Final private Vector3f forwards;
	@Shadow @Final private Vector3f up;
	@Shadow @Final private Vector3f left;
	@Shadow @Final private Quaternionf rotation;
	@Shadow private float xRot;
	@Shadow private float yRot;
	@Shadow private boolean detached;
	@Shadow private int matrixPropertiesDirty;
	@Shadow private float depthFar;
	/** The FOV the last frame was rendered with (driving), sent to the host with the camera. */
	private static float lastFov;

	@Shadow
	protected abstract void setPosition(double x, double y, double z);

	@Inject(method = "update", at = @At("HEAD"))
	private void passthrough$beginFrame(final DeltaTracker deltaTracker, final CallbackInfo ci) {
		HostState.beginFrame();
		PlayerSync.frame(deltaTracker.getGameTimeDeltaPartialTick(true));
	}

	@Inject(method = "alignWithEntity", at = @At("TAIL"))
	private void passthrough$hostCamera(final float partialTicks, final CallbackInfo ci) {
		HostState.Pose p = HostState.frame();
		if (p == null || p.drive()) {
			// driving: Minecraft's own first-person camera (the player already looks where the host looks)
			return;
		}

		this.xRot = p.pitch();
		this.yRot = p.yaw();
		this.rotation.rotationYXZ((float) Math.PI - p.yaw() * DEG, -p.pitch() * DEG, p.roll() * DEG);
		FORWARDS.rotate(this.rotation, this.forwards);
		UP.rotate(this.rotation, this.up);
		LEFT.rotate(this.rotation, this.left);
		this.matrixPropertiesDirty |= 3;
		double x = p.x(), y = p.y(), z = p.z();
		Entity player = Minecraft.getInstance().player;
		this.setPosition(x, y, z);
		// a third-person camera pulled in against a wall would put Minecraft's camera inside Steve's head
		boolean inside = player != null && player.getEyePosition(partialTicks).distanceToSqr(x, y, z) < 0.8 * 0.8;
		this.detached = !p.firstPerson() && !inside;
	}

	@Inject(method = "calculateFov", at = @At("HEAD"), cancellable = true)
	private void passthrough$hostFov(final float partialTicks, final CallbackInfoReturnable<Float> cir) {
		HostState.Pose p = HostState.frame();
		if (p != null && !p.drive()) {
			cir.setReturnValue(p.fov());
		}
	}

	/**
	 * Driving: Minecraft's own FOV (its setting and effects, sprinting widens it) is what the host's camera takes; the
	 * world itself is rendered with tan(fov / 2) 15% wider (overscan), so re-projecting the frame onto the host's newer
	 * camera never runs out of picture at the screen edges. The hand and HUD have their own FOV (calculateHudFov).
	 */
	@Inject(method = "calculateFov", at = @At("RETURN"), cancellable = true)
	private void passthrough$minecraftFov(final float partialTicks, final CallbackInfoReturnable<Float> cir) {
		HostState.Pose p = HostState.frame();
		float fov = cir.getReturnValueF();
		if (p != null && p.drive()) {
			lastFov = fov;
			fov = (float) Math.toDegrees(2.0 * Math.atan(Math.tan(Math.toRadians(fov) / 2.0) * OVERSCAN));
			cir.setReturnValue(fov);
		}
		HostState.setRenderedFov(fov);
	}

	private static final double OVERSCAN = 1.15;

	@Inject(method = "update", at = @At("TAIL"))
	private void passthrough$planes(final DeltaTracker deltaTracker, final CallbackInfo ci) {
		FrameExporter.setFar(this.depthFar);
		HostState.Pose p = HostState.frame();
		Entity player = Minecraft.getInstance().player;
		Camera self = (Camera) (Object) this;
		float partial = deltaTracker.getGameTimeDeltaPartialTick(true);
		if (p != null && p.drive() && player != null) {
			// vanilla's view bob, as a camera transform: the world is never bobbed separately (GameRendererMixin), so
			// Minecraft's frame and the host's camera (which takes this pose) bob together, exactly once
			if (Minecraft.getInstance().options.bobView().get() && player instanceof AbstractClientPlayer avatar) {
				this.passthrough$applyViewBob(avatar, partial);
			}

			Vector3f euler = this.rotation.getEulerAnglesYXZ(new Vector3f());
			float yaw = 180.0F - euler.y / DEG, pitch = -euler.x / DEG, roll = euler.z / DEG;
			HostState.setRenderedCamera(yaw, pitch, roll);
			// the host renders from exactly this camera (first or third person), and puts its player at the eyes
			Vec3 cam = self.position();
			Vec3 eye = player.getEyePosition(partial);
			Passthrough.events.accept(String.format(Locale.ROOT,
				"{\"t\":\"mcpos\",\"eye\":[%.4f,%.4f,%.4f],\"cam\":[%.4f,%.4f,%.4f],\"rot\":[%.3f,%.3f,%.3f],\"look\":[%.3f,%.3f],\"fp\":%b,\"fov\":%.3f,\"tn\":%d}",
				eye.x, eye.y, eye.z, cam.x, cam.y, cam.z, yaw, pitch, roll, p.yaw(), p.pitch(), !this.detached, lastFov > 0.0F ? lastFov : 70.0F,
				System.nanoTime()));
		} else {
			HostState.setRenderedCamera(self.yRot(), self.xRot(), p != null ? p.roll() : 0.0F);
		}
	}

	/**
	 * GameRenderer.bobView turns the world by the walk bob in view space: translate (sin(w pi) b / 2, -|cos(w pi) b|),
	 * roll sin(w pi) b 3 degrees, pitch |cos(w pi - 0.2) b| 5 degrees (w: the backwards interpolated walk distance, b:
	 * the interpolated bob). The same as a camera move: rotation * Rx(-pitch) * Rz(-roll), moved by minus the
	 * translation in that rotated frame.
	 */
	private void passthrough$applyViewBob(final AbstractClientPlayer avatar, final float partial) {
		float w = avatar.avatarState().getBackwardsInterpolatedWalkDistance(partial);
		float b = avatar.avatarState().getInterpolatedBob(partial);
		float s = (float) Math.sin(w * Math.PI), c = (float) Math.cos(w * Math.PI);
		float tx = s * b * 0.5F, ty = -Math.abs(c * b);
		float roll = s * b * 3.0F * DEG, pitch = Math.abs((float) Math.cos(w * Math.PI - 0.2F) * b) * 5.0F * DEG;
		this.rotation.rotateX(-pitch).rotateZ(-roll);
		Vector3f offset = new Vector3f(-tx, -ty, 0.0F).rotate(this.rotation);
		Vec3 at = ((Camera) (Object) this).position();
		this.setPosition(at.x + offset.x, at.y + offset.y, at.z + offset.z);
		FORWARDS.rotate(this.rotation, this.forwards);
		UP.rotate(this.rotation, this.up);
		LEFT.rotate(this.rotation, this.left);
		this.matrixPropertiesDirty |= 3;
	}
}
