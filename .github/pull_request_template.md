## What and why

<!-- What this changes, and the reason. Link the roadmap row or issue if there is one. -->

## How it was checked

<!-- Tests added or run; numbers if it changes accuracy or speed. -->

## Rough edges

<!-- Anything left messy on purpose, to clean up later. "None" is fine. -->

## Checklist

- [ ] Branch from an up-to-date `main`; one job per PR
- [ ] `cmake --build --preset asan && ctest --preset asan` passes locally
- [ ] Docs updated if behaviour changed (`docs/`, README)
- [ ] No secrets, credentials or real customer data
- [ ] Not opened by Scramblehub? Wait for Scramblehub's approval (the **Owner approval** check) before merging
