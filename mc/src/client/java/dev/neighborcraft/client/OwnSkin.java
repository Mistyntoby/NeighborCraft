package dev.neighborcraft.client;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.mojang.blaze3d.platform.NativeImage;
import dev.neighborcraft.Passthrough;
import java.io.ByteArrayInputStream;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Base64;
import java.util.List;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.texture.DynamicTexture;
import net.minecraft.core.ClientAsset;
import net.minecraft.resources.Identifier;
import net.minecraft.world.entity.player.PlayerModelType;
import net.minecraft.world.entity.player.PlayerSkin;

/**
 * The player's own skin (the user: "make it so my skin loads properly"). This Minecraft is a development client with
 * no account, so Minecraft itself never gets one; while HN is attached the player was classic Steve. From
 * %LOCALAPPDATA%\NeighborCraft:
 * <ul>
 * <li>skin.png: a 64x64 skin file, used as it is;</li>
 * <li>skin.txt: a Minecraft username on its first line (that player's public skin is fetched from Mojang's profile
 * service and cached as skin_cache.png), and "slim" on any line for the slim (Alex) arms (a fetched skin says so itself).</li>
 * </ul>
 * Neither: classic Steve, as before.
 */
public final class OwnSkin {
	public static final Identifier ID = Identifier.fromNamespaceAndPath(Passthrough.ID, "skin/own");

	private static volatile byte[] png;         // the skin file, once known (any thread)
	private static volatile boolean slim;
	private static volatile boolean started;
	private static PlayerSkin skin;             // registered (render thread)
	private static NativeImage image;           // its pixels, for native Steve's face textures (ShadowExport)

	private OwnSkin() {
	}

	private static Path dir() {
		return NativeBlocks.dir().getParent();
	}

	/** The own skin, or null (none set up, or not loaded yet). Render thread. */
	public static PlayerSkin skin() {
		if (!started) {
			started = true;
			Thread t = new Thread(OwnSkin::load, "NeighborCraft skin");
			t.setDaemon(true);
			t.start();
		}

		byte[] bytes = png;
		if (skin == null && bytes != null) {
			try {
				NativeImage forTexture = NativeImage.read(new ByteArrayInputStream(bytes));
				image = NativeImage.read(new ByteArrayInputStream(bytes));
				if (forTexture.getWidth() != 64 || forTexture.getHeight() != 64) {
					Passthrough.LOG.warn("skin: {}x{}, not a 64x64 skin: classic Steve", forTexture.getWidth(), forTexture.getHeight());
					png = null;
					return null;
				}

				Minecraft.getInstance().getTextureManager().register(ID, new DynamicTexture(() -> "NeighborCraft own skin", forTexture));
				skin = PlayerSkin.insecure(new ClientAsset.ResourceTexture(ID, ID), null, null, slim ? PlayerModelType.SLIM : PlayerModelType.WIDE);
				Passthrough.LOG.info("skin: the player's own ({} arms)", slim ? "slim" : "wide");
			} catch (Exception ex) {
				Passthrough.LOG.warn("skin: not readable ({}): classic Steve", ex.toString());
				png = null;
			}
		}

		return skin;
	}

	/** The own skin's pixels for `id`, or null (another texture). */
	public static NativeImage image(final Identifier id) {
		return ID.equals(id) ? image : null;
	}

	/** Background: skin.png, else skin.txt's username through Mojang's public profile service (cached). */
	private static void load() {
		try {
			Path file = dir().resolve("skin.png"), txt = dir().resolve("skin.txt"), cache = dir().resolve("skin_cache.png");
			List<String> lines = Files.exists(txt) ? Files.readAllLines(txt, StandardCharsets.UTF_8) : List.of();
			slim = lines.stream().anyMatch(l -> l.trim().equalsIgnoreCase("slim"));
			if (Files.exists(file)) {
				png = Files.readAllBytes(file);
				Passthrough.LOG.info("skin: {}", file);
				return;
			}

			String name = lines.isEmpty() ? "" : lines.get(0).trim();
			if (name.isEmpty() || name.equalsIgnoreCase("slim")) {
				Passthrough.LOG.info("skin: none set up ({} or a username in {}): classic Steve", file, txt);
				return;
			}

			HttpClient http = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(8)).build();
			try {
				String profile = get(http, "https://api.mojang.com/users/profiles/minecraft/" + name);
				String uuid = JsonParser.parseString(profile).getAsJsonObject().get("id").getAsString();
				JsonObject session = JsonParser.parseString(get(http, "https://sessionserver.mojang.com/session/minecraft/profile/" + uuid)).getAsJsonObject();
				String value = session.getAsJsonArray("properties").get(0).getAsJsonObject().get("value").getAsString();
				JsonObject textures = JsonParser.parseString(new String(Base64.getDecoder().decode(value), StandardCharsets.UTF_8)).getAsJsonObject()
					.getAsJsonObject("textures");
				JsonObject skinJson = textures.getAsJsonObject("SKIN");
				if (skinJson.has("metadata") && "slim".equals(skinJson.getAsJsonObject("metadata").get("model").getAsString())) {
					slim = true;
				}

				byte[] bytes = http.send(HttpRequest.newBuilder(URI.create(skinJson.get("url").getAsString().replace("http://", "https://"))).build(),
					HttpResponse.BodyHandlers.ofByteArray()).body();
				Files.write(cache, bytes);
				Files.writeString(dir().resolve("skin_cache.txt"), slim ? "slim" : "wide");
				png = bytes;
				Passthrough.LOG.info("skin: {}'s, fetched from Mojang", name);
			} catch (Exception ex) {
				if (Files.exists(cache)) {
					png = Files.readAllBytes(cache);
					Path model = dir().resolve("skin_cache.txt");
					slim |= Files.exists(model) && Files.readString(model).trim().equals("slim");
					Passthrough.LOG.info("skin: {}'s from the cache (Mojang not reached: {})", name, ex.toString());
				} else {
					Passthrough.LOG.warn("skin: {}'s not fetched ({}): classic Steve", name, ex.toString());
				}
			}
		} catch (Exception ex) {
			Passthrough.LOG.warn("skin: {}", ex.toString());
		}
	}

	private static String get(final HttpClient http, final String url) throws Exception {
		HttpResponse<String> r = http.send(HttpRequest.newBuilder(URI.create(url)).timeout(Duration.ofSeconds(8)).build(), HttpResponse.BodyHandlers.ofString());
		if (r.statusCode() != 200) {
			throw new IllegalStateException(url.substring(0, url.indexOf('/', 8)) + " answered " + r.statusCode());
		}

		return r.body();
	}
}
