package dev.neighborcraft.client.mixin;

import dev.neighborcraft.Passthrough;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft's attack landing on Hello Neighbor's world (a host block: HN's furniture and loose objects are host blocks
 * from their own geometry) goes to the host as a punch, with the hit point, the attack's direction, its charge, the
 * held weapon's attack damage (fist 1, diamond sword 7) and a sprint: the host pushes the physics object there.
 * Minecraft decides the hit (its reach, its ray: nothing through walls), once per attack (startAttack, not the
 * held-button repeats).
 */
@Mixin(Minecraft.class)
abstract class MinecraftAttackMixin {
	@Shadow
	public LocalPlayer player;

	@Shadow
	public ClientLevel level;

	@Shadow
	public HitResult hitResult;

	@Shadow
	protected int missTime;

	@Inject(method = "startAttack", at = @At("HEAD"))
	private void passthrough$punchHost(final CallbackInfoReturnable<Boolean> cir) {
		if (this.missTime > 0 || this.player == null || this.level == null || this.player.isSpectator()
			|| !(this.hitResult instanceof BlockHitResult hit) || hit.getType() != HitResult.Type.BLOCK
			|| !this.level.getBlockState(hit.getBlockPos()).is(Passthrough.HOST)) {
			return;
		}

		Vec3 at = hit.getLocation();
		Vec3 dir = this.player.getViewVector(1.0F);
		Passthrough.events.accept(String.format(Locale.ROOT,
			"{\"t\":\"punch\",\"p\":[%.4f,%.4f,%.4f],\"d\":[%.4f,%.4f,%.4f],\"s\":%.3f,\"dmg\":%.2f,\"sprint\":%b}",
			at.x, at.y, at.z, dir.x, dir.y, dir.z, this.player.getAttackStrengthScale(0.5F),
			this.player.getAttributeValue(Attributes.ATTACK_DAMAGE), this.player.isSprinting()));
	}
}
