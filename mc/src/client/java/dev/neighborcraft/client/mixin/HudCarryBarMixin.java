package dev.neighborcraft.client.mixin;

import dev.neighborcraft.client.CarryBar;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.gui.Font;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.Hud;
import net.minecraft.client.gui.contextualbar.ContextualBar;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

/**
 * Pickup progress and throw charge in the experience bar's place (CarryBar): while one runs, it is drawn instead of
 * the contextual bar Minecraft has chosen (the experience bar, the locator bar, a mount's jump bar) and the level
 * number above it, so no two bars overlap; otherwise Minecraft's own.
 */
@Mixin(Hud.class)
abstract class HudCarryBarMixin {
	@Redirect(method = "extractHotbarAndDecorations", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/gui/contextualbar/ContextualBar;extractBackground(Lnet/minecraft/client/gui/GuiGraphicsExtractor;Lnet/minecraft/client/DeltaTracker;)V"))
	private void passthrough$barBackground(final ContextualBar bar, final GuiGraphicsExtractor graphics, final DeltaTracker delta) {
		if (CarryBar.active()) {
			CarryBar.draw(graphics);
		} else {
			bar.extractBackground(graphics, delta);
		}
	}

	@Redirect(method = "extractHotbarAndDecorations", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/gui/contextualbar/ContextualBar;extractRenderState(Lnet/minecraft/client/gui/GuiGraphicsExtractor;Lnet/minecraft/client/DeltaTracker;)V"))
	private void passthrough$barForeground(final ContextualBar bar, final GuiGraphicsExtractor graphics, final DeltaTracker delta) {
		if (!CarryBar.active()) {
			bar.extractRenderState(graphics, delta);
		}
	}

	@Redirect(method = "extractHotbarAndDecorations", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/gui/contextualbar/ContextualBar;extractExperienceLevel(Lnet/minecraft/client/gui/GuiGraphicsExtractor;Lnet/minecraft/client/gui/Font;I)V"))
	private void passthrough$level(final GuiGraphicsExtractor graphics, final Font font, final int level) {
		if (!CarryBar.active()) {
			ContextualBar.extractExperienceLevel(graphics, font, level);
		}
	}
}
