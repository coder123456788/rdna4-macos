# rdna4-macos — Navi 44 (Radeon RX 9060 XT, `1002:7590`) on Intel macOS

**Status: this does not work. No Metal device has enumerated and the desktop has never
composited on this card.** For this project, success means a Metal device enumerates and the
desktop composites on the GPU, with `ioreg` output as the evidence. That has not happened,
and nothing in this repository achieves it or provides a way around it. What exists is a
read-only survey: a kext that reads the card, formats what it read, and tries to leave that
record on the boot stick — plus the host tests and a list of dead ends. Every claim below is
either measured on the card, reproduced on a host compiler, or marked unproven.

Read this first if you have the same card and a working Hackintosh or Mac Pro: this will
not make your GPU accelerate. It will tell you what the die reports, and it will save you
from repeating the four experiments in section 7.

## 1. Status

| Area | Status | Evidence |
|---|---|---|
| Metal device enumerated | **does not work** — never observed | no `ioreg` capture of an `IOAccelerator`/`IOMetalCore` child of this card exists in this repository |
| Desktop composited on the GPU | **does not work** — never observed | nothing in this repository shows a GPU-composited desktop; the kit states that a recovery boot draws through Apple's generic framebuffer — one fixed mode, software rendering, no Metal (`START-HERE.md`:19–20) |
| Reading the card across PCIe | **works** — at least once | the values in section 3 are what one survey read of this card returned |
| Survey record left on the boot stick (`NAVI48.TXT`) | **does not work** | `logs/NAVI48.TXT` is still the untouched 8,192-byte placeholder — byte-identical to the snapshot taken before patch 0029 was installed (`logs/INDEX.md`) |
| Patches 0029/0030 (progress marks) on hardware | **unproven** — built, host-tested, never booted | no boot log here carries a 0030 progress line, and no boot wrote `NAVI48.TXT` (`logs/INDEX.md`) |
| Kext builds to a loadable Mach-O bundle | **works** | the `kext-0030b` build of §4.4: 7,270,384 B, `MH_KEXT_BUNDLE`; digests in `MD5SUMS` §C |
| Register-writing stages (`navi48-stage=17`, `navi48-psp=1`) | **unproven, and not attempted by this port's survey work** | section 5 keeps them off on purpose |
| Host suites | **works** — off-hardware only | section 6: 349 + 116 + 769 checks, 0 failures |
| Kext loaded at all | **not provable from this repository** | no `Navi48`/`N48LOG` line exists in any of the 16 boot logs (`logs/INDEX.md`) |

Three of those rows are the whole story: the card answers (row 3), the answer cannot be carried
home (row 4), and the GPU is never handed to macOS (rows 1–2). Everything else is either work
that supports those three facts or work that has not been run yet.

## 2. What this is, and why RDNA 4 on macOS is hard

Apple's own AMD drivers end with RDNA 2 — that sentence is upstream's, in `README.md` of the
Cobalt64 tree this work patches. RDNA 3 and RDNA 4 have no Apple driver at all: nothing that
knows how to initialise this die's engine, program its display, or provide Metal for it. On a
supported card macOS brings the GPU up through a driver Apple wrote; here there is nothing to
configure, because the code that would do the configuring was never written for this silicon.

That leaves a port with two jobs, and this project is stuck between them:

1. **Bring the GPU up itself.** PCIe and BAR mapping, IP discovery (the die's own table of
   which hardware blocks exist and where), PSP firmware loading (the security processor that
   must run before anything else touches the engine), then SMU, GMC, GFX, SDMA, MES and DCN
   (the display engine). Every sequence is re-derived from the Linux `amdgpu` driver and from
   mac-amdgpu's Navi 48 work, on a die nobody has run macOS on.
2. **Hand the running GPU to something that can draw.** Upstream Cobalt64 does this on
   Navi 48 by instantiating Apple's `AMDRadeonX6000` accelerator class and translating its
   command stream (`src/xlat12`). Apple's accelerator only matches device ids its
   personalities list, and none of them lists `0x7590` (START-HERE-INSTALL.md in the kit).

Terminology used below, once:

- **kext** — kernel extension, a Mach-O bundle the kernel loads.
- **BAR5** — the PCI base address register that maps the GPU's register file; reading it is
  how the survey talks to the die.
- **IP discovery** — a table the die exposes in its trusted memory region listing every IP
  block (GC, PSP, DCN, …) with a version and its register segment bases.
- **PSP** — Platform Security Processor; loads and authenticates the firmware the other
  engines run.
