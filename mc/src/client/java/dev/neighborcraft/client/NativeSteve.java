package dev.neighborcraft.client;

import com.mojang.blaze3d.platform.NativeImage;
import dev.neighborcraft.Passthrough;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.IdentityHashMap;
import java.util.Locale;
import java.util.Map;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.resources.Identifier;
import org.joml.Matrix4f;
import org.joml.Vector3f;

/**
 * The local player's model drawn by Hello Neighbor (native rendering, phase 5: HANDOFF.md). Minecraft's picture arrives
 * 1-3 of its frames after HN's camera, a different number each frame, and Steve, moving with the camera, wobbled against
 * the world and left a silhouette behind (the user). Each face of his model's cubes goes to HN every frame as a plane:
 * its texture (the skin's rectangle for that face, written once) and where its corners are. HN draws them lit and
 * shadowed like its own world; Minecraft doesn't draw his body meanwhile (LivingEntityRendererMixin), his held item it does.
 */
public final class NativeSteve {
	/** Set by HN ("nsteve" message). */
	public static volatile boolean enabled;
	/** While ShadowExport builds render states (Minecraft's own Steve stays visible to it). */
	public static boolean exporting;

	private static final Map<ModelPart.Cube, Integer> CUBES = new IdentityHashMap<>();
	private static final Map<String, Integer> TEXTURES = new java.util.HashMap<>();

	private NativeSteve() {
	}

	/**
	 * A face of a model cube for HN, in `out` (render thread): texture id, centre, U edge, V edge (each a full edge, from
	 * the corner of the texture's top-left), outward normal, all in Minecraft's world, relative to `origin`.
	 */
	static void face(final StringBuilder out, final Identifier skinId, final NativeImage skin, final Matrix4f m, final ModelPart.Cube cube,
		final int index, final ModelPart.Polygon polygon, final net.minecraft.world.phys.Vec3 origin, final boolean outer) {
		ModelPart.Vertex[] v = polygon.vertices();
		if (v.length != 4 || skin == null) {
			return;
		}

		float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
		for (ModelPart.Vertex p : v) {
			u0 = Math.min(u0, p.u());
			v0 = Math.min(v0, p.v());
			u1 = Math.max(u1, p.u());
			v1 = Math.max(v1, p.v());
		}

		// the corners at the texture's top-left, top-right and bottom-left
		ModelPart.Vertex a = null, b = null, c = null;
		for (ModelPart.Vertex p : v) {
			boolean left = Math.abs(p.u() - u0) < 1e-5F, top = Math.abs(p.v() - v0) < 1e-5F;
			if (left && top) {
				a = p;
			} else if (!left && top) {
				b = p;
			} else if (left && !top) {
				c = p;
			}
		}

		if (a == null || b == null || c == null) {
			return;
		}

		int id = texture(skinId, skin, cube, index, u0, v0, u1, v1, outer);
		if (id < 0) {
			return;
		}

		Vector3f pa = m.transformPosition(new Vector3f(a.worldX(), a.worldY(), a.worldZ()));
		Vector3f pb = m.transformPosition(new Vector3f(b.worldX(), b.worldY(), b.worldZ()));
		Vector3f pc = m.transformPosition(new Vector3f(c.worldX(), c.worldY(), c.worldZ()));
		Vector3f eu = new Vector3f(pb).sub(pa), ev = new Vector3f(pc).sub(pa);
		Vector3f centre = new Vector3f(pa).add(new Vector3f(eu).mul(0.5F)).add(new Vector3f(ev).mul(0.5F));
		Vector3f n = m.transformDirection(new Vector3f(polygon.normal())).normalize();
		if (eu.lengthSquared() < 1e-10F || ev.lengthSquared() < 1e-10F) {
			return;
		}

		if (!out.isEmpty()) {
			out.append(',');
		}

		out.append(id);
		for (float f : new float[] {(float) (centre.x + origin.x), (float) (centre.y + origin.y), (float) (centre.z + origin.z), eu.x, eu.y, eu.z,
			ev.x, ev.y, ev.z, n.x, n.y, n.z}) {
			out.append(',').append(String.format(Locale.ROOT, "%.4f", f));
		}
	}

	/** The id of a face's texture, written for HN on first use (the skin's rectangle, 8x with nearest pixels); -1: none. */
	private static int texture(final Identifier skinId, final NativeImage skin, final ModelPart.Cube cube, final int index, final float u0,
		final float v0, final float u1, final float v1, final boolean outer) {
		int cubeId = CUBES.computeIfAbsent(cube, k -> CUBES.size());
		// an outer layer's texture keeps its transparency ("_a": HN's masked material for it)
		String name = ("steve_" + skinId.toString() + "_" + cubeId + "_" + index).replace(':', '_').replace('/', '_').replace('.', '_') + (outer ? "_a" : "");
		Integer known = TEXTURES.get(name);
		if (known != null) {
			return known;
		}

		int w = skin.getWidth(), h = skin.getHeight();
		int x0 = Math.round(u0 * w), y0 = Math.round(v0 * h), x1 = Math.round(u1 * w), y1 = Math.round(v1 * h);
		if (x1 <= x0 || y1 <= y0) {
			TEXTURES.put(name, -1);
			return -1;
		}

		if (outer) {
			// a face the skin leaves fully clear (an empty outer layer there): nothing to draw
			boolean any = false;
			for (int y = y0; y < y1 && !any; y++) {
				for (int x = x0; x < x1 && !any; x++) {
					any = (skin.getPixel(x, y) >>> 24) >= 128;
				}
			}

			if (!any) {
				TEXTURES.put(name, -1);
				return -1;
			}
		}

		int k = 8;
		try {
			Path file = NativeBlocks.dir().resolve(name + ".png");
			Files.createDirectories(file.getParent());
			try (NativeImage face = new NativeImage((x1 - x0) * k, (y1 - y0) * k, false)) {
				for (int y = 0; y < (y1 - y0) * k; y++) {
					for (int x = 0; x < (x1 - x0) * k; x++) {
						int c = skin.getPixel(x0 + x / k, y0 + y / k);
						// opaque for the body (HN's base_m is); an outer layer cut at half alpha (base_alpha_m's mask)
						face.setPixel(x, y, outer ? ((c >>> 24) >= 128 ? c | 0xFF000000 : c & 0x00FFFFFF) : c | 0xFF000000);
					}
				}

				face.writeToFile(file);
			}
		} catch (Exception ex) {
			Passthrough.LOG.warn("native Steve: face texture {} not written ({})", name, ex.toString());
			TEXTURES.put(name, -1);
			return -1;
		}

		int id = TEXTURES.size();
		TEXTURES.put(name, id);
		Passthrough.events.accept("{\"t\":\"stex\",\"id\":" + id + ",\"name\":\"" + name + "\"}");
		return id;
	}

	/** HN asks again for every face texture (a new level: its materials went with the old one). */
	public static void resend() {
		for (Map.Entry<String, Integer> e : TEXTURES.entrySet()) {
			if (e.getValue() >= 0) {
				Passthrough.events.accept("{\"t\":\"stex\",\"id\":" + e.getValue() + ",\"name\":\"" + e.getKey() + "\"}");
			}
		}
	}
}
