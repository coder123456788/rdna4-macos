# make-report-target.ps1 - (re)create the 8 KiB NAVI48.TXT the kext's block-device channel needs.
#
#   powershell -NoProfile -File make-report-target.ps1 E:\NAVI48.TXT
#
# Why a pre-created file: the kext writes the report into a file that already exists, so it never
# has to touch a FAT or a directory entry. This script only creates the space; the first boot that
# writes a report replaces the first 1.5 KiB. Run it after a reformat of the stick.
param([Parameter(Mandatory = $true)][string]$Path)
$bytes = New-Object byte[] 8192
for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = 32 }
$line = [Text.Encoding]::ASCII.GetBytes("navi48 raw report target - do not edit")
[Array]::Copy($line, $bytes, $line.Length)
$bytes[$line.Length] = 10
[IO.File]::WriteAllBytes($Path, $bytes)
(Get-Item $Path).Length
