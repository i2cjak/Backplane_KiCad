#!/bin/bash
# Native CI build only. Never installs into /Applications or the user's runtime.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = Darwin ] || { echo 'macOS is required' >&2; exit 2; }
export HOMEBREW_NO_AUTO_UPDATE=1
prefix=$(brew --prefix)
export PATH="$prefix/opt/protobuf@29/bin:$prefix/bin:$PATH"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/opt/curl/lib/pkgconfig"
build="$PWD/build/backplane-macos"
stage="$PWD/stage-macos"
release="$PWD/release/macos"
mkdir -p "$release"
ccache --max-size=2G
cmake -S . -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$stage" \
  -DCMAKE_PREFIX_PATH="$prefix;$prefix/opt/protobuf@29;$prefix/opt/curl;$prefix/opt/opencascade" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF \
  -DKICAD_USE_SENTRY=OFF -DKICAD_USE_PCH=OFF \
  -DKICAD_VERSION_EXTRA=bp.4 -DBACKPLANE_FORK_VERSION=10.99.0-bp.4 \
  -DBACKPLANE_UPSTREAM_COMMIT="$(cat BACKPLANE_UPSTREAM)"
cmake --build "$build" --parallel 3
cmake --install "$build"
bundle="$stage/KiCad.app"
# Upstream installs the GUI bundle; explicitly retain its matched CLI as well.
cli=$(find "$build" -type f -name kicad-cli -perm -111 | head -n 1)
[ -n "$cli" ]
cp "$cli" "$bundle/Contents/MacOS/kicad-cli"
python scripts/backplane-macos-relocate.py "$bundle" | tee "$release/relocation.log"
root="$release/backplane-kicad-darwin-$(uname -m)-10.99.0-bp.4"
mkdir -p "$root/bin" "$root/share/backplane" "$root/licenses"
cp -R "$bundle" "$root/KiCad.app"
cp "$build/api/api.desc" "$root/share/backplane/api.desc"
cat > "$root/bin/kicad-cli" <<'LAUNCHER'
#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export KICAD_KIFACE_HOME="$root/KiCad.app/Contents/PlugIns"
export KICAD_STOCK_PLUGIN_HOME="$root/KiCad.app/Contents/PlugIns"
export DYLD_FALLBACK_LIBRARY_PATH="$root/KiCad.app/Contents/Frameworks${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
exec "$root/KiCad.app/Contents/MacOS/kicad-cli" "$@"
LAUNCHER
chmod +x "$root/bin/kicad-cli"
cp LICENSE* COPYING* AUTHORS* "$root/licenses/" 2>/dev/null || true
git archive HEAD | gzip > "$root/licenses/matching-source.tar.gz"
printf '%s\n' "$(git rev-parse HEAD)" > "$root/SOURCE_COMMIT"
cp BACKPLANE_UPSTREAM "$root/UPSTREAM_COMMIT"
# Exercise the relocated package from a different working directory with fresh state.
export HOME="$RUNNER_TEMP/backplane-kicad-macos-home"
export XDG_CONFIG_HOME="$HOME/config" XDG_CACHE_HOME="$HOME/cache" KICAD_CONFIG_HOME="$HOME/kicad"
mkdir -p "$HOME"
"$root/bin/kicad-cli" --version | tee "$release/version.log"
"$root/bin/kicad-cli" api-server --describe | tee "$root/share/backplane/runtime.json" "$release/runtime.json"
python scripts/backplane-ipc-smoke.py "$root/bin/kicad-cli" | tee "$release/ipc.log"
python scripts/backplane-document-ipc-regression.py "$root/bin/kicad-cli" | tee "$release/documents.log"
# Preserve library licenses alongside every dependency from Homebrew.
brew info --json=v2 --installed > "$root/licenses/homebrew-packages.json"
for formula in "$prefix/Cellar"/*; do
  [ -d "$formula" ] || continue
  name=$(basename "$formula")
  mkdir -p "$root/licenses/homebrew/$name"
  find "$formula" -type f \( -iname '*license*' -o -iname '*copying*' -o -path '*/.brew/*.rb' \) -exec cp {} "$root/licenses/homebrew/$name/" \;
done
archive="$(basename "$root").tar.gz"
tar -C "$release" -czf "$release/$archive" "$(basename "$root")"
(cd "$release" && shasum -a 256 "$archive" > SHA256SUMS)
