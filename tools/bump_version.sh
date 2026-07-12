#!/usr/bin/env bash

set -euo pipefail

# 1. Help flag handling
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    echo "Usage: $0 <vX.Y.Z> \"<Short description>\""
    echo "Automates the release process defined in AGENTS.md:"
    echo "  1. Creates a GitHub release and tag"
    echo "  2. Updates all version pins in YAML files"
    echo "  3. Commits and pushes the pins to main"
    exit 0
fi

# 2. Argument validation
if [ "$#" -lt 2 ]; then
    echo "Error: Missing arguments."
    echo "Usage: $0 <vX.Y.Z> \"<Short description>\""
    exit 1
fi

NEW_VERSION="$1"
DESC="$2"

# Strict SemVer format check (must start with v)
if [[ ! "$NEW_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: Version must match format vX.Y.Z (e.g. v0.11.0)"
    exit 1
fi

# 3. Git state validation
if ! git diff-index --quiet HEAD --; then
    echo "Error: Working directory is not clean. Please commit or stash changes first."
    exit 1
fi

BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$BRANCH" != "main" ]; then
    echo "Error: Releases must be created from the 'main' branch."
    exit 1
fi

echo "Pulling latest main..."
git pull origin main

echo "=========================================="
echo "Starting Release Process for $NEW_VERSION"
echo "=========================================="

echo "Step 1: Creating GitHub Release and Tag..."
gh release create "$NEW_VERSION" \
  --title "${NEW_VERSION} — ${DESC}" \
  --notes "${DESC}" \
  --target main

echo ""
echo "Step 2: Updating version pins in example YAMLs and packages..."

FILES=(
    "hwr-pump-example.yaml"
    "hwr-pairing-example.yaml"
    "hwr-pump-schedule-example.yaml"
    "dhw-demand-example.yaml"
    "packages/alpha_hwr_base.yaml"
    "packages/alpha_hwr_pairing.yaml"
    "packages/dhw_demand_detector.yaml"
)

# Use macOS sed to replace any existing @vX.Y.Z tag with the new tag
for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        sed -i '' -E "s|@v[0-9]+\.[0-9]+\.[0-9]+|@${NEW_VERSION}|g" "$file"
        echo "  Updated $file"
    else
        echo "  Warning: $file not found! Skipping..."
    fi
done

echo ""
echo "Step 3: Committing and pushing version pins..."
git add "${FILES[@]}"
git commit -m "Pin examples and packages to ${NEW_VERSION}"
git push origin main

echo "=========================================="
echo "Release $NEW_VERSION successfully published!"
echo "=========================================="
