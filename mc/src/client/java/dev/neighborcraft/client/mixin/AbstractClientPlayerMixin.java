package dev.neighborcraft.client.mixin;

import dev.neighborcraft.Passthrough;
import java.util.UUID;
import net.minecraft.client.player.AbstractClientPlayer;
import net.minecraft.client.resources.DefaultPlayerSkin;
import net.minecraft.world.entity.player.PlayerSkin;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** While a host is attached the player wears the skin set up for NeighborCraft (OwnSkin), else classic Steve (the wide-armed default). */
@Mixin(AbstractClientPlayer.class)
abstract class AbstractClientPlayerMixin {
	/** DefaultPlayerSkin picks floorMod(uuid.hashCode(), 18); index 15 is entity/player/wide/steve. */
	private static final UUID CLASSIC_STEVE = new UUID(0L, 15L);

	@Inject(method = "getSkin", at = @At("HEAD"), cancellable = true)
	private void passthrough$steve(final CallbackInfoReturnable<PlayerSkin> cir) {
		if (Passthrough.active && !Boolean.getBoolean("neighborcraft.ownSkin")) {
			// the player's own skin if one is set up (OwnSkin: skin.png or a username), else classic Steve
			PlayerSkin own = (Object) this == net.minecraft.client.Minecraft.getInstance().player ? dev.neighborcraft.client.OwnSkin.skin() : null;
			if (own != null) {
				cir.setReturnValue(own);
			} else if (net.fabricmc.loader.api.FabricLoader.getInstance().isDevelopmentEnvironment()) {
				// the development client has no account (a made-up name, a random default skin): classic Steve. A real
				// launcher's signed-in player keeps the account's own skin
				cir.setReturnValue(DefaultPlayerSkin.get(CLASSIC_STEVE));
			}
		}
	}
}
