# Implementation Plan & Roadmap: `ussxmit`

## 1. Directory Structure

```
ussxmit/
├── README.md               # User documentation & CLI examples
├── DESIGN.md               # Architectural design & Netdata specification
├── PLAN.md                 # Implementation plan & milestones
├── LICENSE                 # Project license (Apache 2.0 / MIT)
├── Makefile                # Build system supporting ibm-clang and xlc
├── .gitignore              # Git ignore rules for build artifacts
├── include/
│   ├── ussxmit.h           # Core declarations and configuration
│   ├── netdata.h           # Netdata structures, constants, and text unit keys
│   ├── dynalloc.h          # BPXWDYN / SVC 99 dynamic allocation interface
│   ├── tso_bridge.h        # IKJEFTSR TSO service wrapper
│   ├── dataset_io.h        # Dataset record I/O helpers
│   └── utils.h             # String, EBCDIC/ASCII conversions, and logging
├── src/
│   ├── main.c              # CLI argument parser and action dispatcher
│   ├── xmit.c              # XMIT action implementation
│   ├── receive.c           # RECEIVE action implementation
│   ├── inspect.c           # Archive inspection & metadata query logic
│   ├── netdata.c           # Netdata stream parsing & generation
│   ├── dynalloc.c          # Dynamic dataset allocation and cleanup
│   ├── tso_bridge.c        # IKJEFTSR execution bridge
│   ├── dataset_io.c        # MVS record I/O wrappers
│   └── utils.c             # Utility functions
└── tests/
    ├── Makefile            # Test harness build
    ├── test_netdata.c      # Unit test: Netdata parser & text units
    ├── test_cli.c          # Unit test: CLI argument validation
    └── integration_test.sh # Integration test script for z/OS USS
```

---

## 2. Implementation Milestones

### Milestone 1: Project Scaffolding & CLI Dispatcher
- [ ] Create repository structure, header files, and [`Makefile`](Makefile:1).
- [ ] Implement CLI argument parser in [`src/main.c`](src/main.c:1) supporting `xmit`, `receive`, `--list`, and option flags (`-v`, `-f`, `-u`, `-s`).
- [ ] Implement utility helpers in [`src/utils.c`](src/utils.c:1) (EBCDIC/ASCII translation, logging).

### Milestone 2: Dynamic Allocation & MVS I/O Layer
- [ ] Implement [`src/dynalloc.c`](src/dynalloc.c:1) wrapping `BPXWDYN` for creating and deleting temporary sequential staging datasets (`RECFM=FB,LRECL=80,BLKSIZE=3120`).
- [ ] Implement [`src/dataset_io.c`](src/dataset_io.c:1) to read and write standard 80-byte binary records to/from datasets and Unix `stdin`/`stdout`.
- [ ] Verify dataset allocation and deletion under USS.

### Milestone 3: TSO Service Bridge (IKJEFTSR)
- [ ] Implement [`src/tso_bridge.c`](src/tso_bridge.c:1) to invoke TSO services via `IKJEFTSR` or batch command execution.
- [ ] Implement `xmit_dataset()` in [`src/xmit.c`](src/xmit.c:1):
  - Allocate temporary staging file.
  - Execute `XMIT dummy.dummy OUTDSNAME('TEMP.DSN') NOLOG DATASET('SOURCE.DSN')`.
  - Stream 80-byte records to `stdout`.
  - Delete temporary staging file.
- [ ] Implement `receive_dataset()` in [`src/receive.c`](src/receive.c:1):
  - Read stream from `stdin` into temporary staging file.
  - Parse `INMR01`/`INMR02` to extract default dataset name if not provided.
  - Execute `RECEIVE INDSNAME('TEMP.DSN')` with `RESTORE DATASET('TARGET.DSN')`.
  - Delete temporary staging file.

### Milestone 4: Native Netdata Inspection & Header Parser
- [ ] Implement [`src/netdata.c`](src/netdata.c:1) to decode `INMR01`, `INMR02`, and text units without invoking TSO.
- [ ] Add `ussxmit inspect < file.xmi` / `ussxmit receive --list < file.xmi` to show dataset metadata (DSN, DSORG, RECFM, LRECL, space) directly on USS terminal.

### Milestone 5: Testing, Validation & Documentation
- [ ] Write unit test suite for Netdata record encoding/decoding.
- [ ] Write integration test suite on z/OS exercising:
  - Sequential dataset (`DSORG=PS`) export and restore.
  - Partitioned dataset (`DSORG=PO`) export and restore.
  - PDSE and Load Module / Program Object export and restore.
  - Pipeline verification: `ussxmit xmit "HLQ.PDS" | ussxmit receive "HLQ.PDS.NEW"`.
  - Round-trip integrity comparison (record-by-record verification).
- [ ] Finalize README and documentation for GitHub release.

---

## 3. Build & Compilation Strategy

The build system will support both `ibm-clang` (Clang LP64 on z/OS) and IBM `xlc`:

### Compilation Flags
```makefile
# IBM Open XL C/C++ (ibm-clang / clang)
CC = ibm-clang
CFLAGS = -O2 -fzos-le-char-mode=ascii -D_EXT -D_XOPEN_SOURCE_EXTENDED=1 -I./include

# Or IBM XL C/C++ (xlc)
# CC = xlc
# CFLAGS = -O2 -q64 -qgoff -D_EXT -D_XOPEN_SOURCE_EXTENDED=1 -I./include

LDFLAGS = -Wl,lp64
```

---

## 4. GitHub Readiness Checklist

- [x] Initial design and architecture documentation in [`DESIGN.md`](DESIGN.md).
- [x] CLI specifications and usage examples in [`README.md`](README.md).
- [ ] Standard open-source `LICENSE` (Apache 2.0).
- [ ] `.gitignore` configured for z/OS object files (`*.o`, `*.x`, binary targets).
- [ ] Clean build and test script suitable for automated or local execution.
