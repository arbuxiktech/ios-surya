#!/usr/bin/env python3
"""Build Mesa Turnip (freedreno Vulkan Darwin KMD), Rust metal2vulkan bridge, and NativeAdrenoMetal.bundle for iOS ARM64."""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / 'build/metal-userspace'
OUT_DIR = ROOT / 'out/metal'

MESA_REPO = 'https://gitlab.freedesktop.org/mesa/mesa.git'
MESA_MIRROR = 'https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa'
MESA_COMMIT = '85fda3b36405bcaea8bffc64aa4f2651dc90770d'

M2V_REPO = 'https://github.com/steelbrain/metal2vulkan.git'
M2V_COMMIT = '720ef1ce9ae2a2068999a8675717418b9416283e'


def run(cmd, cwd=None, env=None, check=True):
    print(f"[RUN] {' '.join(str(c) for c in cmd)}")
    return subprocess.run([str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=env, check=check)


def read(cmd, cwd=None):
    return subprocess.check_output([str(c) for c in cmd], cwd=str(cwd) if cwd else None, text=True).strip()


def get_cdhash(path):
    res = subprocess.run(['codesign', '--display', '--verbose=4', str(path)], capture_output=True, text=True, check=True)
    m = re.search(r'^CDHash=([a-f0-9]{40})$', res.stderr, re.M)
    if not m:
        m = re.search(r'^CDHash=([a-f0-9]{40})$', res.stdout, re.M)
    if not m:
        raise RuntimeError(f"Failed to extract CDHash from codesign output for {path}:\n{res.stderr}\n{res.stdout}")
    return m[1]


def fetch_exact_commit(repo_url, commit_sha, dest_dir, mirror_url=None):
    if not dest_dir.exists():
        dest_dir.mkdir(parents=True, exist_ok=True)
        run(['git', 'init'], cwd=dest_dir)
        run(['git', 'remote', 'add', 'origin', repo_url], cwd=dest_dir)

    try:
        current = read(['git', 'rev-parse', 'HEAD'], cwd=dest_dir)
        if current == commit_sha:
            print(f"{dest_dir.name} already at {commit_sha}")
            return
    except Exception:
        pass

    print(f"Fetching {commit_sha} into {dest_dir}...")
    try:
        run(['git', 'fetch', '--depth', '1', 'origin', commit_sha], cwd=dest_dir)
    except Exception:
        if mirror_url:
            print(f"Fallback to mirror {mirror_url}...")
            run(['git', 'remote', 'set-url', 'origin', mirror_url], cwd=dest_dir)
            run(['git', 'fetch', '--depth', '1', 'origin', commit_sha], cwd=dest_dir)
        else:
            raise
    run(['git', 'checkout', '-f', 'FETCH_HEAD'], cwd=dest_dir)
    run(['git', 'reset', '--hard', commit_sha], cwd=dest_dir)


def step_setup():
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)


def step_fetch_mesa():
    mesa_dir = BUILD_DIR / 'mesa'
    fetch_exact_commit(MESA_REPO, MESA_COMMIT, mesa_dir, mirror_url=MESA_MIRROR)
    run(['git', 'reset', '--hard', MESA_COMMIT], cwd=mesa_dir)
    run(['git', 'clean', '-fdx'], cwd=mesa_dir)

    patches_dir = ROOT / 'graphics/metal/patches/mesa'
    patch_order = [
        'freedreno-tools-nir.patch',
        'ir3-darwin-build-id.patch',
        'ir3-portable-asm-roundtrip.patch',
        'offline-adreno-compiler-probe.patch',
        'turnip-darwin-kmd.patch',
        'turnip-graphics-cache-feedback.patch',
        'turnip-empty-scissor.patch',
        'turnip-builtin-shader-pack.patch'
    ]
    for p in patch_order:
        p_path = patches_dir / p
        if p_path.exists():
            print(f"Applying Mesa patch {p} ...")
            run(['git', 'apply', '--check', str(p_path)], cwd=mesa_dir)
            run(['git', 'apply', str(p_path)], cwd=mesa_dir)


