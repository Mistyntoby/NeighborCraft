package dev.neighborcraft.client;

import com.google.gson.JsonObject;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.client.mixin.KeyMappingAccessor;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import com.mojang.blaze3d.platform.Window;
import net.minecraft.client.gui.screens.PauseScreen;
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
				double gx = m.get("x").getAsDouble() * window.getGuiScaledWidth() / Math.max(1, window.getScreenWidth());
				double gy = m.get("y").getAsDouble() * window.getGuiScaledHeight() / Math.max(1, window.getScreenHeight());
				switch (m.get("t").getAsString()) {
					case "mouse" -> {
						if (heldButton >= 0) {
							screen.mouseDragged(new MouseButtonEvent(gx, gy, new MouseButtonInfo(heldButton, 0)), gx - lastX, gy - lastY);
						}

						screen.mouseMoved(gx, gy);
					}
					case "click" -> {
						int button = m.get("b").getAsInt();
						MouseButtonEvent event = new MouseButtonEvent(gx, gy, new MouseButtonInfo(button, 0));
						if (m.get("down").getAsBoolean()) {
							heldButton = button;
							screen.mouseClicked(event, false);
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
