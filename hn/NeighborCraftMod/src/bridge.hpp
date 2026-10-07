// The Hello Neighbor half of the passthrough, run on the game thread after every engine tick.
#pragma once

namespace nc
{
	/// Starts the Minecraft link (background thread). Call once, when Unreal is initialised.
	void start(void *module);
	/// Every engine tick, on the game thread.
	void tick(float delta_seconds);
	void stop();
}
