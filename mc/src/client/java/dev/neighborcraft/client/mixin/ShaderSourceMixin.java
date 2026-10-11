package dev.neighborcraft.client.mixin;

import com.google.common.collect.ImmutableMap;
import com.mojang.renderpearl.api.pipeline.ShaderType;
import dev.neighborcraft.Passthrough;
import java.io.IOException;
import net.minecraft.client.renderer.ShaderManager;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.Resource;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/**
 * Opaque blocks write an opaque alpha. Hello Neighbor blends Minecraft's world layer by its alpha (premultiplied:
 * world.rgb + host * (1 - world.a)), but Minecraft's solid and cutout block passes don't blend: they write whatever
 * alpha the texture has where they draw (mip-filtered at a distance), and in Minecraft that is never seen.
 * tools/rendertest.py, reading the very frame the compositor gets: cutout blocks (leaves, glass, iron bars, trapdoors,
 * cobwebs) 17.9% of their drawn pixels under alpha 250 (down to 0-32), so Hello Neighbor showed through them; solid
 * blocks 0.1%. Minecraft's own terrain and block shaders, as loaded from its resources, get one line after their final
 * colour: alpha 1 for the solid passes (no ALPHA_CUTOUT) and for what the cutout passes keep (ALPHA_CUTOUT 0.5);
 * translucent passes (ALPHA_CUTOUT 0.1, blended) and the order-independent transparency paths are untouched.
 * (SkyCraft, MIT, chasmlol/SkyCraft, avoids this by sending block meshes with a per-vertex cutout/translucent flag
 * instead of a picture; here the picture is kept and its alpha made to mean coverage.) No Mojang file is shipped: the
 * vanilla source is patched as it is read.
 */
@Mixin(ShaderManager.class)
abstract class ShaderSourceMixin {
	private static final String ANCHOR = "fragColor = calculateFinalColor(color);";
	private static final String OPAQUE = ANCHOR + "\n"
		+ "    #ifndef OIT_ACCUMULATE\n"
		+ "    #ifndef ALPHA_CUTOUT\n"
		+ "    fragColor.a = 1.0; // NeighborCraft: solid pass, opaque\n"
		+ "    #else\n"
		+ "    if (ALPHA_CUTOUT > 0.3) fragColor.a = 1.0; // NeighborCraft: what the cutout pass keeps is opaque\n"
		+ "    #endif\n"
		+ "    #endif";

	@Redirect(method = "loadShader", at = @At(value = "INVOKE", target = "Lnet/minecraft/server/packs/resources/Resource;readAllAsString()Ljava/lang/String;"))
	private static String passthrough$opaqueAlpha(final Resource resource, final Identifier file, final Resource same, final ShaderType type,
		final ImmutableMap.Builder<?, String> out) throws IOException {
		String source = resource.readAllAsString();
		String path = file.getPath();
		if (path.endsWith("core/terrain.fsh") || path.endsWith("core/block.fsh")) {
			if (source.contains(ANCHOR) && !source.contains("NeighborCraft:")) {
				source = source.replace(ANCHOR, OPAQUE);
				Passthrough.LOG.info("shaders: {} writes opaque alpha for solid and cutout blocks", file);
			} else {
				Passthrough.LOG.warn("shaders: {} not patched (its final colour line has changed): cutout blocks may show the host through", file);
			}
		}

		return source;
	}
}
