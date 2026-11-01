<!--
  logs/INDEX.md

  Why this file exists: the package ships every boot log the machine produced,
  including the ones that prove nothing and the ones that show a boot dying.
  A pile of 256 KB files is not evidence on its own, so this index says, per
  file, what the boot actually did and what the log can and cannot prove.

  Three things about these files a reader has to know before reading them:

  1. Every log is exactly 262144 bytes because OpenCore writes into a
     preallocated region. Unused space is NUL (0x00). Line counts below are
     "lines of text actually present"; a "0-line" log is 262144 NUL bytes and
     nothing else. Those are kept, not trimmed, because an empty log after a
     handoff attempt is the evidence.
  2. The timestamp in the filename is EFI clock; the AAPL lines carry the
     Apple booter's clock, which agrees with the filename; the FAT directory
     mtime is one hour ahead (local time vs EFI UTC). Do not read the mtime as
     the boot time.
  3. There are no N48LOG / kext lines in any of these files. OpenCore's log
     target carries OC's own messages and whatever the Apple booter prints; a
     kext's IOLog output goes to the kernel log, not here. So no log in this
     directory proves that Navi48Bringup ever loaded — see the last section.
-->

# logs/INDEX.md

16 OpenCore logs from `E:\` — **not** from `E:\EFI\OC\Logs\`, which does not
exist on the stick; the OC file target was pointed at the volume root. Plus
`NAVI48.TXT` copied from `E:\NAVI48.TXT`. Plus the three files under
`D:\Work\gate1\logs\` which are byte-identical duplicates of logs already in
this directory (verified with `cmp`; md5 in `../MD5SUMS`), so they are not
duplicated again here under a second name.

## The table

"Args" is the set reported verbatim in `#[EB|MBA:OUT]`:

* **stock** = `-v debug=0x100 keepsyms=1 revpatch=sbvmm npci=0x2000 slide=203` (no navi48 argument at all)
* **survey** = stock + `navi48bringup=1 navi48-interrupts=0 navi48-survey-report=1`
* **armed** = stock + `navi48bringup=1 navi48-stage=17 navi48-accel-experiment=1 navi48-accel-fire-at-boot=1 navi48-report-hold=20`

| # | log | boot clock (in-log `LOG:INIT`) | booter / target | args | last meaningful line | reset? | what it proves / fails to prove |
|---|---|---|---|---|---|---|---|
| 1 | `opencore-2026-10-31-092121.txt` | 2026-10-31T09:22:02 | `bootbase.efi 583~5906` | stock | `51:676 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T09:22:53` | **yes** — followed by log #2, 0 lines | Proves the machine reaches a macOS handoff with no navi48 argument on the wire at all: the string is identical to `KIT\config.plist.clean-no-kext`. Proves nothing about the driver — with no navi48 argument the kext cannot arm anything, and this log cannot say whether it loaded. |
| 2 | `opencore-2026-10-31-092642.txt` | — | — | — | *(none: 262144 NUL bytes)* | **is** the reset | The signature itself: 0 lines in the session immediately after #1 ended at `EXITBS:START`. Proves the previous boot did not survive `ExitBootServices`. |
| 3 | `opencore-2026-10-31-092802.txt` | 2026-10-31T09:29:11 | `bootbase.efi 583~5906` | **survey** | `45:711 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T09:29:57` | **yes** — followed by #4, 0 lines | First log in the set that shows the survey argument string on the wire, carried through unmodified (MBA:NV == MBA:OUT). Proves the config's survey args reach the booter. Does not prove the kext ran. |
| 4 | `opencore-2026-10-31-123024.txt` | — | — | — | *(none: all NUL)* | **is** the reset | Reset immediately after #3's handoff. |
| 5 | `opencore-2026-10-31-123056.txt` | — | — | — | *(none: all NUL)* | **is** the reset | A second empty session, immediately after the empty #4: the machine came back up twice after #3's handoff and OpenCore wrote nothing either time. The reset inference rests on #3, not on this file. |
| 6 | `opencore-2026-10-31-123147.txt` | 2026-10-31T12:32:28 | `bootbase.efi 583~5906` | **armed** | `46:585 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T12:33:15` | no empty log follows | First writing-stage boot: `navi48-stage=17 navi48-accel-fire-at-boot=1` reach the booter. Proves the armed config of record (`E:\EFI\OC\config-armed.plist`) matches what actually booted. Fails to prove anything after handoff — nothing was logged. |
| 7 | `opencore-2026-10-31-125135.txt` | 2026-10-31T12:52:18 | `bootbase.efi 583~6005`, installer: KF `-rootdmg-ramdisk auth-root-dmg=file:///macOS Install Data/BaseSystem.dmg lca-boot-mode=autoinstall-msu` | **armed** | `51:859 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T12:52:27` | no empty log follows | This is a boot into **macOS Install Data**, not the installed system. Proves the armed run was an installer boot. The first 8 lines are OC's APFS driver failing `efi_container_create` (status `0x8000…07`) before the picker; ~43 s pass between OC start (00:000) and Apple's `LOG:INIT`. |
| 8 | `opencore-2026-10-31-125912.txt` | 2026-10-31T13:00:14 | `bootbase.efi 583~6005`, recovery: KF `-restore -logOnCrash -rootdmg-ramdisk …x86_64SURamDisk.dmg` | **armed** | `67:245 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T13:00:20` | no empty log follows | Armed **recovery/restore** boot. Proves the same armed string also went to the installer's recovery environment. |
| 9 | `opencore-2026-10-31-130134.txt` | 2026-10-31T13:07:38 | `boot.efi 583~6005`, KF `-bsdmgroot` | **armed** | `369:080 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T13:07:43` | no empty log follows | Armed boot from a BaseSystem dmg root, i.e. still not the installed volume. 6 min of picker time before Apple's `LOG:INIT`. |
| 10 | `opencore-2026-10-31-130817.txt` | 2026-10-31T13:08:51 | `boot.efi 583~6005`, KF empty | **armed** | `42:131 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T13:08:59` | no empty log follows | The only armed log that looks like a boot of the **installed** volume (no installer flags). Still stops at `EXITBS:START`. |
| 11 | `opencore-2026-10-31-131116.txt` | — (no `LOG:INIT`) | — | — | `00:310 AAPL: efi_container_create:1107: -efi_container_create, Status = 8000000000000007` | no | 8 lines, all of them OC's APFS driver failing to create a container, four times within 310 ms. The macOS booter never started in this session as far as this file shows. Proves the APFS driver could not read whatever it was pointed at; proves nothing about the GPU. |
| 12 | `opencore-2026-10-31-163037.txt` | — | — | — | *(none: all NUL)* | empty log, but **not** the EXITBS signature | This empty session follows #11, which never reached `EXITBS:START`. It is an empty log after an APFS failure — a weaker finding than #2/#4/#5 and should not be counted as a GPU reset. |
| 13 | `opencore-2026-10-31-164255.txt` | — (OC only) | — | — | `00:456 OC: Failed to show boot menu!` preceded by `OCB: System has no boot entries` | no | Two lines. OC found **no boot entries at all**. Proves the picker had nothing to boot — consistent with a stick whose APFS driver cannot mount, or a freshly partitioned stick; proves nothing about the card. |
| 14 | `opencore-2026-10-31-164435.txt` | — (OC only) | — | — | `00:024 OC: Failed to show boot menu!` preceded by `OCB: System has no boot entries` | no | Same two lines, 100 s later. Repeat of #13. |
| 15 | `opencore-2026-10-31-202533.txt` | 2026-10-31T20:26:20 | `bootbase.efi 583~5906` | **survey** | `45:421 AAPL: #[EB\|LOG:EXITBS:START] 2026-10-31T20:27:05` | **yes** — followed by #16, 0 lines | The last survey boot of the day, and the one that matters for the ApplePanic experiment: it is a survey boot, it ends at `EXITBS:START`, and the next session logged nothing. If the machine reset here, an `AAPL,panic-info-N` variable may exist in NVRAM — which is exactly what `ApplePanic=true` will try to harvest on the *next* boot. |
| 16 | `opencore-2026-10-31-202748.txt` | — | — | — | *(none: all NUL)* | **is** the reset | Closes the set. Nothing was ever logged after the last handoff. |

