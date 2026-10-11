package dev.neighborcraft.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import java.util.function.Supplier;
import net.minecraft.client.resources.model.sprite.Material;
import org.joml.Vector3fc;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;
import org.spongepowered.asm.mixin.gen.Invoker;

/** An item layer's own transform, model extents and sprite (ShadowExport: an item's silhouette). */
@Mixin(targets = "net.minecraft.client.renderer.item.ItemStackRenderState$LayerRenderState")
public interface LayerRenderStateAccessor {
	@Accessor("extents")
	Supplier<Vector3fc[]> passthrough$extents();

	@Accessor("particleMaterial")
	Material.Baked passthrough$particleMaterial();

	@Invoker("applyTransform")
	void passthrough$applyTransform(PoseStack.Pose pose);
}
