package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.PlayerSync;
import net.minecraft.client.player.LocalPlayer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(LocalPlayer.class)
abstract class LocalPlayerMixin {
	/** After the old position was saved for interpolation and before movement is sent to the server. */
	@Inject(method = "tick", at = @At("HEAD"))
	private void passthrough$followHost(final CallbackInfo ci) {
		PlayerSync.tick((LocalPlayer) (Object) this);
	}

	@Inject(method = "sendPosition", at = @At("HEAD"))
	private void passthrough$hostGround(final CallbackInfo ci) {
		PlayerSync.beforeSend((LocalPlayer) (Object) this);
	}
}
