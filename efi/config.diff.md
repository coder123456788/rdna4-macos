<!--
  config.diff.md

  Why this file exists: the ship package hands a reader an OpenCore config they
  can read line by line, and the owner has one binding rule — the machine's real
  serial/MLB/system UUID/ROM must never be published. A raw unified diff is the
  one artifact that would republish all four (the "-" side of section 1 and the
  "+" side of section 2), so both diffs below are reproduced with only the
  identifier tokens elided, marked "…". Nothing else in the text was touched:
  the hunk headers, the context lines and the remaining changed lines are
  byte-identical to `diff -u` output, so a reader can still account for every
  line. The elided values live only on the stick (E:\EFI\OC\*.plist) and in the
  two input copies kept outside this package at
  D:\Work\gate1\scratch-ship\.

  Every changed line is listed in the tables under each diff together with the
  rule that explains it. Anything that the redaction/ApplePanic/boot-args rules
  do NOT explain is filed under "Findings" instead of being quietly fixed.
-->

# config.diff.md

## Provenance and method

| role | file | size | md5 |
|---|---|---|---|
| stick's real current config ("before", section 1) | `E:\EFI\OC\config.plist` (copy kept at `D:\Work\gate1\scratch-ship\config.plist.orig-from-stick`) | 37794 | `360c78477fed9124165fe09a4cf96ddf` |
| shipped survey-only config ("after") | `D:\Work\gate1\ship\efi\config.plist.survey-only` | 37791 | see `../MD5SUMS` |
| armed config (section 2) | `E:\EFI\OC\config-armed.plist` (copy kept at `D:\Work\gate1\scratch-ship\config-armed.plist.from-stick`) | 37813 | `cdb00b10dcceadd23042aeb70d2dfbe2` |

`config.plist.survey-only` was produced by copying the stick's file to D: and
editing the copy in place — same line count (1553), same key order, same
indentation (tabs), no reformatting. Verified structurally with Python
`plistlib`: 669 leaf keys before and after, identical key sets, and exactly five
changed leaves (listed in section 1).

---

## Section 1 — the stick's real current config → `config.plist.survey-only`

`diff -u` of the two, identifier tokens elided as `…` on the left-hand side:

```diff
--- E:\EFI\OC\config.plist
+++ config.plist.survey-only
@@ -1162,7 +1162,7 @@
 			<key>AppleDebug</key>
 			<true/>
 			<key>ApplePanic</key>
-			<false/>
+			<true/>
 			<key>DisableWatchDog</key>
 			<true/>
 			<key>DisplayDelay</key>
@@ -1263,13 +1263,13 @@
 			<key>AdviseFeatures</key>
 			<true/>
 			<key>MLB</key>
-			<string>F5K0…</string>
+			<string>C027J1027J1</string>
 			<key>MaxBIOSVersion</key>
 			<false/>
 			<key>ProcessorType</key>
 			<integer>0</integer>
 			<key>ROM</key>
-			<data>vPzn…</data>
+			<data>000000000000</data>
 			<key>SpoofVendor</key>
 			<true/>
 			<key>SystemMemoryStatus</key>
@@ -1277,9 +1277,9 @@
 			<key>SystemProductName</key>
 			<string>MacPro7,1</string>
 			<key>SystemSerialNumber</key>
-			<string>F5KC…</string>
+			<string>C02TEST027J1</string>
 			<key>SystemUUID</key>
-			<string>BDF3…</string>
+			<string>00000000-0000-0000-0000-000000000000</string>
 		</dict>
 		<key>UpdateDataHub</key>
 		<true/>
```

Five changed lines, nothing else. Each one, with the rule that put it there and
the line it lands on in the shipped file:

| # | line | key | before → after | rule |
|---|---|---|---|---|
| 1 | 1164-1165 | `Misc>Debug>ApplePanic` | `false` → `true` | config-only half of the next experiment. OpenCore only calls `SavePanicLog` when this key is set (`OpenCoreMisc.c:777-779`, `if (Config->Misc.Debug.ApplePanic) { SavePanicLog (Storage); }`), and `SavePanicLog` (`OpenCoreMisc.c:325`) pulls `AAPL,panic-info-N` through `OcReadApplePanicLog` (`Library/OcBootManagementLib/ApplePanic.c:242`, variable name `AAPL,panic-info-%u` read in the loop at `:258-268`) and writes it to the OC partition as `panic-%04u-%02u-%02u-%02u%02u%02u.txt` (`OpenCoreMisc.c:346`). Note the plan cited the call site as `:777`; in the tree at `D:\Work\gate1\ocpkg-src\OpenCorePkg-master` the call is on line **778**, with the `if` on 777 — same statement. |
| 2 | 1265-1266 | `PlatformInfo>Generic>MLB` | real 15-char MLB → `C027J1027J1` | owner's rule: no MLB in the published config. The replacement is deliberately not a valid-looking Apple board serial. |
| 3 | 1271-1272 | `PlatformInfo>Generic>ROM` | real 6-byte ROM → `000000000000` | owner's rule: the ROM is the identity-bearing blob. See the encoding caveat in Findings. |
| 4 | 1279-1280 | `PlatformInfo>Generic>SystemSerialNumber` | real serial → `C02TEST027J1` | owner's rule. `C02TEST…` is the classic non-existent placeholder form. |
| 5 | 1281-1282 | `PlatformInfo>Generic>SystemUUID` | real UUID → all-zero UUID | owner's rule. |

