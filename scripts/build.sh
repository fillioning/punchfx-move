#!/usr/bin/env bash
# Build Punch-In FX module for Schwung (ARM64)
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="punchfx-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Punch-In FX Module Build (via Docker) ==="
    echo ""

    # Convert to Windows paths for Docker on MINGW
    if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
        DOCKER_REPO_ROOT="$(cygpath -w "$REPO_ROOT")"
        DOCKER_DOCKERFILE="$(cygpath -w "$SCRIPT_DIR/Dockerfile")"
    else
        DOCKER_REPO_ROOT="$REPO_ROOT"
        DOCKER_DOCKERFILE="$SCRIPT_DIR/Dockerfile"
    fi

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        MSYS_NO_PATHCONV=1 docker build -t "$IMAGE_NAME" -f "$DOCKER_DOCKERFILE" "$DOCKER_REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    MSYS_NO_PATHCONV=1 docker run --rm \
        -v "$DOCKER_REPO_ROOT:/build" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Punch-In FX Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories
mkdir -p build
mkdir -p dist/punchfx

# Compile DSP plugin (optimized for Cortex-A72)
echo "Compiling DSP plugin..."
${CROSS_PREFIX}gcc -O2 -ffast-math -shared -fPIC \
    -std=gnu11 \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -fno-tree-loop-distribute-patterns \
    -DNDEBUG \
    src/dsp/punchfx.c \
    -o build/punchfx.so \
    -Isrc/dsp \
    -lm -lrt

# Copy files to dist
echo "Packaging..."
cat src/module.json > dist/punchfx/module.json
[ -f src/help.json ] && cat src/help.json > dist/punchfx/help.json
[ -f src/ui_chain.js ] && cat src/ui_chain.js > dist/punchfx/ui_chain.js
cat build/punchfx.so > dist/punchfx/punchfx.so
chmod +x dist/punchfx/punchfx.so

# Create tarball for release
cd dist
tar -czvf punchfx-module.tar.gz punchfx/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/punchfx/"
echo "Tarball: dist/punchfx-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
