package dev.neighborcraft.client.mixin;

import net.minecraft.client.MouseHandler;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

/** The cursor position Minecraft's screens see (hover, tooltips, the inventory character's head): the host's cursor. */
@Mixin(MouseHandler.class)
public interface MouseHandlerAccessor {
	@Accessor("xpos")
	void passthrough$setXpos(double x);

	@Accessor("ypos")
	void passthrough$setYpos(double y);
}
