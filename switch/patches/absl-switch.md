# Abseil edits required by the Switch port

`/tmp/deps/absl` is a scratch copy; these two edits must be reapplied whenever it is rebuilt.

Both exist because Abseil's flag machinery runs during static initialisation, and acquiring a flag's
`absl::Mutex` there aborts with an instruction fault on Horizon (`InstructionAbortLowerEl`), taking the
process down before `main()`. The game runtime never reads, writes or observes absl flags, so the
registration path is neutered rather than fixed.

## 1. `absl/flags/reflection.cc` — `RegisterCommandLineFlag`

```cpp
bool RegisterCommandLineFlag(CommandLineFlag& flag, const char* filename) {
  (void)flag;
  (void)filename;
  return true;
}
```

## 2. `absl/flags/internal/flag.cc` — `FlagImpl::SetCallback`

```cpp
void FlagImpl::SetCallback(const FlagCallbackFunc mutation_callback) {
  (void)mutation_callback;
}
```

Leave the original bodies above them as comments if preferred; nothing else in `flag.cc` changes.

`GetFlag`/`SetFlag` will therefore find no registered flags. Nothing in the port calls them, and
`switch/probe_absl.cpp` is the reproducer that isolated this.
