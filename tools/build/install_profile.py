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
and config.h into a FIXED side prefix (`out/debug-static/`,
`out/release-shared/`, or `out/jit/`), a destination that is a pure function of
-Dprofile. No invocation can cross the streams: switching flavors
never touches another profile's installed outputs.

The test profile keeps native installs into `--prefix` (default
zig-out) per the B1/B2 contract; this script never writes there
(refused: --dest must be exactly out/<profile>).

Behavior:

  * compare-and-copy every --map dest (size, then MD5): unchanged
    bytes are never rewritten; mismatched permissions are repaired
    without recopying the binary. Changed bytes are staged in a
    same-directory temp file and installed with os.replace, so dest
    always gets a fresh inode: macOS caches a code signature per vnode,
    and overwriting a signed executable/shared library in place can
    leave a stale cache entry that kills the next exec/dlopen (SIGKILL,
    exit 137). A no-op rebuild changes zero artifact bytes (mtimes
    untouched too).
  * create/update every --symlink (replaced only when its target
    differs) via a same-directory temporary symlink and os.replace;
    replicate the native versioned-dylib layout exactly.
  * confine every managed path: out/<profile> itself must be a real
    directory, never a symlink (otherwise the link target becomes the
    trust root and one profile can write into another). Before a
    directory is created, a file or symlink is installed, or an orphan
    is removed, every existing component below out/<profile> on the
    managed path is checked with os.path.islink. Any symlink aborts with
    FAIL, even one that stays inside the prefix, because it would alias
    writes/deletes and the manifest would record the alias rather than
    the resolved path. A leaf symlink at a map destination is replaced,
    never followed. The parent's realpath must also stay under
    realpath(out/<profile>) as defense in depth.
  * --symlink targets are bare relative names; '..', '/', and backslash
    are rejected so a managed link cannot point outside the prefix via
    its target string.
  * write the `.profile` marker (profile name + newline) when it
    differs; check_metadata.py asserts it.
  * remove orphans: files/symlinks listed in the previous
    dest/.manifest but absent from the current managed set (graph
    changes only — never user droppings, which are simply kept).
  * self-healing: a manually deleted artifact is re-copied on the
    next build (the step runs every build: has_side_effects).

Concurrent installers are not serialized. Every artifact, marker, and
manifest write is atomic (same-directory temp plus os.replace), so a race
leaves one complete writer's result: last-writer-wins. Validation and
mutation are separate syscalls with no lock held between them (TOCTOU):
another process can replace the profile root or a path component after
the preflight check and before the write/remove. Treat out/<profile> as
a cooperative workspace; do not run two builds for the same profile at
once and do not let untrusted writers modify it during an install. A
crashed writer can leave a `.<name>.*.tmp` file; those are not swept so
a live installer is never raced.

Usage: install_profile.py --profile <p> --dest out/<p>
         --map <dest-rel>=<src-abs>... [--symlink <link-rel>=<target>...]
