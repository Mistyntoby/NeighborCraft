package dev.neighborcraft.mixin;

import dev.neighborcraft.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.state.BlockState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * HN's roofs are host blocks, which let the sky light through (lighting inside HN's house is HN's), so to Minecraft a
 * skeleton under the Neighbor's roof stood in the open and burned (the user, 2026-11-01). Any of HN's geometry above a
 * mob's head is a roof against the sun.
 */
@Mixin(Mob.class)
abstract class MobMixin {
	private static final int ROOF_SEARCH = 48;

	@Inject(method = "isSunBurnTick", at = @At("HEAD"), cancellable = true)
	private void passthrough$hnRoof(final CallbackInfoReturnable<Boolean> cir) {
		Mob self = (Mob) (Object) this;
		Level level = self.level();
		BlockPos.MutableBlockPos pos = BlockPos.containing(self.getX(), self.getEyeY(), self.getZ()).mutable();
		for (int i = 0; i < ROOF_SEARCH && pos.getY() < level.getMaxY(); i++, pos.move(0, 1, 0)) {
			BlockState state = level.getBlockState(pos);
			if (state.is(Passthrough.HOST) && !state.getCollisionShape(level, pos).isEmpty()) {
				cir.setReturnValue(false);
				return;
			}
		}
	}
}
