package dev.neighborcraft.mixin;

import dev.neighborcraft.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.Redirect;

/**
 * Sprint particles and footsteps on Hello Neighbor's ground: the host block under the player is invisible (no
 * particle texture, a generic sound), so the block standing for HN's surface material there is used instead.
 */
@Mixin(Entity.class)
abstract class SurfaceEffectsMixin {
	@Redirect(method = "spawnSprintParticle", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
	private BlockState passthrough$sprintSurface(final Level level, final BlockPos pos) {
		return Passthrough.surface((Entity) (Object) this, level.getBlockState(pos));
	}

	@ModifyVariable(method = "playStepSound", at = @At("HEAD"), argsOnly = true)
	private BlockState passthrough$stepSurface(final BlockState state) {
		return Passthrough.surface((Entity) (Object) this, state);
	}
}
