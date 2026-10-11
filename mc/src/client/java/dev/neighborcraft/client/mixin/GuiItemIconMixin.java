package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.CarryIcons;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.resources.Identifier;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * Hello Neighbor's objects drawn as their own icons wherever Minecraft draws an item in a GUI (hotbar, inventory, the
 * stack on the cursor): every one of those goes through this method, so they always agree (see CarryIcons).
 */
@Mixin(GuiGraphicsExtractor.class)
abstract class GuiItemIconMixin {
	@Inject(method = "item(Lnet/minecraft/world/entity/LivingEntity;Lnet/minecraft/world/level/Level;Lnet/minecraft/world/item/ItemStack;III)V",
		at = @At("HEAD"), cancellable = true)
	private void passthrough$hnIcon(final LivingEntity owner, final Level level, final ItemStack stack, final int x, final int y, final int seed,
		final CallbackInfo ci) {
		Identifier icon = CarryIcons.icon(stack);
		if (icon != null) {
			int size = CarryIcons.size(icon);
			((GuiGraphicsExtractor) (Object) this).blit(RenderPipelines.GUI_TEXTURED, icon, x, y, 0.0F, 0.0F, 16, 16, size, size, size, size);
			ci.cancel();
		}
	}
}
