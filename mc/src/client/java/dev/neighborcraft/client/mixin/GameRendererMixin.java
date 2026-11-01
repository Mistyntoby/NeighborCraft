package dev.neighborcraft.client.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import dev.neighborcraft.Passthrough;
import com.mojang.blaze3d.vertex.PoseStack;
import dev.neighborcraft.client.FrameExporter;
import dev.neighborcraft.client.PlayerSync;
import net.minecraft.client.Minecraft;
import com.mojang.renderpearl.api.textures.GpuTextureView;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import net.minecraft.client.renderer.state.level.PlayerRenderState;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyArg;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * No sky while a host is attached (the host's sky shows through), no view bob on the world (only on the hand), and
 * the frame is split into world and overlay layers.
 */
@Mixin(GameRenderer.class)
abstract class GameRendererMixin {
	@Shadow @Final private RenderTarget mainRenderTarget;

	@Shadow
	private void bobView(final CameraRenderState state, final PoseStack poseStack) {
	}

	/** The world isn't bobbed while attached: its camera has to stay exactly on the host's. */
	@Redirect(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;bobView(Lnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/blaze3d/vertex/PoseStack;)V")
	)
	private void passthrough$noWorldBob(final GameRenderer self, final CameraRenderState state, final PoseStack poseStack) {
		if (!Passthrough.active) {
			this.bobView(state, poseStack);
		}
	}

	/** The hand bobs as it does when walking in Minecraft, from the host's movement. */
	@Inject(method = "bobView", at = @At("HEAD"))
	private void passthrough$handBob(final CameraRenderState state, final PoseStack poseStack, final CallbackInfo ci) {
		if (Passthrough.active && !Passthrough.driving && state.entityRenderState != null) {
			float[] b = PlayerSync.handBob(Minecraft.getInstance().getDeltaTracker().getGameTimeDeltaPartialTick(true));
			state.entityRenderState.backwardsInterpolatedWalkDistance = b[0];
			state.entityRenderState.bob = b[1];
		}
	}

	@ModifyArg(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/LevelRenderer;render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V"),
		index = 5
	)
	private boolean passthrough$noSky(final boolean shouldRenderSky) {
		return shouldRenderSky && !Passthrough.active;
	}

	@Inject(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render3dHud(Lnet/minecraft/client/renderer/state/level/CameraRenderState;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/OptionsRenderState;Z)V")
	)
	private void passthrough$captureWorld(final CallbackInfo ci) {
		FrameExporter.captureWorld(this.mainRenderTarget);
	}

	@Inject(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render3dHud(Lnet/minecraft/client/renderer/state/level/CameraRenderState;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/OptionsRenderState;Z)V", shift = At.Shift.AFTER)
	)
	private void passthrough$captureHand(final CallbackInfo ci) {
		FrameExporter.captureHand(this.mainRenderTarget);
	}

	/** The hand's projection FOV, for where the carrying fist is on screen (CarryHand). */
	@Inject(method = "renderItemInHand", at = @At("HEAD"))
	private void passthrough$hudFov(final CameraRenderState camera, final PlayerRenderState player, final GpuTextureView depth, final CallbackInfo ci) {
		dev.neighborcraft.client.CarryHand.hudFov = camera.hudFov;
	}

	/**
	 * Before the frame: the Neighbor's entity exactly where HN's Neighbor is now, not where the server last put it and the
	 * client then eased it to (a few ticks behind: the fire on him trailed behind him, the user, 2026-11-01).
	 */
	@Inject(method = "render", at = @At("HEAD"))
	private void passthrough$neighbor(final CallbackInfo ci) {
		net.minecraft.client.multiplayer.ClientLevel level = net.minecraft.client.Minecraft.getInstance().level;
		if (level != null) {
			for (net.minecraft.world.entity.Entity e : level.entitiesForRendering()) {
				if (e instanceof dev.neighborcraft.Neighbor) {
					dev.neighborcraft.Neighbor.snap(e);
				}
			}
		}
	}

	@Inject(method = "render", at = @At("TAIL"))
	private void passthrough$captureOverlay(final CallbackInfo ci) {
		FrameExporter.captureOverlay(this.mainRenderTarget);
		// after the frame is drawn: posing the models again for the shadow boxes can't disturb what was drawn
		dev.neighborcraft.client.ShadowExport.frame();
	}
}
