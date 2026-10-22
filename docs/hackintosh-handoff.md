# SURYA / iOS 16 Hackintosh handoff

Updated 2026-10-22. Read this first, then read the active handoff at the top of
`NOTES.md` and the graphics assessment linked below.

## Project target and current boot

- Device: Poco X3 NFC (`surya`), Qualcomm SM7150, 6 Kryo 470 Silver/A55 plus 2
  Kryo 470 Gold/A76 cores, Adreno 618 (`0x06010800`), Tianma Novatek
  NT36672C touch panel.
- OS target: iOS 16.7.16 / Darwin 22, using the `ios-sweet`-derived XNU 8796
  port. The active fork is `https://github.com/arbuxiktech/ios-surya`, branch
  `codex/surya-port`.
- The owner reports that the latest Apple-root boot with `cpus=8` reached the
  Lock Screen/home screen. The GUI builder defaults to `cpus=8`; the payload
  validator requires all eight SM7150 CPU nodes (MPIDR `0x0` through `0x700`),
  and patch `0078-sweet-smp-8-cores.patch` raises XNU's limit to eight. This
  confirms the eight-core configuration boots to GUI, but there is no captured
  runtime `8/8 CPUs initialized` report yet. The SMP driver logs each CPU_ON
  and SGI test; retrieve those logs on the next diagnostic boot to confirm all
  seven secondaries are online.
- The existing lock-screen GUI uses the CPU renderer. Touch firmware was
  separately loaded from verified Tianma firmware; no GPU acceleration is
  currently enabled for the working desktop.

## Stock apps and root image

- Restore package: `iPhone10,3_iOS16.7.16_20H392_Restore.ipsw` (iPhone X donor,
  D22AP erase identity), SHA256
  `ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb`.
- On Windows it is at
  `C:\Users\Arbuxik\Documents\ChatGPT\surya-drivers\ipsw\`. The extracted
  `OS.dmg`, `SystemOS.dmg`, and `AppOS.dmg` are in `ipsw\extracted-20H392\`.
- The Actions workflow extracts and manifest-verifies those images, copies the
  OS plus cryptexes into an HFS+ sparsebundle, inventories app bundles, and
  checksums copied trees. Run `39848061737` failed because it copied cryptex
  overlays before verifying the base OS; valid overlay files appeared as
  extras to `rsync --delete`. Commit `897dcf7` fixes the ordering (base OS
  first, then AppOS and SystemOS). The fresh run for that commit is
  https://github.com/arbuxiktech/ios-surya/actions/runs/39852862866 . Check its
  status and logs before proceeding.
- Do not claim the stock apps are restored until that run publishes a verified
  root artifact and inventory. The artifact is a sparsebundle, not a
  fastboot-ready image. No partition was flashed for this root-image work.

## GPU / Mesa / Metal findings

Read [`hackios-mesa-surya-assessment.md`](hackios-mesa-surya-assessment.md).
HackiOS's Mesa/Freedreno GLES and AIR-to-NIR Metal paths are routed through its
`RedwoodGPU` IOKit contract, but the inspected kernel driver targets the Poco
X5 Pro Adreno 642L (`0x06030500`). Surya's Adreno 618 has different registers,
clocks, power sequencing, GMU firmware, SMMU setup, and firmware requirements.
Setting `gpu=1` or renaming the A660 firmware is not an A618 port. GLES and the
Metal compatibility layer are possible later integration targets; neither
hardware-accelerated CoreAnimation nor general Metal execution has been
verified on this phone. Do not enable the A642L driver on Surya.

## Files for the Mac

The complete source repository and the external `surya-drivers` asset folder
are staged together at
`C:\Users\Arbuxik\Documents\ChatGPT\Surya-Hackintosh-Handoff-2026-10-22`
on the Windows NTFS volume. Its `START-HERE.md` repeats this summary. The
external folder contains local firmware, device trees, graphics source, patch
copies, boot/payload artifacts, IPSW and extracted IPSW images; it is not part
of the GitHub repository.

When booted into macOS, copy that handoff folder from the mounted Windows NTFS
volume to an APFS home folder. Use `ls /Volumes` to find the mounted Windows
volume, then copy the package, for example:

```sh
ditto "/Volumes/<Windows volume>/Users/Arbuxik/Documents/ChatGPT/Surya-Hackintosh-Handoff-2026-10-22" "$HOME/Surya-Hackintosh-Handoff-2026-10-22"
```

The 1 TB PNY second disk is GPT with a 200 MB EFI
partition and a hidden APFS partition; Windows cannot mount the APFS volume, so
no files were written to the Hackintosh disk from this Windows session. The
GitHub branch remains the authoritative source:

```sh
git clone --branch codex/surya-port https://github.com/arbuxiktech/ios-surya.git
```

Then read `NOTES.md`, this file, and `docs/hackios-mesa-surya-assessment.md`.
For the root workflow and supported host commands, inspect
`.github/workflows/ios16-stock-root.yml` and `tools/mkiosroot-macos.sh`.

## Guardrails for continuation

- Use temporary `fastboot boot` for bring-up; preserve Android, bootloader,
  recovery, modem, persist, and device backups.
- Review any generated root artifact before installing it. A write to userdata
  or any partition is a separate destructive operation and requires fresh
  confirmation from the owner.
- Distinguish checked-in source, Actions artifacts, local generated binaries,
  and owner-reported screen results. Do not infer runtime success from build or
  upload success.
