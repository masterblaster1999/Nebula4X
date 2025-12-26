# Contributing

Thanks for wanting to help!

## Quick start

1. Fork + clone
2. Create a branch (`git checkout -b feature/my-thing`)
3. Build + run tests
4. Open a PR

## Ground rules

- Keep the **core simulation** (`nebula4x_core`) independent from SDL/ImGui so it can run headless.
- Prefer **data-driven** changes in `data/` over hard-coded numbers.
- Add tests for non-trivial simulation logic.

## Style

- C++20, `clang-format` (see `.clang-format`)
- Prefer `std::optional`, `std::variant`, `std::span` where appropriate
- No exceptions in hot loops; return errors via `Result`/`Status` types

## Commit messages

- Short, imperative subject line
- Include context in body if the change is not obvious