- **DCN** — Display Core Next, the display engine.
- **TTL** — upstream's table of entry points into Apple's accelerator; a hook that lets a
  foreign driver answer Apple's own calls.

What this work contains is the part that could be done without winning that fight: a patch
series over upstream (`0000`–`0028`), then `0029` and `0030`, whose job is to make the survey
write itself to the stick, because on this machine the verbose console is gone before the boot
ends (`src/navi48-bringup/src/survey_report.h`:5–8).

One reading convention, because it affects every citation below: paths that begin
`src/navi48-bringup/` or `src/` are into the Cobalt64 tree **after** the series in section 4
is applied. Neither that tree nor the series is among the files shipped in this package — the
"Sources in this repo" list at the end says exactly what is. The paths are here so that you
can open the line and check the claim once you have the same tree.

## 3. The card, as measured

One survey boot read the following from the card. The only copy of that read in this package
is the formatter fixture — a test that pins the exact values so a later change cannot silently
drop a fact (`src/navi48-bringup/tests/survey_report_test.cpp`). The survey's own console
output is not here either: OpenCore's log carries OpenCore's and the booter's lines, not a
kext's (`logs/INDEX.md`), and the text had scrolled away before it could be read — which is
why patches 0029/0030 exist.

| Fact | Value | Pinned at |
|---|---|---|
| vendor / device | `0x1002` / `0x7590` | `survey_report_test.cpp`:58–59 |
| BAR5 | 16 MiB (`16777216` bytes) | `survey_report_test.cpp`:61 |
| VRAM | `16368` MiB | `survey_report_test.cpp`:62 |
| IP discovery | valid, 46 IPs, table at TMR offset `0x00034900` | `survey_report_test.cpp`:64–65 |
| GC (graphics) | v12.0.0, 2 segment bases | `survey_report_test.cpp`:72 |
| PSP | v14.0.2, 15 segment bases | `survey_report_test.cpp`:74 |
| DCN (display) | v4.1.0, 5 segment bases | `survey_report_test.cpp`:79 |
| geometry | `GB_ADDR_CONFIG=0x0000a008`, decoded `SE=4 RB/SE=2`, die caps `2/2` | `survey_report_test.cpp`:109–111 |
| firmware set chosen | `navi44`, via IP discovery, `confirmed=1` | `survey_report_test.cpp`:66–68 |

Two cautions, both from the same file:

- The fixture fills ten IP rows so the formatter's worst case is exercised. The rows for
  MMHUB, SMU, SDMA, IH, NBIF, HDP and UMC, the DMU segments, the framebuffer location and the
  PSP mailbox values are **formatter inputs, not claims about the card**. Only the fields in
  the table above are stated as read.
- The decoded geometry and the die caps disagree in the printed line
  (`SE=4 RB/SE=2 (die caps 2/2)`). Nothing in these files reconciles them; the runbook in the
  kit expects `2 / 2 / 4` and 32 CUs from the GC info table on a 9060 XT
  (RECOVERY.md, kit). Treat the register decode as an open question, not a result.

For the record, the kext's own match list does carry the card's id, so binding is not the
missing piece: `IOPCIPrimaryMatch` is `0x75501002 0x75511002 0x75901002`
(`src/navi48-bringup/Info.plist`:89–90), and `probe()` refuses to run without
`navi48bringup=1` (`src/Navi48Bringup.cpp`:7729).

## 4. Build it

Built and verified on Windows with no Mac, no Xcode and no Apple SDK: zig's bundled clang
compiles Mach-O objects, LLVM's `ld64.lld` links them against MacKernelSDK, and a Python step
rewrites the Mach-O header. The recipe is upstream's own build, cross-targeted. The script
that runs it is `xbuild-kext.sh`; in the owner's workspaces it sits in `KIT/` and
`gate0/tools/`, with a `build-kext.sh` wrapper at the gate 0 root that pins the linker
version. Neither script is among the files shipped in this package. Section 4.3 lists the five
steps it performs, in order; nothing else is hidden in it, so if you do not have the script
those five steps are the whole build.

### 4.1 Toolchain — versions matter

| Tool | Version verified | Note |
|---|---|---|
| zig | `0.15.2` (`zig version`) | provides `zig cc` / `zig c++` as a Mach-O-capable clang |
| ld64.lld | **LLD 20.1.0** (`ld64.lld --version`) | see 4.3 — this one is not optional |
| Python | `3.12.10` | the post-link header patch |
| MacKernelSDK | third-party, not in this repo | kernel headers plus `libkmod` / `libkmodc++` |

