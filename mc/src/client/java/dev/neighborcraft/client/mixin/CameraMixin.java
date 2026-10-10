package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.FrameExporter;
import dev.neighborcraft.client.HostState;
import dev.neighborcraft.client.PlayerSync;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
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

	/** Driving: Minecraft's FOV with its effects (sprint, ...), relative to the host's base FOV instead of Minecraft's. */
	@Inject(method = "calculateFov", at = @At("RETURN"), cancellable = true)
	private void passthrough$minecraftFovEffects(final float partialTicks, final CallbackInfoReturnable<Float> cir) {
		HostState.Pose p = HostState.frame();
		if (p != null && p.drive()) {
			float fov = cir.getReturnValueF() * p.fov() / Minecraft.getInstance().options.fov().get();
			cir.setReturnValue(fov);
			lastFov = fov;
		}
	}

	@Inject(method = "update", at = @At("TAIL"))
	private void passthrough$planes(final DeltaTracker deltaTracker, final CallbackInfo ci) {
		FrameExporter.setFar(this.depthFar);
		HostState.Pose p = HostState.frame();
		Entity player = Minecraft.getInstance().player;
		if (p != null && p.drive() && player != null) {
			// the host renders from exactly this camera (first or third person), and puts its player at the eyes
			Camera self = (Camera) (Object) this;
			Vec3 cam = self.position();
			Vec3 eye = player.getEyePosition(deltaTracker.getGameTimeDeltaPartialTick(true));
			Passthrough.events.accept(String.format(Locale.ROOT,
				"{\"t\":\"mcpos\",\"eye\":[%.4f,%.4f,%.4f],\"cam\":[%.4f,%.4f,%.4f],\"rot\":[%.3f,%.3f],\"fov\":%.3f,\"tn\":%d}",
				eye.x, eye.y, eye.z, cam.x, cam.y, cam.z, self.yRot(), self.xRot(), lastFov > 0.0F ? lastFov : p.fov(), System.nanoTime()));
		}
	}
}
