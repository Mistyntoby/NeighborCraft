// ReShade add-on half: uploads Minecraft's latest frame (shared memory "Local\NeighborCraftFrame") into textures
// that NeighborCraft.fx composites into Hello Neighbor's picture against its depth buffer.
// Adapted from universal-modder's minecraft-gta5-passthrough compositor (MIT, see THIRD_PARTY_NOTICES.md).
#pragma once

namespace compositor
{
	/// Registers with ReShade once it is loaded; call until it returns true.
	bool try_register(void *module);
	void unregister(void *module);
	/// Whether to composite this frame (off in menus and loading screens).
	void set_active(bool active);
	/// HN's pause menu is up: Minecraft's world stays composited, its overlay (its own pause screen) and hand don't.
	void set_paused(bool paused);
	/// The host camera's clip planes in metres (Unreal: near only, the far plane is infinite).
	void set_host_planes(float near_clip, float far_clip);
	/// The host's current camera in Minecraft's convention (rotation in degrees, vertical fov, position in Minecraft
	/// coordinates): Minecraft's frame is re-projected (with its depth) from the pose it was rendered with to this
	/// one, hiding the link's latency on camera turns and moves.
	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z);
	/// How many poses back the presented picture is (0: the pose read after the world tick is the one drawn).
	void set_pose_lag(int frames);
	/// Composite Minecraft's picture as rendered, without re-projecting it to the host's newer pose.
	void set_camera_locked(bool locked);
	/// A scene's look: the effect's light matching, depth bias and slope bias (negative: the preset's own values).
	void set_look(float light_match, float depth_bias, float slope_bias);
	/// Camera shake applied to the finished picture (x, y: fraction of the screen height; roll: radians), and a
	/// full-screen warp amount (0..1).
	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp);
	/// The colour of the host's light (its sun's LightColor, 0..1): what tints Minecraft's surfaces.
	void set_light_colour(float r, float g, float b);
	/// The direction towards the host's sun in Minecraft's world (unit) and how strongly it shines (0: no sun).
	void set_sun(float x, float y, float z, float strength);
	/// The host's backbuffer size as ReShade sees it (0 until the first frame).
	void backbuffer_size(int &width, int &height);
}
