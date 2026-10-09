package dev.neighborcraft.client;

import dev.neighborcraft.Passthrough;
import dev.neighborcraft.WorldBridge;
import java.util.List;
import java.util.Optional;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.fabricmc.fabric.api.networking.v1.ServerPlayConnectionEvents;
import net.minecraft.client.CloudStatus;
import net.minecraft.client.InactivityFpsLimit;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Options;
import net.minecraft.client.gui.screens.DeathScreen;
import net.minecraft.client.gui.screens.TitleScreen;
import net.minecraft.client.tutorial.TutorialSteps;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.HolderSet;
import net.minecraft.core.registries.Registries;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.level.GameType;
import net.minecraft.world.level.LevelSettings;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.levelgen.FlatLevelSource;
import net.minecraft.world.level.levelgen.WorldDimensions;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.flat.FlatLayerInfo;
import net.minecraft.world.level.levelgen.flat.FlatLevelGeneratorSettings;
import net.minecraft.world.level.levelgen.presets.WorldPresets;

public class PassthroughClient implements ClientModInitializer {
	/** The world the host plays in: an empty (void) creative world, so only what the player builds is Minecraft. */
	private static final String WORLD = "neighborcraft";
	/** Run on the server once the player has joined. */
	private static final List<String> SETUP = List.of(
		"gamerule advance_time false",
		"gamerule advance_weather false",
		"gamerule spawn_mobs false",
		"gamerule spawn_monsters false",
		"gamerule spawn_patrols false",
		"gamerule spawn_phantoms false",
		"gamerule spawn_wandering_traders false",
		"gamerule send_command_feedback false",
		"gamerule log_admin_commands false",
		"gamerule keep_inventory true",
		"difficulty normal",
		"gamerule show_advancement_messages false",
		"gamerule player_movement_check false",
		"time set noon",
		"weather clear",
		"clear @a",
		"item replace entity @a hotbar.0 with minecraft:diamond_sword",
		"item replace entity @a hotbar.1 with minecraft:oak_planks 64",
		"item replace entity @a hotbar.2 with minecraft:cobblestone 64",
		"item replace entity @a hotbar.3 with minecraft:bricks 64",
		"item replace entity @a hotbar.4 with minecraft:glass 64",
		"item replace entity @a hotbar.5 with minecraft:oak_log 64",
		"item replace entity @a hotbar.6 with minecraft:white_wool 64",
		"item replace entity @a hotbar.7 with minecraft:torch 64",
		"item replace entity @a hotbar.8 with minecraft:diamond_pickaxe"
	);
	private static boolean configured;
	private static boolean worldRequested;
	/** Server ticks until the setup commands run (the player isn't in the player list yet when JOIN fires). */
	private static int setupIn = -1;
	private static int respawnIn;
	/** Started by Hello Neighbor (-Dneighborcraft.exitWithHost=true): quit when it has been gone for a while. */
	private static final boolean EXIT_WITH_HOST = Boolean.getBoolean("neighborcraft.exitWithHost");
	private static final long HOST_GONE_NANOS = 20_000_000_000L;
	private static long hostLastSeen = System.nanoTime();
	private static boolean hostEverSeen;

	@Override
	public void onInitializeClient() {
		HostLink.launch();
		ClientTickEvents.END_CLIENT_TICK.register(PassthroughClient::tick);
		ServerPlayConnectionEvents.JOIN.register((handler, sender, server) -> {
			ServerPlayer player = handler.player;
			player.getAbilities().flying = true;
			player.onUpdateAbilities();
			setupIn = 10;
		});
		ServerTickEvents.END_SERVER_TICK.register(server -> {
			if (setupIn > 0 && --setupIn == 0) {
				SETUP.forEach(WorldBridge::command);
			}
		});
	}

