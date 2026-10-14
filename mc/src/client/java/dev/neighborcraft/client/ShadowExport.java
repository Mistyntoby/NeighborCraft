package dev.neighborcraft.client;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.math.Axis;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.client.mixin.ItemStackRenderStateAccessor;
import dev.neighborcraft.client.mixin.LayerRenderStateAccessor;
import dev.neighborcraft.client.mixin.LivingEntityRendererInvoker;
import dev.neighborcraft.client.mixin.ModelPartAccessor;
import dev.neighborcraft.client.mixin.ParticleAccessor;
import dev.neighborcraft.client.mixin.ParticleEngineAccessor;
import dev.neighborcraft.client.mixin.ParticleGroupAccessor;
import dev.neighborcraft.client.mixin.SingleQuadParticleAccessor;
import dev.neighborcraft.client.mixin.SpriteContentsAccessor;
import net.minecraft.client.resources.model.sprite.Material;
import org.joml.Vector3fc;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import net.minecraft.client.Minecraft;
import net.minecraft.client.model.ArmedModel;
import net.minecraft.client.model.EntityModel;
import net.minecraft.client.model.geom.ModelPart;
import net.minecraft.client.particle.Particle;
import net.minecraft.client.particle.ParticleGroup;
import net.minecraft.client.particle.SingleQuadParticle;
import net.minecraft.client.renderer.entity.EntityRenderer;
import net.minecraft.client.renderer.entity.ItemEntityRenderer;
import net.minecraft.client.renderer.entity.LivingEntityRenderer;
import net.minecraft.client.renderer.entity.state.ArmedEntityRenderState;
import net.minecraft.client.renderer.entity.state.EntityRenderState;
import net.minecraft.client.renderer.entity.state.ItemEntityRenderState;
import net.minecraft.client.renderer.entity.state.LivingEntityRenderState;
import net.minecraft.client.renderer.item.ItemStackRenderState;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.entity.Pose;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import com.mojang.blaze3d.platform.NativeImage;
import net.minecraft.resources.Identifier;
import java.io.InputStream;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.Map;
import org.joml.Matrix4f;
import org.joml.Vector3f;

/**
 * What Minecraft draws in the world, as boxes for Hello Neighbor's shadows: every frame, each box's centre and three
 * half-axis vectors in world space ({"t":"shadows","b":[cx,cy,cz, ux,uy,uz, vx,vy,vz, wx,wy,wz, ...],"hand":[...]}).
 * HN gives each a hidden, shadow-casting cube of its own (bridge.cpp shadow_boxes_tick), so its lights cast them.
 * "k": per box, 1 if it is a model's real surface (living entities' cubes): HN also draws those, plain grey, so the
 * compositor can read the light HN puts on them (shadow edges across Steve).
 *
 * <p>The boxes are the renderers' own geometry, placed by the renderers' own steps, so they move exactly as drawn:
 * <ul>
 * <li>living entities (Steve too, also in first person where he isn't drawn): LivingEntityRenderer.submit replayed
 * (render offset, sleeping, scale, setupRotations, the flip, the renderer's scale, -1.501), the model's setupAnim
 * with the frame's render state, and every cube of the model's visible parts (the skin's outer layers too: hat,
 * jacket, sleeves and trousers stand out a little, and a receiver must be where the drawn surface is); the items in their hands as ItemInHandLayer places them (the hand, -90 X, 180 Y, (±1, 2, -10)/16), sized
 * by the item model's extents;</li>
 * <li>dropped items as ItemEntityRenderer places them (lifted off their model's bottom, the bob, the spin), sized by
 * their model's extents;</li>
 * <li>other entities: their collision box;</li>
 * <li>particles: the nearest {@value #MAX_PARTICLES}, each a cube of its drawn size.</li>
 * </ul>
 * "hand": Steve's main-hand item frame (origin and unit axes), where HN puts a held object in third person.
 * Only while the host drives; within {@value #RANGE} blocks of the camera; at most {@value #MAX_BOXES} boxes.
 */
public final class ShadowExport {
	private ShadowExport() {
	}

	static final int MAX_BOXES = 400, MAX_PARTICLES = 120;
	static final double RANGE = 32.0;
	private static boolean failedOnce;

