#!/usr/bin/env python3
"""Fetch NativeAdrenoMetal bundle artifact from GitHub Actions run and extract to out/metal."""
import json
from pathlib import Path
import subprocess
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / 'out/metal'

class NoAuthRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new_req = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new_req and 'Authorization' in new_req.headers:
            del new_req.headers['Authorization']
        return new_req

def get_token():
    p = subprocess.run(['git', 'credential', 'fill'], cwd=str(ROOT),
                       input='protocol=https\nhost=github.com\n', text=True, capture_output=True)
    return [l[len('password='):].strip() for l in p.stdout.splitlines() if l.startswith('password=')][0]

def get_latest_run_id(token):
    req = urllib.request.Request(
        'https://api.github.com/repos/arbuxiktech/ios-surya/actions/runs?per_page=10',
        headers={'Authorization': f'Bearer {token}', 'User-Agent': 'Antigravity', 'Accept': 'application/vnd.github+json'}
    )
    with urllib.request.urlopen(req) as resp:
        data = json.loads(resp.read())
    for r in data.get('workflow_runs', []):
        if r.get('name') == 'metal' and r.get('head_branch') in ('main', 'codex/surya-port'):
            return str(r['id'])
    raise RuntimeError("No metal workflow runs found!")

def main(run_id='latest'):
    token = get_token()
    opener = urllib.request.build_opener(NoAuthRedirect)
    
    if run_id == 'latest':
        run_id = get_latest_run_id(token)

    print(f"Monitoring GitHub Actions run {run_id} ...")
    while True:
        req = urllib.request.Request(
            f'https://api.github.com/repos/arbuxiktech/ios-surya/actions/runs/{run_id}',
            headers={'Authorization': f'Bearer {token}', 'User-Agent': 'Antigravity', 'Accept': 'application/vnd.github+json'}
        )
        with urllib.request.urlopen(req) as resp:
            data = json.loads(resp.read())
        status = data.get('status')
        conclusion = data.get('conclusion')
        print(f"Run {run_id}: status={status}, conclusion={conclusion}")
        if status == 'completed':
            if conclusion != 'success':
                raise RuntimeError(f"Run {run_id} completed with conclusion: {conclusion}")
            break
        time.sleep(15)

    print("Fetching artifacts list...")
    req = urllib.request.Request(
        f'https://api.github.com/repos/arbuxiktech/ios-surya/actions/runs/{run_id}/artifacts',
        headers={'Authorization': f'Bearer {token}', 'User-Agent': 'Antigravity', 'Accept': 'application/vnd.github+json'}
    )
    with urllib.request.urlopen(req) as resp:
        arts = json.loads(resp.read()).get('artifacts', [])
    
    bundle_art = next((a for a in arts if a['name'] == 'NativeAdrenoMetal-bundle'), None)
    if not bundle_art:
        raise RuntimeError("Artifact NativeAdrenoMetal-bundle not found!")

    download_url = bundle_art['archive_download_url']
    print(f"Downloading {bundle_art['name']} ({bundle_art['size_in_bytes']} bytes) from {download_url} ...")
    
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    zip_dest = OUT_DIR / 'NativeAdrenoMetal-bundle-art.zip'
    
    dl_req = urllib.request.Request(
        download_url,
        headers={'Authorization': f'Bearer {token}', 'User-Agent': 'Antigravity'}
    )
    with opener.open(dl_req) as resp, open(zip_dest, 'wb') as f:
        f.write(resp.read())

    print(f"Saved to {zip_dest} ({zip_dest.stat().st_size} bytes)")
    
    with zipfile.ZipFile(zip_dest, 'r') as z:
        z.extractall(OUT_DIR)
    
    inner_zip = OUT_DIR / 'NativeAdrenoMetal-iOS16-arm64.zip'
    if inner_zip.exists():
        print("Extracting inner package...")
        extracted_dir = OUT_DIR / 'extracted'
        extracted_dir.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(inner_zip, 'r') as z:
            z.extractall(extracted_dir)

    print("PASS: Artifact downloaded and extracted to out/metal/")

if __name__ == '__main__':
    import sys
    run_id = sys.argv[1] if len(sys.argv) > 1 else 'latest'
    main(run_id)