MacKernelSDK is a separate project (RDNA4FB ships a copy;
`https://github.com/somestupidgirl/RDNA4FB`). Clone or download it yourself and point the
build at it. Do not expect it in this repository.

### 4.2 Apply the series

The series was verified against Cobalt64 commit
`d1f7bad8408a0a2c016b067c477422975a59de44`; all 29 patches `0000`–`0028` apply cleanly to it
(gate 0 notes §2 — the build record, not shipped here), and `0029`/`0030` apply on top. The
diffs are LF; a CRLF checkout makes `patch` refuse them with `(different line endings)` — that
failure is recorded in `scratch-0029-dryrun-0029.log` in the gate 1 workspace. Check with
`file src/navi48-bringup/src/survey_report.h` before blaming the patch.

```sh
git clone <Cobalt64> cobalt64
cd cobalt64
git checkout d1f7bad8408a0a2c016b067c477422975a59de44
for p in /path/to/patches/00[01][0-9]-*.patch \
         /path/to/patches/002[0-8]-*.patch \
         /path/to/0029-*.patch \
         /path/to/0030-*.patch; do
    patch -p1 < "$p" || { echo "FAILED: $p"; exit 1; }
done
```

The first two globs are `0000`–`0028`, deliberately split so the two loose patches are
applied exactly once each and from their own directory. Applied in that order the series ends
at `0030`; `patch` prints one line per file and stops at the first rejection.

Cobalt64's own README is the only source here for where to get it — it links `cobalt64.com` —
so the clone URL is left as a placeholder rather than invented. Check that the checkout is at
the commit above before applying anything: the series was written against that tree.

Use `patch -p1` (or `git apply`), not `git am`: `0000`–`0013` and `0029`/`0030` are bare
diffs without mail headers.

`0029` is `0029-survey-report-to-the-stick.patch`; `0030` is
`0030-survey-progress-marks.patch`. Neither diff is among the files shipped here; `MD5SUMS`
§C records their sizes and digests — `0029` 38,720 B, MD5
`09e605fbbece2429f267de8a27b13f65`; `0030` 41,635 B, MD5
`e9816a1a906b15ca9bc01ed699bfa5fb` — so you can verify a copy before applying it.

### 4.3 Build and link

```sh
export ZIG=/path/to/zig-0.15.2/zig.exe
export LD64=/path/to/llvm-20.1.0/bin/ld64.lld.exe     # NOT the 18.x one
export NM=/path/to/llvm-20.1.0/bin/llvm-nm.exe
/path/to/xbuild-kext.sh /path/to/cobalt64 /path/to/MacKernelSDK /path/to/out
```

The three exports are the whole contract: the script takes the tree, MacKernelSDK and an output
directory as arguments, and reads `ZIG`, `LD64`, `NM` from the environment when they are set.

The critical, easy-to-get-wrong part: **link with LLD 20.1.0.** `xbuild-kext.sh` defaults
`LD64` to a sibling `llvm-bin` directory that holds LLD 18.1.8; that build is not the same
artifact. Linking the identical objects with LLD 18.1.8 produced **7,425,512 bytes instead of
7,270,384** — a 155 KB "phantom delta" that looks like a source change and is not. Either set
`LD64` explicitly as above, or use the `gate0/build-kext.sh` wrapper, whose whole body is those
exports plus a call to the script — that wrapper is what produced the numbers below. If your
result is 7,425,512 B, you linked with the wrong LLD; nothing else about your tree is wrong.

What the script does, in order (this is the part to reproduce if you do not have it):

1. embeds `firmware/*.bin` and `shaders/*.bin` as C arrays with
   `src/navi48-bringup/tools/embed-firmware.py`, writing `src/fw/fw_*.c` **into the source
   tree** — build from a copy you do not mind modifying;
2. compiles 81 objects with `-target x86_64-macos.11.0 -mkernel -fapple-kext -Os -g`
   and `-std=c++17 -fno-exceptions -fno-rtti -fcheck-new`;
3. links with `ld64.lld -kext -undefined dynamic_lookup -no_deduplicate`, `-L Library/universal`
   before `-L Library/x86_64`;
4. patches the Mach-O header: `ld64.lld` accepts `-kext` and ignores it, so the output is
   `MH_EXECUTE`(2) with `LC_MAIN` and `LC_LOAD_DYLINKER`. The script drops both and sets
   filetype 11 (`MH_KEXT_BUNDLE`). `LC_MAIN` is `0x80000028` (`LC_REQ_DYLD` set), **not**
   `0x28` — filtering on `0x28` drops nothing and the bundle stays unloadable;
