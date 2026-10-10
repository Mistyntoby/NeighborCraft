package dev.neighborcraft;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.BitSetDiscreteVoxelShape;
import net.minecraft.world.phys.shapes.BooleanOp;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.CubeVoxelShape;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;

/**
 * Hello Neighbor's world inside Minecraft: an invisible block whose collision shape is HN's own geometry within that
 * block, probed by the host at 1/8 block (12.5 cm), so walls, floors, stairs and furniture are off-grid and accurate.
 * Steve collides with it, and blocks can be placed against it; it is never drawn, never outlined, never broken.
 */
public final class HostBlock extends Block {
	/** Shape per block position. Single player: the integrated server and the client share this map. */
	static final Map<Long, VoxelShape> SHAPES = new ConcurrentHashMap<>();

	public HostBlock(final Properties properties) {
		super(properties);
	}

	/**
	 * The shape at a position; none known: empty. The map is shared by the server and the client (one process), and
	 * when HN empties a block (a door leaf swung away) the server drops its shape before the client hears the block is
	 * gone: for that moment the client had a host block without a shape, which was a full cube, at Steve's head beside
	 * an opening door, and Minecraft made him crawl (the recorder: a whole block in his standing box the tick before
	 * every forced crawl, gone the tick after). A host block without a shape is on its way out; full ones say so.
	 */
	static VoxelShape shape(final BlockPos pos) {
		VoxelShape s = SHAPES.get(pos.asLong());
		return s == null ? Shapes.empty() : s;
	}

	/**
	 * Heights in thousandths of a block, one per column of an n x n grid over the block (x * n + z; 0: none), as
	 * slabs: `up` true for floors (the slab is under the surface), false for ceilings (above it).
	 */
	private static void surfaces(final List<VoxelShape> parts, final String heights, final boolean up) {
		if (heights.isEmpty()) {
			return;
		}

		String[] h = heights.split(",");
		int n = (int) Math.round(Math.sqrt(h.length));
		for (int i = 0; i < h.length; i++) {
			double at = Integer.parseInt(h[i]) / 1000.0;
			if (at <= 0.0) {
				continue;
			}

			double x = (i / n) / (double) n, z = (i % n) / (double) n, w = 1.0 / n;
			double lo = up ? Math.max(0.0, at - 0.125) : Math.min(at, 1.0), hi = up ? Math.min(1.0, at) : Math.min(1.0, at + 0.125);
			if (hi > lo) {
				parts.add(Shapes.box(x, lo, z, x + w, hi, z + w));
			}
		}
	}

	/**
	 * An 8x8x8 occupancy mask (bit x * 64 + y * 8 + z, 64 bytes as hex: walls, furniture) plus the exact floor and
	 * ceiling surfaces measured by rays (see surfaces) as a shape: floors and ceilings sit at their real heights, so
	 * Steve stands on HN's ground, stairs are their real steps and low ceilings their real clearance.
	 *
	 * <p>Built fast: thousands arrive when a level starts. Joined box by box with Shapes.or (which optimizes the whole
	 * shape after every box) they took ~6-15 ms each (8.7 s for a level start's burst in tools/drivetest.py) and kept
	 * Steve waiting for his ground. The grid part is now filled straight into a voxel bitset, and the slabs at free
	 * heights are joined pairwise, unoptimized, with one optimize at the end: the same shape (checked against the old
	 * construction for 300 shapes: none differed), ~0.3 ms each.
	 */
	static VoxelShape fromMask(final String hex, final String floor, final String ceiling) {
		List<VoxelShape> parts = new ArrayList<>();
		BitSetDiscreteVoxelShape grid = new BitSetDiscreteVoxelShape(8, 8, 8);
		boolean any = false;
		for (int x = 0; x < 8; x++) {
			for (int y = 0; y < 8; y++) {
				int row = Integer.parseInt(hex.substring((x * 8 + y) * 2, (x * 8 + y) * 2 + 2), 16);
				for (int z = 0; z < 8; z++) {
					if ((row >> z & 1) != 0) {
						grid.fill(x, y, z);
						any = true;
					}
				}
			}
		}

		if (any) {
			parts.add(new CubeVoxelShape(grid));
		}

		surfaces(parts, floor, true);
		surfaces(parts, ceiling, false);
		if (parts.isEmpty()) {
			return Shapes.empty();
		}

		// pairwise, like a tournament: n log n instead of n * n
		while (parts.size() > 1) {
			List<VoxelShape> next = new ArrayList<>((parts.size() + 1) / 2);
			for (int i = 0; i + 1 < parts.size(); i += 2) {
				next.add(Shapes.joinUnoptimized(parts.get(i), parts.get(i + 1), BooleanOp.OR));
			}

			if (parts.size() % 2 == 1) {
				next.add(parts.get(parts.size() - 1));
			}

			parts = next;
		}

		return parts.get(0).optimize();
	}

	@Override
	protected VoxelShape getShape(final BlockState state, final BlockGetter level, final BlockPos pos, final CollisionContext context) {
		return shape(pos);
	}

	@Override
	protected VoxelShape getCollisionShape(final BlockState state, final BlockGetter level, final BlockPos pos, final CollisionContext context) {
		return shape(pos);
	}

	/** The third-person camera stops at visual shapes: it stays out of HN's walls as it does out of blocks. */
	@Override
	protected VoxelShape getVisualShape(final BlockState state, final BlockGetter level, final BlockPos pos, final CollisionContext context) {
		return shape(pos);
	}

	@Override
	protected RenderShape getRenderShape(final BlockState state) {
		return RenderShape.INVISIBLE;
	}

	@Override
	protected boolean propagatesSkylightDown(final BlockState state) {
		return true;
	}
}
