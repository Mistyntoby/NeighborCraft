package dev.neighborcraft.client.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.neighborcraft.Passthrough;
import dev.neighborcraft.client.CarryHand;
import net.minecraft.client.renderer.FirstPersonHandsAndItemsRenderer;
import net.minecraft.client.renderer.SubmitNodeCollector;
import net.minecraft.client.renderer.entity.state.AvatarRenderState;
import net.minecraft.client.renderer.state.level.FirstPersonHandsAndItemsRenderState;
import net.minecraft.client.renderer.state.level.PlayerRenderState;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.item.ItemStack;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Holding one of Hello Neighbor's objects, the main hand is the player's own arm in a carrying pose (see CarryHand). */
@Mixin(FirstPersonHandsAndItemsRenderer.class)
abstract class FirstPersonCarryMixin {
	@Shadow
	protected abstract void renderPlayerArm(PoseStack pose, SubmitNodeCollector collector, int light, float equip, float swing, HumanoidArm arm, PlayerRenderState state);

	@Inject(method = "submitArmWithItem", at = @At("HEAD"), cancellable = true)
	private void passthrough$carryingArm(final PlayerRenderState state, final FirstPersonHandsAndItemsRenderState hands, final float partial, final float pitch,
		final InteractionHand hand, final float swing, final ItemStack stack, final float equip, final PoseStack pose, final SubmitNodeCollector collector,
		final int light, final CallbackInfo ci) {
		if (hand != InteractionHand.MAIN_HAND || !stack.is(Passthrough.HN_OBJECT) || hands.isScoping) {
			return;
		}

		AvatarRenderState avatar = state.avatarRenderState;
		if (avatar == null || avatar.isInvisible) {
			return;
		}

		HumanoidArm arm = avatar.mainArm;
		pose.pushPose();
		CarryHand.applyPose(pose, arm);
		CarryHand.capturing = true;
		try {
			this.renderPlayerArm(pose, collector, light, equip, swing, arm, state);
		} finally {
			CarryHand.capturing = false;
			pose.popPose();
		}

		ci.cancel();
	}

	@Inject(
		method = "renderPlayerArm",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/FirstPersonHandsAndItemsRenderer;renderPlayerHand(Lcom/mojang/blaze3d/vertex/PoseStack;Lnet/minecraft/client/renderer/SubmitNodeCollector;ILnet/minecraft/world/entity/HumanoidArm;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;)V")
	)
	private void passthrough$fist(final PoseStack pose, final SubmitNodeCollector collector, final int light, final float equip, final float swing,
		final HumanoidArm arm, final PlayerRenderState state, final CallbackInfo ci) {
		if (CarryHand.capturing) {
			CarryHand.fist(pose.last().pose(), arm);
		}
	}
}