5. refuses to finish if any symbol of ours is undefined (`llvm-nm -u`).

### 4.4 The artifact

| | |
|---|---|
| path | `build/kext-0030b/Navi48Bringup.kext` in the gate 1 build tree — **not shipped here** |
| binary | `Contents/MacOS/Navi48Bringup`, **7,270,384 bytes** |
| binary MD5 | `281906f04321b428beab8d62bce96701` |
| `Info.plist` MD5 | `cbc157cc8835a6e32d32ba2a93217dca` (unchanged since 0029) |
| Mach-O | filetype 11, ncmds 12, sizeofcmds 1800, no `LC_MAIN`, no `LC_LOAD_DYLINKER` |

`MD5SUMS` §C carries these digests plus the two earlier builds; `kext-0030` is the same
sources before the review fixes, 7,270,304 B. The full build record is `0030-build-notes.md`,
gate 1 — also outside this package. Both exist as provenance, not as downloads: they let you
tell whether the bundle you built is the one described here.

Check yours with:

```sh
md5sum Navi48Bringup.kext/Contents/MacOS/Navi48Bringup
python -c "import struct;d=open('Navi48Bringup.kext/Contents/MacOS/Navi48Bringup','rb').read();print('filetype',struct.unpack('<I',d[12:16])[0],'ncmds',struct.unpack('<I',d[16:20])[0])"
```

Expect `filetype 11`. One caveat on the hash: the binary embeds the absolute source paths of
the build in STABS strings, so an exact MD5 also requires the same absolute build path. A
build elsewhere carries the same code and a different hash (gate 0 notes §5, item 2).

The kext is **unsigned**. It loads only on a Mac with reduced security and through OpenCore's
kext injection (`Kernel > Add` with a `PlistPath`); with SIP enabled it is refused silently.
Adding it to `EFI/OC/Kexts` and the config is described in the kit's `START-HERE.md` and
`RECOVERY.md`.

## 5. Run it

### 5.1 Boot arguments for a survey boot

The survey configuration boots read-only. These are the arguments it used, verbatim from the
config shipped here (`efi/config.plist.survey-only`:1231–1232):

```
-v debug=0x100 keepsyms=1 revpatch=sbvmm npci=0x2000 slide=203 navi48bringup=1 navi48-interrupts=0 navi48-survey-report=1
```

- `navi48bringup=1` lets `probe()` match and start at all (`src/Navi48Bringup.cpp`:7729).
- `navi48-interrupts=0` keeps interrupts off and falls back to the polled path
  (`src/Navi48Bringup.cpp`:7819–7822); interrupts are on by default.
- `navi48-survey-report=1` turns the survey report on (`src/accel_takeover.cpp`:291).
- Add `navi48-raw-report=1` only when you deliberately want the block-device channel
  (`src/accel_takeover.cpp`:286). Patch 0030 made it opt-in precisely so a boot without it
  cannot write through raw media.

Do **not** add `navi48-stage=N` or `navi48-psp=1` to a survey boot. Those are the writing
stages — registers, VRAM and the PSP mailbox (`src/Navi48Bringup.cpp`:871–878; the PSP stage
also turns itself on whenever `navi48-stage>=5`). The survey needs none of it.

One mismatch costs a boot if you skip it: `efi/config.plist.survey-only`:133 loads
`Navi48Bringup-survey.kext`, the bundle name that was on the stick, while a fresh build from
section 4 is called `Navi48Bringup.kext`. Match one to the other — rename the folder you copy
into `EFI/OC/Kexts`, or edit `BundlePath` and `ExecutablePath` (`:133`–`:139`) — otherwise
OpenCore injects nothing and the boot looks exactly like an unloaded kext
(`efi/config.diff.md`, finding 1). Check that first if a boot after this section changes
nothing.

### 5.2 The two report channels

`NAVI48.TXT` sits in the root of the boot stick (a FAT32 data partition, 8.3 name, an 8,192-byte
placeholder created by `make-report-target.ps1` in the kit). The kext fills its **first 1,536
bytes** and leaves the rest (`src/accel_report_file.h`:28–32). Two ways out:

1. **Mounted volume** — `/Volumes/OPENCORE/NAVI48.TXT`, opened with `vnode_open` and written
   with `vn_rdwr` (`src/accel_takeover.cpp`:197–200). Works only after `diskarbitrationd`
   mounts the stick. On this machine it never did during a survey boot.
