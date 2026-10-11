package dev.neighborcraft.client;

import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.resources.Identifier;

/**
 * Pickup progress and throw charge, drawn where Minecraft draws the experience bar and the way it draws it (its own
 * background and progress sprites), in place of it while one of them runs. HN sends each as its own kind
 * ({"t":"bar","k":"pickup"|"throw","v":0..1}, v < 0 ends it): the two never share a value, a new kind replaces the
 * other, and a value HN stops sending runs out after STALE_MS, so a cancelled pickup or a throw that never came can't
 * leave a stale bar. It used to be text in the action bar ("Throw [####......]").
 */
public final class CarryBar {
	private CarryBar() {
	}

	private static final Identifier BACKGROUND = Identifier.withDefaultNamespace("hud/experience_bar_background");
	private static final Identifier PROGRESS = Identifier.withDefaultNamespace("hud/experience_bar_progress");
	private static final long STALE_MS = 400;

	private static String kind = "";
	private static float value;
	private static long at;

	/** A value from HN (render thread). */
	public static void set(final String newKind, final float newValue) {
		if (newValue < 0.0F) {
			if (newKind.equals(kind)) {
				kind = "";
			}

			return;
		}

		kind = newKind;
		value = Math.max(0.0F, Math.min(1.0F, newValue));
		at = System.currentTimeMillis();
	}

	/** Whether a bar is showing now (else Minecraft's experience bar is drawn as usual). */
	public static boolean active() {
		if (!kind.isEmpty() && System.currentTimeMillis() - at > STALE_MS) {
			kind = "";
		}

		return !kind.isEmpty();
	}

	/** Which one shows (a test tool reads it): "" for none. */
	public static String kind() {
		return active() ? kind : "";
	}

	public static float value() {
		return value;
	}

	/** The bar at the experience bar's place: 182x5 at the bottom middle, 29 px above the bottom (ContextualBar). */
	public static void draw(final GuiGraphicsExtractor graphics) {
		int left = (graphics.guiWidth() - 182) / 2, top = graphics.guiHeight() - 24 - 5;
		graphics.blitSprite(RenderPipelines.GUI_TEXTURED, BACKGROUND, left, top, 182, 5);
		int filled = (int) (value * 183.0F);
		if (filled > 0) {
			graphics.blitSprite(RenderPipelines.GUI_TEXTURED, PROGRESS, 182, 5, 0, 0, left, top, Math.min(filled, 182), 5);
		}
	}
}
