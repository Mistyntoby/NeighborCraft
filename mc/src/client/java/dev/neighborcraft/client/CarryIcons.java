package dev.neighborcraft.client;

import com.mojang.blaze3d.platform.NativeImage;
import dev.neighborcraft.Carry;
import dev.neighborcraft.Passthrough;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.texture.DynamicTexture;
import net.minecraft.resources.Identifier;
import net.minecraft.world.item.ItemStack;

/**
 * The icons of Hello Neighbor's objects in Minecraft's hotbar and inventory: one source for both (and for every
 * screen that draws items), keyed by the object's class, as the token's data has it. HN sends a picture of the object
 * when it is picked up ({"t":"icon","key":class,"file":path}, a scene capture of the object alone: Radiance .hdr from
 * UE 4.20's ExportRenderTarget, or PNG); until then, or if HN couldn't make one, a fallback drawn here: a crate in the
 * class's own colour with a pattern from its name, so different kinds of objects never look alike and an occupied
 * slot never looks empty.
 */
public final class CarryIcons {
	private CarryIcons() {
	}

	private static final Map<String, Identifier> PICTURES = new HashMap<>();
	private static final Map<String, Identifier> FALLBACKS = new HashMap<>();
	private static final Map<Identifier, Integer> SIZES = new HashMap<>();

	/** The width (= height) of an icon's texture: 16 for the fallbacks. */
	public static int size(final Identifier icon) {
		return SIZES.getOrDefault(icon, 16);
	}

	/** The icon to draw for a token (its picture, else its fallback), or null for other items. */
	public static Identifier icon(final ItemStack stack) {
		if (stack.isEmpty() || !stack.is(Passthrough.HN_OBJECT)) {
			return null;
		}

		String key = Carry.data(stack).getStringOr("cls", "");
		Identifier picture = PICTURES.get(key);
		return picture != null ? picture : FALLBACKS.computeIfAbsent(key, CarryIcons::fallback);
	}

	/** A picture from HN (render thread). */
	public static void load(final String key, final String file) {
		try {
			byte[] bytes = Files.readAllBytes(Path.of(file));
			NativeImage image = isRadiance(bytes) ? radiance(bytes) : NativeImage.read(bytes);
			Identifier id = Identifier.fromNamespaceAndPath("neighborcraft", "hn_icon/" + Integer.toHexString(key.hashCode()));
			Minecraft.getInstance().getTextureManager().register(id, new DynamicTexture(() -> "neighborcraft icon " + key, image));
			PICTURES.put(key, id);
			SIZES.put(id, image.getWidth());
			Passthrough.LOG.info("carry: icon for {} ({}x{}) from {}", key, image.getWidth(), image.getHeight(), file);
		} catch (IOException | RuntimeException e) {
			Passthrough.LOG.warn("carry: no icon for {} from {} ({}): the fallback stays", key, file, e.toString());
		}
	}

	private static boolean isRadiance(final byte[] b) {
		String head = new String(b, 0, Math.min(10, b.length), StandardCharsets.US_ASCII);
		return head.startsWith("#?RADIANCE") || head.startsWith("#?RGBE");
	}

	/**
	 * A Radiance RGBE file as an image: tone values 0..1 to 0..255; the capture shows only the object, on black, so
	 * black is transparent.
	 */
	private static NativeImage radiance(final byte[] b) throws IOException {
		int i = 0;
		int width = 0, height = 0;
		// header lines, an empty line, then "-Y h +X w"
		while (true) {
			int end = i;
			while (end < b.length && b[end] != '\n') {
				end++;
			}

			if (end >= b.length) {
				throw new IOException("no size line");
			}

			String line = new String(b, i, end - i, StandardCharsets.US_ASCII).trim();
			i = end + 1;
			if (line.startsWith("-Y ")) {
				String[] parts = line.split("\\s+");
				height = Integer.parseInt(parts[1]);
				width = Integer.parseInt(parts[3]);
				break;
			}
		}

		NativeImage image = new NativeImage(width, height, true);
		byte[] scan = new byte[width * 4];
		for (int y = 0; y < height; y++) {
			if (width >= 8 && width < 32768 && b[i] == 2 && b[i + 1] == 2 && (b[i + 2] & 0x80) == 0) {
				// new run-length encoding: four planes
				i += 4;
				for (int plane = 0; plane < 4; plane++) {
					int x = 0;
					while (x < width) {
						int count = b[i++] & 0xFF;
						if (count > 128) {
							count -= 128;
							byte value = b[i++];
							for (int k = 0; k < count; k++) {
								scan[(x++) * 4 + plane] = value;
							}
						} else {
							for (int k = 0; k < count; k++) {
								scan[(x++) * 4 + plane] = b[i++];
							}
						}
					}
				}
			} else {
				System.arraycopy(b, i, scan, 0, width * 4);
				i += width * 4;
			}

			for (int x = 0; x < width; x++) {
				int e = scan[x * 4 + 3] & 0xFF;
				float f = e == 0 ? 0.0F : (float) Math.scalb(1.0, e - 136);
				int r = tone((scan[x * 4] & 0xFF) * f), g = tone((scan[x * 4 + 1] & 0xFF) * f), bl = tone((scan[x * 4 + 2] & 0xFF) * f);
				int a = r + g + bl < 6 ? 0 : 255;
				image.setPixel(x, y, a << 24 | r << 16 | g << 8 | bl);
			}
		}

		return image;
	}