2. **The file's own data cluster, through `IOMedia`** — no mount, no filesystem driver. The
   kext walks the FAT32 BPB, finds the root entry named `NAVI48.TXT`, and overwrites the first
   1,536 bytes of that file's first cluster. It accepts a device only after five checks (media
   size class, FAT32 BPB in sector 0, volume label `OPENCORE`, the exact 8.3 entry, and a file
   whose first four bytes are `navi`); the FAT, the root directory and every other cluster stay
   read-only (`src/accel_report_raw.h`:10–16, error codes `-1`…`-5` at `:27–32`).

Patch 0029 tried the mounted volume for 30 s before the block device. Patch 0030 cut that
grace to 1 s (`kSurveyVfsGraceSeconds = 1`, pinned in `tests/survey_report_test.cpp`:324)
because no survey boot ever mounted the stick in time.

### 5.3 What `NAVI48.TXT` should look like

A completed survey writes this, one fact per line, fixed length. The text below is generated
by the formatter from the fixture of section 3 — not hand-written — so a file that differs in
shape is telling you something:

```
navi48 survey report - patch 0030; written by the kext, no photograph needed
card: vendor 0x1002 device 0x7590 navi44=1
bar5: 16777216 bytes
vram: 16368 MiB
discovery: valid=1 ips=46 offset=0x00034900
firmware: navi44 via IP discovery, confirmed=1
ip: GC v12.0.0 bases=2: 00001260 00001261
ip: MMHUB v1.0.0 bases=4: 00000000 00000001 00000002 +1
ip: PSP v14.0.2 bases=15: 00000000 00000001 00000002 +12
ip: SMU v14.0.2 bases=1: 00016000
ip: SDMA v7.0.0 bases=2: 0001a000 0001a001
ip: IH v7.0.0 bases=2: 0001c000 0001c001
ip: NBIF v6.3.1 bases=1: 00000000
ip: DCN v4.1.0 bases=5: 00000012 00000013 00000014 +2
ip: HDP v7.0.0 bases=1: 0000d000
ip: UMC v8.0.0 bases=2: 00014000 00014001
dcn: dmu segs=5: 00000012 000000c0 000034c0 00009000 02403c00 allowlist=ARM rebased=0 reason=ok
geometry: GB_ADDR_CONFIG=0x0000a008 SE=4 RB/SE=2 (die caps 2/2)
fb_location: base=0x00000001 top=0x00000111 mc=0x0000000001000000 top=0x0000000111000000
psp: C2PMSG_35=0x00000000 C2PMSG_81=0x00000000 C2PMSG_64=0x00000000
mode: survey only; interrupts=0 (armed=0); accel-experiment=0 fire-at-boot=0
writes: none requested beyond the IP-discovery read (no stage, no PSP mailbox, no accel fire)
end of survey report - fixed 1536-byte record, rewritten every boot
```

A boot that dies before the survey completes leaves the **progress line** instead, which is
the whole point of patch 0030 (`src/survey_report.h`:283–304):

```
navi48 survey progress: start=1 mmio=0 discovery=0 report=0; patch 0030 - the full record replaces this line when the survey completes
```

It is written on the first mark and rewritten as the boot advances, so `mmio=1 discovery=0`
says the boot mapped BAR5 and died before IP discovery was parsed. `report=1` means the full
record has replaced the line.

If the file still begins with `navi48 raw report target - do not edit`, the kext never wrote
on that boot — that is the state of the copy in this repository (`logs/NAVI48.TXT`, 8,192 B).

### 5.4 Making the next boot explain itself

Two facts are missing, and OpenCore can carry one of them home without a photo of the console:

- **Boot a config that has the panic log on.** `Misc > Debug > ApplePanic` was `false` in every
  config that booted, so a kernel panic could not leave anything behind (`efi/config.diff.md`,
  section 1 row 1; `logs/INDEX.md`, last paragraph). The config shipped here already sets it
  `<true/>` (`efi/config.plist.survey-only`:1164–1165, one of the five edits recorded in
  `efi/config.diff.md`). There is nothing for you to change: booting that config is the step.
  On the **next** boot OpenCore reads the Apple panic variables and writes
  `panic-YYYY-MM-DD-HHMMSS.txt` next to its own `opencore-*.txt` logs (OpenCorePkg:
  `SavePanicLog`, `OpenCoreMisc.c`:325, called under `if (Config->Misc.Debug.ApplePanic)` at
  `:777–779`, name format at `:346`; the reader is `OcReadApplePanicLog` with variables
  `AAPL,panic-info-%u`, `ApplePanic.c`:242). A panic during boot *N* therefore produces its
  file during boot *N+1* — one extra power cycle between you and the answer.