Deliberately **unchanged**, for the reason given:

* `PlatformInfo>Generic>SystemProductName` = `MacPro7,1` — a stock Apple model
  identifier, not this machine's hardware identity; removing it would stop the
  file from being the config it claims to be. `PlatformInfo>Generic` is still
  present and complete, so the file parses (verified with `plistlib`).
* `boot-args` (line 1231-1232) — already exactly the survey-only string:
  `-v debug=0x100 keepsyms=1 revpatch=sbvmm npci=0x2000 slide=203 navi48bringup=1 navi48-interrupts=0 navi48-survey-report=1`.
  It carries `navi48-survey-report=1` plus the read-only args that were already
  there (`navi48bringup=1` is the kext's master enable, `Navi48Bringup.cpp:7729`;
  `navi48-interrupts=0` selects the polled path in `Navi48Bringup.cpp:7821-7822`
  and writes nothing), and it carries **none** of `navi48-accel-fire-at-boot`,
  `navi48-stage=17`, `navi48-psp=1`, `navi48-raw-report`. So the boot-args line
  is not in the diff at all — the stick's current config already met the rule.
* `ScanPolicy` (2166531), `Target` (67), `DisplayLevel` (2147483650) — explicit
  instruction not to touch them. Untouched; confirmed identical in both files.

