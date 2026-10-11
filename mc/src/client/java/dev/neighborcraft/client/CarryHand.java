package dev.neighborcraft.client;

import com.mojang.blaze3d.vertex.PoseStack;
import dev.neighborcraft.Passthrough;
import java.util.Locale;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.phys.Vec3;
import org.joml.Matrix4f;
import org.joml.Vector3f;
import org.joml.Vector3fc;

/**
 * The first-person hand holding one of Hello Neighbor's objects. Minecraft draws the bare arm only when the main hand
 * is empty; holding the object's token (an item with an empty model in the hand) it drew nothing, and HN showed the
 * object floating in front of the camera. Now the real right arm (the player's skin, Minecraft's own arm model and
 * swing) is drawn in a carrying pose (FirstPersonCarryMixin), and where its fist is goes to HN every frame, which
 * rests the object on it: it follows the hand through the swing, the equip motion, the view bob and the look.
 *
 * <p>Where the fist is: renderPlayerArm builds the arm's pose and AvatarRenderer.renderHand resets the arm part (pivot
 * (-5, 2, 0) px, zRot 0.1 for the right arm, mirrored for the left) and submits it; the fist is the bottom of the arm's
 * cube, (-1, 10, 0) px in the part. That point, in the hand's view space, is drawn with the hand's own projection
 * (GameRenderer.render3dHud: vertical FOV hudFov); the same screen point is a direction from the world camera (FOV
 * as HN renders it), at the distance where an object looks as big next to the arm as it would in Minecraft.
 */
public final class CarryHand {
	private CarryHand() {
	}

	/** The arm being drawn is the carrying one (set around renderPlayerArm by the mixin). */
	public static boolean capturing;

	private static boolean reported;

	/** The hand projection's vertical FOV this frame (degrees). */
	public static float hudFov = 70.0F;

	/** Lift and inward shift of the carrying arm (hand view space): the fist higher, the object in sight above it. */
	private static final float LIFT = 0.16F, INWARD = 0.10F, FORWARD = -0.05F;

	/** The carrying pose, before Minecraft's own arm transforms (which add the swing and the equip motion). */
	public static void applyPose(final PoseStack pose, final HumanoidArm arm) {
		float side = arm == HumanoidArm.RIGHT ? 1.0F : -1.0F;
		pose.translate(-side * INWARD, LIFT, FORWARD);
	}

	/** The arm's pose as it is submitted: the fist to HN. */
	public static void fist(final Matrix4f armPose, final HumanoidArm arm) {
		Minecraft minecraft = Minecraft.getInstance();
		if (!reported) {
			reported = true;
			Passthrough.LOG.info("carrying hand: drawn (driving {})", Passthrough.driving);
		}

		if (!Passthrough.driving || minecraft.player == null) {
			return;
		}

		float side = arm == HumanoidArm.RIGHT ? 1.0F : -1.0F;
		Matrix4f part = new Matrix4f(armPose).translate(side * -5.0F / 16.0F, 2.0F / 16.0F, 0.0F).rotateZ(side * 0.1F);
		Vector3f v = part.transformPosition(new Vector3f(side * -1.0F / 16.0F, 10.0F / 16.0F, 0.0F));
		// The hand's space looks along +z with +x to the left (its pose is turned half round): measured, the fist
		// (-0.66, -0.28, 1.15) and the shoulder (-0.38, -0.68, 0.58) put on screen where the hand layer shows them.
		if (v.z < 1.0E-3F) {
			return;
		}

		var window = minecraft.getWindow();
		float aspect = (float) window.getWidth() / Math.max(1, window.getHeight());
		double hudTan = Math.tan(Math.toRadians(hudFov) / 2.0);
		double ndcX = -v.x / v.z / (hudTan * aspect), ndcY = v.y / v.z / hudTan;

		// the world camera's FOV as HN renders it: the rendered one has the overscan (CameraMixin) taken out
		double worldTan = Math.tan(Math.toRadians(HostState.renderedFov()) / 2.0) / 1.15;
		Camera camera = minecraft.gameRenderer.mainCamera();
		Vector3fc f = camera.forwardVector(), u = camera.upVector(), l = camera.leftVector();
		double rx = ndcX * worldTan * aspect, uy = ndcY * worldTan;
		Vec3 dir = new Vec3(f.x() + u.x() * uy - l.x() * rx, f.y() + u.y() * uy - l.y() * rx, f.z() + u.z() * uy - l.z() * rx).normalize();
		// as far as the fist, with the hand's and the world's FOV told apart (the same size on screen)
		double distance = v.length() * hudTan / worldTan;
		Vec3 at = camera.position().add(dir.scale(distance));
		// and in the camera's own frame (right, up, forward): HN places the object from the camera it renders with
		double n = Math.sqrt(1.0 + uy * uy + rx * rx);
		Passthrough.events.accept(String.format(Locale.ROOT,
			"{\"t\":\"hand\",\"p\":[%.4f,%.4f,%.4f],\"u\":[%.4f,%.4f,%.4f],\"l\":[%.4f,%.4f,%.4f],\"d\":%.4f}",
			at.x, at.y, at.z, u.x(), u.y(), u.z(), rx / n * distance, uy / n * distance, distance / n, distance));
	}
}
