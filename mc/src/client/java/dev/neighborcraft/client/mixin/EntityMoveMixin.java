package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.CollisionRecorder;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** The local player's own movement (what it tried, what collision let it do) for the collision recorder. */
@Mixin(Entity.class)
abstract class EntityMoveMixin {
	@Inject(method = "move", at = @At("HEAD"))
	private void passthrough$beforeMove(final MoverType type, final Vec3 movement, final CallbackInfo ci) {
		if (type == MoverType.SELF && (Object) this instanceof LocalPlayer player) {
			CollisionRecorder.beforeMove(player, movement);
		}
	}

	@Inject(method = "move", at = @At("RETURN"))
	private void passthrough$afterMove(final MoverType type, final Vec3 movement, final CallbackInfo ci) {
		if (type == MoverType.SELF && (Object) this instanceof LocalPlayer player) {
			CollisionRecorder.afterMove(player);
		}
	}
}
