#!/bin/bash
# Build an iOS 16.7.16 root incrementally so standard 14 GB macOS Actions runners fit.
# Usage:
#   mkiosroot-macos.sh init ROOT.sparsebundle [SIZE]
#   mkiosroot-macos.sh add OS|SystemOS|AppOS IMAGE.dmg ROOT.sparsebundle INVENTORY.json
#   mkiosroot-macos.sh finalize ROOT.sparsebundle INVENTORY.json
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)

usage() {
  sed -n '1,8p' "$0" >&2
  exit 2
}

plist_mount() {
  python3 - "$1" "$2" <<'PY'
import plistlib, sys
from pathlib import Path
with open(sys.argv[1], 'rb') as f:
    p = plistlib.load(f)
kind = sys.argv[2]
candidates = []
mounted = []
for entity in p.get('system-entities', []):
    mount = entity.get('mount-point')
    if not mount:
        continue
    root = Path(mount)
    mounted.append(root)
    if kind == 'OS' and (root / 'System/Library/xpc/launchd.plist').is_file():
        candidates.append(root)
    elif kind == 'SystemOS' and (root / 'System/Library').is_dir() and (root / 'usr/lib').is_dir():
        candidates.append(root)
    elif kind == 'AppOS' and (root / 'System/Library/Assistant').is_dir():
        candidates.append(root)
if len(candidates) != 1:
    raise SystemExit(f'expected one {kind} root volume, found {[str(x) for x in candidates]}; '
                     f'mounted volumes: {[str(x) for x in mounted]}')
print(candidates[0])
PY
}