- **Read the progress marks.** They are the other half of the same experiment (section 8).

## 6. What the host tests cover, and what they cannot

These run on any machine — no Mac, no GPU. All three totals below were re-run for this README
and match the build record (`0030-build-notes.md`, gate 1, outside this package).

| Suite | Checks | Failures | Command (from `src/navi48-bringup`) |
|---|---|---|---|
| `survey_report_test` | 349 | 0 | `$ZIG c++ -std=c++17 -Wall -Wextra -O1 -I src tests/survey_report_test.cpp -o /tmp/survey_report_test.exe && /tmp/survey_report_test.exe .` |
| `accel_takeover_report_test` | 116 | 0 | `$ZIG c++ -std=c++17 -Wall -Wextra -O1 -I tests/dalshim -I src tests/accel_takeover_report_test.cpp -o /tmp/accel_takeover_report_test.exe && /tmp/accel_takeover_report_test.exe .` |
| `navi44_die_test firmware` | 769 | 0 | see below |

`navi44_die_test` needs the embedded firmware objects compiled first, or it is vacuous:

```sh
mkdir -p /tmp/fw
for f in src/fw/*.c; do $ZIG cc -std=c11 -O1 -c "$f" -o "/tmp/fw/$(basename "$f" .c).o"; done
$ZIG c++ -std=c++17 -Wall -Wextra -O1 -I hosttest/shim -I src/amd -I src/fw \
    -o /tmp/navi44_die_test.exe tests/navi44_die_test.cpp src/amd/fw_table.cpp src/amd/gfx_v12_0.cpp /tmp/fw/*.o
/tmp/navi44_die_test.exe firmware     # argv[1] is the firmware DIRECTORY
```

The argument is a directory. Point it at a path that does not exist and every size check fails
with `want 18446744073709551615` (that is `(size_t)-1` from a missing file, not a silicon
fact) — 80 failures out of 769, which reads like a broken port and is a broken command line.

The suites are backed by mutation plants: scripts that plant one break at a time into the real
sources and fail if the suite stays green. `accel_takeover_report_plant.sh` caught 51 and let 0
escape; `accel_report_file_plant.sh` 35/0; `accel_report_raw_plant.sh` 44/0, with 0
plant-errors each time (`0030-build-notes.md`; logs in the gate 1 workspace).

**Why this is not enough.** All of it compiles the product sources against shim headers and
checks strings and formatter output; none of it compiles the kext. This gap let a real defect
through a fully green suite: two new log lines referenced `kReportFilePath` unqualified inside
an anonymous namespace. The suites pin strings, not C++ name lookup, so they stayed green over
a file that would not compile into the kext. The Mach-O cross-build is the only coverage of
`Navi48Bringup.cpp` — run it after touching that file.

## 7. Dead ends already explored

Each of these cost a boot or more. None of them produced acceleration.

- **Spoofing `0x7550`.** `0x7550` is not an id Apple lists, so injecting it cannot produce a
  takeover; the script that did it is kept and renamed `use-spoof7550.bat.do-not-use.txt`
  (kit), and `RECOVERY.md`:46–48 says so in one line. This port's own `IOPCIPrimaryMatch` still
  lists `0x75501002`, which is why the spoof looked plausible.
- **Waiting for Apple's accelerator to take the card by itself.** A recovery image does not
  contain `AMDRadeonX6000`, so the TTL hook cannot install and `LoadAccelerator` is never
  published: the handover cannot even be attempted from recovery (`START-HERE.md`, section 8;
  `START-HERE-INSTALL.md`:3–7). Even installed, none of Apple's personalities lists `0x7590`
  (`START-HERE-INSTALL.md`:69–70).
- **The `0x73BF` pair boot.** Apple's own id for the accelerator class this port instantiates,
  injected together with the kext's four match lists (`use-73bf-pair.bat`, kit). It puts
  Apple's own HW-services and Navi 21 framebuffer personalities into the match; per
  `START-HERE-INSTALL.md`:110–144 that is the next boot to try after a refused or TTL-less one.
  There is no record in these files of it being booted with a takeover result.
- **Picker / scan-policy trouble.** Logs #13 and #14 (`opencore-2026-10-31-164255.txt`,
  `-164435.txt`) hold two lines each: `OCB: System has no boot entries`, then `OC: Failed to
  show boot menu!`, 100 s apart. That is all they say. `logs/INDEX.md` reads them as "the
  picker had nothing to boot", equally consistent with a stick whose APFS driver cannot mount
  or with a freshly partitioned stick, and explicitly does not attribute the failure to the
  card. Two config facts are worth carrying away regardless: the survey config keeps
  `ScanPolicy = 2166531` while the armed config on the stick sets `0`, i.e. scan every
  filesystem, and the armed config also enables the `ResetNvramEntry.efi` picker entry
  (`efi/config.diff.md`, findings 2 and 3). Whether an NTFS volume was being misclassified is
  **not stated in any file available here** — do not treat this bullet as a diagnosis.

