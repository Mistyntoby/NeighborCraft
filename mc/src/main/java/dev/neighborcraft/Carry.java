package dev.neighborcraft;

import java.util.Locale;
import net.minecraft.core.component.DataComponents;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.network.chat.Component;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.sounds.SoundEvents;
import net.minecraft.sounds.SoundSource;
import net.minecraft.world.InteractionHand;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.component.CustomData;
import net.minecraft.world.item.component.SwingAnimation;
import net.minecraft.world.phys.AABB;

/**
 * Hello Neighbor's objects in Minecraft's inventory. The object itself stays in HN's world (hidden, without collision or
 * physics) while Minecraft holds a token for it: an hn_object item named after it, with its id (HN's handle for that
 * very object) and its class (to make a new one if the object is gone, e.g. another level). Every step is confirmed:
 * HN takes the object out of its world only after the token is in the inventory ("picked" ok), and the token goes
 * only after HN put the object back ("consume"), so nothing is lost or doubled when a step fails.
 */
public final class Carry {
	private Carry() {
	}

	/** The token's data, or an empty tag for other items. */
	public static CompoundTag data(final ItemStack stack) {
		if (stack.isEmpty() || !stack.is(Passthrough.HN_OBJECT)) {
			return new CompoundTag();
		}

		CustomData custom = stack.get(DataComponents.CUSTOM_DATA);
		return custom == null ? new CompoundTag() : custom.copyTag();
	}

	public static String id(final ItemStack stack) {
		return data(stack).getStringOr("id", "");
	}

	/**
	 * HN picked an object up: its token into the hand (the selected slot, which must be empty: one object at a time, held
	 * until placed or thrown), then HN is told; only then the hand swings and the pickup sound plays, once.
	 */
	public static void give(final String id, final String name, final String cls, final int kg) {
		MinecraftServer s = WorldBridge.server();
		if (s == null) {
			reply(id, false, "no world");
			return;
		}

		s.execute(() -> {
			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			if (player == null) {
				reply(id, false, "no player");
				return;
			}

			Inventory inventory = player.getInventory();
			for (int i = 0; i < inventory.getContainerSize(); i++) {
				if (id.equals(id(inventory.getItem(i)))) {
					reply(id, true, "already there"); // a repeated message: never a second token
					return;
				}
			}

			if (!inventory.getSelectedItem().isEmpty()) {
				Passthrough.LOG.info("carry: {} not taken: the hand holds {}", id, inventory.getSelectedItem());
				reply(id, false, "hand not empty");
				return;
			}

			ItemStack token = new ItemStack(Passthrough.HN_OBJECT);
			CompoundTag tag = new CompoundTag();
			tag.putString("id", id);
			tag.putString("cls", cls);
			tag.putInt("kg", kg);
			token.set(DataComponents.CUSTOM_DATA, CustomData.of(tag));
			token.set(DataComponents.ITEM_NAME, Component.literal(name));
			inventory.setItem(inventory.getSelectedSlot(), token);
			Passthrough.LOG.info("carry: {} ({}) into the hand (slot {})", name, id, inventory.getSelectedSlot());
			reply(id, true, "");
			// as when Minecraft picks an item up: the sound (to everyone, this player too) and the hand's swing
			player.swing(InteractionHand.MAIN_HAND, SwingAnimation.DEFAULT, true);
			player.level().playSound(null, player.getX(), player.getY(), player.getZ(), SoundEvents.ITEM_PICKUP, SoundSource.PLAYERS, 0.4F,
				(player.getRandom().nextFloat() - player.getRandom().nextFloat()) * 0.7F + 1.6F);
		});
	}

	/** HN put the object back into its world: its token goes (wherever it is: a slot, the cursor). */
	public static void consume(final String id) {
		MinecraftServer s = WorldBridge.server();
		if (s == null) {
			return;
		}

		s.execute(() -> {
			ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
			if (player == null) {
				return;
			}

			Inventory inventory = player.getInventory();
			for (int i = 0; i < inventory.getContainerSize(); i++) {
				if (id.equals(id(inventory.getItem(i)))) {
					inventory.setItem(i, ItemStack.EMPTY);
					Passthrough.LOG.info("carry: {} back in HN's world, token removed from slot {}", id, i);
					return;
				}
			}

			if (id.equals(id(player.containerMenu.getCarried()))) {
				player.containerMenu.setCarried(ItemStack.EMPTY);
				return;
			}

			Passthrough.LOG.warn("carry: {} back in HN's world, but no token for it was found", id);
		});
	}

	/**
	 * Every server tick: a token Minecraft dropped as an item entity (thrown out of the inventory screen) goes back into
	 * the inventory and HN throws the real object instead, so the object never lies around as a Minecraft item.
	 */
	public static void tick(final MinecraftServer s) {
		ServerPlayer player = s.getPlayerList().getPlayers().isEmpty() ? null : s.getPlayerList().getPlayers().get(0);
		if (player == null) {
			return;
		}

		ServerLevel level = player.level();
		for (ItemEntity entity : level.getEntitiesOfClass(ItemEntity.class, new AABB(player.blockPosition()).inflate(16.0),
			e -> e.getItem().is(Passthrough.HN_OBJECT))) {
			ItemStack stack = entity.getItem().copy();
			String id = id(stack);
			entity.discard();
			if (player.getInventory().add(stack)) {
				Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"throwreq\",\"id\":\"%s\"}", id));
			}
		}
	}

	private static void reply(final String id, final boolean ok, final String why) {
		Passthrough.events.accept(String.format(Locale.ROOT, "{\"t\":\"picked\",\"id\":\"%s\",\"ok\":%b,\"why\":\"%s\"}", id, ok, why));
	}
}
