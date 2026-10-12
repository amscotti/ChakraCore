# Preserved B3.1 legacy-baseline logs (B3.5 carried close)

The `/tmp/b3-baselines/` originals rot with the machine. The SMALL,
exit-code-bearing logs are preserved here byte-identical (`cmp`-clean);
everything else is regenerable and cited with its regeneration recipe
in `doc/b3-profiles.md` section 5.

Preserved (14 files, ~18 KB):

- `00-progress.log`, `08-debug-configure.log`,
  `09-release-configure.log` — configure evidence (`exit: 0`).
- `11-debug-check.log` (`Total: passed 2011, failed 0`, 8 static
  PASS) and `13-release-check.log` (smoke gate: `hello world` +
  `PASS`, shared PASS). NOTE: these two were written by the
  pre-runner script, so they carry no `exit:` marker line — the
  `rerun/` copies below are the exit-code evidence for the same
  steps (see `doc/b3-profiles.md` section 5).
- `10/11/12/13-*.time` — wall/user/sys timings quoted in §5.
- `rerun/` — exit-code re-runs under
  `tools/legacy-baseline/run-baselines.sh` (`exit: 0` in every
  log): no-op rebuilds plus the full debug `ninja check`
  (2011/0 in 2m53s) and the release smoke check.

Deliberately NOT preserved (regenerate instead):

- `10-debug-build.log` / `12-release-build.log` (~60 KB each of
  compile lines): `LOG=<dir> tools/legacy-baseline/run-baselines.sh`.
- `21/22/23-zig-*.log` (~1.6 MB each of verbose zig compile
  lines): `mise exec -- zig build [-Dprofile=...] --verbose`.
  Section 6 quotes the two load-bearing lines in full.
- `/tmp/b33-*.log`, `/tmp/b33-relkeep/`, `/tmp/b34-incr-logs/`:
  ephemeral by design; sections 9–10 quote every durable fact,
  and the B3.5 CI artifacts (`b3-evidence` bundle) supersede them.
