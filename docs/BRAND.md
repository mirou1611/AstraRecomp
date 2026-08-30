# AstraRecomp visual system

The visual direction is **astral signal**: the calm depth of an early console
system menu, expressed through modern technical imagery. It should feel precise,
nocturnal, optimistic, and compact—not nostalgic imitation.

## Principles

- Show translation, timing, packets, layers, and constrained hardware through
  abstract light and geometry.
- Use generous dark space. Cyan communicates active data; violet communicates
  translation or boundary crossing; magenta is a rare emphasis.
- Never copy Sony or PlayStation logos, controller symbols, boot visuals, product
  silhouettes, or proprietary typefaces.
- Never imply that generated art is gameplay or an emulator screenshot.

## Color palette

| Token | Hex | Use |
| --- | --- | --- |
| Void | `#050711` | Primary background |
| Midnight | `#0B1026` | Elevated dark surface |
| Slate | `#18234A` | Panels, dividers, quiet badges |
| Signal cyan | `#35D9FF` | Active data, links, success accents |
| Runtime blue | `#5271FF` | Vita/runtime concepts |
| Translation violet | `#875BFF` | Recompiler and boundary concepts |
| Pulse magenta | `#FF4FD8` | Sparse warnings or focal accents |
| Frost | `#EAF7FF` | Primary text on dark surfaces |
| Mist | `#9CB2CE` | Secondary text |

## Typography

Use open, widely available families rather than proprietary console fonts.

- Display: **Space Grotesk**, fallback `Inter, Segoe UI, sans-serif`
- Body/UI: **Inter**, fallback `Segoe UI, Arial, sans-serif`
- Code/data: **IBM Plex Mono**, fallback `Cascadia Mono, Consolas, monospace`

Repository Markdown should remain readable with GitHub's native type stack. These
families are direction for future web pages, screenshots, and the Vita UI; font
files are not bundled until their licenses and memory cost are reviewed.

## Image use

- `assets/astra-hero.png`: repository landing hero; code transforms into a
  compact runtime. Do not label it as a screenshot.
- `assets/astra-architecture.png`: architecture and roadmap header; synchronized
  processors, timed packets, and framebuffer layers.

Keep meaningful alt text on every use. Text must stay outside raster artwork so
it remains crisp, searchable, and accessible.