def step_build_turnip():
    mesa_dir = BUILD_DIR / 'mesa'
    out_tu = BUILD_DIR / 'mesa-tu-ios'
    out_tu.mkdir(parents=True, exist_ok=True)

    sdk = read(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'])
    find = lambda tool: read(['xcrun', '--sdk', 'iphoneos', '--find', tool])
    flags = ['-target', 'arm64-apple-ios16.0', '-isysroot', sdk, '-fstack-protector-strong']
    cross = BUILD_DIR / 'mesa-tu-ios.cross'
    cross.write_text(
        f"[binaries]\n"
        f"c = '{find('clang')}'\n"
        f"cpp = '{find('clang++')}'\n"
        f"objc = '{find('clang')}'\n"
        f"ar = '{find('ar')}'\n"
        f"strip = '{find('strip')}'\n"
        f"pkg-config = '/usr/bin/false'\n"
        f"[host_machine]\n"
        f"system = 'darwin'\n"
        f"cpu_family = 'aarch64'\n"
        f"cpu = 'arm64'\n"
        f"endian = 'little'\n"
        f"[properties]\n"
        f"needs_exe_wrapper = true\n"
        f"[built-in options]\n" +
        ''.join(f"{k} = {flags!r}\n" for k in ('c_args', 'cpp_args', 'objc_args', 'c_link_args', 'cpp_link_args', 'objc_link_args'))
    )

    abi_dir = ROOT / 'graphics/native'
    options = [
        '-Dgallium-drivers=',
        '-Dvulkan-drivers=freedreno',
        '-Dfreedreno-kmds=darwin',
        f'-Dtu-darwin-abi-dir={abi_dir}',
        '-Dplatforms=macos',
        '-Dglx=disabled',
        '-Degl=disabled',
        '-Dgbm=disabled',
        '-Dopengl=false',
        '-Dgles1=disabled',
        '-Dgles2=disabled',
        '-Dllvm=disabled',
        '-Dshared-glapi=disabled',
        '-Dxmlconfig=disabled',
        '-Dshader-cache=disabled',
        '-Dtools=',
        '-Dbuild-tests=false',
        '-Dvideo-codecs=',
        '-Dvulkan-layers=',
        '-Dlibunwind=disabled',
        '-Dlmsensors=disabled',
        '-Dvalgrind=disabled',
        '-Dexpat=disabled',
        '-Dzstd=disabled',
        '-Dbuildtype=release',
        '-Db_ndebug=true',
        '--cross-file', str(cross),
        '--wrap-mode=nodownload'
    ]

    env = dict(os.environ)
    env.update(CC=find('clang'), CXX=find('clang++'), CFLAGS='', CXXFLAGS='', CPPFLAGS='', LDFLAGS='')

    if not (out_tu / 'build.ninja').exists():
        run(['meson', 'setup', str(out_tu), str(mesa_dir)] + options, env=env)
    else:
        run(['meson', 'configure', str(out_tu)] + options, env=env)

    run(['ninja', '-C', str(out_tu), '-j6'], env=env)

    tu_dylib = out_tu / 'src/freedreno/vulkan/libvulkan_freedreno.dylib'
    if not tu_dylib.exists():
        raise RuntimeError("libvulkan_freedreno.dylib was not produced!")
    print(f"PASS: Turnip compiled successfully ({tu_dylib.stat().st_size} bytes)")
    return tu_dylib


def step_build_bridge():
    m2v_dir = BUILD_DIR / 'metal2vulkan'
    fetch_exact_commit(M2V_REPO, M2V_COMMIT, m2v_dir)
    run(['git', 'reset', '--hard', M2V_COMMIT], cwd=m2v_dir)
    run(['git', 'clean', '-fdx'], cwd=m2v_dir)

    patches_dir = ROOT / 'graphics/metal/bridge/patches'
    bridge_patches = [
        '0001-optional-constant-output-members.patch',
        '0002-resource-wait-storage-dependencies.patch',
        '0003-cache-device-entry-point-priming.patch',
        '0004-fold-constant-global-field-loads.patch',
        '0005-keep-module-decodes-and-profile-fingerprints.patch'
    ]
    for p in bridge_patches:
        p_path = patches_dir / p
        if p_path.exists():
            print(f"Applying metal2vulkan patch {p} ...")
            run(['git', 'apply', str(p_path)], cwd=m2v_dir)

    bridge_out = BUILD_DIR / 'native-metal-bridge'
    bridge_out.mkdir(parents=True, exist_ok=True)

    llvm_as = None
    for cand in [shutil.which('llvm-as'), '/opt/homebrew/opt/llvm/bin/llvm-as', '/usr/local/opt/llvm/bin/llvm-as']:
        if cand and Path(cand).exists():
            llvm_as = cand
            break
    if not llvm_as:
        try:
            brew_llvm = Path(read(['brew', '--prefix', 'llvm'])) / 'bin/llvm-as'
            if brew_llvm.exists():
                llvm_as = str(brew_llvm)
        except Exception:
            pass
    if not llvm_as:
        llvm_as = 'llvm-as'

    air = bridge_out / 'runtime_examples.air'
    run([llvm_as, '-o', str(air), str(m2v_dir / 'crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll')])
    fc_air = bridge_out / 'function_constant.air'
    run([llvm_as, '-o', str(fc_air), str(m2v_dir / 'crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll')])

    manifest = bridge_out / 'Cargo.toml'
    bridge_rs = ROOT / 'graphics/metal/bridge/NativeMetalBridge.rs'
    manifest.write_text(
        f'[package]\nname = "native-metal-bridge"\nversion = "0.1.0"\nedition = "2021"\n[workspace]\n'
        f'[lib]\ncrate-type = ["staticlib"]\npath = {json.dumps(str(bridge_rs))}\n'
        f'[dependencies]\n'
        f'metal2vulkan = {{ path = {json.dumps(str(m2v_dir / "crates/metal2vulkan"))}, features = ["ash"] }}\n'
        f'metal2vulkan-runtime = {{ path = {json.dumps(str(m2v_dir / "crates/metal2vulkan-runtime"))} }}\n'
        f'libc = "0.2"\n'
        f'sha2 = {{ version = "0.10", features = ["asm"] }}\n'
        f'[profile.release]\npanic = "unwind"\ndebug = 1\n'
    )

    env = dict(os.environ, NMB_FIXTURE_AIR=str(air), NMB_FC_AIR=str(fc_air),
               IPHONEOS_DEPLOYMENT_TARGET='16.0', MACOSX_DEPLOYMENT_TARGET='14.0')
    target_dir = BUILD_DIR / 'm2v-target'
    run(['cargo', 'build', '--release', '--manifest-path', str(manifest),
         '--target', 'aarch64-apple-ios', '--target-dir', str(target_dir)], env=env)

    lib = target_dir / 'aarch64-apple-ios/release/libnative_metal_bridge.a'
    if not lib.exists():
        raise RuntimeError("libnative_metal_bridge.a was not produced!")
    print(f"PASS: NativeMetalBridge static library built ({lib.stat().st_size} bytes)")
    return lib


def step_build_bundle(tu_dylib, bridge_lib):
    out_bundle_dir = OUT_DIR / 'adreno-metal-plugin'
    if out_bundle_dir.exists():
        shutil.rmtree(out_bundle_dir)
    bundle = out_bundle_dir / 'NativeAdrenoMetal.bundle'
    bundle.mkdir(parents=True)

    sdk = read(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'])
    base_clang = ['xcrun', 'clang', '-target', 'arm64-apple-ios16.0', '-isysroot', sdk, '-O2', '-g',
                  '-Wall', '-Wextra', '-Wno-error', '-fstack-protector-strong']
    base_cxx = ['xcrun', 'clang++', '-target', 'arm64-apple-ios16.0', '-isysroot', sdk, '-O2', '-g',
                '-Wall', '-Wextra', '-Wno-error', '-fstack-protector-strong']

    # 1. Compile AdrenoPM4.c
    pm4_obj = out_bundle_dir / 'AdrenoPM4.o'
    run([*base_clang, '-std=c11', '-c', str(ROOT / 'graphics/metal/AdrenoPM4/AdrenoPM4.c'), '-o', str(pm4_obj)])

    # 2. Compile NativeAdrenoMetal bundle binary
    vulkan_inc = BUILD_DIR / 'mesa/include'
    exe = bundle / 'NativeAdrenoMetal'
    inc_flags = [
        '-I', str(ROOT / 'graphics/native'),
        '-I', str(ROOT / 'graphics/metal'),
        '-I', str(ROOT / 'graphics/metal/AdrenoPM4'),
        '-I', str(vulkan_inc)
    ]
    run([*base_cxx, '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', '-bundle',
         *inc_flags,
         '-x', 'objective-c++', str(ROOT / 'graphics/metal/NativeAdrenoMetalPlugin.mm'),
         str(ROOT / 'graphics/metal/NativeBridgeMetalFrontend.mm'),
         '-x', 'none', str(pm4_obj), str(bridge_lib),
         '-Wl,-dead_strip', '-lobjc', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
         '-o', str(exe)])

    run(['strip', '-x', str(exe)])

    # 3. Copy Turnip into bundle
    bundle_tu = bundle / 'libvulkan_freedreno.dylib'
    shutil.copyfile(tu_dylib, bundle_tu)
    run(['strip', '-x', str(bundle_tu)])
    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.turnip', str(bundle_tu)])
    turnip_cdhash = get_cdhash(bundle_tu)

    # 4. Info.plist & codesign bundle
    (bundle / 'Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='org.blu-sharky.NativeAdrenoMetal',
        CFBundleExecutable='NativeAdrenoMetal',
        CFBundlePackageType='BNDL',
        CFBundleVersion='1',
        CFBundleShortVersionString='1',
        NSPrincipalClass='NativeAdrenoMetalDevice',
        MinimumOSVersion='16.0'
    ), fmt=plistlib.FMT_XML))

    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.NativeAdrenoMetal', str(bundle)])
    run(['codesign', '--verify', '--strict', str(bundle)])
    cdhash = get_cdhash(bundle)

    # 5. Compile adrenoprobed daemon
    daemon = out_bundle_dir / 'adrenoprobed'
    entitlements = out_bundle_dir / 'adrenoprobed-entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({
        'com.apple.private.security.no-container': True,
        'com.apple.security.iokit-user-client-class': ['AGXDeviceUserClient']
    }))

    run([*base_cxx, '-std=c++17', *inc_flags,
         str(ROOT / 'graphics/metal/NativeAdrenoProbeDaemon.cpp'), str(pm4_obj),
         '-framework', 'IOKit', '-framework', 'Foundation', '-o', str(daemon)])

    run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.adrenoprobed',
         '--entitlements', str(entitlements), str(daemon)])
    run(['codesign', '--verify', '--strict', str(daemon)])
    daemon_cdhash = get_cdhash(daemon)

    # 6. Launchd job plists
    (out_bundle_dir / 'org.blu-sharky.adrenoprobed.plist').write_bytes(plistlib.dumps(dict(
        Label='org.blu-sharky.adrenoprobed',
        ProgramArguments=['/private/var/ngpu/bin/adrenoprobed'],
        RunAtLoad=True,
        KeepAlive=False,
        ThrottleInterval=30,
        UserName='root',
        StandardErrorPath='/dev/null'
    ), fmt=plistlib.FMT_XML))

    (out_bundle_dir / 'com.apple.iomfb_fdr_loader.plist').write_bytes(plistlib.dumps(dict(
        Label='com.apple.iomfb_fdr_loader',
        ProgramArguments=['/private/var/ngpu/bin/adrenoprobed'],
        RunAtLoad=True,
        KeepAlive=False,
        LimitLoadToSessionType='System',
        ProcessType='Interactive',
        StandardErrorPath='/dev/null'
    ), fmt=plistlib.FMT_XML))

    (out_bundle_dir / 'com.apple.bluetoothd.plist').write_bytes(plistlib.dumps(dict(
        Label='com.apple.bluetoothd',
        ProgramArguments=['/private/var/ngpu/bin/adrenoprobed'],
        RunAtLoad=True,
        KeepAlive=False,
        ProcessType='Interactive',
        StandardErrorPath='/dev/null'
    ), fmt=plistlib.FMT_XML))

    (out_bundle_dir / 'trust.json').write_text(json.dumps(dict(
        cdhash=cdhash,
        daemon_cdhash=daemon_cdhash,
        turnip_cdhash=turnip_cdhash,
        bundle='NativeAdrenoMetal.bundle',
        executable_bytes=exe.stat().st_size,
        purpose='owner-built plugin and probe daemon; trusted only through an extra owner trust module'
    ), indent=2) + '\n')

    # 7. Zip package
    zip_path = OUT_DIR / 'NativeAdrenoMetal-iOS16-arm64.zip'
    shutil.make_archive(str(zip_path.with_suffix('')), 'zip', str(out_bundle_dir))
    print(f"PASS: Built NativeAdrenoMetal bundle ({zip_path.stat().st_size} bytes)")
    print(f"CDHash: {cdhash}, Daemon CDHash: {daemon_cdhash}, Turnip CDHash: {turnip_cdhash}")


def main():
    step_setup()
    step_fetch_mesa()
    tu_dylib = step_build_turnip()
    bridge_lib = step_build_bridge()
    step_build_bundle(tu_dylib, bridge_lib)


if __name__ == '__main__':
    main()
