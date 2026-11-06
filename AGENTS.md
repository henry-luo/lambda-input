# lambda-input agent instructions

The authoritative architecture is Lambda formal design D9.1.1v3–D9.1.4v2. Input must not import the Lambda runtime or Radiant, and input-owned values must retain their existing allocation and lifetime rules. Preserve the precise-rooting contract and do not add allocation fallbacks.

Use C++17 and the project `lib` types, rather than C++ standard-library containers. Do not patch vendored dependencies, edit generated Lua files, change `log.conf`, or write temporary files outside `./temp/`. Fix root causes and preserve test coverage. During extraction, follow the fuller instructions in the sibling Lambda project's `AGENTS.md`.
