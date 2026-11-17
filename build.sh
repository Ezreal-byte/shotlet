#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_APP=1
BUILD_NODE=0
BUILD_TESTS=0
BUILD_DIR="build"

usage() {
    echo "Usage:"
    echo "  $0"
    echo "  $0 --app|-a|app"
    echo "  $0 --node|-n|node"
    echo "  $0 --tests|-t|tests"
    echo "  $0 --all|all"
    echo ""
    echo "Commands:"
    echo "  --app, -a, app       build shotlet app/executable"
    echo "  --node, -n, node     build Node-API addon"
    echo "  --tests, -t, tests   build shotlet_tests"
    echo "  --all, all           build app, Node addon, and tests"
    echo ""
    echo "Outputs:"
    echo "  app/tests            build/"
    echo "  node on macOS        build/native/darwin-arm64/shotlet.node and build/native/darwin-x64/shotlet.node"
    echo "  node on other OS     build/shotlet.node"
    echo ""
    echo "macOS node addons are stripped and ad-hoc codesigned after packaging."
}

if [ $# -gt 0 ]; then
    BUILD_APP=0
    for arg in "$@"; do
        case "$arg" in
            --app|-a|app)
                BUILD_APP=1
                ;;
            --node|-n|node)
                BUILD_NODE=1
                ;;
            --tests|-t|tests)
                BUILD_TESTS=1
                ;;
            --all|all)
                BUILD_APP=1
                BUILD_NODE=1
                BUILD_TESTS=1
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "Error: unknown argument '$arg'"
                usage
                exit 1
                ;;
        esac
    done
fi

# 检查 imgui 目录
if [ ! -d "./imgui" ]; then
    echo "Error: imgui directory not found at ./imgui"
    exit 1
fi

BUILD_JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

configure_build_dir() {
    local build_dir="$1"
    local build_tests="$2"
    local build_node="$3"
    local macos_arch="${4:-}"

    echo "Configuring $build_dir..."
    cmake -S "$SCRIPT_DIR" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DIMGUI_DIR="$SCRIPT_DIR/imgui" \
        -DSHOTLET_BUILD_TESTS="$build_tests" \
        -DSHOTLET_BUILD_NODE_ADDON="$build_node" \
        $([ -n "$macos_arch" ] && echo "-DCMAKE_OSX_ARCHITECTURES=$macos_arch")
}

build_macos_node_arch() {
    local arch="$1"
    local platform_dir="$2"
    local arch_build_dir="$SCRIPT_DIR/$BUILD_DIR/node-$platform_dir"
    local output_dir="$SCRIPT_DIR/$BUILD_DIR/native/$platform_dir"

    configure_build_dir "$arch_build_dir" OFF ON "$arch"
    echo "Building Node addon for $platform_dir..."
    cmake --build "$arch_build_dir" --target shotlet_node -j"$BUILD_JOBS"

    mkdir -p "$output_dir"
    cp "$arch_build_dir/shotlet.node" "$output_dir/shotlet.node"
    # 构建目录保留未剥离文件，只处理发布副本，随后重新签名。
    strip -x "$output_dir/shotlet.node"
    codesign --force --sign - "$output_dir/shotlet.node"
    codesign --verify --verbose=2 "$output_dir/shotlet.node"
    echo "Build complete. Node addon at: $BUILD_DIR/native/$platform_dir/shotlet.node"
}

NEEDS_ROOT_BUILD=0
if [ "$BUILD_APP" -eq 1 ] || [ "$BUILD_TESTS" -eq 1 ]; then
    NEEDS_ROOT_BUILD=1
fi
if [ "$BUILD_NODE" -eq 1 ] && [ "$(uname -s)" != "Darwin" ]; then
    NEEDS_ROOT_BUILD=1
fi

if [ "$NEEDS_ROOT_BUILD" -eq 1 ]; then
    ROOT_BUILD_NODE=$([ "$BUILD_NODE" -eq 1 ] && [ "$(uname -s)" != "Darwin" ] && echo ON || echo OFF)
    configure_build_dir "$SCRIPT_DIR/$BUILD_DIR" \
        $([ "$BUILD_TESTS" -eq 1 ] && echo ON || echo OFF) \
        "$ROOT_BUILD_NODE"
fi

if [ "$BUILD_APP" -eq 1 ]; then
    echo "Building app..."
    cmake --build "$SCRIPT_DIR/$BUILD_DIR" --target shotlet -j"$BUILD_JOBS"
    if [ -d "$SCRIPT_DIR/$BUILD_DIR/shotlet.app" ]; then
        echo "Build complete. App at: $BUILD_DIR/shotlet.app"
    else
        echo "Build complete. Binary at: $BUILD_DIR/shotlet"
    fi
fi

if [ "$BUILD_NODE" -eq 1 ]; then
    if [ "$(uname -s)" = "Darwin" ]; then
        build_macos_node_arch arm64 darwin-arm64
        build_macos_node_arch x86_64 darwin-x64
    else
        echo "Building Node addon..."
        cmake --build "$SCRIPT_DIR/$BUILD_DIR" --target shotlet_node -j"$BUILD_JOBS"
        echo "Build complete. Node addon at: $BUILD_DIR/shotlet.node"
    fi
fi

if [ "$BUILD_TESTS" -eq 1 ]; then
    echo "Building tests..."
    cmake --build "$SCRIPT_DIR/$BUILD_DIR" --target shotlet_tests -j"$BUILD_JOBS"
    echo "Build complete. Binary at: $BUILD_DIR/tests/shotlet_tests"
fi
