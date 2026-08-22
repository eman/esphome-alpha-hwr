#!/usr/bin/env bash

set -euo pipefail

# 1. Help flag handling
if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    echo "Usage: $0 <vX.Y.Z> \"<Short description>\""
    echo "Automates the release process:"
    echo "  1. Prepares CHANGELOG.md for the new release"
    echo "  2. Updates all version pins in YAML files"
    echo "  3. Commits and pushes the release changes to main"
    echo "  4. Creates the GitHub release and tag"
    exit 0
fi

# 2. Argument validation
if [ "$#" -lt 1 ]; then
    echo "Error: Missing arguments."
    echo "Usage: $0 <vX.Y.Z> [\"Short description\"]"
    exit 1
fi

NEW_VERSION="$1"
DESC="${2:-}"

# Strict SemVer format check (must start with v)
if [[ ! "$NEW_VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: Version must match format vX.Y.Z (e.g. v0.11.0)"
    exit 1
fi

VER_NO_V="${NEW_VERSION#v}"
TODAY=$(date +%Y-%m-%d)

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

echo "Step 1: Preparing CHANGELOG.md..."
if grep -q "## \[Unreleased\]" CHANGELOG.md; then
    # Insert the new version header below the Unreleased header
    perl -pi -e "s/## \[Unreleased\]/## [Unreleased]\n\n## [${VER_NO_V}] - ${TODAY}/" CHANGELOG.md
    echo "  Updated CHANGELOG.md"
else
    echo "  Warning: '## [Unreleased]' header not found in CHANGELOG.md!"
fi

echo ""
echo "Step 2: Updating version pins in example YAMLs and packages..."
FILES=(
    "hwr-pump-example.yaml"
    "hwr-pairing-example.yaml"
    "hwr-pump-schedule-example.yaml"
    "dhw-demand-example.yaml"
    "hwr-pump-dhw-example.yaml"
    "packages/alpha_hwr_base.yaml"
    "packages/alpha_hwr_pairing.yaml"
    "packages/dhw_demand_detector.yaml"
)

for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        # Use perl -pi (like the CHANGELOG edit above) rather than sed -i '' so the
        # in-place edits are portable across BSD (macOS) and GNU (Linux) systems.
        perl -pi -e "s|\@v[0-9]+\.[0-9]+\.[0-9]+|\@${NEW_VERSION}|g" "$file"
        # Keep the "Component Version" diagnostic substitution in sync (issue #95).
        perl -pi -e "s|(component_version: \")[0-9]+\.[0-9]+\.[0-9]+(\")|\${1}${VER_NO_V}\${2}|g" "$file"
        echo "  Updated $file"
    else
        echo "  Warning: $file not found! Skipping..."
    fi
done

# The Lovelace card, which is not a YAML pin and needs its own two rules.
#
# It ships through HACS from dist/ (issue #183), and HACS resolves the version
# from the release tag -- so the stamp below is not what HACS reads. It is what
# a user who copied the file into /config/www by hand can check, and what the
# card prints to the browser console. Before this the header carried a
# card-local "v6" unrelated to any release, and the card had drifted out of step
# with the firmware twice without anyone being able to tell.
CARD="dist/alpha-hwr-schedule-card.js"
if [ -f "$CARD" ]; then
    # No em-dash in this anchor, deliberately: perl -pi works on bytes, so a
    # multi-byte "—" defeats a "." in the pattern and the header silently does
    # not get stamped while CARD_VERSION below does.
    perl -pi -e "s|(Alpha HWR Schedule Card )v[0-9]+\\.[0-9]+\\.[0-9]+|\${1}${NEW_VERSION}|" "$CARD"
    perl -pi -e "s|(CARD_VERSION = ')[0-9]+\\.[0-9]+\\.[0-9]+(')|\${1}${VER_NO_V}\${2}|" "$CARD"
    echo "  Updated $CARD"
else
    echo "  Warning: $CARD not found! Skipping..."
fi

echo ""
echo "Step 3: Committing and pushing release changes..."
git add CHANGELOG.md "${FILES[@]}" "$CARD"
git commit -m "Release ${NEW_VERSION}"
git push origin main

echo ""
echo "Step 4: Creating GitHub Release and Tag..."
if [ -n "$DESC" ]; then
    gh release create "$NEW_VERSION" \
      --title "${NEW_VERSION} — ${DESC}" \
      --generate-notes \
      --target main
else
    gh release create "$NEW_VERSION" \
      --title "${NEW_VERSION}" \
      --generate-notes \
      --target main
fi

echo "=========================================="
echo "Release $NEW_VERSION successfully published!"
echo "=========================================="
