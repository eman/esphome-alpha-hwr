#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <new_version>"
    echo "Example: $0 v0.11.0"
    exit 1
fi

NEW_VERSION="$1"
# Ensure version starts with 'v'
if [[ ! "$NEW_VERSION" =~ ^v ]]; then
    NEW_VERSION="v${NEW_VERSION}"
fi

# The version without the 'v' prefix for the regex substitution replacement
VER_STR="${NEW_VERSION#v}"

echo "Bumping all esphome-alpha-hwr git references to ${NEW_VERSION}..."

# Find all YAML files that might contain the github source reference
# We look for github://eman/esphome-alpha-hwr@v* and replace it
find . -type f -name "*.yaml" -not -path "*/\.esphome/*" -not -path "*/venv/*" -not -path "*/\.venv/*" -print0 | while IFS= read -r -d '' file; do
    if grep -qE "github://eman/esphome-alpha-hwr(@|/packages/.*@)v[0-9]+\.[0-9]+\.[0-9]+" "$file"; then
        echo "Updating $file"
        # Match github://eman/esphome-alpha-hwr@vX.Y.Z or github://eman/esphome-alpha-hwr/packages/...@vX.Y.Z
        perl -pi -e "s|(github://eman/esphome-alpha-hwr(?:/packages/[a-zA-Z0-9_.-]+)?)(\@v[0-9]+\.[0-9]+\.[0-9]+(?:-[a-zA-Z0-9.]+)?)|\\1\@v${VER_STR}|g" "$file"
    fi
done

echo "Done! Please review the changes using 'git diff'."
