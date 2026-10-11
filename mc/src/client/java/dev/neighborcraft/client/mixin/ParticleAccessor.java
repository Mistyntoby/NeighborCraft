package dev.neighborcraft.client.mixin;

import net.minecraft.client.particle.Particle;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(Particle.class)
public interface ParticleAccessor {
	@Accessor("x") double passthrough$x();
	@Accessor("y") double passthrough$y();
	@Accessor("z") double passthrough$z();
	@Accessor("xo") double passthrough$xo();
	@Accessor("yo") double passthrough$yo();
	@Accessor("zo") double passthrough$zo();
	@Accessor("bbWidth") float passthrough$bbWidth();
	@Accessor("bbHeight") float passthrough$bbHeight();
}