	private static final class Out {
		final StringBuilder b = new StringBuilder(16384);
		final StringBuilder kinds = new StringBuilder(512);
		/** Per box: its colour (0xRRGGBB, a model cube's average over its part of the skin), -1 (none: HN's grey), or -2
		 * (a cube the skin leaves fully transparent: Minecraft draws nothing there, HN mustn't either). */
		final StringBuilder colours = new StringBuilder(512);
		/** The skin of the model being exported (null: none readable), and the colour of the next box. */
		NativeImage skin;
		Identifier skinId;
		int colour = -1;
		/** The local player's model faces for HN's native Steve (NativeSteve.face), 13 numbers each. */
		final StringBuilder faces = new StringBuilder(4096);
		int boxes;
		/**
		 * What the next boxes are: 0 shadow only (items, particles, other entities' boxes), 1 a model's real surface
		 * (HN draws it to light it: NeighborCraft.fx receivers), 2 the local player's model (drawn only in third
		 * person: in first person Minecraft doesn't draw him, and HN's grey boxes would show).
		 */
		int surface;
		final Vec3 origin;

		Out(final Vec3 origin) {
			this.origin = origin;
		}

		/** A box: centre and half-axes in the frame of `m` (relative to `origin`). */
		void box(final Matrix4f m, final float cx, final float cy, final float cz, final float hx, final float hy, final float hz) {
			if (boxes >= MAX_BOXES || hx <= 1e-4F && hy <= 1e-4F && hz <= 1e-4F) {
				return;
			}

			Vector3f c = m.transformPosition(new Vector3f(cx, cy, cz));
			Vector3f u = m.transformDirection(new Vector3f(hx, 0, 0));
			Vector3f v = m.transformDirection(new Vector3f(0, hy, 0));
			Vector3f w = m.transformDirection(new Vector3f(0, 0, hz));
			if (boxes > 0) {
				b.append(',');
			}

			num(origin.x + c.x).append(',');
			num(origin.y + c.y).append(',');
			num(origin.z + c.z).append(',');
			num(u.x).append(',');
			num(u.y).append(',');
			num(u.z).append(',');
			num(v.x).append(',');
			num(v.y).append(',');
			num(v.z).append(',');
			num(w.x).append(',');
			num(w.y).append(',');
			num(w.z);
			kinds.append(boxes > 0 ? "," : "").append(surface);
			colours.append(boxes > 0 ? "," : "").append(colour);
			boxes++;
		}

		StringBuilder num(final double v) {
			// three decimals: a millimetre
			long r = Math.round(v * 1000.0);
			if (r < 0) {
				b.append('-');
				r = -r;
			}

			b.append(r / 1000).append('.');
			long f = r % 1000;
			if (f < 100) {
				b.append('0');
			}

			if (f < 10) {
				b.append('0');
			}

			return b.append(f);
		}
	}

	/** Once per frame, after the frame is drawn (GameRendererMixin). */
	public static void frame() {
		Minecraft minecraft = Minecraft.getInstance();
		if (!Passthrough.driving || minecraft.level == null || minecraft.player == null) {
			return;
		}

		try {
			float partial = minecraft.getDeltaTracker().getGameTimeDeltaPartialTick(true);
			Vec3 camera = minecraft.gameRenderer.mainCamera().position();
			Out out = new Out(camera);
			float[] hand = null;
			for (Entity e : minecraft.level.entitiesForRendering()) {
				if (e.distanceToSqr(camera) > RANGE * RANGE || e.isInvisible() && e != minecraft.player) {
					continue;
				}

				float[] h = entity(minecraft, e, partial, out);
				if (e == minecraft.player) {
					hand = h;
				}
			}

			particles(minecraft, partial, out);
			StringBuilder msg = new StringBuilder(out.b.length() + 200).append("{\"t\":\"shadows\",\"b\":[").append(out.b).append("],\"k\":[").append(out.kinds).append(']');
			// the frame these boxes belong to: HN poses its stand-ins from the frame its compositor shows
			msg.append(",\"f\":").append(FrameExporter.lastCaptured());
			msg.append(",\"c\":[").append(out.colours).append(']');
			if (NativeSteve.enabled) {
				msg.append(",\"nf\":[").append(out.faces).append(']');
			}
			if (hand != null) {
				msg.append(",\"hand\":[");
				for (int i = 0; i < hand.length; i++) {
					msg.append(i == 0 ? "" : ",").append(String.format(Locale.ROOT, "%.4f", hand[i] + (i < 3 ? (i == 0 ? camera.x : i == 1 ? camera.y : camera.z) : 0.0)));
				}

				msg.append(']');
			}

			Passthrough.events.accept(msg.append('}').toString());
		} catch (RuntimeException ex) {
			if (!failedOnce) {
				failedOnce = true;
				Passthrough.LOG.warn("shadows: export failed ({}): no shadow boxes this frame", ex.toString(), ex);
			}
		}
	}

