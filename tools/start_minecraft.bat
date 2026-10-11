@echo off
rem Starts the NeighborCraft Minecraft client (Fabric dev client, its own game folder) for Hello Neighbor.
rem Hello Neighbor's mod runs this when it starts (minecraft_launch.txt); Minecraft quits when Hello Neighbor does.
set JAVA_HOME=%USERPROFILE%\NeighborCraft-lab\deps\jdk25
cd /d "%~dp0..\mc"
rem --no-daemon: no Gradle daemon outlives the game (after a crash one stayed, and Steam showed the game "Stopping"
rem and wouldn't start it again until it was ended)
call gradlew.bat --no-daemon runClient -PexitWithHost=true > "%USERPROFILE%\NeighborCraft-lab\mc-client.log" 2>&1
