#!/usr/bin/env python3
"""Wait for CI run 39329442405 to finish, download xnu-kernel artifact, and extract to xnu/out-surya-apple-root."""
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

RUN_ID = int(sys.argv[1]) if len(sys.argv) > 1 else 39337488012
REPO = "arbuxiktech/ios-surya"

def get_token():
    p = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n", text=True, capture_output=True)
    for line in p.stdout.splitlines():
        if line.startswith("password="):
            return line[len("password="):].strip()
    raise RuntimeError("No GitHub token found in git credential helper")

def fetch_json(url, token):
    req = urllib.request.Request(url, headers={
        "Authorization": f"Bearer {token}",
        "User-Agent": "Antigravity",
        "Accept": "application/vnd.github+json"
    })
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read())

def download_artifact(url, token, out_zip):
    print(f"Downloading artifact from {url} ...")
    cmd = [
        "curl.exe", "-L", "-s",
        "-H", f"Authorization: Bearer {token}",
        "-H", "User-Agent: Antigravity",
        url, "-o", str(out_zip)
    ]
    subprocess.run(cmd, check=True)
    print(f"Downloaded {out_zip.stat().st_size} bytes")

def main():
    token = get_token()
    print(f"Polling CI run {RUN_ID} on {REPO} ...")
    start = time.time()
    while True:
        data = fetch_json(f"https://api.github.com/repos/{REPO}/actions/runs/{RUN_ID}", token)
        status = data.get("status")
        conclusion = data.get("conclusion")
        elapsed = int(time.time() - start)
        print(f"[{elapsed:03d}s] status={status}, conclusion={conclusion}")
        if status == "completed":
            if conclusion != "success":
                print(f"Run completed with failure/cancellation: {conclusion}")
                sys.exit(1)
            break
        time.sleep(15)

    print("Run succeeded! Querying artifacts ...")
    artifacts = fetch_json(f"https://api.github.com/repos/{REPO}/actions/runs/{RUN_ID}/artifacts", token).get("artifacts", [])
    kernel_art = next((a for a in artifacts if a["name"] == "xnu-kernel"), None)
    if not kernel_art:
        print("xnu-kernel artifact not found among:", [a["name"] for a in artifacts])
        sys.exit(1)

    dl_url = kernel_art["archive_download_url"]
    zip_path = Path("xnu/kernel-art.zip")
    download_artifact(dl_url, token, zip_path)

    out_dir = Path("xnu/out-surya-apple-root")
    out_dir.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(zip_path, 'r') as zf:
        zf.extractall(out_dir)
    print(f"Extracted to {out_dir}")

    kernel_bin = out_dir / "kernel.development.surya"
    if kernel_bin.exists():
        kbytes = kernel_bin.read_bytes()
        sha = hashlib.sha256(kbytes).hexdigest()
        print(f"Kernel size: {len(kbytes)} bytes")
        print(f"Kernel SHA256: {sha}")
        has_touch = b"SURYA TOUCH: NT36672C" in kbytes
        print(f"Contains NT36672C touch driver string: {has_touch}")
        has_gpu_pt = b"SURYA GPU PAGE TABLE DIAGNOSTIC" in kbytes
        print(f"Contains GPU page table diagnostic string: {has_gpu_pt}")
        has_native_adreno = b"SURYA: AGXAccelerator registered" in kbytes
        print(f"Contains NativeAdreno AGXAccelerator driver string: {has_native_adreno}")
    else:
        print("ERROR: kernel.development.surya missing after extraction!")
        sys.exit(1)

if __name__ == "__main__":
    main()
