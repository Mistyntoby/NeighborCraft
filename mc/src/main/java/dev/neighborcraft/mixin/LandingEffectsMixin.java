package dev.neighborcraft.mixin;

import dev.neighborcraft.Passthrough;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

/** Landing particles on Hello Neighbor's ground: its surface material, as for sprinting (SurfaceEffectsMixin). */
@Mixin(LivingEntity.class)
abstract class LandingEffectsMixin {
	@ModifyVariable(method = "checkFallDamage", at = @At("HEAD"), argsOnly = true)
	private BlockState passthrough$landingSurface(final BlockState state) {
		return Passthrough.surface((Entity) (Object) this, state);
	}
}
