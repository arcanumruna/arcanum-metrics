#!/bin/bash
set -e

VERSION=$(cat VERSION.txt | tr -d '[:space:]')
PACKAGE="metrics/${VERSION}@vradu/dev"

echo "🔧 Installing Conan dependencies..."
conan install . --build=missing -s build_type=Release --format=json > conan-graph.json

echo "🏗️ Generating CMake build system..."
cmake . -B build \
  -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_PREFIX_PATH="$PWD/build/Release/generators" \
  -DCMAKE_BUILD_TYPE=Release

echo "🚧 Building project..."
cmake --build build -- VERBOSE=1

echo "📦 Creating Conan package..."
conan create . --user=vradu --channel=dev

echo "📤 Exporting package to local cache..."
conan export . --user=vradu --channel=dev

echo "📤 Uploading to remote..."
conan list --graph=conan-graph.json --graph-binaries=build --format=json > conan-pkglist.json
conan upload --list=conan-pkglist.json --remote=artifactory -c
conan upload "$PACKAGE" --remote=artifactory -c

echo "✅ Done."
