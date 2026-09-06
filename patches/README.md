# patches/

Changes to shared components that this project needs but that are not yet tagged in
[`esp_components`](https://github.com/benvonhandorf/esp_components).

`managed_components/` is gitignored and re-extracted from the pinned tag, so an edit made
there does not survive and cannot be committed. AGENTS.md describes the workflow this
implies: *"Anything changed that way has to be committed and tagged in `esp_components`,
**and pushed**, before a plain `idf.py build` resolves it."* A patch here is that change,
parked where it can be reviewed with the code that depends on it.

Delete a patch once its component is tagged and `main/idf_component.yml` names the new
tag.

## cli-text-resolver.patch

**`main` does not build against `cli-v0.1.0` without this.** `app_console.c`'s `i2c`
group carries its help text as ids rather than literals, which needs
`cli_command_text_t` and `cli_set_text_resolver()`.

Apply, test and tag:

```sh
cd ../esp_components
git apply /path/to/esp_board_bringup2/patches/cli-text-resolver.patch
make -C cli/test
git commit -am "Let a group carry help text as ids instead of pointers"
git tag cli-v0.2.0 && git push --follow-tags
```

Then point `main/idf_component.yml` at `cli-v0.2.0` and delete this patch.

Until then, build against the checkout:

```sh
idf.py -DESP_COMPONENTS_DIR=/abs/path/to/esp_components build
```

That rewrites `dependencies.lock` with local paths — `idf.py reconfigure` puts the pins
back, and `git diff` before committing is worth the habit.

### What it changes

The ids go in a `cli_command_text_t` array hanging off `cli_group_t`, parallel to
`commands[]`, rather than in two more fields on `cli_command_t`. `cli_command_t` rows are
written as positional initialisers throughout this project and others, and appending to
that struct makes every one of them a `-Wmissing-field-initializers` error under
`-Wextra -Werror`. This way existing tables compile untouched, and with no resolver
registered the ids are ignored — so a project with no string catalogue behaves exactly as
it did.
