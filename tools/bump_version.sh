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
git fetch -q --tags origin

# packages/alpha_hwr_pairing.yaml is a one-release shim for the pump package's
# old name. The shim, the README and the changelog all promise it goes in the
# release after the one that first shipped it, so hold the release to that
# rather than relying on anyone to remember. Keyed on the previous tag's copy
# being the shim (it says "Deprecated name"), not on a version number: before
# the rename that path held the real package.
SHIM="packages/alpha_hwr_pairing.yaml"
LAST_TAG=$(git describe --tags --abbrev=0 2>/dev/null || true)
if [ -f "$SHIM" ] && [ -n "$LAST_TAG" ] \
   && git show "$LAST_TAG:$SHIM" 2>/dev/null | grep -q "Deprecated name"; then
    echo "Error: $LAST_TAG already shipped the $SHIM shim, so this release must remove it."
    echo "Delete the file, drop its mentions in README.md and packages/README.md, and add a Removed entry to CHANGELOG.md."
    exit 1
fi

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
    "examples/hwr-pump-example.yaml"
    "examples/hwr-pump-controls-example.yaml"
    "examples/hwr-pump-dhw-example.yaml"
    "packages/alpha_hwr.yaml"
    "packages/dhw_demand_detector.yaml"
)

for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        # The pump package was renamed from alpha_hwr_pairing.yaml after
        # v0.16.0. Release-pinned references had to keep the old name until a
        # tag carried the new one; this moves them (and drops the note that
        # explained the lag) the first time it runs, and is a no-op after.
        perl -pi -e "s|packages/alpha_hwr_pairing\.yaml\@|packages/alpha_hwr.yaml\@|g" "$file"
        perl -0pi -e "s|  # Pinned to the last release, where the pump package still carried its old\n  # name\. On main it is packages/alpha_hwr\.yaml; the release script moves this\n  # line at the next release\.\n||g" "$file"
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

# The repo-root manifest, which is the odd one out: nothing consumes it. HACS
# validates this repository as a plugin and reads hacs.json; ESPHome's
# external_components loader reads components/ directly and knows manifest.json
# only as its own bundle format, which needs manifest_version and
# config_filename and is a different file entirely. So this stamp is for a
# human reading the repo, not for a tool -- which is exactly why nobody noticed
# it sitting at 0.1.0 from the initial commit through fifteen releases. Kept
# and stamped rather than deleted, so the version it declares is true.
MANIFEST="manifest.json"
if [ -f "$MANIFEST" ]; then
    perl -pi -e "s|(\"version\": \")[0-9]+\.[0-9]+\.[0-9]+(\")|\${1}${VER_NO_V}\${2}|" "$MANIFEST"
    echo "  Updated $MANIFEST"
else
    echo "  Warning: $MANIFEST not found! Skipping..."
fi

echo ""
echo "Step 3: Committing and pushing release changes..."
git add CHANGELOG.md "${FILES[@]}" "$CARD" "$MANIFEST"
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
