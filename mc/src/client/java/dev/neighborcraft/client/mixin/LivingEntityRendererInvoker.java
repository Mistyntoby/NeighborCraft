package dev.neighborcraft.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.renderer.entity.LivingEntityRenderer;
import net.minecraft.client.renderer.entity.state.LivingEntityRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Invoker;

/** The renderer's own steps of placing a living entity's model (ShadowExport replays its submit). */
@Mixin(LivingEntityRenderer.class)
public interface LivingEntityRendererInvoker {
	@Invoker("setupRotations")
	void passthrough$setupRotations(LivingEntityRenderState state, PoseStack pose, float bodyRot, float scale);

	@Invoker("scale")
	void passthrough$scale(LivingEntityRenderState state, PoseStack pose);
}