	private static int tone(final float v) {
		return Math.max(0, Math.min(255, Math.round((float) Math.pow(Math.max(0.0F, v), 1.0 / 2.2) * 255.0F)));
	}

	/** A 3x5 pixel font: 15 bits per character, rows top to bottom, left to right. */
	private static final Map<Character, Integer> GLYPHS = new HashMap<>();

	static {
		String[] rows = {
			"A111101111101101", "B110101110101110", "C111100100100111", "D110101101101110", "E111100110100111",
			"F111100110100100", "G111100101101111", "H101101111101101", "I111010010010111", "J001001001101111",
			"K101101110101101", "L100100100100111", "M101111111101101", "N111101101101101", "O111101101101111",
			"P111101111100100", "Q111101101111001", "R111101110101101", "S111100111001111", "T111010010010010",
			"U101101101101111", "V101101101101010", "W101101111111101", "X101101010101101", "Y101101010010010",
			"Z111001010100111", "0111101101101111", "1010110010010111", "2111001111100111", "3111001111001111",
			"4101101111001001", "5111100111001111", "6111100111101111", "7111001001001001", "8111101111101111",
			"9111101111001111"};
		for (String r : rows) {
			GLYPHS.put(r.charAt(0), Integer.parseInt(r.substring(1), 2));
		}
	}

	private static int hsb(final float hue, final float s, final float v) {
		float h = (hue - (float) Math.floor(hue)) * 6.0F, f = h - (float) Math.floor(h);
		float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
		float[] rgb = switch ((int) h) {
			case 0 -> new float[] {v, t, p};
			case 1 -> new float[] {q, v, p};
			case 2 -> new float[] {p, v, t};
			case 3 -> new float[] {p, q, v};
			case 4 -> new float[] {t, p, v};
			default -> new float[] {v, p, q};
		};
		return Math.round(rgb[0] * 255) << 16 | Math.round(rgb[1] * 255) << 8 | Math.round(rgb[2] * 255);
	}

	/** A crate in the class's colour, with a stripe pattern from its name (16x16). */
	private static Identifier fallback(final String key) {
		// mixed (murmur3's finaliser): String.hashCode of similar class names (BP_Mug_C, BP_Vase_C) lies close together,
		// and their hues did too (three greens in tools/itemtest.py)
		int h = key.hashCode();
		h ^= h >>> 16;
		h *= 0x85EBCA6B;
		h ^= h >>> 13;
		h *= 0xC2B2AE35;
		h ^= h >>> 16;
		float hue = ((h >>> 8) & 0xFFFF) / 65536.0F;
		int base = hsb(hue, 0.55F, 0.85F), dark = hsb(hue, 0.65F, 0.45F);
		NativeImage image = new NativeImage(16, 16, true);
		for (int y = 1; y < 15; y++) {
			for (int x = 1; x < 15; x++) {
				boolean edge = x == 1 || y == 1 || x == 14 || y == 14;
				boolean stripe = ((h >>> ((x + y) & 15)) & 1) != 0 && (x + y) % 4 == 0;
				image.setPixel(x, y, 0xFF000000 | (edge || stripe ? dark : base));
			}
		}

		// and the first letter of its name (BP_Vase3_C: V), white with a dark edge, in the middle
		String shortName = key.substring(Math.max(key.lastIndexOf('.'), key.lastIndexOf('/')) + 1).replaceFirst("^BP_", "");
		char letter = shortName.isEmpty() ? '?' : Character.toUpperCase(shortName.charAt(0));
		int glyph = GLYPHS.getOrDefault(letter, 0x7FFF);
		for (int gy = 0; gy < 5; gy++) {
			for (int gx = 0; gx < 3; gx++) {
				if ((glyph >> (14 - (gy * 3 + gx)) & 1) != 0) {
					for (int sy = 0; sy < 2; sy++) {
						for (int sx = 0; sx < 2; sx++) {
							image.setPixel(5 + gx * 2 + sx, 3 + gy * 2 + sy, 0xFFFFFFFF);
						}
					}
				}
			}
		}

		Identifier id = Identifier.fromNamespaceAndPath("neighborcraft", "hn_fallback/" + Integer.toHexString(h).toLowerCase(Locale.ROOT));
		Minecraft.getInstance().getTextureManager().register(id, new DynamicTexture(() -> "neighborcraft fallback " + key, image));
		return id;
	}
}
