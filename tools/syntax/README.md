# Syntax check without ESP-IDF

```bash
./check.sh
```

A full `idf.py build` is the only real proof, but it is slow and it is not
always to hand. This compiles the firmware's own C++ against stub headers for
the ESP-IDF and WAMR APIs — enough to catch the errors that come from *editing*
rather than from hardware: a definition outside the namespace its declaration
lives in, a missing include, a signature that no longer matches, a name that
does not exist.

It exists because of a specific mistake. Six new host functions were appended to
`sdk/wasm_host_functions.cc` after `}  // namespace sdk`, so they compiled as
globals while the header declared them inside `sdk`. Every reference to
`MPX_OK` in them then failed to resolve. A code review does not catch that; a
one-second compile does.

The stubs are deliberately minimal — just enough to typecheck. They are **not**
a simulator, they prove nothing about behaviour, and a green run here does not
mean the firmware is correct. It means it will get past the compiler.

Keep them honest: if a stub drifts from the real API the check goes green while
the build goes red, which is worse than not having it. When a real signature
changes, change the stub in the same commit.
