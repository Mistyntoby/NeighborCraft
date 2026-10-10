package dev.neighborcraft.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.client.CollisionRecorder;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.state.level.LevelRenderState;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * No Minecraft outline on Hello Neighbor's own surfaces (host blocks): only Minecraft's blocks get one. With the
 * collision debug view on (F12), the outline shows HN's collision shape under the crosshair as Minecraft has it.
 */
@Mixin(LevelRenderer.class)
abstract class LevelRendererMixin {
	@Inject(method = "submitBlockOutline", at = @At("HEAD"), cancellable = true)
	private void passthrough$noHostOutline(final PoseStack poseStack, final SubmitNodeCollector collector, final LevelRenderState state, final CallbackInfo ci) {
		Minecraft minecraft = Minecraft.getInstance();
		if (!CollisionRecorder.on() && minecraft.hitResult instanceof BlockHitResult hit && minecraft.level != null
			&& minecraft.level.getBlockState(hit.getBlockPos()).is(Passthrough.HOST)) {
			ci.cancel();
		}
	}
}
