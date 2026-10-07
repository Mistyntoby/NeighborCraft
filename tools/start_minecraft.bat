@echo off
rem Starts the NeighborCraft Minecraft client (Fabric dev client, its own game folder) for Hello Neighbor.
rem Hello Neighbor's mod runs this when it starts (minecraft_launch.txt); Minecraft quits when Hello Neighbor does.
set JAVA_HOME=%USERPROFILE%\NeighborCraft-lab\deps\jdk25
cd /d "%~dp0..\mc"
call gradlew.bat runClient -PexitWithHost=true > "%USERPROFILE%\NeighborCraft-lab\mc-client.log" 2>&1
