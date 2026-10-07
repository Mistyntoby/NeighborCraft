package dev.neighborcraft.mixin;

import dev.neighborcraft.Passthrough;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.player.Player;
import org.objectweb.asm.Opcodes;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * When the host moves the player, neither side should collide it with blocks: host terrain arrives as barriers
 * that the player can end up inside, and the server would otherwise reject those moves. When Minecraft moves the
 * player (driving), it collides normally: the barriers are the host's world.
 */
@Mixin(Player.class)
abstract class PlayerMixin {
	@Inject(method = "tick", at = @At(value = "FIELD", target = "Lnet/minecraft/world/entity/player/Player;noPhysics:Z", opcode = Opcodes.PUTFIELD, shift = At.Shift.AFTER))
	private void passthrough$ghost(final CallbackInfo ci) {
		if (Passthrough.active && !Passthrough.driving) {
			((Entity) (Object) this).noPhysics = true;
		}
	}
}
