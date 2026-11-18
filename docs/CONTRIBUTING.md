# How work is done in this repo

Kept short on purpose. Change it when it stops fitting.

## Branches

- `main` always installs and passes its tests. No direct commits once Phase 0 is done.
- One branch per job from the roadmap, named by kind:
  - `feat/<name>`: a new capability, e.g. `feat/frontend-python`
  - `fix/<name>`: a bug fix
  - `exp/<name>`: a research spike that may be thrown away. Merge only what's worth keeping, or
    just write down what you learned in `docs/notes/`
  - `docs/<name>`: documentation only
- Merge through a pull request, even working alone. The PR description is the record of why.
- Messy first, clean later: a branch may merge rough, as long as `main` still passes its tests and
  the roughness is noted in the PR.

## Milestones

Each roadmap phase ends with a tag (`v0.1`, `v0.2`, ...) and a short entry in `docs/CHANGELOG.md`.

## Decisions

Decisions that would be expensive to reverse (IR format, implementation language, generated
stack) go in `docs/decisions/NNNN-title.md`: the context, the decision, and the alternatives turned down.