	private static void tick(final Minecraft minecraft) {
		// a death (the void) would leave the death screen over the host's picture: respawn straight away
		if (minecraft.player != null && minecraft.gui.screen() instanceof DeathScreen && --respawnIn <= 0) {
			respawnIn = 40;
			minecraft.player.respawn();
		}

		if (!configured) {
			configured = true;
			configure(minecraft.options);
		}

		if (HostLink.connected()) {
			hostLastSeen = System.nanoTime();
			hostEverSeen = true;
		} else if (EXIT_WITH_HOST && hostEverSeen && System.nanoTime() - hostLastSeen > HOST_GONE_NANOS) {
			Passthrough.LOG.info("Hello Neighbor is gone: quitting");
			hostEverSeen = false;
			minecraft.stop();
		}

		// back on the title screen (first start, or the world was quit by accident): open the world again
		if (minecraft.level != null) {
			worldRequested = false;
		} else if (!worldRequested && minecraft.gui.screen() instanceof TitleScreen && !Boolean.getBoolean("neighborcraft.noAutoWorld")) {
			worldRequested = true;
			openWorld(minecraft);
		}
	}

	/** Settings for sitting behind another game: keep running unfocused, and no sky/cloud/bobbing effects in the picture. */
	private static void configure(final Options options) {
		options.pauseOnLostFocus = false;
		options.onboardAccessibility = false;
		options.tutorialStep = TutorialSteps.NONE;
		options.cloudStatus().set(CloudStatus.OFF);
		// the hand bobs as in Minecraft; the world never does while attached (GameRendererMixin)
		options.bobView().set(true);
		options.vignette().set(false);
		options.improvedTransparency().set(false);
		options.inactivityFpsLimit().set(InactivityFpsLimit.MINIMIZED);
		options.fovEffectScale().set(0.0);
		options.damageTiltStrength().set(0.0);
		options.menuBackgroundBlurriness().set(0);
		options.enableVsync().set(false);
		// Hello Neighbor has its own soundtrack; Minecraft keeps its sounds (blocks, hits) but no music
		options.getSoundSourceOptionInstance(SoundSource.MUSIC).set(0.0);
		// Hello Neighbor's stairs arrive as whole blocks: step up them without jumping
		options.autoJump().set(true);
		// the host shows ~60-120 fps: rendering faster only competes with it for the GPU
		options.framerateLimit().set(120);
		options.save();
	}

	/** Whether this run already started the world fresh (it is only reopened, not wiped, after an accidental quit). */
	private static boolean freshThisRun;

	private static void openWorld(final Minecraft minecraft) {
		// Every run starts from an empty world: Hello Neighbor's geometry is re-probed each session (and aligned to where
		// the player stands then), so blocks kept from an earlier session would sit in the wrong places.
		if (!freshThisRun && minecraft.getLevelSource().levelExists(WORLD)) {
			try (var access = minecraft.getLevelSource().createAccess(WORLD)) {
				access.deleteLevel();
				Passthrough.LOG.info("deleted last session's world {}", WORLD);
			} catch (java.io.IOException e) {
				Passthrough.LOG.warn("couldn't delete last session's world", e);
			}
		}

		freshThisRun = true;
		if (minecraft.getLevelSource().levelExists(WORLD)) {
			Passthrough.LOG.info("opening world {}", WORLD);
			minecraft.createWorldOpenFlows().openWorld(WORLD, () -> minecraft.gui.setScreen(new TitleScreen()));
		} else {
			Passthrough.LOG.info("creating world {}", WORLD);
			LevelSettings settings = new LevelSettings("NeighborCraft", GameType.CREATIVE, LevelSettings.DifficultySettings.DEFAULT, true, WorldDataConfiguration.DEFAULT);
			minecraft.createWorldOpenFlows().createFreshLevel(WORLD, settings, new WorldOptions(0L, false, false), PassthroughClient::voidWorld, minecraft.gui.screen());
		}
	}

	/** A flat world with a single layer of air: nothing but what gets built (the host's ground arrives as barriers). */
	private static WorldDimensions voidWorld(final HolderLookup.Provider registries) {
		FlatLevelGeneratorSettings flat = new FlatLevelGeneratorSettings(
			Optional.of(HolderSet.direct()), registries.lookupOrThrow(Registries.BIOME).getOrThrow(Biomes.PLAINS), List.of()
		);
		flat.getLayersInfo().add(new FlatLayerInfo(1, Blocks.AIR));
		flat.updateLayers();
		return WorldPresets.createNormalWorldDimensions(registries).replaceOverworldGenerator(registries, new FlatLevelSource(flat));
	}
}