Safety rules that constrained all of the above, and should constrain you: survey boots are
read-only; no register-writing stage on a survey config; nothing written to an internal disk —
the kit's `START-HERE.md`:329–335 names the two scripts that would touch the installed volume
and says to leave them alone; a spare USB stick is the only writable target; and armed boots
are capped at three before the approach changes (gate 0 notes §7, §9).

## 8. What the next experiment decides

The last boot of the day is in this package, and it is the one that matters here:
`logs/opencore-2026-10-31-202533.txt`. It is a survey boot — line 59 echoes the argument
string through `MBA:NV` unchanged, `navi48-survey-report=1` included — it reaches the handoff
(`LOG:DT`, then `45:421 ... LOG:EXITBS:START 2026-10-31T20:27:05`, which is the last line of
the file), and it wrote nothing to `NAVI48.TXT`. The session after it,
`logs/opencore-2026-10-31-202748.txt`, holds 0 text lines — 262,144 NUL bytes — the same
reset signature as the earlier pairs in `logs/INDEX.md`. The config on the stick names
`Navi48Bringup-survey.kext` (`efi/config.plist.survey-only`:133–139) — the 0029 bundle, whose
writer waited 30 s for a mount and then wrote through the block device. Whether that bundle
actually loaded is not knowable from these logs (`logs/INDEX.md`). Two explanations survive:

- the writer never ran, or
- the raw block-device write is what reset the machine.

Neither is proven. No panic log exists to separate them, because `ApplePanic` was `false` for
every boot in that set (`logs/INDEX.md`; `efi/config.diff.md`, section 1 row 1). The next
experiment is built to separate them, and it is already compiled:

| Change | What it decides |
|---|---|
| `ApplePanic` on — already set in `efi/config.plist.survey-only`:1164–1165 | a panic on the survey boot leaves `panic-*.txt` next to the OpenCore log on the boot after it, which names the faulting path |
| patch 0030's progress marks | whether `start()` ran at all (`start=1`), reached MMIO (`mmio=1`), finished discovery (`discovery=1`), or completed (`report=1`) |
| 0030's 1 s volume grace | the mount gets one second, not thirty, so the timing of a reset relative to the write changes |
| `navi48-raw-report=0` (default) | the block-device channel stays off entirely: a boot that still resets with it off exonerates the raw write |

Combinations and what they mean:

- **Progress line, no reset** — the writer works, the channel works; the survey simply did not
  finish. Read the marks and fix the stage that stopped.
- **Progress line, then reset** — the channel is fine and the reset is downstream of a stage
  the file names. That is the first real diagnosis this project would have.
- **No file, panic log present** — read the panic; the fault is where the log stops.
- **No file, no panic log** — the writer never reached the media, and the next patch belongs
  in the writer, not in the bring-up ladder.

Patches 0029 and 0030 have been built and host-tested but **never booted**. Nothing in this
section has happened yet.

## 9. Licence, notice, and what was redacted

- The Cobalt64 tree this is a patch series over is **MIT** (`LICENSE` at its root,
  "Copyright (c) 2026 Navi48-MacOS contributors").
