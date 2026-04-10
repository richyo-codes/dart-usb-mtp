#!/usr/bin/env bash
set -euo pipefail

git describe --always --tags --dirty 2>/dev/null || git rev-parse --short HEAD