Exit code: 0 iff the prefix holds exactly the managed set.
"""

import hashlib
import os
import shutil
import stat
import sys
import tempfile

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


def safe_link_target(target):
    """Reject a link target that is not a bare relative name.

    Managed links replicate the native versioned-dylib layout, so their
    target is always a sibling name. '..', path separators, and
    backslashes are refused instead of being resolved.
    """
    if (not target or target == "." or ".." in target
            or "/" in target or "\\" in target):
        raise ValueError("symlink target must be a bare relative name "
                         "(no '..', '/', or backslash), got %r"
                         % (target,))
    return target


def checked_root(dest_abs, profile):
    """Reject a symlinked profile root before anything below it is used.

    realpath(dest_abs) would otherwise make the link target the trust
    root, allowing writes and orphan removals to cross profiles or leave
    the checkout entirely.
    """
    if os.path.islink(dest_abs):
        raise ValueError("out/%s is a symlink (-> %s); refusing to install "
                         "through a linked profile root"
                         % (profile, os.readlink(dest_abs)))


def within_prefix(prefix_real, path_real):
    """True when path_real is prefix_real or a descendant of it."""
    try:
        return os.path.commonpath((prefix_real, path_real)) == prefix_real
    except ValueError:  # different drives
        return False


def guarded_parent(dest_abs, dest_real, profile, rel, what):
    """Return rel's parent after proving it is a real path inside prefix.

    Walk every existing component of rel's parent lexically and reject
    any symlink below dest_abs, even one that resolves inside the
    prefix: it would alias another managed path, so a write or orphan
    removal would land in the link target and the manifest would record
    the alias. The realpath containment check remains as defense in
    depth. Call this before os.makedirs or any write/remove.
    """
    checked_root(dest_abs, profile)
    parent = os.path.dirname(os.path.join(dest_abs, rel)) or dest_abs
    current = dest_abs
    for part in os.path.dirname(rel).split(os.sep):
        if part in ("", "."):
            continue
        current = os.path.join(current, part)
        if os.path.islink(current):
            raise ValueError(
                "%s %r traverses symlinked component %r under out/%s; "
                "refusing to follow it"
                % (what, rel, os.path.relpath(current, dest_abs), profile))
    parent_real = os.path.realpath(parent)
    if not within_prefix(dest_real, parent_real):
        raise ValueError("%s %r escapes out/%s (parent resolves to %s)"
                         % (what, rel, profile, parent_real))
    return parent


def replace_atomically(src, dest):
    """Copy src to a unique same-directory temp, then rename it over dest.

    os.replace installs a fresh inode and never follows a symlink at
    dest, so a signed binary is never rewritten in place (see the
    module docstring). The temp is removed if the copy or rename fails.
    """
    parent = os.path.dirname(dest) or "."
    fd, tmp = tempfile.mkstemp(prefix=".%s." % os.path.basename(dest),
                               suffix=".tmp", dir=parent)
    try:
        os.close(fd)
        shutil.copy2(src, tmp)
        # rename cannot replace a directory; managed dests are files,
        # but remove an empty stray directory so the install can heal.
        if os.path.isdir(dest) and not os.path.islink(dest):
            os.rmdir(dest)
        os.replace(tmp, dest)
        tmp = None
    finally:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass


def default_file_mode():
    """Return the mode a plain open(..., "w") would use under the umask."""
    mask = os.umask(0)
    os.umask(mask)
    return 0o666 & ~mask


def write_atomically(path, data, mode=None):
    """Write data to a same-directory temp and rename it over path.

    Used for the .profile marker and .manifest so a crash mid-write cannot
    leave a truncated managed file. The caller keeps the compare-and-skip
    no-op decision. mkstemp creates 0600, so apply either the caller's mode
    or the process umask instead of hardcoding 0644.

    Concurrent installers are not locked out (see the module docstring);
    last-writer-wins is the documented limitation.
    """
    parent = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(prefix=".%s." % os.path.basename(path),
                               suffix=".tmp", dir=parent)
    try:
        with os.fdopen(fd, "w") as f:
            f.write(data)
        os.chmod(tmp, default_file_mode() if mode is None else mode)
        os.replace(tmp, path)
        tmp = None
    finally:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass


def copy_if_changed(src, dest):
    """Repair dest bytes or permissions.

    Return "copied" when bytes were written, "repaired" when only
    permissions were fixed, None when dest was already identical.
    """
    try:
        dest_stat = os.lstat(dest)
    except FileNotFoundError:
        dest_stat = None
    if (dest_stat is not None and stat.S_ISREG(dest_stat.st_mode)
          and os.path.getsize(src) == dest_stat.st_size
          and md5_of(src) == md5_of(dest)):
        if stat.S_IMODE(os.stat(src).st_mode) != stat.S_IMODE(dest_stat.st_mode):
            shutil.copymode(src, dest)
            return "repaired"
        return None
    replace_atomically(src, dest)
    return "copied"


def make_temp_symlink(target, parent, name):
    """Create a unique symlink in parent and return its path."""
    for _ in range(100):
        tmp = os.path.join(parent, ".%s.%s.tmp"
                           % (name, os.urandom(8).hex()))
        try:
            os.symlink(target, tmp)
            return tmp
        except FileExistsError:
            continue
    raise OSError("cannot create a temporary symlink in %s" % parent)


def symlink_if_changed(target, link):
    """Point link at target iff it differs; return True when written.

    The new symlink is created in link's directory and renamed over link
    with os.replace, so link is never momentarily absent and a failed
    swap leaves the previous link (and its target) untouched.
    """
    if os.path.islink(link) and os.readlink(link) == target:
        return False
    parent = os.path.dirname(link) or "."
    tmp = make_temp_symlink(target, parent, os.path.basename(link))
    try:
        os.replace(tmp, link)
        tmp = None
    finally:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass
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
    if profile not in ("debug-static", "release-shared", "jit"):
        print("install_profile: --profile must be debug-static, "
              "release-shared, or jit, got %r" % (profile,), file=sys.stderr)
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

    managed = []
    copied = repaired = skipped = 0
    try:
        checked_root(dest_abs, profile)
        os.makedirs(dest_abs, exist_ok=True)
        dest_real = os.path.realpath(dest_abs)

        # Parse and validate the whole managed set, plus every orphan's
        # parent, before the first mutation: a symlinked root or
        # component must not leave a half-installed tree behind.
        parsed_maps = []
        for m in maps:
            rel, _, src = m.partition("=")
            safe_rel(rel, "--map dest")
            if not src or not os.path.isfile(src):
                return fail("source missing for %s: %r" % (rel, src))
            parent = guarded_parent(dest_abs, dest_real, profile, rel,
                                    "--map dest")
            parsed_maps.append((rel, src, parent))
        parsed_symlinks = []
        for s in symlinks:
            rel, _, target = s.partition("=")
            safe_rel(rel, "--symlink link")
            safe_link_target(target)
            parent = guarded_parent(dest_abs, dest_real, profile, rel,
                                    "--symlink link")
            parsed_symlinks.append((rel, target, parent))
        managed_now = {rel for rel, _, _ in parsed_maps}
        managed_now.update(rel for rel, _, _ in parsed_symlinks)
        managed_now.add(MARKER)

        manifest_path = os.path.join(dest_abs, MANIFEST)
        try:
            with open(manifest_path) as f:
                previous = [line.strip() for line in f if line.strip()]
        except OSError:
            previous = []
        orphans = []
        for rel in previous:
            if rel in (MARKER, MANIFEST) or rel in managed_now:
                continue
            safe_rel(rel, "manifest entry")
            guarded_parent(dest_abs, dest_real, profile, rel,
                           "manifest entry")
            orphans.append(rel)

        # Revalidate immediately before each mutation so a component
        # swapped after the preflight pass is refused rather than
        # followed (the check/use gap itself remains a documented TOCTOU
        # limitation; there is no cross-process lock).
        for rel, src, parent in parsed_maps:
            parent = guarded_parent(dest_abs, dest_real, profile, rel,
                                    "--map dest")
            os.makedirs(parent, exist_ok=True)
            result = copy_if_changed(src, os.path.join(dest_abs, rel))
            if result == "copied":
                copied += 1
            elif result == "repaired":
                repaired += 1
            else:
                skipped += 1
            managed.append(rel)
        for rel, target, parent in parsed_symlinks:
            parent = guarded_parent(dest_abs, dest_real, profile, rel,
                                    "--symlink link")
            os.makedirs(parent, exist_ok=True)
            if symlink_if_changed(target, os.path.join(dest_abs, rel)):
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
        try:
            write_atomically(marker, want)
        except OSError as e:
            return fail(str(e))
        copied += 1
    else:
        skipped += 1
    managed.append(MARKER)

    # Orphans: previously managed but no longer in the set (graph
    # changes orphan their old paths; unlisted user files are kept).
    # Revalidate immediately before removal so a parent swapped after
    # the preflight pass is still refused rather than followed.
    removed = 0
    for rel in orphans:
        try:
            parent = guarded_parent(dest_abs, dest_real, profile, rel,
                                    "manifest entry")
        except (OSError, ValueError) as e:
            return fail(str(e))
        victim = os.path.join(dest_abs, rel)
        try:
            if os.path.islink(victim) or os.path.isfile(victim):
                os.remove(victim)
                removed += 1
                if parent != dest_abs:
                    try:
                        os.rmdir(parent)
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
        try:
            write_atomically(manifest_path, new_manifest)
        except OSError as e:
            return fail(str(e))

    print("install_profile: %s: %d copied, %d repaired, %d unchanged, "
          "%d orphans removed (%d managed)"
          % (dest, copied, repaired, skipped, removed, len(managed_sorted)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
