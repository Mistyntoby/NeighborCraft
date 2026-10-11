package dev.neighborcraft.client.mixin;

import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.world.level.CardinalLighting;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Blocks without Minecraft's fixed face shading (top 1.0, north/south 0.8, east/west 0.6, bottom 0.5, whatever the
 * sun does): every face as bright as its texture. The compositor lights each face by Hello Neighbor's sun instead
 * (NeighborCraft.fx: the face's normal from Minecraft's depth against the sun's direction), so the face the sun
 * shines on is the bright one (the user: the sunlit front of a wall was as dark as its shaded side).
 */
@Mixin(ClientLevel.class)
abstract class ClientLevelLightingMixin {
	private static final CardinalLighting FLAT = new CardinalLighting(1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F);

	@Inject(method = "cardinalLighting", at = @At("HEAD"), cancellable = true)
	private void passthrough$flat(final CallbackInfoReturnable<CardinalLighting> cir) {
		cir.setReturnValue(FLAT);
	}
}
