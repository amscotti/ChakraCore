#!/usr/bin/env python3
#-------------------------------------------------------------------------------------------------------
# Copyright (c) ChakraCore Project Contributors. All rights reserved.
# Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
#-------------------------------------------------------------------------------------------------------
"""B3.4 per-profile side-prefix installer for non-test profiles.

Why this exists: Zig 0.17 owns the whole install prefix per build
configuration — installing the test profile purges other profiles'
subdir trees (doc/b3-profiles.md sections 8.5, 9.7) — and build.zig
cannot read or set `--prefix` (no such field on Build/Graph in the
shipped 0.17 std). So non-test profiles do NOT use native install
steps at all: this script copies the linked artifacts plus headers
and config.h into a FIXED side prefix (`out/debug-static/` or
`out/release-shared/`), a destination that is a pure function of
-Dprofile. No invocation can cross the streams: switching flavors
never touches another profile's installed outputs.

The test profile keeps native installs into `--prefix` (default
zig-out) per the B1/B2 contract; this script never writes there
(refused: --dest must be exactly out/<profile>).

Behavior:

  * compare-and-copy every --map dest (size, then MD5): unchanged
    bytes are never rewritten, so a no-op rebuild changes zero
    artifact bytes (mtimes untouched too).
  * create/update every --symlink (replaced only when its target
    differs); replicate the native versioned-dylib layout exactly.
  * write the `.profile` marker (profile name + newline) when it
    differs; check_metadata.py asserts it.
  * remove orphans: files/symlinks listed in the previous
    dest/.manifest but absent from the current managed set (graph
    changes only — never user droppings, which are simply kept).
  * self-healing: a manually deleted artifact is re-copied on the
    next build (the step runs every build: has_side_effects).

Usage: install_profile.py --profile <p> --dest out/<p>
         --map <dest-rel>=<src-abs>... [--symlink <link-rel>=<target>...]
Exit code: 0 iff the prefix holds exactly the managed set.
"""

import hashlib
import os
import shutil
import sys

MANIFEST = ".manifest"
MARKER = ".profile"


def fail(msg):
    print("install_profile: FAIL: %s" % msg, file=sys.stderr)
    return 1


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def safe_rel(value, what):
    if not value or os.path.isabs(value) or ".." in value.split(os.sep):
        raise ValueError("%s must be a clean relative path, got %r"
                         % (what, value))
    return value


def copy_if_changed(src, dest):
    """Copy src over dest iff bytes differ; return True when written."""
    if os.path.islink(dest) or not os.path.isfile(dest):
        if os.path.islink(dest) or os.path.exists(dest):
            os.remove(dest)
    elif (os.path.getsize(src) == os.path.getsize(dest)
          and md5_of(src) == md5_of(dest)):
        return False
    shutil.copy2(src, dest)
    return True


def symlink_if_changed(target, link):
    """Point link at target iff it differs; return True when written."""
    if os.path.islink(link) and os.readlink(link) == target:
        return False
    if os.path.islink(link) or os.path.exists(link):
        os.remove(link)
    os.symlink(target, link)
    return True


def main(argv):
    args = list(argv[1:])
    profile = None
    dest = None
    maps = []
    symlinks = []
    i = 0
    while i < len(args):
        if args[i] == "--profile":
            i += 1
            if i >= len(args):
                print("install_profile: --profile needs a value",
                      file=sys.stderr)
                return 2
            profile = args[i]
        elif args[i] == "--dest":
            i += 1
            if i >= len(args):
                print("install_profile: --dest needs a value",
                      file=sys.stderr)
                return 2
            dest = args[i]
        elif args[i].startswith("--map="):
            maps.append(args[i][len("--map="):])
        elif args[i].startswith("--symlink="):
            symlinks.append(args[i][len("--symlink="):])
        else:
            print("install_profile: bad arg %r" % args[i], file=sys.stderr)
            return 2
        i += 1
    if profile not in ("debug-static", "release-shared"):
        print("install_profile: --profile must be debug-static or "
              "release-shared, got %r" % (profile,), file=sys.stderr)
        return 2
    if dest != "out/%s" % profile:
        print("install_profile: --dest must be exactly out/%s, got %r "
              "(this script never writes to the test prefix)"
              % (profile, dest), file=sys.stderr)
        return 2
    if not maps:
        return fail("at least one --map is required "
                    "(refusing to run with an empty managed set)")
    root = os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))
    dest_abs = os.path.join(root, dest)
    os.makedirs(dest_abs, exist_ok=True)

    managed = []
    copied = skipped = 0
    try:
        for m in maps:
            rel, _, src = m.partition("=")
            safe_rel(rel, "--map dest")
            if not src or not os.path.isfile(src):
                return fail("source missing for %s: %r" % (rel, src))
            out = os.path.join(dest_abs, rel)
            parent = os.path.dirname(out)
            if parent:
                os.makedirs(parent, exist_ok=True)
            if copy_if_changed(src, out):
                copied += 1
            else:
                skipped += 1
            managed.append(rel)
        for s in symlinks:
            rel, _, target = s.partition("=")
            safe_rel(rel, "--symlink link")
            if not target or target.startswith("/"):
                return fail("symlink target must be a bare relative "
                            "name, got %r" % (target,))
            link = os.path.join(dest_abs, rel)
            parent = os.path.dirname(link)
            if parent:
                os.makedirs(parent, exist_ok=True)
            if symlink_if_changed(target, link):
                copied += 1
            else:
                skipped += 1
            managed.append(rel)
    except (OSError, ValueError) as e:
        return fail(str(e))

    # Marker (managed, asserted by check_metadata.py).
    marker = os.path.join(dest_abs, MARKER)
    want = profile + "\n"
    try:
        with open(marker, "r") as f:
            have = f.read()
    except OSError:
        have = None
    if have != want:
        with open(marker, "w") as f:
            f.write(want)
        copied += 1
    else:
        skipped += 1
    managed.append(MARKER)

    # Orphans: previously managed but no longer in the set (graph
    # changes orphan their old paths; unlisted user files are kept).
    removed = 0
    manifest_path = os.path.join(dest_abs, MANIFEST)
    try:
        with open(manifest_path) as f:
            previous = [line.strip() for line in f if line.strip()]
    except OSError:
        previous = []
    for rel in previous:
        if rel in (MARKER, MANIFEST) or rel in managed:
            continue
        if os.path.isabs(rel) or ".." in rel.split(os.sep):
            continue
        victim = os.path.join(dest_abs, rel)
        try:
            if os.path.islink(victim) or os.path.isfile(victim):
                os.remove(victim)
                removed += 1
                try:
                    os.rmdir(os.path.dirname(victim))
                except OSError:
                    pass
        except OSError as e:
            print("install_profile: WARNING: cannot remove orphan %s: %s"
                  % (rel, e), file=sys.stderr)
    managed_sorted = sorted(set(managed))
    new_manifest = "\n".join(managed_sorted) + "\n"
    try:
        with open(manifest_path) as f:
            old_manifest = f.read()
    except OSError:
        old_manifest = None
    if old_manifest != new_manifest:
        with open(manifest_path, "w") as f:
            f.write(new_manifest)

    print("install_profile: %s: %d copied, %d unchanged, %d orphans "
          "removed (%d managed)"
          % (dest, copied, skipped, removed, len(managed_sorted)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
