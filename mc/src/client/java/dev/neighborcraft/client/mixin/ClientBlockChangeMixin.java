package dev.neighborcraft.client.mixin;

import dev.neighborcraft.Passthrough;
import java.util.Locale;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * A block that stops being a full opaque cube on the client (broken, the client predicts it at once) tells HN right
 * away: HN hides that block's drawn stand-in (its receiver) before the server's change arrives a tick later. In the
 * frames between, Minecraft's picture already had no block there, and HN's grey stand-in showed (the user: "the
 * white box for just a split second").
 */
@Mixin(Level.class)
abstract class ClientBlockChangeMixin {
	@Inject(method = "setBlock(Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;II)Z", at = @At("HEAD"))
	private void passthrough$tellHost(final BlockPos pos, final BlockState state, final int flags, final int recursion, final CallbackInfoReturnable<Boolean> cir) {
		if (!((Object) this instanceof ClientLevel level) || !Passthrough.driving || state.isSolidRender()) {
			return;
		}

		if (level.getBlockState(pos).isSolidRender()) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"unreceive\",\"p\":[%d,%d,%d]}", pos.getX(), pos.getY(), pos.getZ()));
		}
	}
}
