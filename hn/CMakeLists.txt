cmake_minimum_required(VERSION 3.22)
project(NeighborCraftHN)

# RE-UE4SS source checked out at the commit of the UE4SS build installed in the game (outside this repo).
set(UE4SS_DIR "$ENV{USERPROFILE}/NeighborCraft-lab/deps/RE-UE4SS" CACHE PATH "RE-UE4SS checkout")
add_subdirectory("${UE4SS_DIR}" RE-UE4SS)
add_subdirectory(NeighborCraftMod)