init_root() {
  [[ $# -ge 1 && $# -le 2 ]] || usage
  local root=$1 size=${2:-24g}
  [[ ! -e $root ]] || { echo "refusing to overwrite $root" >&2; exit 1; }
  command -v hdiutil >/dev/null || { echo 'missing hdiutil' >&2; exit 1; }
  mkdir -p "$(dirname "$root")"
  hdiutil create -type SPARSEBUNDLE -size "$size" -fs JHFS+X -layout NONE \
    -volname iOSRoot "$root"
  echo "initialized $root (capacity $size)"
}

add_image() {
  [[ $# -eq 4 ]] || usage
  local kind=$1 image=$2 root_image=$3 inventory=$4
  [[ $kind == OS || $kind == SystemOS || $kind == AppOS ]] || usage
  [[ -f $image ]] || { echo "missing input image: $image" >&2; exit 1; }
  [[ -d $root_image ]] || { echo "missing root sparsebundle: $root_image" >&2; exit 1; }
  for tool in hdiutil diskutil python3; do
    command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
  done
  local rsync
  rsync=$(brew --prefix rsync 2>/dev/null)/bin/rsync
  [[ -x $rsync ]] || { echo 'install Homebrew rsync before building' >&2; exit 1; }

  local work root_disk= source_disk= source_root target
  work=$(mktemp -d "${TMPDIR:-/tmp}/surya-ios-root.XXXXXX")
  cleanup_add() {
    if [[ -n $root_disk ]]; then hdiutil detach "$root_disk" >/dev/null 2>&1 || true; fi
    if [[ -n $source_disk ]]; then hdiutil detach "$source_disk" >/dev/null 2>&1 || true; fi
    rm -rf "$work"
  }
  trap cleanup_add EXIT

  mkdir -p "$work/source" "$work/root"
  local source_plist="$work/source.plist" root_plist="$work/root.plist"
  hdiutil attach -plist -readonly -nobrowse -owners on -mountroot "$work/source" \
    "$image" > "$source_plist"
  source_disk=$(python3 "$repo/tools/hdiutil_disk.py" "$source_plist")
  source_root=$(plist_mount "$source_plist" "$kind")
  hdiutil attach -plist -owners on -nobrowse -mountpoint "$work/root" \
    "$root_image" > "$root_plist"
  root_disk=$(python3 "$repo/tools/hdiutil_disk.py" "$root_plist")

  case "$kind" in
    OS) target="$work/root" ;;
    SystemOS) target="$work/root/private/preboot/Cryptexes/OS" ;;
    AppOS) target="$work/root/private/preboot/Cryptexes/App" ;;
  esac
  echo "copying $source_root -> ${target#"$work/root"}"
  sudo mkdir -p "$target"
  sudo "$rsync" -aHAX --numeric-ids "$source_root/" "$target/"
  echo "checking copied $kind files and metadata"
  sudo "$rsync" -aHAXnc --numeric-ids --delete --itemize-changes \
    "$source_root/" "$target/" > "$work/rsync-verify.txt"
  if [[ -s $work/rsync-verify.txt ]]; then
    cat "$work/rsync-verify.txt" >&2
    echo "$kind copy differs from its IPSW source" >&2
    exit 1
  fi
  sudo python3 "$repo/tools/verify_ios_stock_root.py" --kind "$kind" \
    --image "$source_root" --root "$work/root" --inventory "$inventory"

  hdiutil detach "$root_disk" >/dev/null
  root_disk=
  hdiutil detach "$source_disk" >/dev/null
  source_disk=
  rm -rf "$work"
  trap - EXIT
  echo "added $kind from $image"
}

finalize_root() {
  [[ $# -eq 2 ]] || usage
  local root_image=$1 inventory=$2 work root_disk= dsc db label cryptex relative target source
  [[ -d $root_image ]] || { echo "missing root sparsebundle: $root_image" >&2; exit 1; }
  for tool in hdiutil fsck_hfs python3; do
    command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
  done
  work=$(mktemp -d "${TMPDIR:-/tmp}/surya-ios-root-final.XXXXXX")
  cleanup_final() {
    if [[ -n $root_disk ]]; then hdiutil detach "$root_disk" >/dev/null 2>&1 || true; fi
    rm -rf "$work"
  }
  trap cleanup_final EXIT
  mkdir "$work/root"
  local plist="$work/root.plist"
  hdiutil attach -plist -owners on -nobrowse -mountpoint "$work/root" \
    "$root_image" > "$plist"
  root_disk=$(python3 "$repo/tools/hdiutil_disk.py" "$plist")

  dsc="$work/root/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
  [[ -d $dsc ]] || { echo "missing SystemOS shared cache directory: $dsc" >&2; exit 1; }
  sudo mkdir -p "$work/root/System/Library/Caches/com.apple.dyld"
  while IFS= read -r -d '' source; do
    target="$work/root/System/Library/Caches/com.apple.dyld/${source##*/}"
    sudo ln "$source" "$target"
  done < <(find "$dsc" -mindepth 1 -maxdepth 1 -print0)

  # Early dyld does not search cryptexes; expose absent cryptex files through hard links.
  for label in OS App; do
    cryptex="$work/root/private/preboot/Cryptexes/$label"
    [[ -d $cryptex ]] || { echo "missing $label cryptex" >&2; exit 1; }
    (
      cd "$cryptex"
      find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*' -print0
    ) | while IFS= read -r -d '' relative; do
      relative=${relative#./}
      target="$work/root/$relative"
      source="$cryptex/$relative"
      if ! sudo test -e "$target" && ! sudo test -L "$target"; then
        sudo mkdir -p "$work/root/$(dirname "$relative")"
        if sudo test -L "$source"; then sudo cp -P "$source" "$target"; else sudo ln "$source" "$target"; fi
      fi
    done
  done

  db="$work/root/private/var/db"
  sudo install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
  sudo install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
  for directory in diagnostics uuidtext diagnostics/timesync; do
    sudo install -o 272 -g 272 -m 0644 /dev/null "$db/$directory/.migrated"
  done
  sudo install -d -m 0700 -o 0 -g 0 "$work/root/private/var/keybags"
  printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
    '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
    sudo install -m 0600 -o 0 -g 0 /dev/stdin "$work/root/private/var/keybags/systembag.kb"

  sudo python3 "$repo/tools/sweet-root-tweaks.py" "$work/root"
  sudo python3 "$repo/tools/verify_ios_stock_root.py" --finalize \
    --root "$work/root" --inventory "$inventory"
  df -h "$work/root"
  hdiutil detach "$root_disk" >/dev/null
  root_disk=

  local fsck_plist="$work/fsck.plist"
  hdiutil attach -plist -nomount -readonly "$root_image" > "$fsck_plist"
  root_disk=$(python3 "$repo/tools/hdiutil_disk.py" "$fsck_plist")
  fsck_hfs -fn "$root_disk"
  hdiutil detach "$root_disk" >/dev/null
  root_disk=
  trap - EXIT
  rm -rf "$work"
  python3 "$repo/tools/hash_sparsebundle.py" "$root_image" "${root_image}.sha256"
  echo "finalized $root_image"
}

[[ $# -ge 1 ]] || usage
command=$1
shift
case "$command" in
  init) init_root "$@" ;;
  add) add_image "$@" ;;
  finalize) finalize_root "$@" ;;
  *) usage ;;
esac
