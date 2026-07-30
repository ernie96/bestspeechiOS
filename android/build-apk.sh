#!/bin/sh
# Build a debug APK using the SDK build-tools directly -- no Gradle, no network.
#
#   ./build-apk.sh [ABI ...]        # default: arm64-v8a
#
# Expects c/build-android/<abi>/libb32tts.so to exist already (run
# c/build-android.sh first). Produces android/build/bestspeech-debug.apk.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
SDK="${ANDROID_HOME:-C:/Users/Mew/scoop/apps/android-clt/current}"
JDK="${JAVA_HOME:-C:/Users/Mew/scoop/apps/openjdk17/current}"
BT="$SDK/build-tools/36.0.0"
ANDROID_JAR="$SDK/platforms/android-35/android.jar"
MIN_SDK=24
TARGET_SDK=35

# Windows build-tools mix .exe and .bat wrappers, and bash only auto-appends
# .exe; resolve each tool explicitly so this works on Windows and Linux alike.
bt() {
    for ext in "" ".exe" ".bat" ".sh"; do
        [ -f "$BT/$1$ext" ] && { echo "$BT/$1$ext"; return; }
    done
    echo "build-tools/$1 not found in $BT" >&2
    exit 1
}
AAPT2=$(bt aapt2)
D8=$(bt d8)
ZIPALIGN=$(bt zipalign)
APKSIGNER=$(bt apksigner)

ABIS="${*:-arm64-v8a}"
out="$here/build"
rm -rf "$out"
mkdir -p "$out/res" "$out/classes" "$out/dex" "$out/apk/lib"

echo "--- compiling resources ---"
"$AAPT2" compile --dir "$here/res" -o "$out/res/compiled.zip"

echo "--- linking resources ---"
"$AAPT2" link \
    -o "$out/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$here/AndroidManifest.xml" \
    --min-sdk-version "$MIN_SDK" \
    --target-sdk-version "$TARGET_SDK" \
    "$out/res/compiled.zip"

echo "--- compiling java ---"
# -encoding matters: the sources carry the curly-quote characters they map, and
# javac otherwise assumes the platform encoding (windows-1252 here).
"$JDK/bin/javac" -Xlint:all -encoding UTF-8 -classpath "$ANDROID_JAR" \
    -d "$out/classes" "$here"/src/com/bestspeech/tts/*.java

echo "--- dexing ---"
# Package the classes first: passing an @list of paths breaks on Windows, where
# d8 is a .bat and the shell does not rewrite paths inside a file's contents.
"$JDK/bin/jar" --create --file "$out/classes.jar" -C "$out/classes" .
"$D8" --min-api "$MIN_SDK" --output "$out/dex" \
    --lib "$ANDROID_JAR" "$out/classes.jar"

echo "--- assembling ---"
cp "$out/base.apk" "$out/unsigned.apk"
cd "$out/apk"
cp "$out/dex/classes.dex" .
for abi in $ABIS; do
    so="$root/c/build-android/$abi/libb32tts.so"
    [ -f "$so" ] || { echo "missing $so -- run c/build-android.sh $abi" >&2; exit 1; }
    mkdir -p "lib/$abi"
    cp "$so" "lib/$abi/"
done
# Native libraries must be stored uncompressed so the loader can mmap them.
"$JDK/bin/jar" --update --file "$out/unsigned.apk" --no-compress \
    classes.dex $(find lib -type f)
cd "$here"

echo "--- signing ---"
# Kept outside $out, which is wiped each run: a fresh key every build would make
# every rebuild fail to install over the previous one with a signature mismatch.
ks="$here/debug.keystore"
if [ ! -f "$ks" ]; then
    echo "generating a debug keystore (once)"
    "$JDK/bin/keytool" -genkeypair -keystore "$ks" -storepass android \
        -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 \
        -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

"$ZIPALIGN" -f -p 4 "$out/unsigned.apk" "$out/aligned.apk"
"$APKSIGNER" sign --ks "$ks" --ks-pass pass:android \
    --key-pass pass:android --min-sdk-version "$MIN_SDK" \
    --out "$out/bestspeech-debug.apk" "$out/aligned.apk"
"$APKSIGNER" verify --print-certs "$out/bestspeech-debug.apk" | head -3

rm -f "$out/aligned.apk" "$out/unsigned.apk" "$out/base.apk" "$out/classes.jar"
ls -la "$out/bestspeech-debug.apk"
echo
echo "install with: adb install -r android/build/bestspeech-debug.apk"
