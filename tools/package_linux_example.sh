#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <artifact-version>" >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
artifact_version="$1"
bundle_dir="$repo_root/example/build/linux/x64/release/bundle"
dist_dir="$repo_root/dist"
artifact_name="usb_sync_example-linux-x64-${artifact_version}.tar.gz"

mkdir -p "$dist_dir"
tar -C "$bundle_dir" -czf "$dist_dir/$artifact_name" .
printf '%s\n' "$dist_dir/$artifact_name"
