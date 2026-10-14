package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.NativeBlocks;
import net.minecraft.client.renderer.block.BlockAndTintGetter;
import net.minecraft.client.renderer.block.dispatch.BlockStateModel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Pseudo;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Blocks Hello Neighbor draws itself (NativeBlocks), left out where chunks are really meshed: Fabric API's renderer
 * (fabric-renderer-api SectionCompilerMixin) hands every block to Indigo's AltModelBlockRendererImpl.tesselateBlock, so
 * Minecraft's own ModelBlockRenderer.tesselateBlock never ran (the 22:58 session: no block hooked). The vanilla hook
 * (SectionCompilerMixin) stays for a game without Indigo. require = 0: a failure leaves the blocks Minecraft's.
 */
@Pseudo
@Mixin(targets = "net.fabricmc.fabric.impl.client.indigo.renderer.render.AltModelBlockRendererImpl", remap = false)
abstract class IndigoBlockMixin {
	@Inject(method = "tesselateBlock", at = @At("HEAD"), cancellable = true, require = 0, remap = false)
	private void passthrough$native(final net.fabricmc.fabric.api.client.renderer.v1.mesh.QuadEmitter emitter, final float x, final float y,
		final float z, final BlockAndTintGetter level, final BlockPos pos, final BlockState state, final BlockStateModel model, final long seed,
		final CallbackInfo ci) {
		NativeBlocks.hooked();
		if (NativeBlocks.handle(pos, state, model, level)) {
			ci.cancel();
		}
	}
}
