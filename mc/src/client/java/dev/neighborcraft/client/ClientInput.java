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
import net.minecraft.client.gui.components.EditBox;
import net.minecraft.client.gui.screens.ChatScreen;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.gui.screens.inventory.AbstractContainerScreen;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.CharacterEvent;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.client.input.MouseButtonInfo;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
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

	/** A text field of the open screen has the keyboard (chat, a sign, the anvil's name). */
	private static boolean typing(final Minecraft minecraft) {
		Screen screen = minecraft.gui.screen();
		return screen instanceof ChatScreen || screen != null && screen.getFocused() instanceof EditBox;
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

				if (k.equals("chat") || k.equals("command")) {
					// T and /, as Minecraft's own keys open it (cheats are on in this world: commands work)
					if (minecraft.gui.screen() == null && player != null) {
						minecraft.gui.setScreen(new ChatScreen(k.equals("command") ? "/" : "", false));
					}

					return;
				}

				if (typing(minecraft)) {
					return; // E, Q, 1-9 are letters in a text field (chat, a sign, the anvil)
				}

				if (k.equals("drop")) {
					// Q: one item; Ctrl+Q: the whole stack. Minecraft's own drop key asks its window whether Ctrl is
					// down, and its hidden window never has the keys: the host says it ("stack"). Never with a screen open
					// (the inventory has its own controls), never from an empty slot (dropItem drops nothing then).
					if (down && player != null && minecraft.gameMode != null && minecraft.gui.screen() == null && !player.isSpectator()
						&& !player.getMainHandItem().isEmpty()) {
						boolean stack = m.has("stack") && m.get("stack").getAsBoolean();
						minecraft.gameMode.dropItem(player, stack);
						player.swing(InteractionHand.MAIN_HAND, SwingAnimation.DEFAULT, true);
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
					if (k.equals("swap") && down) {
						Passthrough.LOG.info("keys: offhand swap from the host (screen {})", minecraft.gui.screen() == null ? "-" : minecraft.gui.screen().getClass().getSimpleName());
					}

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
						// Shift and Ctrl from the host: screens read them from the click (shift-click moves a whole stack)
						int mods = m.has("mod") ? m.get("mod").getAsInt() : 0;
						MouseButtonEvent event = new MouseButtonEvent(gx, gy, new MouseButtonInfo(button, mods));
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
			case "char" -> {
				// typed in HN's window (its WM_CHAR): Minecraft's hidden window never has the keyboard
				Screen screen = minecraft.gui.screen();
				if (screen != null) {
					screen.charTyped(new CharacterEvent(m.get("c").getAsInt()));
				}
			}
			case "keycode" -> {
				// a text field's editing keys (Enter, Backspace, arrows, Tab, Ctrl+A/C/V): SDL's scancode, keycode and modifiers
				Screen screen = minecraft.gui.screen();
				if (screen != null) {
					KeyEvent event = new KeyEvent(m.get("k").getAsInt(), m.get("kc").getAsInt(), m.get("mod").getAsInt());
					if (m.get("down").getAsBoolean()) {
						screen.keyPressed(event);
					} else {
						screen.keyReleased(event);
					}
				}
			}
			case "pickup" -> {
				// HN took one of its objects out of its world for Minecraft's inventory (it waits for the answer)
				dev.neighborcraft.Carry.give(m.get("id").getAsString(), m.get("name").getAsString(), m.get("cls").getAsString(),
					m.has("kg") ? m.get("kg").getAsInt() : 0);
			}
			case "consume" -> dev.neighborcraft.Carry.consume(m.get("id").getAsString());
			case "punched" -> {
				// an attack moved one of HN's objects: Minecraft's attack sound, as for hitting a mob (a charged hit
				// strong, a sprinting one knockback, a quick uncharged swing weak)
				if (player != null && minecraft.level != null) {
					double strength = m.has("s") ? m.get("s").getAsDouble() : 1.0;
					boolean sprint = m.has("sprint") && m.get("sprint").getAsBoolean();
					var sound = sprint && strength > 0.9 ? SoundEvents.PLAYER_ATTACK_KNOCKBACK
						: strength > 0.9 ? SoundEvents.PLAYER_ATTACK_STRONG : SoundEvents.PLAYER_ATTACK_WEAK;
					minecraft.level.playLocalSound(player.getX(), player.getEyeY(), player.getZ(), sound, SoundSource.PLAYERS, 1.0F, 1.0F, false);
				}
			}
			case "icon" -> CarryIcons.load(m.get("key").getAsString(), m.get("file").getAsString());
			case "bar" -> CarryBar.set(m.get("k").getAsString(), m.get("v").getAsFloat());
			case "barq" -> dev.neighborcraft.Passthrough.events.accept(String.format(java.util.Locale.ROOT, "{\"t\":\"barq\",\"k\":\"%s\",\"v\":%.3f}",
				CarryBar.kind(), CarryBar.value())); // a test tool's question: the bar showing now
			case "ungrab" -> minecraft.mouseHandler.releaseMouse(); // a test tool: Minecraft's window as in the game (hidden, the mouse not its own)
			case "inv" -> {
				// a test tool's question: the hotbar (item and count per slot) and the selected slot
				if (player != null) {
					StringBuilder b = new StringBuilder("{\"t\":\"inv\",\"sel\":").append(player.getInventory().getSelectedSlot()).append(",\"slots\":[");
					for (int i = 0; i < 9; i++) {
						var stack = player.getInventory().getItem(i);
						b.append(i == 0 ? "" : ",").append("[\"").append(BuiltInRegistries.ITEM.getKey(stack.getItem())).append("\",").append(stack.getCount()).append(']');
					}

					dev.neighborcraft.Passthrough.events.accept(b.append("]}").toString());
				}
			}
			case "shot" -> {
				// a test tool's screenshot of Minecraft's own frame (tools/handtest.py): run/screenshots/<name>
				String name = m.has("n") ? m.get("n").getAsString() : "neighborcraft-test.png";
				if (m.has("layer") && "hand".equals(m.get("layer").getAsString())) {
					FrameExporter.handShot = name; // the hand is cleared from the frame once captured: taken there
				} else {
					net.minecraft.client.Screenshot.grab(minecraft.gameDirectory, name, minecraft.gameRenderer.mainRenderTarget(), 1,
						message -> dev.neighborcraft.Passthrough.LOG.info("screenshot: {}", message.getString()));
				}
			}
			case "native" -> NativeBlocks.setLimit(minecraft, m.get("n").getAsInt()); // blocks HN draws itself (NativeBlocks)
			case "nsteve" -> {
				// HN draws Steve (NativeSteve); its face textures again for a new level's materials
				NativeSteve.enabled = m.get("on").getAsBoolean();
				dev.neighborcraft.Passthrough.LOG.info("native Steve: {}", NativeSteve.enabled ? "drawn by Hello Neighbor" : "Minecraft's");
				NativeSteve.resend();
			}
			case "hostfps" -> {
				// Minecraft renders at Hello Neighbor's rate (to the option's nearest step of 10: rounded up, HN's 60.2 gave 70):
				// faster, it drew frames HN never showed, and the picture HN showed was several of its frames behind (Steve's
				// trail on turns)
				// twice HN's rate: at exactly HN's rate the two clocks drifted against each other and a frame now and then
				// showed twice or was skipped (Steve juddered as he moved, the user); at twice it there is always a fresh one,
				// and the stand-ins are posed from the frame HN will show (HN select_shadow_frame)
				int want = Math.clamp((int) Math.round(m.get("v").getAsDouble() / 10.0) * 20, 30, 260);
				if (minecraft.options.framerateLimit().get() != want) {
					dev.neighborcraft.Passthrough.LOG.info("frame rate limit {} -> {} (Hello Neighbor's rate)", minecraft.options.framerateLimit().get(), want);
					minecraft.options.framerateLimit().set(want);
				}
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
				if (player != null && !typing(minecraft)) {
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
