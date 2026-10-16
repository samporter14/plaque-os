# Development Workflow

This page defines the expected local workflow before opening a pull request.

## 1) Fork and create a focused branch

- Fork the repository to your own GitHub account
- Clone your fork locally and add the upstream repository if needed
- Enable repo hooks once per clone: `git config core.hooksPath .githooks && chmod +x .githooks/pre-commit`

- Branch from `main`
- Keep each PR focused on one fix or feature area

## 2) Implement with scope in mind

- Plaque OS is a small fork: open an issue to discuss anything large before you build it
- Prefer incremental changes over broad refactors

## 3) Run local checks

```sh
./bin/clang-format-fix -g
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e x4pro
```

CI builds `x4pro` and `x4c` and runs the Python guards. Use clang-format 21+ locally (the base files were not all formatted when this fork started, so format only what you change: `./bin/clang-format-fix -g`).
If `clang-format` is missing or too old locally, see [Getting Started](./getting-started.md).

## 4) Open the PR

- Target `main` (the repository's default branch)
- Use a short, descriptive title (example: `fix: avoid crash when opening malformed epub`)
- Fill out `.github/PULL_REQUEST_TEMPLATE.md`
- Describe the problem, approach, and any tradeoffs
- Include reproduction and verification steps for bug fixes

## 5) Review etiquette

- Be explicit and concise in responses
- Keep discussions technical and respectful
- Assume good intent and focus on code-level feedback

Be respectful: this is a small volunteer project.
