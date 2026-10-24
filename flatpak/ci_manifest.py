#!/usr/bin/env python3
"""v0.7.17: the Flatpak manifest for a release bundle, built from this checkout.

The published manifest (io.github.TomasUribe.SwitchFrameTap.yaml) builds the
viewer from a tag's archive with its sha256 - which only exists once the tag
is pushed, and is bumped after each release. For the .flatpak attached to a
release, CI builds the same modules from the checked-out source instead:

    python3 flatpak/ci_manifest.py 0.7.17 > flatpak/ci.yaml
"""
import re
import sys

ver = sys.argv[1]
here = sys.path[0]
src = open(f"{here}/io.github.TomasUribe.SwitchFrameTap.yaml").read()

# the viewer's source: this checkout (the manifest sits in flatpak/)
archive = re.compile(r"      - type: archive\n        url: https://github.com/TomasUribe/switch-frame-tap/archive/refs/tags/[^\n]+\n        sha256: [0-9a-f]+\n")
assert len(archive.findall(src)) == 1, "the switch-frame-tap archive source was not found"
# (not the builder's own folders, which sit in the checkout too)
src = archive.sub("      - type: dir\n        path: ..\n        skip:\n          - .git\n          - .flatpak-builder\n          - flatpak_app\n          - repo\n          - dist\n", src)

# the version the viewer reports (its Updates section)
version = re.compile(r"-DSFT_VERSION='\"[^\"]*\"'")
assert len(version.findall(src)) == 1, "SFT_VERSION was not found"
src = version.sub(f"-DSFT_VERSION='\"{ver}\"'", src)

sys.stdout.write(src)
