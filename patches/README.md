# patches/

Changes to shared components that this project needs but that are not yet tagged in
[`esp_components`](https://github.com/benvonhandorf/esp_components).

`managed_components/` is gitignored and re-extracted from the pinned tag, so an edit made
there does not survive and cannot be committed. AGENTS.md describes the workflow this
implies: *"Anything changed that way has to be committed and tagged in `esp_components`,
**and pushed**, before a plain `idf.py build` resolves it."* A patch here is that change,
parked where it can be reviewed with the code that depends on it.

**Nothing is pending.** The directory is kept for the next such change, and for the two
notes below, both of which cost real time to find.

## Retagging a component is not enough on its own

A component's own manifest pins its siblings, and the component manager takes that
transitive pin over this project's direct one **without reporting a conflict**.

`cli-v0.2.0` was tagged and `main/idf_component.yml` asked for it, and the build still
resolved `cli-v0.1.0` — because `cli_web-v0.2.0` pinned `cli-v0.1.0`. The symptom was
`app_console.c` failing on `unknown type name 'cli_command_text_t'` while the manifest
plainly named the tag that defines it. Fixing it meant tagging `cli_web-v0.2.1` too.

The same trap is noted for `diag` in `main/idf_component.yml`, where the direct pin asks
for `diag-v0.1.1` and the build resolves `v0.1.0`. So:

- **`main/idf_component.yml` states an intention. `dependencies.lock` states the fact.**
  When they disagree, the lock is right.
- Better still, check the extracted source, which is the only thing the compiler sees:

  ```sh
  grep -c cli_command_text_t managed_components/cli/include/cli.h
  ```

- When a bump does not seem to take, `rm dependencies.lock` and the affected
  `managed_components/<name>` directories, then `idf.py reconfigure`. A stale lock is
  sticky: the solver will report `Dependency "cli": <old> -> <new>` and still leave the
  old one in place.

## Push tags by name, not with `--follow-tags`

`--follow-tags` only pushes *annotated* tags. `git tag <name>` creates a lightweight one,
so the commit goes up and the tag silently stays local — and every build keeps resolving
the previous version with nothing in any manifest to show why.

```sh
git push origin main
git push origin <tag>          # explicitly
git ls-remote --tags origin    # the only real confirmation
```

Or create tags with `git tag -a`, in which case `--follow-tags` does cover them.