**Patch 0030's raw channel**: `navi48-raw-report` appears in **no** config on
the stick (`grep -rl navi48-raw-report E:\EFI\OC\` returns nothing) and in no
config given as an input here, so there was nothing to remove. The argument name
itself is real — patch 0030 gates the block-device writer behind
`PE_parse_boot_argn("navi48-raw-report", ...)` (`0030-survey-progress-marks.patch`
lines 141-151) — it is simply not armed anywhere on disk.

---

## Section 2 — `config.plist.survey-only` → the armed config

### Which file is "armed", and why

Candidates found and rejected:

| candidate | boot-args | verdict |
|---|---|---|
| `D:\Work\MacOS on unsupported GPU's\KIT\config.plist.bak-20261029-110752-before-plistpath` | `… navi48bringup=1` | stale (Oct 29, pre-0029; no writing-stage args) |
| `…\KIT\config.plist.bak-20261029-195606-before-usbfix` | `… navi48bringup=1` | stale, same |
| `…\KIT\config.plist.bak-20261029-201126-before-xhci-injector` | `… navi48bringup=1` | stale, same |
| `…\KIT\config.plist.fixed-20261029-1110` | `… navi48bringup=1` | stale, same |
| `…\KIT\config.plist.clean-no-kext` | `-v debug=0x100 keepsyms=1 revpatch=sbvmm npci=0x2000 slide=203` (no navi48 at all) | matches the first boot of the day (`opencore-2026-10-31-092121.txt`), i.e. the no-kext control, not armed |
| anything under `D:\Work\gate1` | — | **no config plist exists anywhere under `D:\Work\gate1`** except the two input copies I made in `scratch-ship\`; searched with `find D:\Work\gate1 -iname "*config*.plist*"` |

Used as "armed": **`E:\EFI\OC\config-armed.plist`** (37813 bytes, md5
`cdb00b10dcceadd23042aeb70d2dfbe2`), for three reasons:

1. It is the only file on the stick named for the state, and it is byte-identical
   (`diff -q`) to `E:\EFI\OC\config-stock.plist` and to
   `E:\EFI\OC\config.plist.bak-20261031-152859-before-survey` — i.e. it is the
   pre-survey config the survey work started from.
2. It is one of only four stick configs whose `boot-args` carry the writing-stage
   set `navi48-stage=17 navi48-accel-experiment=1 navi48-accel-fire-at-boot=1
   navi48-report-hold=20`; the other three (`config-spoof73bf.plist`,
   `config-spoof7550.plist`, `config-stock.plist`) are either the same file or
   GPU-identity spoof experiments that inject `device-id` under
   `DeviceProperties>Add>` at the card's own PCI path
   (`PciRoot(0x0)/Pci(0x3,0x1)/…`, deliberately abbreviated here) with
   `0x73BF` / `0x7550` in little-endian, and so do not describe this card.
3. The logs corroborate it: every writing-stage boot of the day
   (`123147`, `125135`, `125912`, `130134`, `130817`) reports exactly this
   string in `#[EB|MBA:NV]` / `#[EB|MBA:OUT]`. None of the KIT backups matches
   any boot log.

If a *newer* armed config exists somewhere offline, it is not on this disk; the
comparison below is against the one that is.

### The diff (armed side elided as `…`)

```diff
--- config.plist.survey-only
+++ E:\EFI\OC\config-armed.plist
@@ -130,9 +130,9 @@
 				<key>Arch</key>
 				<string>x86_64</string>
 				<key>BundlePath</key>
-				<string>Navi48Bringup-survey.kext</string>
+				<string>Navi48Bringup.kext</string>
 				<key>Comment</key>
-				<string>Cobalt64 bring-up survey-report kext (patch 0029; writes NAVI48.TXT)</string>
+				<string>Cobalt64 bring-up survey kext (series 0013, 0.0.665)</string>
 				<key>Enabled</key>
 				<true/>
 				<key>ExecutablePath</key>
@@ -1162,7 +1162,7 @@
 			<key>AppleDebug</key>
 			<true/>
 			<key>ApplePanic</key>
-			<true/>
+			<false/>
 			<key>DisableWatchDog</key>
 			<true/>
 			<key>DisplayDelay</key>
@@ -1201,7 +1201,7 @@
 			<key>PasswordSalt</key>
 			<data></data>
 			<key>ScanPolicy</key>
-			<integer>2166531</integer>
+			<integer>0</integer>
 			<key>SecureBootModel</key>
 			<string>Disabled</string>
 			<key>Vault</key>
@@ -1229,7 +1229,7 @@
 			<key>7C436110-AB2A-4BBB-A880-FE41995C9F82</key>
 			<dict>
 				<key>boot-args</key>
-				<string>-v debug=0x100 keepsyms=1 revpatch=sbvmm npci=0x2000 slide=203 navi48bringup=1 navi48-interrupts=0 navi48-survey-report=1</string>
+				<string>-v debug=0x100 keepsyms=1 revpatch=sbvmm npci=0x2000 slide=203 navi48bringup=1 navi48-stage=17 navi48-accel-experiment=1 navi48-accel-fire-at-boot=1 navi48-report-hold=20</string>
 				<key>csr-active-config</key>
 				<data>AAAAAA==</data>
 				<key>prev-lang:kbd</key>
@@ -1263,13 +1263,13 @@
 			<key>AdviseFeatures</key>
 			<true/>
 			<key>MLB</key>
-			<string>C027J1027J1</string>
+			<string>F5K0…</string>
 			<key>MaxBIOSVersion</key>
 			<false/>
 			<key>ProcessorType</key>
 			<integer>0</integer>
 			<key>ROM</key>
-			<data>000000000000</data>
+			<data>vPzn…</data>
 			<key>SpoofVendor</key>
 			<true/>
 			<key>SystemMemoryStatus</key>
@@ -1277,9 +1277,9 @@
 			<key>SystemProductName</key>
 			<string>MacPro7,1</string>
 			<key>SystemSerialNumber</key>
-			<string>C02TEST027J1</string>
+			<string>F5KC…</string>
 			<key>SystemUUID</key>
-			<string>00000000-0000-0000-0000-000000000000</string>
+			<string>BDF3…</string>
 		</dict>
 		<key>UpdateDataHub</key>
 		<true/>
@@ -1401,7 +1401,7 @@
 				<key>Comment</key>
 				<string>NVRAM reset entry</string>
 				<key>Enabled</key>
-				<false/>
+				<true/>
 				<key>HideVerbose</key>
 				<false/>
 				<key>LoadEarly</key>
```

### Line by line: explained, or filed as a finding

| # | key | survey-only | armed | explained by the rules? |
|---|---|---|---|---|
| A | `Kernel>Add` entry 2: `BundlePath` + `Comment` | `Navi48Bringup-survey.kext` / "patch 0029; writes NAVI48.TXT" | `Navi48Bringup.kext` / "series 0013, 0.0.665" | **No — finding.** Pre-existing difference between the two configs, not produced by any rule here. |
| B | `Misc>Debug>ApplePanic` | `true` | `false` | **Yes.** The armed config predates the ApplePanic experiment; my change is the intended delta. |
| C | `Misc>Security>ScanPolicy` | `2166531` | `0` | **No — finding.** Pre-existing. Not one of the keys I was allowed to change, so I did not touch it on either side. |
| D | `boot-args` | survey set (`navi48-interrupts=0 navi48-survey-report=1`) | writing-stage set (`navi48-stage=17 navi48-accel-experiment=1 navi48-accel-fire-at-boot=1 navi48-report-hold=20`) | **Yes.** This is the whole point of the pair: the armed string is what a writing-stage boot uses, and all four of the arguments the survey-only config is forbidden to carry are present on the armed side and absent on ours. |
| E | `PlatformInfo>Generic` MLB/ROM/SystemSerialNumber/SystemUUID | placeholders | real values | **Yes.** Redaction. |
| F | `UEFI>Drivers` `ResetNvramEntry.efi>Enabled` | `false` | `true` | **No — finding.** Pre-existing. |

Cross-sections: 5 changed lines (5 keys) in section 1, 10 changed lines (6
areas: the kext entry, ApplePanic, ScanPolicy, boot-args, the four identity
keys, the NVRAM-reset driver) in section 2. A, C and F are the three areas in
section 2 that fall outside the stated rules.

---

## Findings (reported, not silently fixed)

1. **Kext bundle path differs between the two configs and from the built kext.**
   The shipped survey config loads `Navi48Bringup-survey.kext` (line 133) — that
   bundle does exist on the stick at `E:\EFI\OC\Kexts\Navi48Bringup-survey.kext`,
   so the config is not broken as written. But the kext built for this project
   is `D:\Work\gate1\build\kext-0030b\Navi48Bringup.kext` (hashed in
   `../MD5SUMS` section C), bundle name `Navi48Bringup.kext`. A reader who
   copies that build onto a stick and boots `config.plist.survey-only` will load
   **nothing**, because the `BundlePath` will not match. I left both the config
   and the kext name alone — renaming would change a line the rules do not
   cover, and it would break the correspondence with the logs, which were
   produced with the `-survey` bundle.
2. **`ScanPolicy` differs (2166531 vs 0).** The armed config scans everything,
   the survey config restricts scanning. Relevant to the "which disks does OC
   touch" question, and explicitly outside my remit to change. Also relevant to
   finding 3.
3. **`ResetNvramEntry.efi` is enabled in the armed config, disabled in the survey
   config.** An armed boot therefore shows an NVRAM-reset entry in the picker.
   Not changed.
4. **ROM is a `<data>` field: `000000000000` is base64, not hex.** The prescribed
   placeholder is 12 ASCII zeros; decoded, `<data>000000000000</data>` is the six
   bytes `D3 4D 34 D3 4D 34`, not six zero bytes (base64 of six zero bytes would
   be `AAAAAAAA`). I used the literal token that was prescribed, because it is
   unmistakably a placeholder and shares no byte with the real ROM, but a reader
   should know what the decoder sees. Flagged rather than "improved", since the
   exact replacement string was specified.
5. **No `MACAddress`, no `HardwareModel`, no `Booter>Secure`, no
   `NVRAM 4D1FDA02-…-rom-to-memory` key exists in this config at all**
   (`grep -n -i -E "MACAddress|HardwareModel|4D1FDA02"` finds nothing; the only
   `Secure` hits are `ProtectSecureBoot`, `ForceSecureBootScheme`,
   `SecureBootModel`, `AppleSecureBoot`, none of which encodes identity). There
   was nothing of that kind to blank, so nothing was blanked — the instruction
   covered a case this config does not have. The real ROM bytes are MAC-shaped
   (six bytes, the first three of which would be an OUI), which is why the scan
   in `../REDACT-SCAN.txt` also looks for that MAC in colon, dash, space and
   bare-hex forms, in both byte orders — so that a MAC-shaped leak would be
   caught even though the key it would normally live under does not exist here.
6. **`config.plist.survey-only` still names `PlatformInfo>Generic>SystemProductName`
   = `MacPro7,1`.** Intentional: a model name, not an identity.
7. **`README.md` (authored elsewhere, not by this task) points at the two config
   copies that this task deliberately kept out of the package.** It cites
   `efi/config.plist.orig-from-stick` and `efi/config-armed.plist.from-stick` as
   files "in this repository" — they are not, because they hold the machine's
   real serial/MLB/UUID/ROM and publishing them is exactly what the owner's rule
   forbids. They live at `D:\Work\gate1\scratch-ship\` and their md5s are in
   `../MD5SUMS` section D. The README also asserts that "the `MLB` field is
   still a real value" in the shipped copy — true of those two inputs, false of
   `efi/config.plist.survey-only`, which is the only config in this package.
   Whoever owns `README.md` has to update those four references; changing it
   from here would have been editing another writer's file mid-flight.
