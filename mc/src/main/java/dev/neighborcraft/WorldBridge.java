package dev.neighborcraft;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

/** Server-side half: Hello Neighbor's collision as invisible barrier blocks, commands, and events back to the host. */
public final class WorldBridge {
	private static volatile MinecraftServer server;
	/** Barriers we placed (so a reset only removes ours, never the player's builds). */
	private static final Set<BlockPos> barriers = ConcurrentHashMap.newKeySet();
	/** Block changes this tick (server thread only): true = now solid, false = gone. Sent at the end of the tick. */
	private static final Map<BlockPos, Boolean> changes = new LinkedHashMap<>();
	/** While placing or removing the host's own collision: those changes aren't news to the host (server thread only). */
	private static boolean placingGround;

	private WorldBridge() {
	}

	static void attach(final MinecraftServer s) {
		server = s;
	}

	/** The world is loaded and the player has joined: what the host sends now takes effect. */
	private static volatile boolean playerReady;

	public static boolean playerReady() {
		return playerReady;
	}

	/** Tell the host the world is ready: it (re)sends its world, then starts the player. */
	public static void markReady() {
		playerReady = true;
		Passthrough.events.accept("{\"t\":\"ready\"}");
	}

	static void detach() {
		playerReady = false;
		server = null;
		barriers.clear();
	}

	public static boolean ready() {
		return server != null;
	}

	static MinecraftServer server() {
		return server;
	}

