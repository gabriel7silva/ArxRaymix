# Maintenance guide

A reference for people changing Arx Raymix. It answers "where does this live", "what happens if I change it", and "what has already gone wrong here". Everything under `arx/` that is not a fork addition is upstream Arx Libertatis; this guide covers the fork.

This tree is English, like the rest of the repository.

## Start here

Find the question you arrived with.

| I want to… | Read |
|---|---|
| Understand what the pieces are and who owns them | [COMPONENTS.md](COMPONENTS.md) |
| See how it fits together as a picture | [MAP.md](MAP.md) |
| Change something and I do not know which file | [WHERE-TO-EDIT.md](WHERE-TO-EDIT.md) |
| Find where ray tracing is implemented | [RAYTRACING.md](RAYTRACING.md) |
| Turn an effect up or down | [TUNING.md](TUNING.md) |
| Know what breaks if I change X | [INVARIANTS.md](INVARIANTS.md) |
| Look up a word this project uses oddly | [GLOSSARY.md](GLOSSARY.md) |

If you are debugging something that is already broken, start at the symptom index in [INVARIANTS.md](INVARIANTS.md).

## How this guide stays true

The rendering code changes fast. A guide full of line numbers and current values is wrong within a day, and a confidently wrong guide is worse than no guide. These rules exist so that does not happen. Follow them when you edit this tree.

1. **Quote a number only if it is a bound or a relationship.** A *bound* is a limit imposed from outside the file that holds it, such as an API cap. A *relationship* is an equation between named symbols. A *setting* — any value chosen because it looked right on screen — is never printed here. Give the grep that prints it instead.
2. **Anchor on symbols, never line numbers.** Say "in `D3D12Rtao::apply`", not "at line 1693". Line numbers rot on the next commit; a symbol name survives until someone renames it, and a rename is a loud, reviewed event.
3. **Invariant ids are permanent.** `INV-NN` numbers are append-only. Other files link them. A rule that stops applying is struck through in place, keeping the reason it was ever a rule. Never renumber, never delete, never reuse an id.
4. **Every grep printed here must return a hit.** A grep that matches nothing is a bug report against this guide, not against the code. `scripts/Check-MaintenanceDocs.ps1` checks them all.
5. **No performance numbers.** No frame rates, no milliseconds per pass, no "High costs about 4 ms". They are specific to one GPU, one driver, one scene, and they rot faster than anything else. Describe cost by mechanism instead: what grows, and roughly how.
6. **Repo-relative paths only.** `docs/CONTRIBUTING.md` forbids committing drive letters, user profile directories and anything under `runtime/`. That applies here, and in commit messages, and in pull request descriptions.

Rule 1 is the load-bearing one. It is why this guide describes the descriptor heap as an equation between five constants rather than as five numbers: the numbers have all changed, the equation has not.

## Who owns what

Three documents describe the renderer and they do not overlap. Keeping the boundary sharp is what stops them from contradicting each other.

| Document | Owns | Does not own |
|---|---|---|
| `arx/src/graphics/dxr/README.md` | Why a ray tracing value is what it is, the algorithms, the phase history | Where things live, project-wide concerns |
| `docs/maintenance/` (this tree) | Where things live, in what order they run, what breaks when you change them | Any "why this number" — link there instead |
| `docs/ARCHITECTURE.md` | The public one-window, two-backends story | Anything a maintainer needs in order to make a change |

The practical test while writing here: if a sentence contains "because" followed by a tuning value, it belongs in `arx/src/graphics/dxr/README.md`.

## Conventions

- Paths are repo-relative and use forward slashes.
- Code is referenced as `path` plus the enclosing symbol.
- Every "read it" grep is a runnable one-liner. Run them from the repository root.
- Log lines are quoted as the literal prefix you would grep for, without the values that follow.

## Checking this guide

```
pwsh -File scripts/Check-MaintenanceDocs.ps1
```

It extracts every fenced `grep` from this tree and fails on the first one that matches nothing. Run it after changing anything here, and after any change that renames a symbol this guide names.
