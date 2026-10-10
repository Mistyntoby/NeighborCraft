package dev.neighborcraft.client;

import com.google.gson.JsonObject;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.client.mixin.KeyMappingAccessor;
import dev.neighborcraft.client.mixin.MouseHandlerAccessor;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.MouseHandler;
import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.gui.screens.inventory.AbstractContainerScreen;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.client.input.MouseButtonInfo;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.item.component.SwingAnimation;
import net.minecraft.world.entity.player.Inventory;
import org.lwjgl.sdl.SDLVideo;

/** Host input, applied on the client thread: the host window has the focus, so Minecraft never sees these itself. */
final class ClientInput {
	/** The mouse button held over an open screen (-1: none), and where the cursor was, for drags. */
	private static int heldButton = -1;
	private static double lastX, lastY;
	private static int clicksLogged;
	/** The last press over a screen (button, time), for Minecraft's double click (collecting a stack). */
	private static int lastPressButton = -1;
	private static long lastPressTime;

	/**
	 * The link's buttons (0 left, 1 right, 2 middle, as the host reads them) as Minecraft's. Minecraft 26 takes its
	 * mouse from SDL: left is 1, middle 2, right 3 (InputConstants.MOUSE_BUTTON_*). Passing the host's numbers through
	 * made a left click button 0, which no screen handles (it picked nothing up), and a right click button 1, which
	 * screens take as the left button (it picked the whole stack up).
	 */
	private static int minecraftButton(final int linkButton) {
		return switch (linkButton) {
			case 0 -> InputConstants.MOUSE_BUTTON_LEFT;
			case 1 -> InputConstants.MOUSE_BUTTON_RIGHT;
			case 2 -> InputConstants.MOUSE_BUTTON_MIDDLE;
			default -> linkButton + 1;
		};
	}

	private ClientInput() {
	}

