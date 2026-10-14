package dev.neighborcraft.client;

import com.mojang.blaze3d.platform.NativeImage;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.client.mixin.SpriteContentsAccessor;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Queue;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.block.dispatch.BlockStateModel;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.state.BlockState;

/**
 * Blocks Hello Neighbor draws itself (native rendering, phase 2 of the migration: see HANDOFF.md). Minecraft's picture
 * and Hello Neighbor's stand-ins were two renderers that never agreed to the pixel (a trailing clone, light bleeding in
 * from around a block); a block HN draws is lit, shadowed, depth-tested and anti-aliased by HN like its own furniture.
 * <p>
 * Eligible: a full opaque cube whose six faces use one sprite and no tint (stone, cobblestone, planks, dirt): HN draws it
 * as Unreal's 1 m cube with that texture. Minecraft then skips it in its chunk meshes ({@code SectionCompilerMixin}), and
 * its picture has nothing there: what it still supplies (Steve, the hand, the HUD, particles, other blocks) is depth-tested
 * against HN's block as before. At most {@link #limit} blocks while the prototype is checked (HN says how many).
 */
public final class NativeBlocks {
	/** Set by HN ("native" message): 0 off, else how many blocks may be native. */
	public static volatile int limit;
	/** Native blocks now (chunk compile threads add, the client thread reads). */
	private static final Set<BlockPos> NATIVE = ConcurrentHashMap.newKeySet();
	/** Messages for HN, sent from the client thread (compile threads only queue them). */
	private static final Queue<String> OUT = new ConcurrentLinkedQueue<>();
	/** Sprites already written for HN. */
	private static final Map<String, Boolean> WRITTEN = new ConcurrentHashMap<>();

	private NativeBlocks() {
	}

	/** Where HN reads the textures: %LOCALAPPDATA%\NeighborCraft\blocks (bridge.cpp native_texture_dir). */
	static Path dir() {
		String base = System.getenv("LOCALAPPDATA");
		return Path.of(base != null ? base : System.getProperty("java.io.tmpdir"), "NeighborCraft", "blocks");
	}

	/**
	 * From a chunk compile thread, before Minecraft meshes a block: true if HN draws it (Minecraft then skips it). Sends
	 * HN the block and its texture.
	 */
	public static boolean handle(final BlockPos pos, final BlockState state, final BlockStateModel model) {
		if (limit <= 0 || !state.isSolidRender()) {
			return false;
		}

		BlockPos key = pos.immutable();
		if (!NATIVE.contains(key) && NATIVE.size() >= limit) {
			return false;
		}

		TextureAtlasSprite sprite = singleSprite(model, state.getSeed(pos));
		if (sprite == null) {
			return false;
		}

		String name = sprite.contents().name().toString().replace(':', '_').replace('/', '_');
		if (!write(name, sprite)) {
			return false;
		}

		if (NATIVE.add(key)) {
			OUT.add("{\"t\":\"nblock\",\"p\":[" + key.getX() + "," + key.getY() + "," + key.getZ() + "],\"tex\":\"" + name + "\"}");
		}

		return true;
	}

	/** The one sprite all six faces use, with no tint; null if the faces differ, are tinted or missing. */
	private static TextureAtlasSprite singleSprite(final BlockStateModel model, final long seed) {
		List<BlockStateModelPart> parts = new ArrayList<>();
		model.collectParts(RandomSource.create(seed), parts);
		TextureAtlasSprite one = null;
		for (Direction d : Direction.values()) {
			boolean found = false;
			for (BlockStateModelPart part : parts) {
				for (BakedQuad q : part.getQuads(d)) {
					if (q.materialInfo().isTinted()) {
						return null;
					}

					TextureAtlasSprite s = q.materialInfo().sprite();
					if (one == null) {
						one = s;
					} else if (s != one) {
						return null;
					}

					found = true;
				}
			}

			if (!found) {
				return null; // not a full cube's six faces
			}
		}

		return one;
	}

	/** The sprite as a PNG, scaled up 8x with nearest pixels (HN filters textures smoothly: 16 px would blur). */
	private static boolean write(final String name, final TextureAtlasSprite sprite) {
		if (WRITTEN.containsKey(name)) {
			return true;
		}

		try {
			NativeImage image = ((SpriteContentsAccessor) sprite.contents()).passthrough$originalImage();
			if (image == null) {
				return false;
			}

			int w = sprite.contents().width(), h = sprite.contents().height(), k = 8;
			Path file = dir().resolve(name + ".png");
			Files.createDirectories(file.getParent());
			try (NativeImage big = new NativeImage(w * k, h * k, false)) {
				for (int y = 0; y < h * k; y++) {
					for (int x = 0; x < w * k; x++) {
						big.setPixel(x, y, image.getPixel(x / k, y / k) | 0xFF000000);
					}
				}

				big.writeToFile(file);
			}

			WRITTEN.put(name, Boolean.TRUE);
			return true;
		} catch (Exception ex) {
			Passthrough.LOG.warn("native blocks: texture {} not written ({})", name, ex.toString());
			return false;
		}
	}

	/** Client thread, every tick: blocks gone (no longer the eligible block) leave the set; queued messages go to HN. */
	public static void tick(final Minecraft minecraft) {
		if (minecraft.level != null && !NATIVE.isEmpty()) {
			for (BlockPos pos : NATIVE) {
				if (!minecraft.level.getBlockState(pos).isSolidRender()) {
					NATIVE.remove(pos);
					OUT.add("{\"t\":\"nblock\",\"p\":[" + pos.getX() + "," + pos.getY() + "," + pos.getZ() + "],\"tex\":\"\"}");
				}
			}
		}

		for (String m; (m = OUT.poll()) != null;) {
			Passthrough.events.accept(m);
		}
	}

	/** HN turned native blocks on or off: every chunk is meshed again under the new rule. */
	public static void setLimit(final Minecraft minecraft, final int n) {
		if (n == limit) {
			return;
		}

		limit = n;
		NATIVE.clear();
		Passthrough.LOG.info("native blocks: up to {} drawn by Hello Neighbor", n);
		if (minecraft.levelRenderer != null) {
			minecraft.levelRenderer.resetLevelRenderData(); // every chunk meshed again
		}
	}
}
