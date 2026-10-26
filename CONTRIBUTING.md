# Contributing to Vex base

This directory builds the Vex system [Jinx](https://github.com/Mintsuki/Jinx). Other software is built by recipes. The `base-files` directory holds files that go into the image as-is.  

### Development sub-trees

Vex is still in active development, hence its ABI is subject to change. So both the kernel, and the mlibc fork are located in submodules `kernel` and `mlibc`. Whenever the kernel and mlibc fork get stable enough, the submodules will go away and recipes will change to use `git_url` instead.  

Read this document before committing any changes. These rules apply only for the bootstrap repository. Other subtrees, namely `kernel` have different contribution rules.  

## AI-assisted contributions

You can use AI tools, but a human drives the work:

- Review every line that a tool generates. Understand it well enough to explain it and to fix it.
- You are responsible for the change, as if you wrote all of it yourself.
- Unreviewed, bulk or fully automated changes are not accepted.
- Mark a commit that has material AI help with an `Assisted-by:` trailer (see [Trailers](#trailers)).

## Building

```sh
scripts/configure.sh -a x86_64   # creates the build directory
scripts/build.sh                 # builds all recipes
scripts/jinx-wrap <command>      # any other jinx command, e.g. 'rebuild kernel'
```

- The build directory is `build/`. Set `BUILD_DIR` to use another one.
- `scripts/jinx-wrap` runs `jinx` from `PATH`. If there is none, it runs `scripts/jinx-fallback`.
- Clone with `--recurse-submodules`, or run `git submodule update --init` after the clone.
- `JINX_MAJOR_VER` in `Jinxfile` must match the major version of the `jinx` that runs. Change both together.

## Recipes

- A recipe is `recipes/<name>/recipe`. A host tool is `host-recipes/<name>/recipe`. The directory name is the name of the recipe.
- Jinx takes `ARCH` as `x86_64`. Vex calls the same architecture `amd64`, so a recipe maps `JINX_ARCH` to the Vex name when it needs one.
- A recipe for code that lives in this repository or in a submodule uses `source_dir`. Jinx does not patch or copy such a tree. Change the code in its submodule, not with patches.
- Raise `revision` when a recipe changes and the version does not. Jinx rebuilds a recipe only when `<version>_<revision>` changes.
- Do not edit `jinx-working-patch.patch`. `jinx regen` writes it.

## Code style

- Scripts are POSIX `sh`, unless the file says otherwise. Check them with `sh -n`, and with `shellcheck` if you have it.
- Use four spaces in scripts and tabs inside recipe functions, as Jinx does in its own documentation.
- Quote variables, and write `"${name}"` with the braces.

### Comments

A comment is only worth having when it says something the code cannot: why it is done this way and not the obvious way, or an outside
requirement. Never restate what the code next to it does.

- Write comments approximately in ASD-STE100 Simplified Technical English: short sentences, one statement per sentence, active voice, present tense.
- Start with a capital letter, and leave off the trailing period.
- Do not leave commented-out code.

### Vendored code

`scripts/jinx-fallback` comes from Jinx. Do not format it and do not edit it by hand. Replace the whole file when you update it.

## Commit messages

- Make one logical change per commit.
- The subject line is `<area>: <summary>`:
  - Take the area from the directories that the commit changes: `scripts:` for a change only under `scripts/`, `kernel:` or `mlibc:` for a new commit of a submodule, the name of the recipe for a change only in that recipe. Use `general:` only when a commit spans several areas. Keep unrelated areas out of a commit instead of widening its area.
  - Write the summary in lowercase and in the imperative mood (`scripts: let configure.sh take an architecture`), with no trailing period.
- Keep all lines within 72 columns.
- Most commits need no body. Write one only when the reader needs something that the subject and the diff do not show: why the change is done this way, or an outside contract. Do not list the changed files.
- Write the body in ASD-STE100 Simplified Technical English: short sentences, one statement per sentence, active voice, present tense.

### Trailers

Trailers go in the last paragraph of the message, after a blank line.

- A commit that has material AI help gets an `Assisted-by:` trailer in the form `AGENT_NAME:MODEL_VERSION`. Use the full lowercase model identifier, not only the model family:

  ```
  scripts: let configure.sh take an architecture

  Assisted-by: Claude:claude-opus-5-5
  ```

- Do not add `Co-authored-by:` for AI tools.
