package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.NativeBlocks;
import net.minecraft.client.renderer.block.BlockAndTintGetter;
import net.minecraft.client.renderer.block.BlockQuadOutput;
import net.minecraft.client.renderer.block.ModelBlockRenderer;
import net.minecraft.client.renderer.block.dispatch.BlockStateModel;
import net.minecraft.client.renderer.chunk.SectionCompiler;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/** Blocks Hello Neighbor draws itself (NativeBlocks) are left out of Minecraft's chunk meshes: no duplicate in its picture. */
@Mixin(SectionCompiler.class)
abstract class SectionCompilerMixin {
	@Redirect(method = "compile", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/renderer/block/ModelBlockRenderer;tesselateBlock(Lnet/minecraft/client/renderer/block/BlockQuadOutput;FFFLnet/minecraft/client/renderer/block/BlockAndTintGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/client/renderer/block/dispatch/BlockStateModel;J)V"))
	private void passthrough$native(final ModelBlockRenderer renderer, final BlockQuadOutput out, final float x, final float y, final float z,
		final BlockAndTintGetter level, final BlockPos pos, final BlockState state, final BlockStateModel model, final long seed) {
		if (!NativeBlocks.handle(pos, state, model)) {
			renderer.tesselateBlock(out, x, y, z, level, pos, state, model, seed);
		}
	}
}