	/** One entity's boxes; for a player, its main-hand item frame (origin relative to the camera, then unit axes). */
	@SuppressWarnings({"unchecked", "rawtypes"})
	private static float[] entity(final Minecraft minecraft, final Entity e, final float partial, final Out out) {
		EntityRenderer renderer = minecraft.getEntityRenderDispatcher().getRenderer(e);
		NativeSteve.exporting = true; // Minecraft's own Steve stays visible here (LivingEntityRendererMixin)
		EntityRenderState state;
		try {
			state = renderer.createRenderState(e, partial);
		} finally {
			NativeSteve.exporting = false;
		}
		Vec3 offset = renderer.getRenderOffset(state);
		PoseStack pose = new PoseStack();
		pose.translate(state.x + offset.x - out.origin.x, state.y + offset.y - out.origin.y, state.z + offset.z - out.origin.z);
		if (renderer instanceof LivingEntityRenderer living && state instanceof LivingEntityRenderState s) {
			if (s.isInvisible || s.isInvisibleToPlayer && e != minecraft.player) {
				return null;
			}

			// LivingEntityRenderer.submit, step by step
			if (s.hasPose(Pose.SLEEPING) && s.bedOrientation != null) {
				float f = s.eyeHeight - 0.1F;
				pose.translate(-s.bedOrientation.getStepX() * f, 0.0F, -s.bedOrientation.getStepZ() * f);
			}

			float scale = s.scale;
			pose.scale(scale, scale, scale);
			((LivingEntityRendererInvoker) living).passthrough$setupRotations(s, pose, s.bodyRot, scale);
			pose.scale(-1.0F, -1.0F, 1.0F);
			((LivingEntityRendererInvoker) living).passthrough$scale(s, pose);
			pose.translate(0.0F, -1.501F, 0.0F);
			EntityModel model = living.getModel();
			model.setupAnim(s);
			out.surface = e == minecraft.player ? 2 : 1;
			@SuppressWarnings("unchecked")
			Identifier texture = ((LivingEntityRenderer<?, LivingEntityRenderState, ?>) living).getTextureLocation(s);
			out.skinId = texture;
			out.skin = skin(minecraft, texture);
			part(model.root(), "", pose, out);
			out.surface = 0;
			out.skin = null;
			out.colour = -1;
			float[] hand = null;
			if (model instanceof ArmedModel armed && s instanceof ArmedEntityRenderState armedState) {
				for (HumanoidArm arm : HumanoidArm.values()) {
					ItemStackRenderState item = arm == HumanoidArm.RIGHT ? armedState.rightHandItemState : armedState.leftHandItemState;
					pose.pushPose();
					armed.translateToHand(s, arm, pose);
					pose.rotateDegrees(Axis.XP, -90.0F);
					pose.rotateDegrees(Axis.YP, 180.0F);
					pose.translate((arm == HumanoidArm.LEFT ? -1.0F : 1.0F) / 16.0F, (s.isBaby ? 1.0F : 2.0F) / 16.0F, (s.isBaby ? -4.5F : -10.0F) / 16.0F);
					Matrix4f m = pose.last().pose();
					if (arm == armedState.mainArm) {
						Vector3f o = m.transformPosition(new Vector3f());
						Vector3f x = m.transformDirection(new Vector3f(1, 0, 0)).normalize();
						Vector3f y = m.transformDirection(new Vector3f(0, 1, 0)).normalize();
						Vector3f z = m.transformDirection(new Vector3f(0, 0, 1)).normalize();
						hand = new float[] {o.x, o.y, o.z, x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
					}

					if (!item.isEmpty()) {
						extents(item, m, out);
					}

					pose.popPose();
				}
			}

			return hand;
		}

		if (renderer instanceof ItemEntityRenderer && state instanceof ItemEntityRenderState s && e instanceof ItemEntity) {
			// ItemEntityRenderer.submit: off the model's bottom, the bob, the spin
			if (s.item.isEmpty()) {
				return null;
			}

			AABB box = s.item.getModelBoundingBox();
			float bob = (float) Math.sin(s.ageInTicks / 10.0F + s.bobOffset) * 0.1F + 0.1F;
			pose.translate(0.0F, (float) -box.minY + 0.0625F + bob, 0.0F);
			pose.rotate(Axis.YP, ItemEntity.getSpin(s.ageInTicks, s.bobOffset));
			extents(s.item, pose.last().pose(), out);
			return null;
		}

		// anything else: its collision box
		AABB bb = e.getBoundingBox().move(state.x - e.getX(), state.y - e.getY(), state.z - e.getZ());
		Matrix4f id = new Matrix4f();
		out.box(id, (float) ((bb.minX + bb.maxX) * 0.5 - out.origin.x), (float) ((bb.minY + bb.maxY) * 0.5 - out.origin.y), (float) ((bb.minZ + bb.maxZ) * 0.5 - out.origin.z),
			(float) bb.getXsize() * 0.5F, (float) bb.getYsize() * 0.5F, (float) bb.getZsize() * 0.5F);
		return null;
	}

	/** Skins read once (null: not a resource, e.g. a downloaded skin: those boxes stay HN's grey). */
	private static final Map<Identifier, NativeImage> SKINS = new HashMap<>();
	private static final Map<Identifier, Map<ModelPart.Cube, Integer>> CUBE_COLOURS = new HashMap<>();

	private static NativeImage skin(final Minecraft minecraft, final Identifier id) {
		if (id == null) {
			return null;
		}

		return SKINS.computeIfAbsent(id, key -> {
			try (InputStream in = minecraft.getResourceManager().open(key)) {
				return NativeImage.read(in);
			} catch (Exception ex) {
				Passthrough.LOG.info("shadows: skin {} not readable ({}): its stand-ins stay grey", key, ex.toString());
				return null;
			}
		});
	}

	/**
	 * A cube's colour: the average of the opaque skin pixels its faces use. HN draws the model's stand-ins (receivers)
	 * in it, so a stand-in a frame behind Steve, or peeking past him, looks like him rather than a white clone (the user,
	 * 2026-10-13).
	 */
	private static int cubeColour(final Identifier id, final NativeImage skin, final ModelPart.Cube cube) {
		if (skin == null) {
			return -1;
		}

		return CUBE_COLOURS.computeIfAbsent(id, key -> new IdentityHashMap<>()).computeIfAbsent(cube, c -> {
			long r = 0, g = 0, b = 0, n = 0;
			int w = skin.getWidth(), h = skin.getHeight();
			for (ModelPart.Polygon p : c.polygons) {
				float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
				for (ModelPart.Vertex v : p.vertices()) {
					u0 = Math.min(u0, v.u());
					v0 = Math.min(v0, v.v());
					u1 = Math.max(u1, v.u());
					v1 = Math.max(v1, v.v());
				}

				for (int y = Math.max(0, (int) (v0 * h)); y < Math.min(h, (int) Math.ceil(v1 * h)); y++) {
					for (int x = Math.max(0, (int) (u0 * w)); x < Math.min(w, (int) Math.ceil(u1 * w)); x++) {
						int argb = skin.getPixel(x, y);
						if ((argb >>> 24) < 128) {
							continue;
						}

						r += argb >> 16 & 0xFF;
						g += argb >> 8 & 0xFF;
						b += argb & 0xFF;
						n++;
					}
				}
			}

			// no opaque pixel: an outer layer (hat, jacket, sleeves, trousers) this skin leaves empty: its stand-in was a
			// grey shell around the body, bigger than Steve as Minecraft draws him
			return n == 0 ? -2 : (int) (r / n) << 16 | (int) (g / n) << 8 | (int) (b / n);
		});
	}

	private static final java.util.Set<String> OUTER_LAYERS = java.util.Set.of("hat", "jacket", "left_sleeve", "right_sleeve", "left_pants", "right_pants");

	/** A model part's cubes and its children's, as drawn (visible ones only: the skin's outer layers as the skin has them). */
	private static void part(final ModelPart p, final String name, final PoseStack pose, final Out out) {
		if (!p.visible) {
			return;
		}

		pose.pushPose();
		p.translateAndRotate(pose);
		ModelPartAccessor access = (ModelPartAccessor) (Object) p;
		if (!p.skipDraw) {
			Matrix4f m = pose.last().pose();
			for (ModelPart.Cube c : access.passthrough$cubes()) {
				// native Steve: the local player's body faces (not the skin's outer layers: mostly transparent pixels, and
				// HN's material is opaque)
				if (NativeSteve.enabled && out.surface == 2 && !OUTER_LAYERS.contains(name)) {
					for (int i = 0; i < c.polygons.length; i++) {
						NativeSteve.face(out.faces, out.skinId, out.skin, m, c, i, c.polygons[i], out.origin);
					}
				}
				out.colour = cubeColour(out.skinId, out.skin, c);
				out.box(m, (c.minX + c.maxX) / 32.0F, (c.minY + c.maxY) / 32.0F, (c.minZ + c.maxZ) / 32.0F, (c.maxX - c.minX) / 32.0F, (c.maxY - c.minY) / 32.0F,
					(c.maxZ - c.minZ) / 32.0F);
			}
		}

		access.passthrough$children().forEach((childName, child) -> part(child, childName, pose, out));
		pose.popPose();
	}

	/** Opaque rectangles of a sprite (x0, y0, x1, y1 in pixels, rows from the top), per sprite, made once. */
	private static final java.util.Map<net.minecraft.client.renderer.texture.SpriteContents, List<int[]>> SILHOUETTES = new java.util.HashMap<>();

	/**
	 * An item as it is drawn, layer by layer, in the frame it is submitted in: each layer's own transform (its display
	 * transform for the context, and its local one), then its model. A flat item (a sprite one pixel deep: swords,
	 * sticks, most items) by its sprite's opaque pixels, merged into rectangles, on the model's plane: a single box
	 * made every item's shadow a square (the user). Anything else by the box of its model's extents in the layer's
	 * frame (a block item: its cube).
	 */
	private static void extents(final ItemStackRenderState item, final Matrix4f m, final Out out) {
		ItemStackRenderStateAccessor items = (ItemStackRenderStateAccessor) item;
		ItemStackRenderState.LayerRenderState[] layers = items.passthrough$layers();
		for (int i = 0; i < items.passthrough$activeLayerCount() && i < layers.length; i++) {
			LayerRenderStateAccessor layer = (LayerRenderStateAccessor) (Object) layers[i];
			PoseStack.Pose pose = new PoseStack.Pose();
			pose.pose().set(m);
			layer.passthrough$applyTransform(pose);
			Matrix4f lm = pose.pose();
			Vector3fc[] corners = layer.passthrough$extents().get();
			if (corners == null || corners.length == 0) {
				continue;
			}

			float x0 = Float.MAX_VALUE, y0 = Float.MAX_VALUE, z0 = Float.MAX_VALUE, x1 = -Float.MAX_VALUE, y1 = -Float.MAX_VALUE, z1 = -Float.MAX_VALUE;
			for (Vector3fc c : corners) {
				x0 = Math.min(x0, c.x());
				y0 = Math.min(y0, c.y());
				z0 = Math.min(z0, c.z());
				x1 = Math.max(x1, c.x());
				y1 = Math.max(y1, c.y());
				z1 = Math.max(z1, c.z());
			}

			Material.Baked material = layer.passthrough$particleMaterial();
			List<int[]> rects = z1 - z0 < 0.08F && x1 - x0 > 0.3F && material != null ? silhouette(material.sprite().contents()) : null;
			if (rects == null) {
				out.box(lm, (x0 + x1) * 0.5F, (y0 + y1) * 0.5F, (z0 + z1) * 0.5F, Math.max(x1 - x0, 0.03F) * 0.5F, Math.max(y1 - y0, 0.03F) * 0.5F,
					Math.max(z1 - z0, 0.03F) * 0.5F);
				continue;
			}

			int w = material.sprite().contents().width(), h = material.sprite().contents().height();
			float sx = (x1 - x0) / w, sy = (y1 - y0) / h;
			for (int[] r : rects) {
				// texture rows run down, the model's y up
				float cx = x0 + (r[0] + r[2]) * 0.5F * sx, cy = y1 - (r[1] + r[3]) * 0.5F * sy;
				out.box(lm, cx, cy, (z0 + z1) * 0.5F, (r[2] - r[0]) * 0.5F * sx, (r[3] - r[1]) * 0.5F * sy, Math.max(z1 - z0, 0.03F) * 0.5F);
			}
		}
	}

	/** A sprite's opaque pixels (its first frame) as rectangles: runs per row, merged down while the same. */
	private static List<int[]> silhouette(final net.minecraft.client.renderer.texture.SpriteContents sprite) {
		return SILHOUETTES.computeIfAbsent(sprite, s -> {
			com.mojang.blaze3d.platform.NativeImage image = ((SpriteContentsAccessor) s).passthrough$originalImage();
			int w = s.width(), h = s.height();
			List<int[]> done = new ArrayList<>(), open = new ArrayList<>();
			for (int y = 0; y <= h; y++) {
				List<int[]> runs = new ArrayList<>();
				for (int x = 0; y < h && x < w; ) {
					if ((image.getPixel(x, y) >>> 24) < 128) {
						x++;
						continue;
					}

					int start = x;
					while (x < w && (image.getPixel(x, y) >>> 24) >= 128) {
						x++;
					}

					runs.add(new int[] {start, x});
				}

				List<int[]> next = new ArrayList<>();
				for (int[] o : open) {
					int[] same = null;
					for (int[] r : runs) {
						if (r[0] == o[0] && r[1] == o[2]) {
							same = r;
							break;
						}
					}

					if (same != null) {
						o[3] = y + 1;
						runs.remove(same);
						next.add(o);
					} else {
						done.add(o);
					}
				}

				for (int[] r : runs) {
					next.add(new int[] {r[0], y, r[1], y + 1});
				}

				open = next;
			}

			done.addAll(open);
			return done;
		});
	}

	/** The nearest particles, each a cube of its drawn size (a quad's size; else its collision box). */
	private static void particles(final Minecraft minecraft, final float partial, final Out out) {
		List<double[]> found = new ArrayList<>();
		for (ParticleGroup<?> group : ((ParticleEngineAccessor) minecraft.particleEngine).passthrough$particles().values()) {
			for (Particle p : ((ParticleGroupAccessor) group).passthrough$particles()) {
				ParticleAccessor a = (ParticleAccessor) p;
				double x = a.passthrough$xo() + (a.passthrough$x() - a.passthrough$xo()) * partial - out.origin.x;
				double y = a.passthrough$yo() + (a.passthrough$y() - a.passthrough$yo()) * partial - out.origin.y;
				double z = a.passthrough$zo() + (a.passthrough$z() - a.passthrough$zo()) * partial - out.origin.z;
				double d = x * x + y * y + z * z;
				if (d > 24.0 * 24.0) {
					continue;
				}

				float half = p instanceof SingleQuadParticle ? ((SingleQuadParticleAccessor) p).passthrough$quadSize() : a.passthrough$bbWidth() * 0.5F;
				found.add(new double[] {d, x, y, z, half});
			}
		}

		found.sort((l, r) -> Double.compare(l[0], r[0]));
		Matrix4f id = new Matrix4f();
		for (int i = 0; i < found.size() && i < MAX_PARTICLES; i++) {
			double[] f = found.get(i);
			float h = (float) Math.max(0.01, f[4]);
			out.box(id, (float) f[1], (float) f[2], (float) f[3], h, h, h);
		}
	}
}
