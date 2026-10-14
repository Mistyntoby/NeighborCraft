package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.NativeSteve;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.entity.LivingEntityRenderer;
import net.minecraft.client.renderer.entity.state.LivingEntityRenderState;
import net.minecraft.world.entity.LivingEntity;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * With HN drawing Steve (NativeSteve), Minecraft doesn't draw his body: his render state says invisible (and invisible
 * to the player: no see-through ghost). His held item and armour layers still draw, as for an invisible player.
 * ShadowExport builds its own render states meanwhile (NativeSteve.exporting) and still gets the model.
 */
@Mixin(LivingEntityRenderer.class)
abstract class LivingEntityRendererMixin {
	@Inject(method = "extractRenderState(Lnet/minecraft/world/entity/LivingEntity;Lnet/minecraft/client/renderer/entity/state/LivingEntityRenderState;F)V",
		at = @At("TAIL"), require = 0)
	private void passthrough$nativeSteve(final LivingEntity entity, final LivingEntityRenderState state, final float partial, final CallbackInfo ci) {
		if (NativeSteve.enabled && !NativeSteve.exporting && entity == Minecraft.getInstance().player) {
			state.isInvisible = true;
			state.isInvisibleToPlayer = true;
		}
	}
}
