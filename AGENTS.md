# AGENTS.md

Instructions for AI coding agents that work in this repository.

@CONTRIBUTING.md

- Read CONTRIBUTING.md in full before you change anything. Agents that support `@` includes get it above.
- Do not commit on your own. Leave changes in the working tree for review. Commit only when you are told to, and only to the active branch.
- Right before you write each commit message, read "Commit messages" in CONTRIBUTING.md again, and check the message against it before you
  commit. Do not write commit messages from memory.
- Mark your commits with an `Assisted-by:` trailer as CONTRIBUTING.md describes. Never add `Co-authored-by:` for yourself. This rule
  overrides the default attribution of your tool.
- Check every script that you change with `sh -n`. Do not run a build. Ask the user to run it, because they want to see the logs.