- The bring-up kext `src/navi48-bringup` is **BSD 3-Clause** ("Copyright (c) 2026 the Navi48
  project"). Its `NOTICE` credits RDNA4FB (BSD 3-Clause) for the IP-discovery and BAR5 survey
  technique, and mac-amdgpu (MIT) for the IP-block bring-up sequences; Linux `amdgpu`
  (GPL-2.0) is used as documentation only.
- The firmware blobs under `src/navi48-bringup/firmware/` come from `linux-firmware` and keep
  that project's licence. They are not covered by the kext's BSD licence.
- Nothing here is affiliated with or endorsed by AMD or Apple. Kernel extensions write
  directly to hardware; a wrong write can hang or reset the machine.

**Redaction: what was removed from the config, and how to check it.** An OpenCore config is
one machine's identity: `PlatformInfo > Generic` carries that machine's `SystemSerialNumber`,
`MLB`, `SystemUUID` and `ROM`, and with real values in them a config is not a configuration
detail but a fingerprint. The config shipped here has all four replaced with visible
placeholders: serial `C02TEST027J1`, MLB `C027J1027J1`, all-zero UUID, ROM
`<data>000000000000</data>` (`efi/config.plist.survey-only`:1265–1282), and
`efi/config.diff.md` §1 shows the before and after of each line. The two unredacted originals
are deliberately outside this package
(`MD5SUMS` §D), and `REDACT-SCAN.txt` is the check that no real value survived: seven
patterns built from the actual serial, MLB, UUID and ROM in every encoding the config could
carry them, 0 hits, 0 leaks.

Two caveats on that file before you reuse it. The ROM placeholder is `<data>`, which is base64:
`000000000000` decodes to the six bytes `D3 4D 34 D3 4D 34`, not six zero bytes
(`efi/config.diff.md`, finding 4). And `SystemProductName` is still `MacPro7,1` on purpose —
a stock Apple model name, not this machine's identity. If you copy your own config into a fork,
run the same kind of check on it before you push.

## Sources in this repo

The files below are the package, in the order you are likely to want them. Digests and sizes
for every one of them are in `MD5SUMS` §A and §B, so a claim here can be checked against the
bytes rather than trusted.

- `README.md` — this file. It is not hashed in `MD5SUMS`, which was written while this file was
  still being revised; hash it yourself if you need to pin it.
- `efi/config.plist.survey-only` — the survey-only OpenCore config with the placeholders:
  boot args at 1231–1232, `ApplePanic` at 1164–1165, `ScanPolicy` at 1203, kext `BundlePath`
  at 133, identity keys at 1265–1282. This is the config to boot.
- `efi/config.diff.md` — every change made to that config (five lines), the diff against the
  armed config on the stick, and six findings including the `BundlePath` mismatch and the
  `ScanPolicy` difference.
- `logs/INDEX.md` — one row per boot: boot clock, args, last line, whether a reset follows,
  and what the log cannot prove. Read it before reading the logs.
- `logs/opencore-2026-10-31-*.txt` — all 16 OpenCore logs, byte for byte, 262,144 B each.
  `-164255` and `-164435` are the "no boot entries" pair; `-202533` is the last survey boot
  and `-202748` its 0-line successor.
- `logs/NAVI48.TXT` — the report target as recovered: untouched, 8,192 B, first line
  `navi48 raw report target - do not edit`, MD5 `9b42aa2aca6b86cec82f5d1af2e2e43d`.
- `MD5SUMS` — §A and §B hash the files above; §C and §D are provenance only, for things not
  shipped: the three kext builds, the two diffs, and the two unredacted configs.
- `REDACT-SCAN.txt` — the identity scan over this package, and its verdict.

What IS in this package, alongside the three sections above:

- `patches/0000-*.patch` ... `patches/0030-*.patch` - the 31-patch series (0000-0028 from the
  `KIT/` runbook, 0029 and 0030 from the gate 1 work). Section 4 is the only correct way to
  apply them: `git -c core.autocrlf=true apply`, one patch at a time, `--check` first. The
  series is LF-bodied and the tree it patches is CRLF, so on a macOS or Linux default checkout
  30 of the 31 fail with "different line endings" unless autocrlf is pinned; and `git am`
  rejects 0029/0030 because they carry no `From`/`Subject` header. Verified: with that pin the
  whole series applies and all 93 touched files then byte-match the tree that was built and
  tested (after `tr -d '`\r`'). The `From 05ae8f484bd0b32d8f0bded0ac5bc721d0dcf972` line in
  the patch headers names a commit that is not reachable in the upstream repository - the series
  was generated against a local tree, and the tarball it applies to is `Cobalt64` upstream
  `main` at `d1f7bad`. That is a defect in the headers, recorded here rather than fixed, because
  editing 31 patches by hand would create a worse one.
- `kext/Navi48Bringup.kext` - the built bundle (patch 0030 with the review fixes, unsigned).
- `build/` - `xbuild-kext.sh` and its wrapper, `build-survey.sh`, `mesa-host-tests.sh`,
  `make-report-target.ps1`, and `0030-build-notes.md` (toolchain versions and artifact MD5s).
- `kext-navi48-tests/` - the host suites as they stand in the tested tree, so the numbers in
  section 6 can be re-run without applying the series first.

Nothing here identifies the machine: `efi/config.plist.survey-only` carries placeholder
SerialNum/MLB/Uuid/Rom, and the identity scan over the whole package (seven encodings of the
four real values, across every file including the 7.27 MB binary and the 16 boot logs) returned
zero hits. See `REDACT-SCAN.txt`.