## The reset signature, stated plainly

No log in this directory contains `LOG:EXITBS:END`. Every boot that reached the
handoff prints `LOG:DT` and then `LOG:EXITBS:START` and stops. That is true of
all eight non-empty Apple-booter logs (#1, #3, #6, #7, #8, #9, #10, #15). So:

* **Established from these files:** five sessions produced a 0-byte log (#2, #4,
  #5, #12, #16), and four of them directly follow an `EXITBS:START` (#2→#1,
  #4→#3, #5→#4, #16→#15). That pair is the reset signature: the handoff was
  attempted, the machine came back up, and OpenCore had nothing to write.
* **Not established:** that the machine reset after #6–#10. Those boots also end
  at `EXITBS:START`, but the next log in each case has content, so the logs
  themselves do not say what happened between handoff and the next session. Do
  not read "ends at EXITBS:START" as "reset" on its own.

## `NAVI48.TXT` — did anything ever write it?

Copied from `E:\NAVI48.TXT`, now at `logs/NAVI48.TXT`, 8192 bytes,
md5 `9b42aa2aca6b86cec82f5d1af2e2e43d`.

* It begins with the ASCII magic line `navi48 raw report target - do not edit`
  and then **only 0x20 space bytes** to the end of the 8192-byte region. It
  contains **no NUL bytes at all** (checked: `tr -cd '\000' | wc -c` = 0).
* It is byte-identical (`cmp`) to `D:\Work\gate1\NAVI48.TXT.before-0029`, the
  snapshot taken **before** patch 0029 was installed, and has the same md5.
* **Conclusion, unhappy but honest: no boot has ever written this file.** The
  pre-created magic is intact. Patch 0029's survey-report channel has produced
  zero bytes on the stick across every boot in this directory, including the two
  survey boots that carried `navi48-survey-report=1` (#3 at 09:29 and #15 at
  20:26). That is the open question of 0029/0030 and it stays open: the stick
  file is evidence of *nothing having been written*, not evidence of a successful
  write. (Patch 0030's raw block-device channel is opt-in behind
  `navi48-raw-report=1`, which appears in no config and in no log here.)

## What these logs cannot be used for

* **They cannot show the kext loaded or ran.** There is no `Navi48`, `N48LOG` or
  `navi48bringup` driver line anywhere in the 16 files (grep: 0 files). The only
  navi48 text in any of them is the boot-args string the firmware was handed.
* **They cannot show a GPU enumeration.** No `PciRoot`/`VenHw`/device-path line
  appears in any of the 16 (grep: 0 files), so they neither confirm nor leak the
  machine's PCI topology.
* **They cannot locate the panic.** No `panic-*.txt` file exists on the stick and
  `Misc>Debug>ApplePanic` was `false` for every boot in this set (see
  `../efi/config.diff.md`); the config shipped here is the first one with it
  enabled, so the first panic log this project can read is the one from the
  *next* boot, not from any boot in this directory.
