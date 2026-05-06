# Documentation Style Guide

Single source of truth for all Markdown documentation in this repository.

When adding or updating any `*.md` file, follow the rules below. Templates for
each doc type live in [`docs/templates/`](templates).

## Scope

These rules apply to:

- the root `README.md`
- `AGENTS.md` and other contributor guides
- `components/<name>/README.md` for first-party components
- `products/<name>/README.md` and `products/<name>/ARCHITECTURE.md`
- `products/<name>/docs/*.md` (service / module docs)
- `products/<name>/specs/*.md` (design specs)
- `tests/README.md` and other test-runner docs

These rules do **not** apply to:

- third-party / vendor component docs (e.g. `components/esp-nimble-cpp/`,
  `components/embedded-i2c-scd4x/`, `components/libnmea-esp32/`); leave
  those untouched
- auto-generated content (e.g. `.pytest_cache/README.md`)
- `AGENTS.md` and other agent-instruction files; they are not
  contributor documentation and do not need to follow this guide

## Doc Lifecycle

The repository keeps two kinds of Markdown files. They have different
intents and different lifecycles.

| Kind | Path | Tense | Audience | Lifetime |
|---|---|---|---|---|
| Doc | `README.md`, `components/<name>/README.md`, `products/<name>/README.md`, `products/<name>/ARCHITECTURE.md`, `products/<name>/docs/*.md`, `tests/README.md` | Present — describes what currently exists | Anyone reading the codebase | Evergreen — kept in sync with the code |
| Spec | `products/<name>/specs/*.md` | Future / imperative — describes how a feature will be built | The implementer | Temporary — usually deleted once shipped |

Rules that follow from this:

- A doc must reflect the **shipped reality**. If the code and the doc
  disagree, the doc is wrong.
- A spec must capture **intent before implementation**. Once the feature
  ships, update the corresponding doc to describe what was actually built.
- After a spec is fully implemented and the corresponding doc reflects
  reality, the spec is **typically deleted**. Deletion is manual and at the
  author's discretion — there is no archive folder.
- When implementing a spec, prefer focused commits that update the spec,
  the code, and the corresponding doc together so they don't drift.

## Brand and Casing

- Always **AirGradient** (never "Airgradient", "airgradient" outside of
  identifiers, "AIRGRADIENT", etc.)
- First-party component H1: **`# airgradient-<name>`** matching the directory
  name exactly. No `Component` suffix in the H1.
- Section headings: **Title Case** (`Directory Layout`, not
  `Directory layout`)
- File names for new docs: **`snake_case.md`**

## Required Heading Layout

Every Markdown file:

- has exactly **one** H1 at the top
- uses **ATX-style** headings (`#`, `##`, `###`) — no underline (`====`)
  headings
- never skips a heading level (no `#` then `###`)
- uses sentence-style prose under each heading; do not nest the H1's title
  inside the body

## Code Fences

Always include a language hint:

| Content | Fence language |
|---|---|
| Shell commands | ```` ```sh ```` |
| C / C++ code | ```` ```cpp ```` |
| Python code | ```` ```python ```` |
| ASCII trees, register maps, plain output | ```` ```text ```` |
| Mermaid diagrams | ```` ```mermaid ```` |
| JSON, YAML, TOML, etc. | matching language hint |

Do not use ```` ```bash ```` — use ```` ```sh ```` for portability.

Do not use untagged fences in first-party docs.

## Code in Documentation

Docs describe what the code does; they are not a place to **copy** the code.
Readers should read the source for full implementations.

Rules for non-spec docs (READMEs, service docs, ARCHITECTURE):

- **Soft cap of ≤ 15 lines per code block.** If you need more, you are
  copying code that will rot.
- For longer examples, replace the block with a short preview plus a link
  to the source file. Use a relative path:

  ```text
  See [`go_ble.cpp`](../main/go_ble.cpp) for the full call sequence.
  ```

- Prefer **method or function signatures** over working call examples when
  the goal is to enumerate the API.
- Prefer **a 5–10 line minimal call site** plus a pointer to a real example
  when the goal is to show how something is wired up.
- Never paste an entire `init()` or `start()` body into a doc — link to it.

Specs are **exempt** from this cap. Specs guide implementation and may
include longer code sketches, interface drafts, and pseudo-code.

## Diagrams

Use the right tool for the shape of the information. Both ASCII and
Mermaid render on GitHub; Mermaid produces clearer output for non-trivial
flows and is preferred whenever the content has state, branching, or
multiple actors.

| Diagram type | Use | Fence |
|---|---|---|
| State machine | Mermaid `stateDiagram-v2` | ```` ```mermaid ```` |
| Multi-actor sequence (more than 3 steps) | Mermaid `sequenceDiagram` | ```` ```mermaid ```` |
| Branched / decision flow (any branch, or more than 4 nodes) | Mermaid `flowchart TD` | ```` ```mermaid ```` |
| Class / module relationship | Mermaid `classDiagram` | ```` ```mermaid ```` |
| Linear flow with no branches, ≤ 4 steps | ASCII (`A -> B -> C`) | ```` ```text ```` |
| Directory tree | ASCII | ```` ```text ```` |
| Register address map | ASCII or table | ```` ```text ```` or table |

