#!/bin/bash

set -e

BASE_DIR=$(mktemp -d)
CURRENT_DIR=$(pwd)

echo "Temporary directory at $BASE_DIR"

APP_BUNDLE_PATH=$1
SIGNING_CERTIFICATE=$2
ENTITLEMENTS_PATH=$3
OUTPUT_FILE=$4

mkdir "$BASE_DIR/Payload"

cp -R "$APP_BUNDLE_PATH" "$BASE_DIR/Payload/"

# Sign inside-out: frameworks (including StikJIT.framework), then each embedded extension, then the main executable.
codesign -f -s "$SIGNING_CERTIFICATE" "$BASE_DIR/Payload/iCube.app/Frameworks/"*
EXTENSION_ENTITLEMENTS="$(dirname "$ENTITLEMENTS_PATH")/Extension.entitlements"
JIT_HELPER_ENTITLEMENTS="$(dirname "$ENTITLEMENTS_PATH")/JITHelper.entitlements"
if [ -d "$BASE_DIR/Payload/iCube.app/PlugIns" ]; then
  for appex in "$BASE_DIR/Payload/iCube.app/PlugIns/"*.appex; do
    [ -e "$appex" ] || continue
    # PrimeHack for iPad: iCubeJITHelper.appex must carry only get-task-allow (no App Group).
    if [ "$(basename "$appex")" = "iCubeJITHelper.appex" ]; then
      [ -f "$JIT_HELPER_ENTITLEMENTS" ] || { echo "Missing $JIT_HELPER_ENTITLEMENTS" >&2; exit 1; }
      codesign -f -s "$SIGNING_CERTIFICATE" --entitlements "$JIT_HELPER_ENTITLEMENTS" "$appex"
    else
      [ -f "$EXTENSION_ENTITLEMENTS" ] || { echo "Missing $EXTENSION_ENTITLEMENTS" >&2; exit 1; }
      codesign -f -s "$SIGNING_CERTIFICATE" --entitlements "$EXTENSION_ENTITLEMENTS" "$appex"
    fi
  done
fi
codesign -f -s "$SIGNING_CERTIFICATE" --entitlements "$ENTITLEMENTS_PATH" "$BASE_DIR/Payload/iCube.app"

cd "$BASE_DIR"

zip -r "$OUTPUT_FILE" .

cd "$CURRENT_DIR"

echo "Cleaning up"

rm -rf "$BASE_DIR"

echo "Done"