	/** Columns of solid ground from the host: {x, z, yBottom, yTop, ...} in block coordinates (inclusive). Only air is replaced. */
	public static void solid(final int[] columns) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			BlockState barrier = Blocks.BARRIER.defaultBlockState();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			placingGround = true;
			for (int i = 0; i + 3 < columns.length; i += 4) {
				for (int y = columns[i + 2]; y <= columns[i + 3]; y++) {
					pos.set(columns[i], y, columns[i + 1]);
					if (level.isInWorldBounds(pos) && level.getBlockState(pos).isAir()) {
						level.setBlock(pos, barrier, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
						barriers.add(pos.immutable());
					}
				}
			}

			placingGround = false;
		});
	}

	/** Host blocks we placed (HN's world): removed on a re-level, never reported back as the player's blocks. */
	private static final Set<BlockPos> hosts = ConcurrentHashMap.newKeySet();

	/**
	 * HN's world, block by block: {"t":"shape","c":[x,y,z,kind, ...],"m":[hex, ...],"f":[heights, ...]}. kind 0:
	 * nothing of HN's there (our host block goes), 1: HN fills the whole block, 2: HN fills part of it (the next mask in
	 * "m", 8x8x8 bits, and the next floor in "f": 16 surface heights, see HostBlock.fromMask).
	 * Only air and our own host blocks are ever replaced; the player's blocks are never touched.
	 */
	public static void shapes(final int[] cells, final String[] masks, final String[] floors, final String[] ceilings) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			BlockState host = Passthrough.HOST.defaultBlockState();
			BlockPos.MutableBlockPos pos = new BlockPos.MutableBlockPos();
			placingGround = true;
			int mask = 0;
			for (int i = 0; i + 3 < cells.length; i += 4) {
				pos.set(cells[i], cells[i + 1], cells[i + 2]);
				int kind = cells[i + 3];
				if (!level.isInWorldBounds(pos)) {
					if (kind == 2) {
						mask++;
					}
					continue;
				}

				BlockState current = level.getBlockState(pos);
				boolean ours = current.is(Passthrough.HOST);
				if (kind == 0) {
					if (ours) {
						level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
					}

					HostBlock.SHAPES.remove(pos.asLong());
					hosts.remove(pos);
					continue;
				}

				if (kind == 2) {
					HostBlock.SHAPES.put(pos.asLong(), HostBlock.fromMask(masks[mask], floors[mask], ceilings[mask]));
					mask++;
				} else {
					HostBlock.SHAPES.remove(pos.asLong());
				}

				if (current.isAir() || current.is(Blocks.BARRIER)) {
					level.setBlock(pos, host, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
					hosts.add(pos.immutable());
				}
			}

			placingGround = false;
		});
	}

	/** Remove every barrier we placed (e.g. when the host loads another level). */
	public static void clearSolid() {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			placingGround = true;
			for (BlockPos pos : barriers) {
				if (level.getBlockState(pos).is(Blocks.BARRIER)) {
					level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
				}
			}

			for (BlockPos pos : hosts) {
				if (level.getBlockState(pos).is(Passthrough.HOST)) {
					level.setBlock(pos, Blocks.AIR.defaultBlockState(), Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
				}
			}

			placingGround = false;
			barriers.clear();
			hosts.clear();
			HostBlock.SHAPES.clear();
		});
	}

	/** Run a command as the server (op). Results go to the log, not to chat (send_command_feedback is off). */
	public static void command(final String command) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			Passthrough.LOG.info("command: {}", command);
			s.getCommands().performPrefixedCommand(s.createCommandSourceStack(), command);
		});
	}

	private static boolean solidForHost(final ServerLevel level, final BlockPos pos, final BlockState state) {
		return !state.isAir() && !state.is(Blocks.BARRIER) && !state.is(Passthrough.HOST) && !state.getCollisionShape(level, pos).isEmpty();
	}

	/** Every server tick: send the block changes. */
	static void tick(final MinecraftServer s) {
		flush(s);
	}

	/** Server thread, from Level.setBlock: remember the change; flushed once per tick. */
	public static void onBlockChanged(final ServerLevel level, final BlockPos pos, final BlockState state) {
		if (!Passthrough.active || level != level.getServer().overworld()) {
			return;
		}

		if (!placingGround) {
			changes.put(pos.immutable(), solidForHost(level, pos, state));
		}
	}

	/** End of each server tick: {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...]}. */
	static void flush(final MinecraftServer s) {
		if (changes.isEmpty()) {
			return;
		}

		StringBuilder set = new StringBuilder();
		StringBuilder clear = new StringBuilder();
		for (Map.Entry<BlockPos, Boolean> e : changes.entrySet()) {
			StringBuilder b = e.getValue() ? set : clear;
			BlockPos p = e.getKey();
			b.append(b.isEmpty() ? "" : ",").append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ());
		}

		changes.clear();
		Passthrough.events.accept("{\"t\":\"blocks\",\"set\":[" + set + "],\"clear\":[" + clear + "]}");
	}

	/** Every solid block within `radius` of the player, as one "blocks" message (the host's colliders start from this). */
	public static void sync(final int radius) {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerLevel level = s.overworld();
			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			if (player == null) {
				return;
			}

			BlockPos c = player.blockPosition();
			BlockPos.MutableBlockPos p = new BlockPos.MutableBlockPos();
			int found = 0;
			for (int x = -radius; x <= radius && found < 3000; x++) {
				for (int z = -radius; z <= radius && found < 3000; z++) {
					for (int y = -24; y <= 40; y++) {
						p.set(c.getX() + x, c.getY() + y, c.getZ() + z);
						if (!level.isInWorldBounds(p) || !level.isLoaded(p)) {
							continue;
						}

						BlockState state = level.getBlockState(p);
						if (solidForHost(level, p, state)) {
							changes.put(p.immutable(), true);
							found++;
						}
					}
				}
			}

			flush(s);
		});
	}

	/** `source`: what exploded or was blown up, e.g. "tnt", "creeper". */
	public static void onExplosion(final Vec3 center, final float radius, final String source) {
		if (Passthrough.active) {
			Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"explosion\",\"pos\":[%.3f,%.3f,%.3f],\"r\":%.2f,\"src\":\"%s\"}",
				center.x, center.y, center.z, radius, source));
		}
	}
}