Notes:

- GitHub renders Mermaid natively; no extra tooling or build step is
  required.
- Keep diagram source readable in the raw file — break long Mermaid blocks
  with comments or whitespace.
- If a diagram needs more than ~30 nodes, the doc is too dense. Split it.

## Required Sections

Each doc type has a fixed section order. Use the matching template under
[`docs/templates/`](templates) as the starting point.

### Component README (`components/<name>/README.md`)

1. Title (`# airgradient-<name>`) and one-line summary
2. `## Status` — `Stable`, `Experimental`, or `Scaffold`
3. `## Scope` — what the component owns / does not own
4. `## Directory Layout` — `text` tree plus per-directory bullets
5. `## Public Includes` — include-by-role examples
6. `## Design` — short ASCII flow plus prose
7. `## Usage` — minimal `cpp` example
8. `## Configuration` — Kconfig table (`Symbol` / `Default` / `Purpose`),
   omit the section if the component has none
9. `## Dependencies` — bullet list of other components / managed components
10. `## Tests` — link to `tests/README.md` and component-local test path
11. `## Notes` — optional caveats, gotchas, follow-up work

### Product README (`products/<name>/README.md`)

1. Title and one-line product description
2. `## Sensors` — bullet list with sensor model and quantity
3. `## Hardware Notes` — required pre-init order, power rails, etc.
4. `## Build` — exact `idf.py` invocation
5. `## Documentation` — links to `ARCHITECTURE.md`, `docs/`, `specs/`,
   board config

### Service Doc (`products/<name>/docs/<service>.md`)

1. Title and one-paragraph summary
2. `## Files` — table (`File` / `Purpose`)
3. `## Dependencies` — table (`Dependency` / `Source` / `Usage`)
4. `## Public API`
5. `## Behavior` — state machines, lifecycle, timing
6. `## Edge Cases / Errors`

### Spec Doc (`products/<name>/specs/<topic>.md`)

1. Title and one-paragraph problem statement
2. `## Problem`
3. `## Goals`
4. `## Non-Goals`
5. `## Design`
6. `## Implementation Plan`
7. `## Testing Strategy`
8. `## Open Questions`

## Subdirectory READMEs

Avoid `README.md` files inside component subdirectories (`hal/`, `drivers/`,
`backends/`, `tests/`, etc.) unless they carry **substantial unique content**
beyond what the parent component README already says.

If a subdirectory genuinely needs its own README, follow the same heading
discipline as a component README in miniature: a single H1, Title Case
headings, language-tagged fences.

## Tables

- Use Title Case for header cells
- Keep tables compact; if a table grows beyond ~10 rows of dense prose,
  consider switching to a bulleted list or splitting it
- Surround tables with blank lines

## Cross-References

- Use **relative** links between repo docs (`../components/airgradient-ble/README.md`),
  not absolute paths or full GitHub URLs
- Link to anchors with kebab-case slugs (`#directory-layout`)
- Verify links still resolve when files move; broken cross-refs are bugs

## Configuration Tables

For Kconfig, settings fields, and similar enumerations, use:

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_FOO_BAR_MS` | `1000` | One-line intent |

Never document configuration purely in prose.

## Lint Enforcement

The repository runs `markdownlint-cli2` via `pre-commit` on every staged
`*.md` file. Configuration lives in `.markdownlint.json` and mirrors the
rules above.

To run the lint locally:

```sh
pre-commit run markdownlint-cli2 --all-files
```

## Contributor Checklist

Before opening a PR that touches docs:

- [ ] Single H1, ATX style, Title Case section headings
- [ ] Every code fence has a language hint
- [ ] Component / product / service / spec doc follows the matching template
- [ ] No `Airgradient` typos (always `AirGradient`)
- [ ] Cross-links resolve
- [ ] Code blocks in non-spec docs are ≤ 15 lines, or replaced with a
      short preview plus a link to the source file
- [ ] State machines, sequences, and branched flows use Mermaid; ASCII is
      reserved for ≤ 4-step linear flows and directory trees
- [ ] Doc tense matches its kind: docs describe what _exists_ (present),
      specs describe what _will be built_ (future)
- [ ] If this PR ships a spec, the matching doc is updated and the spec
      is deleted (or scheduled for deletion)
- [ ] `pre-commit run --all-files` passes