	static void handle(final Minecraft minecraft, final JsonObject m) {
		LocalPlayer player = minecraft.player;
		switch (m.get("t").getAsString()) {
			case "key" -> {
				String k = m.get("k").getAsString();
				boolean down = !m.has("down") || m.get("down").getAsBoolean();
				if (k.equals("escape")) {
					if (down && minecraft.gui.screen() != null) {
						minecraft.gui.screen().onClose();
					}

					return;
				}

				KeyMapping key = switch (k) {
					case "use" -> minecraft.options.keyUse;
					case "attack" -> minecraft.options.keyAttack;
					case "pick" -> minecraft.options.keyPickItem;
					case "inventory" -> minecraft.options.keyInventory;
					case "drop" -> minecraft.options.keyDrop;
					case "swap" -> minecraft.options.keySwapOffhand;
					case "perspective" -> minecraft.options.keyTogglePerspective;
					default -> null;
				};
				if (k.equals("attack") && down && player != null
					&& BuiltInRegistries.ITEM.getKey(player.getMainHandItem().getItem()).getPath().endsWith("_sword")) {
					// a sword swing: the host hits what's in front of Steve in its own world
					Passthrough.events.accept("{\"t\":\"melee\"}");
				}

				if (key != null) {
					if (down && !key.isDown()) {
						KeyMappingAccessor access = (KeyMappingAccessor)key;
						access.passthrough$setClickCount(access.passthrough$getClickCount() + 1);
					}

					key.setDown(down);
				}
			}
			case "mouse", "click", "wheel" -> {
				// a Minecraft screen (inventory) is open over HN: HN's cursor, in HN's window pixels, drives it
				Screen screen = minecraft.gui.screen();
				if (screen == null || !m.has("x")) {
					break;
				}

				Window window = minecraft.getWindow();
				double px = m.get("x").getAsDouble() * window.getScreenWidth(), py = m.get("y").getAsDouble() * window.getScreenHeight();
				((MouseHandlerAccessor) minecraft.mouseHandler).passthrough$setXpos(px);
				((MouseHandlerAccessor) minecraft.mouseHandler).passthrough$setYpos(py);
				double gx = MouseHandler.getScaledXPos(window, px), gy = MouseHandler.getScaledYPos(window, py);
				switch (m.get("t").getAsString()) {
					case "mouse" -> {
						if (heldButton >= 0) {
							screen.mouseDragged(new MouseButtonEvent(gx, gy, new MouseButtonInfo(heldButton, 0)), gx - lastX, gy - lastY);
						}

						screen.mouseMoved(gx, gy);
					}
					case "click" -> {
						int button = minecraftButton(m.get("b").getAsInt());
						MouseButtonEvent event = new MouseButtonEvent(gx, gy, new MouseButtonInfo(button, 0));
						if (m.get("down").getAsBoolean()) {
							heldButton = button;
							// a double click as Minecraft's mouse handler judges it: the same button again within its threshold
							long now = System.currentTimeMillis();
							boolean doubleClick = button == lastPressButton && now - lastPressTime < MouseHandler.DOUBLE_CLICK_THRESHOLD_MS;
							lastPressButton = button;
							lastPressTime = now;
							boolean handled = screen.mouseClicked(event, doubleClick);
							if (clicksLogged++ < 40) {
								Passthrough.LOG.info("screen click: button {} at gui {} {} (window {} x {}) on {}: {}, carrying {}", button, Math.round(gx),
									Math.round(gy), window.getGuiScaledWidth(), window.getGuiScaledHeight(), screen.getClass().getSimpleName(),
									handled ? "handled" : "not handled",
									screen instanceof AbstractContainerScreen<?> container ? container.getMenu().getCarried() : "-");
							}
						} else {
							heldButton = -1;
							screen.mouseReleased(event);
						}
					}
					default -> screen.mouseScrolled(gx, gy, 0.0, m.get("d").getAsDouble());
				}

				lastX = gx;
				lastY = gy;
			}
			case "swing" -> {
				// the player used one of HN's objects (a door): the hand swings as when using a block in Minecraft
				if (player != null) {
					player.swing(InteractionHand.MAIN_HAND, SwingAnimation.DEFAULT, true);
				}
			}
			case "pause" -> {
				// Hello Neighbor paused (or left gameplay): Minecraft's world stops too (single player pauses while the
				// pause screen is open), and goes on when HN does
				boolean on = m.get("on").getAsBoolean();
				if (on && minecraft.gui.screen() == null) {
					minecraft.pauseGame(false);
				} else if (!on && minecraft.gui.screen() instanceof PauseScreen) {
					minecraft.gui.setScreen(null);
				}
			}
			case "move" -> {
				// the movement keys held in the host's window, every host frame (bits: 1 forward, 2 back, 4 left,
				// 8 right, 16 jump, 32 sneak, 64 sprint): Minecraft's own movement reads them like real keys
				int bits = m.get("m").getAsInt();
				minecraft.options.keyUp.setDown((bits & 1) != 0);
				minecraft.options.keyDown.setDown((bits & 2) != 0);
				minecraft.options.keyLeft.setDown((bits & 4) != 0);
				minecraft.options.keyRight.setDown((bits & 8) != 0);
				minecraft.options.keyJump.setDown((bits & 16) != 0);
				minecraft.options.keyShift.setDown((bits & 32) != 0);
				minecraft.options.keySprint.setDown((bits & 64) != 0);
			}
			case "slot" -> {
				if (player != null) {
					player.getInventory().setSelectedSlot(Math.clamp(m.get("n").getAsInt(), 0, Inventory.getSelectionSize() - 1));
				}
			}
			case "scroll" -> {
				if (player != null) {
					Inventory inventory = player.getInventory();
					int size = Inventory.getSelectionSize();
					inventory.setSelectedSlot(Math.floorMod(inventory.getSelectedSlot() - m.get("d").getAsInt(), size));
				}
			}
			case "hud" -> {
				if (minecraft.gui.hud.isHidden() != m.get("hidden").getAsBoolean()) {
					minecraft.gui.hud.toggle();
				}
			}
			case "view" -> {
				// match the host's picture exactly: un-minimize/un-maximize first (resizing a maximized window is ignored)
				int w = m.get("w").getAsInt(), h = m.get("h").getAsInt();
				long handle = minecraft.getWindow().handle();
				SDLVideo.SDL_RestoreWindow(handle);
				minecraft.getWindow().setWindowed(w, h);
				SDLVideo.SDL_SetWindowSize(handle, w, h);
				SDLVideo.SDL_SyncWindow(handle);
			}
			default -> {
			}
		}
	}
}
