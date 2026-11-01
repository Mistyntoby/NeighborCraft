package dev.neighborcraft.client;

import com.mojang.blaze3d.platform.NativeImage;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.client.mixin.MultiPlayerGameModeAccessor;
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
import net.minecraft.resources.Identifier;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.BlockAndLightGetter;

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
	/** What HN was last told per native block (its faces' light levels): told again when that changes. */
	private static final Map<BlockPos, String> TOLD_FACES = new ConcurrentHashMap<>();

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
	/** The first decisions, logged (the 22:55 session placed blocks and none became native; nothing said why). */
	private static final java.util.concurrent.atomic.AtomicInteger TOLD = new java.util.concurrent.atomic.AtomicInteger();

	private static boolean say(final String why, final BlockState state, final boolean result) {
		if (limit > 0 && TOLD.getAndIncrement() < 12) {
			Passthrough.LOG.info("native blocks: {} -> {} ({})", state, result ? "HN's" : "Minecraft's", why);
		}

		return result;
	}

	public static boolean handle(final BlockPos pos, final BlockState state, final BlockStateModel model, final BlockAndLightGetter level) {
		if (limit <= 0) {
			return false;
		}

		if (!state.isSolidRender()) {
			return say("not a solid cube", state, false);
		}

		BlockPos key = pos.immutable();
		if (!NATIVE.contains(key) && NATIVE.size() >= limit) {
			return say("the limit (" + limit + ") reached", state, false);
		}

		TextureAtlasSprite sprite = singleSprite(model, state.getSeed(pos));
		if (sprite == null) {
			return say("faces differ, tinted or not six", state, false);
		}

		String name = sprite.contents().name().toString().replace(':', '_').replace('/', '_');
		if (!write(name, sprite)) {
			return say("texture not written", state, false);
		}

		say("native: " + name, state, true);

		// each face HN draws, with the light level of the air in front of it (Minecraft's own: sky and torch light). HN's
		// world can't darken a space its runtime blocks enclose (its sky and bounce light are precomputed from its own
		// level): a box built around Steve stayed lit inside (the user). -1: a face against another solid block, not drawn
		String tex = texture(key, name);
		StringBuilder faces = new StringBuilder();
		for (Direction d : Direction.values()) {
			BlockPos n = key.relative(d);
			int light = level.getBlockState(n).isSolidRender() ? -1 : level.getRawBrightness(n, 0);
			if (light >= 0 && !write(tex, sprite, light)) {
				light = -1;
			}

			faces.append(faces.isEmpty() ? "" : ",").append(light);
		}

		NATIVE.add(key);
		SPRITES.put(key, sprite);
		String f = faces.toString();
		if (!f.equals(TOLD_FACES.put(key, f))) {
			OUT.add("{\"t\":\"nblock\",\"p\":[" + key.getX() + "," + key.getY() + "," + key.getZ() + "],\"tex\":\"" + tex + "\",\"f\":[" + f + "]}");
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

	/**
	 * Minecraft's brightness for a light level (its light curve without the "brightness" option: 15 -> 1, 8 -> 0.22, 0 ->
	 * nearly black).
	 */
	static float lightFactor(final int level) {
		float f = level / 15.0F;
		return Math.max(0.03F, f / (4.0F - 3.0F * f));
	}

	/**
	 * The sprite at a light level ("<name>_l<level>"), written once. A cracked name ("<sprite>_c<stage>", see texture())
	 * gets Minecraft's crack on it as its crumbling blend draws it (colour = 2 x crack x block where the crack isn't clear).
	 */
	private static boolean write(final String name, final TextureAtlasSprite sprite, final int light) {
		String file = name + "_l" + light;
		if (WRITTEN.containsKey(file)) {
			return true;
		}

		int cut = name.lastIndexOf("_c");
		NativeImage crack = null;
		if (cut >= 0 && name.substring(cut + 2).matches("\\d")) {
			crack = crack(Integer.parseInt(name.substring(cut + 2)));
			if (crack == null) {
				return false;
			}
		}

		float k = lightFactor(light);
		try {
			NativeImage image = ((SpriteContentsAccessor) sprite.contents()).passthrough$originalImage();
			if (image == null) {
				return false;
			}

			int w = sprite.contents().width(), h = sprite.contents().height(), s = 8;
			Path path = dir().resolve(file + ".png");
			Files.createDirectories(path.getParent());
			try (NativeImage big = new NativeImage(w * s, h * s, false)) {
				for (int y = 0; y < h * s; y++) {
					for (int x = 0; x < w * s; x++) {
						int c = image.getPixel(x / s, y / s);
						float m = 1.0F;
						if (crack != null) {
							int d = crack.getPixel(x * crack.getWidth() / (w * s), y * crack.getHeight() / (h * s));
							if ((d >>> 24) >= 26) { // the crumbling shader discards alpha < 0.1
								m = 2.0F * (d >> 16 & 0xFF) / 255.0F; // grey: one channel will do
							}
						}

						int r = Math.min(255, Math.round((c >> 16 & 0xFF) * k * m)), g = Math.min(255, Math.round((c >> 8 & 0xFF) * k * m)),
							b = Math.min(255, Math.round((c & 0xFF) * k * m));
						big.setPixel(x, y, 0xFF000000 | r << 16 | g << 8 | b);
					}
				}

				big.writeToFile(path);
			}

			WRITTEN.put(file, Boolean.TRUE);
			return true;
		} catch (Exception ex) {
			Passthrough.LOG.warn("native blocks: texture {} not written ({})", file, ex.toString());
			return false;
		}
	}

	/** The sprite as a PNG, scaled up 8x with nearest pixels (HN filters textures smoothly: 16 px would blur). */
	private static boolean write(final String name, final TextureAtlasSprite sprite) {
		if (WRITTEN.containsKey(name)) {
			return true;
		}

		try {
			NativeImage image = ((SpriteContentsAccessor) sprite.contents()).passthrough$originalImage();
			if (image == null) {
				Passthrough.LOG.warn("native blocks: {} has no image in memory", name);
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

	/**
	 * The block Steve is breaking and its crack stage (0-9). Minecraft's crack (LevelRenderer.submitBlockDestroyAnimation)
	 * draws over the block without writing depth: on a block HN draws, the compositor put HN's block in front of it. Handing
	 * the block back to Minecraft while it broke showed it in Minecraft's own look (the user: "looks really bad"), so the
	 * crack is baked into HN's face textures instead ("<name>_c<stage>_l<light>").
	 */
	private static volatile BlockPos breaking;
	private static volatile int breakingStage = -1;
	/** Each native block's sprite, for its cracked textures. */
	private static final Map<BlockPos, TextureAtlasSprite> SPRITES = new ConcurrentHashMap<>();
	/** Minecraft's destroy_stage_0..9 textures, read on first use. */
	private static final NativeImage[] CRACKS = new NativeImage[10];

	/** The texture name HN is given for a block: its sprite's, cracked while Steve breaks it. */
	private static String texture(final BlockPos pos, final String name) {
		int stage = breakingStage;
		return stage >= 0 && pos.equals(breaking) ? name + "_c" + stage : name;
	}

	/** Tells HN a native block's textures again (its crack changed). */
	private static void retell(final Minecraft minecraft, final BlockPos pos) {
		TextureAtlasSprite sprite = SPRITES.get(pos);
		String faces = TOLD_FACES.get(pos);
		if (sprite == null || faces == null || !NATIVE.contains(pos) || !minecraft.level.getBlockState(pos).isSolidRender()) {
			return;
		}

		String name = sprite.contents().name().toString().replace(':', '_').replace('/', '_');
		String tex = texture(pos, name);
		for (String light : faces.split(",")) {
			int l = Integer.parseInt(light);
			if (l >= 0 && !write(tex, sprite, l)) {
				return;
			}
		}

		OUT.add("{\"t\":\"nblock\",\"p\":[" + pos.getX() + "," + pos.getY() + "," + pos.getZ() + "],\"tex\":\"" + tex + "\",\"f\":[" + faces + "]}");
	}

	/** Minecraft's crack texture for a stage, or null. */
	private static synchronized NativeImage crack(final int stage) {
		if (CRACKS[stage] == null) {
			Identifier id = Identifier.withDefaultNamespace("textures/block/destroy_stage_" + stage + ".png");
			try (java.io.InputStream in = Minecraft.getInstance().getResourceManager().open(id)) {
				CRACKS[stage] = NativeImage.read(in);
			} catch (Exception ex) {
				Passthrough.LOG.warn("native blocks: crack {} not read ({})", id, ex.toString());
				return null;
			}
		}

		return CRACKS[stage];
	}

	/** Client thread, every tick: blocks gone (no longer the eligible block) leave the set; queued messages go to HN. */
	public static void tick(final Minecraft minecraft) {
		BlockPos now = null;
		int stage = -1;
		if (limit > 0 && minecraft.gameMode != null && minecraft.gameMode.isDestroying()) {
			now = ((MultiPlayerGameModeAccessor) minecraft.gameMode).passthrough$destroyBlockPos();
			stage = Math.min(minecraft.gameMode.getDestroyStage(), 9);
		}

		if (stage < 0) {
			now = null;
		}

		if (minecraft.level != null && (!java.util.Objects.equals(now, breaking) || stage != breakingStage)) {
			BlockPos was = breaking;
			breaking = now == null ? null : now.immutable();
			breakingStage = stage;
			if (was != null && !was.equals(breaking)) {
				retell(minecraft, was); // uncracked again
			}

			if (breaking != null) {
				retell(minecraft, breaking);
			}
		}

		if (minecraft.level != null && !NATIVE.isEmpty()) {
			for (BlockPos pos : NATIVE) {
				if (!minecraft.level.getBlockState(pos).isSolidRender()) {
					NATIVE.remove(pos);
					TOLD_FACES.remove(pos);
					SPRITES.remove(pos);
					OUT.add("{\"t\":\"nblock\",\"p\":[" + pos.getX() + "," + pos.getY() + "," + pos.getZ() + "],\"tex\":\"\"}");
				}
			}
		}

		for (String m; (m = OUT.poll()) != null;) {
			Passthrough.events.accept(m);
		}
	}

	private static volatile boolean hookedOnce;

	/** The tesselateBlock hook runs (SectionCompilerMixin): logged once, so a hook that didn't apply shows by its absence. */
	public static void hooked() {
		if (!hookedOnce) {
			hookedOnce = true;
			Passthrough.LOG.info("native blocks: Minecraft's block meshing is hooked");
		}
	}

	/** HN turned native blocks on or off: every chunk is meshed again under the new rule. */
	public static void setLimit(final Minecraft minecraft, final int n) {
		if (n == limit) {
			return;
		}

		limit = n;
		NATIVE.clear();
		TOLD_FACES.clear();
		SPRITES.clear();
		Passthrough.LOG.info("native blocks: up to {} drawn by Hello Neighbor", n);
		if (minecraft.levelRenderer != null) {
			minecraft.levelRenderer.resetLevelRenderData(); // every chunk meshed again
		}
	}
}
