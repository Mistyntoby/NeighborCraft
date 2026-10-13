package dev.neighborcraft;

import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;

/** Server-side half: Hello Neighbor's collision as invisible barrier blocks, commands, and events back to the host. */
public final class WorldBridge {
	private static volatile MinecraftServer server;
	/** Barriers we placed (so a reset only removes ours, never the player's builds). */
	private static final Set<BlockPos> barriers = ConcurrentHashMap.newKeySet();
	/** Block changes this tick (server thread only): 0 gone, 1 solid, 2 solid full opaque cube (with its colour above the
	 * low two bits, see kind). Sent at the end of the tick. */
	private static final Map<BlockPos, Integer> changes = new LinkedHashMap<>();
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

	public static MinecraftServer server() {
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

	/** HN's collision received but not placed yet by the server (position -> messages waiting for it). */
	private static final Map<Long, Integer> pending = new ConcurrentHashMap<>();

	/**
	 * Whether HN's world at {@code pos} is in the client's world as it will be: false while collision HN sent for it is
	 * still waiting for the server, or the server placed it and the client hasn't received it. After a long teleport
	 * (a level's start) the server falls seconds behind (the log: "Can't keep up! ... 12590ms behind") and the client
	 * gets the chunks later still: Steve, moved by the client, walked onto ground that wasn't there yet and fell.
	 */
	public static boolean arrived(final Level clientLevel, final BlockPos pos) {
		if (clientLevel.getBlockState(pos).is(Passthrough.HOST)) {
			return true;
		}

		return !pending.containsKey(pos.asLong()) && !hosts.contains(pos);
	}

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

		for (int i = 0; i + 3 < cells.length; i += 4) {
			if (cells[i + 3] != 0) {
				pending.merge(BlockPos.asLong(cells[i], cells[i + 1], cells[i + 2]), 1, Integer::sum);
			}
		}

		// The shapes are built on their own thread (in order): each is up to 512 sub-cell boxes and the floor and
		// ceiling slabs joined one by one, and built on the server thread for thousands of blocks they kept the server
		// seconds behind ("Can't keep up! ... 10913ms") the whole way down Act 1's road, Steve waiting for his ground
		// again and again. The server only places the finished blocks; the link's thread stays free for the camera.
		SHAPE_BUILDER.execute(() -> {
			long buildStart = System.nanoTime();
			VoxelShape[] built = new VoxelShape[masks.length];
			for (int i = 0; i < masks.length; i++) {
				built[i] = HostBlock.fromMask(masks[i], floors[i], ceilings[i]);
			}

			long sent = System.nanoTime();
			stats(cells.length / 4, sent - buildStart, 0L, 0L);
			s.execute(() -> {
				long start = System.nanoTime();
				try {
					placeShapes(s, cells, built);
				} finally {
					stats(0, 0L, start - sent, System.nanoTime() - start);
					for (int i = 0; i + 3 < cells.length; i += 4) {
						if (cells[i + 3] != 0) {
							pending.computeIfPresent(BlockPos.asLong(cells[i], cells[i + 1], cells[i + 2]), (k, n) -> n > 1 ? n - 1 : null);
						}
					}
				}
			});
		});
	}

	private static final ExecutorService SHAPE_BUILDER = Executors.newSingleThreadExecutor(r -> {
		Thread t = new Thread(r, "NeighborCraft shapes");
		t.setDaemon(true);
		return t;
	});

	private static long statBlocks, statBuild, statWait, statPlace, statMaxWait, statNext;

	/** Where HN's world costs time: blocks received, building their shapes, waiting for the server, placing them. */
	private static synchronized void stats(final int blocks, final long build, final long wait, final long place) {
		statBlocks += blocks;
		statBuild += build;
		statWait += wait;
		statPlace += place;
		statMaxWait = Math.max(statMaxWait, wait);
		long now = System.nanoTime();
		if (now > statNext) {
			if (statBlocks > 0) {
				Passthrough.LOG.info("HN's world: {} blocks in 5 s; shapes built in {} ms (link thread), placed in {} ms (server); longest wait for the server {} ms",
					statBlocks, statBuild / 1_000_000L, statPlace / 1_000_000L, statMaxWait / 1_000_000L);
			}

			statBlocks = statBuild = statWait = statPlace = statMaxWait = 0L;
			statNext = now + 5_000_000_000L;
		}
	}

	private static void placeShapes(final MinecraftServer s, final int[] cells, final VoxelShape[] built) {
		{
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
					HostBlock.SHAPES.put(pos.asLong(), built[mask]);
					mask++;
				} else {
					HostBlock.SHAPES.put(pos.asLong(), Shapes.block()); // full: said, never assumed (see HostBlock.shape)
				}

				if (current.isAir() || current.is(Blocks.BARRIER)) {
					level.setBlock(pos, host, Block.UPDATE_CLIENTS | Block.UPDATE_KNOWN_SHAPE);
					hosts.add(pos.immutable());
				}
			}

			placingGround = false;
		}
	}

	/** Remove every barrier we placed (e.g. when the host loads another level). */
	public static void clearSolid() {
		MinecraftServer s = server;
		if (s == null) {
			return;
		}

		// behind the shapes still being built (they go to the server through the same queue), never before them
		SHAPE_BUILDER.execute(() -> s.execute(() -> {
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
		}));
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

	/** What HN hears of a block: 0 nothing, 1 solid (HN's NPCs collide with it), 2 solid and a full opaque cube (it also casts
	 * HN's shadow and is drawn by HN as a receiver, in its map colour: bits 2-25, 0xRRGGBB; what of the receiver
	 * peeks past Minecraft's picture then has the block's colour, not grey). */
	private static int kind(final ServerLevel level, final BlockPos pos, final BlockState state) {
		if (!solidForHost(level, pos, state)) {
			return 0;
		}

		if (!(state.isSolidRender() && state.isCollisionShapeFullBlock(level, pos))) {
			return 1;
		}

		return 2 | (state.getMapColor(level, pos).col & 0xFFFFFF) << 2;
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
			changes.put(pos.immutable(), kind(level, pos, state));
		}
	}

	/** End of each server tick: {"t":"blocks","set":[x,y,z,...],"clear":[x,y,z,...],"opaque":[x,y,z,...],"col":[rgb,...]} (opaque: the set
	 * ones that are full opaque cubes; col: their colours, one per opaque block). */
	static void flush(final MinecraftServer s) {
		if (changes.isEmpty()) {
			return;
		}

		StringBuilder set = new StringBuilder();
		StringBuilder clear = new StringBuilder();
		StringBuilder opaque = new StringBuilder();
		StringBuilder colours = new StringBuilder();
		for (Map.Entry<BlockPos, Integer> e : changes.entrySet()) {
			BlockPos p = e.getKey();
			int kind = e.getValue() & 3;
			for (StringBuilder b : kind == 0 ? new StringBuilder[] {clear} : kind == 2 ? new StringBuilder[] {set, opaque} : new StringBuilder[] {set}) {
				b.append(b.isEmpty() ? "" : ",").append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ());
			}

			if (kind == 2) {
				colours.append(colours.isEmpty() ? "" : ",").append(e.getValue() >>> 2);
			}
		}

		changes.clear();
		Passthrough.events.accept("{\"t\":\"blocks\",\"set\":[" + set + "],\"clear\":[" + clear + "],\"opaque\":[" + opaque + "],\"col\":[" + colours + "]}");
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
							changes.put(p.immutable(), kind(level, p, state));
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
