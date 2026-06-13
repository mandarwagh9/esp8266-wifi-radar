# Contributing to WiFi Radar

Thanks for your interest — contributions are very welcome! 🎉

## Development setup

- Install [PlatformIO Core](https://platformio.org/install) (or the VS Code extension).
- `cp src/secrets.example.h src/secrets.h` and add your WiFi credentials.
- Build: `pio run` · Flash: `pio run -t upload` · Monitor: `pio device monitor`

CI runs `pio run` on every push and PR, so please make sure your change builds.

## Project layout

```
src/main.cpp           firmware: sampling, detection, web server, dashboard
src/secrets.example.h  template for WiFi credentials (copy to secrets.h)
platformio.ini         board / build configuration
docs/                  documentation
.github/               CI workflow + issue / PR templates
```

## Coding style

- Keep it readable and match the surrounding style (2‑space indent, descriptive names).
- It runs on a microcontroller — avoid heap churn in hot paths; prefer stack buffers.
- The dashboard is a single self‑contained page in the `PAGE[]` string; keep it dependency‑light and small.

## Commits & pull requests

- Use [Conventional Commits](https://www.conventionalcommits.org/) (`feat:`, `fix:`, `docs:`, `refactor:`, `chore:`).
- Branch from `main` (`feat/...`, `fix/...`), open a PR, and describe **what** changed and **why**.
- One logical change per PR where possible.
- Never commit `src/secrets.h` (it's git‑ignored — keep it that way).

## Reporting bugs & ideas

Open an issue using the templates. Your board + flash size and a snippet of `pio device monitor` output help enormously.

By contributing you agree your work is licensed under the project's [MIT License](LICENSE).
