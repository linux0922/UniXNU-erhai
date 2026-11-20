#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: bash build-erhai-metal-smoke.sh OUTPUT" >&2
    exit 2
fi
if ! command -v xcrun >/dev/null || ! command -v codesign >/dev/null; then
    echo "The Metal probe requires macOS with Xcode." >&2
    exit 1
fi

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
if [[ ! -d "$sdk" || ! -f "$sdk/System/Library/Frameworks/Metal.framework/Headers/Metal.h" ]]; then
    echo "A complete iPhoneOS SDK with Metal headers is required." >&2
    exit 1
fi
output=$1
if [[ -e "$output" ]]; then
    echo "Output exists: $output" >&2
    exit 1
fi
work_dir=$(mktemp -d)
trap 'rm -f "$work_dir/erhai-metal-smoke"; rmdir "$work_dir"' EXIT
probe="$work_dir/erhai-metal-smoke"

xcrun --sdk iphoneos clang -target arm64-apple-ios17.0 -isysroot "$sdk" \
    -Wall -Wextra -Werror -O2 -fobjc-arc -fblocks \
    -framework Foundation -framework Metal \
    "$source_dir/erhai-metal-smoke.m" -o "$probe"
codesign --sign - --pagesize 4096 --identifier org.unixnu.erhai.metal-smoke "$probe"
codesign --verify --strict "$probe"
xcrun vtool -show-build "$probe"
mkdir -p -- "$(dirname -- "$output")"
cp -n -- "$probe" "$output"
shasum -a 256 "$output"
