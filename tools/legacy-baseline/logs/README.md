# Preserved B3.1 legacy-baseline timings (`.log` files untracked in Wave 5 `D-4`)

The `/tmp/b3-baselines/` originals were captured during B3.1; every
durable result and exit code is summarized in `doc/b3-profiles.md`
section 5.

In Wave 5 (`D-4` repository hygiene), the 10 tracked `.log` files in
`tools/legacy-baseline/logs/` and `tools/legacy-baseline/logs/rerun/`
(`00-progress.log`, `08-debug-configure.log`, `09-release-configure.log`,
`11-debug-check.log`, `13-release-check.log`, and `rerun/{00-progress,
10-debug-build,11-debug-check,12-release-build,13-release-check}.log`)
were untracked from git (`git rm`).

Preserved in git (4 timing files):

- `10/11/12/13-*.time` — wall/user/sys timings quoted in `doc/b3-profiles.md` §5.

Regenerate locally on demand (note legacy CMake is unmaintained after
`lib/ZigCodec` landed in `Q1.3`):

- `LOG=<dir> tools/legacy-baseline/run-baselines.sh`
- `mise exec -- zig build [-Dprofile=...] --verbose` (for verbose Zig compile logs)
