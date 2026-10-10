package dev.neighborcraft;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.shapes.CollisionContext;
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

	static VoxelShape shape(final BlockPos pos) {
		VoxelShape s = SHAPES.get(pos.asLong());
		return s == null ? Shapes.block() : s;
	}

	/**
	 * An 8x8x8 occupancy mask (bit x * 64 + y * 8 + z, 64 bytes as hex) as a shape: runs along z merged into boxes.
	 */
	static VoxelShape fromMask(final String hex) {
		VoxelShape shape = Shapes.empty();
		for (int x = 0; x < 8; x++) {
			for (int y = 0; y < 8; y++) {
				int row = Integer.parseInt(hex.substring((x * 8 + y) * 2, (x * 8 + y) * 2 + 2), 16);
				int z = 0;
				while (z < 8) {
					if ((row >> z & 1) == 0) {
						z++;
						continue;
					}

					int start = z;
					while (z < 8 && (row >> z & 1) != 0) {
						z++;
					}

					shape = Shapes.or(shape, Shapes.box(x / 8.0, y / 8.0, start / 8.0, (x + 1) / 8.0, (y + 1) / 8.0, z / 8.0));
				}
			}
		}

		return shape.optimize();
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
