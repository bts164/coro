# Cheatsheet PDF — test setup

Renders `doc/cheatsheet.md` as a compact three-column landscape PDF using
Pandoc + XeLaTeX. Try it out before committing to a build approach.

## Quick start

```bash
# Install dependencies (Ubuntu/Debian)
make install-deps

# Build the PDF
make

# Open
xdg-open cheatsheet.pdf
```

## What it produces

- A4 landscape, three columns
- Section headings with a horizontal rule
- Syntax-highlighted code blocks on a light background
- Compact monospace font (DejaVu Sans Mono at 72% scale)

## Tuning

All layout parameters are at the top of `cheatsheet.tex`:

| Setting | Where | Effect |
|---|---|---|
| Font sizes | `\setmainfont` / `\setmonofont` Scale= | Overall density |
| Columns | `\begin{multicols}{3}` | 2 columns = airier, 3 = denser |
| Margins | `\usepackage[margin=...]` | More/less whitespace around the page |
| Code font size | `\fvset{fontsize=...}` | `\tiny`, `\scriptsize`, `\footnotesize` |
| Highlight style | `--highlight-style=` in Makefile | tango, pygments, kate, monochrome, etc. |

## If it doesn't fit on few pages

The cheatsheet content is fairly dense. Options:
- Switch to A3 landscape: change `\documentclass[8pt,landscape]{extarticle}` to use `a3paper`
- Reduce font scale on `\setmonofont`
- Drop the `\setmainfont` Scale to 0.85

## If fonts are missing

DejaVu fonts ship with most Linux distros. If XeLaTeX can't find them:

```bash
sudo apt-get install fonts-dejavu
fc-cache -fv
```

Or swap to any system font — run `fc-list` to see what's available and update
the `\setmainfont` / `\setmonofont` lines in `cheatsheet.tex`.
