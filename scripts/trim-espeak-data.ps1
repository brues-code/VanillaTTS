# This file is part of VanillaTTS.
#
# VanillaTTS is free software: you can redistribute it and/or modify it under the terms
# of the GNU General Public License as published by the Free Software Foundation, either
# version 3 of the License, or (at your option) any later version.
#
# Produces an English-only copy of espeak-ng-data for shipping beside
# VanillaTTS_synth.dll. The full data dir is ~18 MB (≈130 language
# dictionaries); English needs only en_dict plus the shared phoneme tables
# (phondata/phonindex/phontab/...), intonations, lang/, and voices/ — about
# 2 MB. We keep everything EXCEPT the non-English *_dict files.
#
# Usage:
#   scripts\trim-espeak-data.ps1 [-Source <full espeak-ng-data>] [-Dest <out dir>]
# Defaults: Source = the standalone espeak build output; Dest = dll_local.

param(
    [string]$Source = "$PSScriptRoot\..\external\espeak-ng\build\espeak-ng-data",
    [string]$Dest   = "C:\WoW\Octo\dll_local\espeak-ng-data"
)

if (-not (Test-Path $Source)) { throw "espeak-ng-data not found at: $Source (build espeak-ng first)" }

if (Test-Path $Dest) { Remove-Item $Dest -Recurse -Force }
New-Item -ItemType Directory -Path $Dest -Force | Out-Null

foreach ($item in Get-ChildItem -LiteralPath $Source) {
    if ($item.PSIsContainer) {
        # Keep all subdirs (lang/, voices/, ...) — small and language-defining.
        Copy-Item -LiteralPath $item.FullName -Destination $Dest -Recurse -Force
    } elseif ($item.Name -like '*_dict' -and $item.Name -ne 'en_dict') {
        # Drop non-English dictionaries (the bulk).
    } else {
        # Keep shared phoneme tables, intonations, en_dict, etc.
        Copy-Item -LiteralPath $item.FullName -Destination $Dest -Force
    }
}

$mb = [math]::Round((Get-ChildItem $Dest -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 2)
$n  = (Get-ChildItem $Dest -Recurse -File | Measure-Object).Count
Write-Output "Trimmed espeak-ng-data -> $Dest : $n files, $mb MB"
