package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.NativeBlocks;
import net.minecraft.client.renderer.block.BlockAndTintGetter;
import net.minecraft.client.renderer.block.BlockQuadOutput;
import net.minecraft.client.renderer.block.ModelBlockRenderer;
import net.minecraft.client.renderer.block.dispatch.BlockStateModel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Blocks Hello Neighbor draws itself (NativeBlocks) are left out of Minecraft's chunk meshes: no duplicate in its picture.
 * At the start of ModelBlockRenderer.tesselateBlock (a redirect of its call in SectionCompiler.compile found no target in
 * game and crashed Minecraft at start, 2026-10-13 22:50). require = 0: should this ever fail to apply, Minecraft still
 * starts, and blocks are simply all Minecraft's.
 */
@Mixin(ModelBlockRenderer.class)
abstract class SectionCompilerMixin {
	@Inject(method = "tesselateBlock", at = @At("HEAD"), cancellable = true, require = 0)
	private void passthrough$native(final BlockQuadOutput out, final float x, final float y, final float z, final BlockAndTintGetter level,
		final BlockPos pos, final BlockState state, final BlockStateModel model, final long seed, final CallbackInfo ci) {
		NativeBlocks.hooked();
		if (NativeBlocks.handle(pos, state, model)) {
			ci.cancel();
		}
	}
}
