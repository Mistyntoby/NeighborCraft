package dev.neighborcraft.client.mixin;

import net.minecraft.client.Camera;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The third-person camera's distance, smoothed: Minecraft pulls the camera in wherever its ray meets a block, and HN's
 * world is many small collision blocks (grass bumps, steps, door frames), so the distance jumped every few frames
 * (the user: "all bouncy"). It comes in quickly when something is in the way and goes back out slowly.
 */
@Mixin(Camera.class)
abstract class CameraZoomMixin {
	private float passthrough$zoom = -1.0F;

	@Inject(method = "getMaxZoom", at = @At("RETURN"), cancellable = true)
	private void passthrough$smooth(final float max, final CallbackInfoReturnable<Float> cir) {
		float target = cir.getReturnValueF();
		if (this.passthrough$zoom < 0.0F) {
			this.passthrough$zoom = target;
		}

		float rate = target < this.passthrough$zoom ? 0.35F : 0.06F;
		this.passthrough$zoom += (target - this.passthrough$zoom) * rate;
		cir.setReturnValue(Math.min(this.passthrough$zoom, max));
	}
}
