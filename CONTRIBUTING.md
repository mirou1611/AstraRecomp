# Contributing to AstraRecomp

AstraRecomp is in correctness-first bring-up. Small changes with a trace,
conformance source, or deterministic regression test are more useful than broad
untested compatibility claims.

## Before opening a change

1. Keep the portable C++17 core free of Vita-only headers and APIs.
2. Preserve the interpreter as the semantic oracle for AOT and future JIT paths.
3. Add or update a host regression test for behavioral changes.
4. Run `cmake --build build-host` and
   `ctest --test-dir build-host --output-on-failure`.
5. Cross-build the VPK when touching portable runtime or Vita code.

Do not commit BIOS dumps, retail software, SDK files, copyrighted firmware, or
code copied from license-incompatible emulators. External projects may be used to
understand observable semantics; AstraRecomp implementations must remain original
and compatible with the repository's MIT license.

## Useful starting points

- [Roadmap](docs/ROADMAP.md)—milestones and current boundaries
- [Architecture](docs/ARCHITECTURE.md)—ownership and portability rules
- [PS2Recomp integration](docs/PS2RECOMP_INTEGRATION.md)—AOT contracts
- [Visual system](docs/BRAND.md)—art, color, typography, and asset rules
