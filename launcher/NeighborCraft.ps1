# NeighborCraft launcher: install, update, uninstall and start NeighborCraft (Minecraft in Hello Neighbor).
# Double-click NeighborCraft.cmd next to this file. It does what hn/install.sh does, for people without a bash shell:
#   Install / Update  copies the mod (the UE4SS C++ mod, also the ReShade add-on), its shader and settings into
#                     Hello Neighbor and switches the ReShade preset to NeighborCraft (the old one is kept)
#   Uninstall         removes exactly that and restores the preset
#   Play              starts Hello Neighbor through Steam (it starts Minecraft itself)
# It never ships or downloads UE4SS, ReShade or game files: it only checks that UE4SS and ReShade are installed.
# The skin: a Minecraft username saved to %LOCALAPPDATA%\NeighborCraft\skin.txt (the mod fetches that player's public skin).

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$Repo = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $env:USERPROFILE 'NeighborCraft-lab\build\hn\NeighborCraftMod\NeighborCraftMod.dll'
$Data = Join-Path $env:LOCALAPPDATA 'NeighborCraft'
$SteamAppId = 521890

function Find-HelloNeighbor {
	# Steam's own folder, then every library Steam knows (libraryfolders.vdf)
	$roots = @()
	try { $roots += (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch {}
	$roots += 'C:\Program Files (x86)\Steam'
	foreach ($root in ($roots | Select-Object -Unique)) {
		$vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
		$libs = @($root)
		if (Test-Path $vdf) {
			$libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
		}
		foreach ($lib in $libs) {
			$hn = Join-Path $lib 'steamapps\common\Hello Neighbor'
			if (Test-Path (Join-Path $hn 'HelloNeighbor\Binaries\Win64')) { return $hn }
		}
	}
	return $null
}

function Get-Paths($hn) {
	$win64 = Join-Path $hn 'HelloNeighbor\Binaries\Win64'
	[pscustomobject]@{
		Win64   = $win64
		Mod     = Join-Path $win64 'ue4ss\Mods\NeighborCraftMod'
		Dll     = Join-Path $win64 'ue4ss\Mods\NeighborCraftMod\dlls\main.dll'
		Shaders = Join-Path $win64 'reshade-shaders\Shaders'
		Preset  = Join-Path $win64 'ReShadePreset.ini'
	}
}

function Get-Md5($file) {
	if (Test-Path $file) { return (Get-FileHash $file -Algorithm MD5).Hash.ToLower() }
	return $null
}

function Install-NeighborCraft($p) {
	if (-not (Test-Path (Join-Path $p.Win64 'ue4ss'))) { throw "UE4SS isn't installed in $($p.Win64)" }
	if (-not (Test-Path $p.Shaders)) { throw "ReShade (with its shaders) isn't installed in $($p.Win64)" }
	if (-not (Test-Path $Build)) { throw "No built mod at $Build (build it first: hn\build.bat)" }
	if (Get-Process -Name 'HelloNeighbor*' -ErrorAction SilentlyContinue) { throw 'Close Hello Neighbor first: the mod is locked while it runs' }
	New-Item -ItemType Directory -Force (Join-Path $p.Mod 'dlls') | Out-Null
	Copy-Item $Build $p.Dll -Force
	Copy-Item (Join-Path $Repo 'hn\NeighborCraft_grips.txt') (Join-Path $p.Mod 'dlls\NeighborCraft_grips.txt') -Force
	if (-not (Test-Path (Join-Path $p.Mod 'enabled.txt'))) { New-Item -ItemType File (Join-Path $p.Mod 'enabled.txt') | Out-Null }
	$start = Join-Path $Repo 'tools\start_minecraft.bat'
	Set-Content -Path (Join-Path $p.Mod 'minecraft_launch.txt') -Value "cmd.exe /c `"$start`"" -Encoding ascii
	Copy-Item (Join-Path $Repo 'hn\shaders\NeighborCraft.fx') (Join-Path $p.Shaders 'NeighborCraft.fx') -Force
	if ((Test-Path $p.Preset) -and -not (Select-String -Path $p.Preset -Pattern '^Techniques=NeighborCraft@NeighborCraft.fx' -Quiet)) {
		if (-not (Test-Path "$($p.Preset).neighborcraft-bak")) { Copy-Item $p.Preset "$($p.Preset).neighborcraft-bak" }
		(Get-Content $p.Preset) -replace '^Techniques=.*', 'Techniques=NeighborCraft@NeighborCraft.fx' | Set-Content $p.Preset -Encoding ascii
	}
	# the same file, checked (a copy once "succeeded" while the file stayed locked)
	if ((Get-Md5 $Build) -ne (Get-Md5 $p.Dll)) { throw 'The installed mod differs from the built one: is Hello Neighbor still closing?' }
}

function Uninstall-NeighborCraft($p) {
	if (Get-Process -Name 'HelloNeighbor*' -ErrorAction SilentlyContinue) { throw 'Close Hello Neighbor first' }
	if (Test-Path $p.Mod) { Remove-Item -Recurse -Force $p.Mod }
	$fx = Join-Path $p.Shaders 'NeighborCraft.fx'
	if (Test-Path $fx) { Remove-Item -Force $fx }
	if (Test-Path "$($p.Preset).neighborcraft-bak") { Move-Item -Force "$($p.Preset).neighborcraft-bak" $p.Preset }
}

# ---- the window ----
$form = New-Object System.Windows.Forms.Form
$form.Text = 'NeighborCraft'
$form.Size = New-Object System.Drawing.Size(560, 420)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$font = New-Object System.Drawing.Font('Segoe UI', 10)
$form.Font = $font

$title = New-Object System.Windows.Forms.Label
$title.Text = 'NeighborCraft'
$title.Font = New-Object System.Drawing.Font('Segoe UI', 16, [System.Drawing.FontStyle]::Bold)
$title.Location = '20,12'; $title.AutoSize = $true
$form.Controls.Add($title)

$status = New-Object System.Windows.Forms.Label
$status.Location = '20,55'; $status.Size = '510,130'
$form.Controls.Add($status)

$skinLabel = New-Object System.Windows.Forms.Label
$skinLabel.Text = 'Minecraft username for your skin:'
$skinLabel.Location = '20,195'; $skinLabel.AutoSize = $true
$form.Controls.Add($skinLabel)
$skinBox = New-Object System.Windows.Forms.TextBox
$skinBox.Location = '20,220'; $skinBox.Size = '250,25'
$form.Controls.Add($skinBox)
$slimBox = New-Object System.Windows.Forms.CheckBox
$slimBox.Text = 'slim arms'; $slimBox.Location = '285,220'; $slimBox.AutoSize = $true
$form.Controls.Add($slimBox)
$skinSave = New-Object System.Windows.Forms.Button
$skinSave.Text = 'Save skin'; $skinSave.Location = '400,217'; $skinSave.Size = '120,30'
$form.Controls.Add($skinSave)

$install = New-Object System.Windows.Forms.Button
$install.Text = 'Install / Update'; $install.Location = '20,270'; $install.Size = '160,40'
$form.Controls.Add($install)
$uninstall = New-Object System.Windows.Forms.Button
$uninstall.Text = 'Uninstall'; $uninstall.Location = '190,270'; $uninstall.Size = '160,40'
$form.Controls.Add($uninstall)
$play = New-Object System.Windows.Forms.Button
$play.Text = 'Play'; $play.Location = '360,270'; $play.Size = '160,40'
$play.Font = New-Object System.Drawing.Font('Segoe UI', 11, [System.Drawing.FontStyle]::Bold)
$form.Controls.Add($play)

$note = New-Object System.Windows.Forms.Label
$note.Location = '20,325'; $note.Size = '510,40'; $note.ForeColor = 'DimGray'
$form.Controls.Add($note)

$script:HN = Find-HelloNeighbor

function Refresh-Status {
	if (-not $script:HN) {
		$status.Text = "Hello Neighbor: not found in your Steam libraries.`r`nInstall it through Steam, then reopen this window."
		$install.Enabled = $uninstall.Enabled = $play.Enabled = $false
		return
	}
	$p = Get-Paths $script:HN
	$ue4ss = Test-Path (Join-Path $p.Win64 'ue4ss')
	$reshade = Test-Path $p.Shaders
	$built = Get-Md5 $Build
	$installed = Get-Md5 $p.Dll
	$state = if (-not $installed) { 'not installed' } elseif ($installed -eq $built) { 'installed, up to date' } else { 'installed, an update is available' }
	$status.Text = "Hello Neighbor: $($script:HN)`r`n" +
		"UE4SS: $(if ($ue4ss) { 'installed' } else { 'MISSING (needed)' })`r`n" +
		"ReShade: $(if ($reshade) { 'installed' } else { 'MISSING (needed)' })`r`n" +
		"NeighborCraft: $state`r`n" +
		"Skin: $(if (Test-Path (Join-Path $Data 'skin.png')) { 'skin.png' } elseif (Test-Path (Join-Path $Data 'skin.txt')) { (Get-Content (Join-Path $Data 'skin.txt') -TotalCount 1) } else { 'classic Steve' })"
	$install.Enabled = $ue4ss -and $reshade -and [bool]$built
	$uninstall.Enabled = [bool]$installed
	$play.Enabled = [bool]$installed
}

if (Test-Path (Join-Path $Data 'skin.txt')) {
	$lines = Get-Content (Join-Path $Data 'skin.txt')
	$skinBox.Text = ($lines | Select-Object -First 1)
	$slimBox.Checked = [bool]($lines | Where-Object { $_.Trim() -eq 'slim' })
}

$skinSave.Add_Click({
	New-Item -ItemType Directory -Force $Data | Out-Null
	$lines = @($skinBox.Text.Trim())
	if ($slimBox.Checked) { $lines += 'slim' }
	# plain UTF-8, no byte-order mark (Windows PowerShell's utf8 writes one, and the mod read it as part of the name)
	[System.IO.File]::WriteAllLines((Join-Path $Data 'skin.txt'), [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
	Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Data 'skin_cache.png'), (Join-Path $Data 'skin_cache.txt')
	$note.Text = 'Skin saved: used the next time Hello Neighbor starts. (A skin.png in ' + $Data + ' wins over the username.)'
	Refresh-Status
})
$install.Add_Click({
	try { Install-NeighborCraft (Get-Paths $script:HN); $note.Text = 'Installed. Press Play.' }
	catch { $note.Text = "Not installed: $($_.Exception.Message)" }
	Refresh-Status
})
$uninstall.Add_Click({
	$r = [System.Windows.Forms.MessageBox]::Show('Remove NeighborCraft from Hello Neighbor? (UE4SS, ReShade and your saves stay.)', 'NeighborCraft', 'YesNo')
	if ($r -ne 'Yes') { return }
	try { Uninstall-NeighborCraft (Get-Paths $script:HN); $note.Text = 'Removed. Hello Neighbor is back to normal.' }
	catch { $note.Text = "Not removed: $($_.Exception.Message)" }
	Refresh-Status
})
$play.Add_Click({
	Start-Process "steam://rungameid/$SteamAppId"
	$note.Text = 'Starting Hello Neighbor through Steam. Minecraft starts by itself (hidden) and joins in.'
})

Refresh-Status
[void]$form.ShowDialog()
