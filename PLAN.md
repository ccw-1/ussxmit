# Implementation Plan & Roadmap: `ussxmit`

## 1. Directory Structure

```
ussxmit/
├── README.md               # User documentation & CLI examples
├── DESIGN.md               # Architectural design & Netdata specification
├── PLAN.md                 # Implementation plan & milestones
├── LICENSE                 # Project license (Apache 2.0 / MIT)
├── Makefile                # Build system supporting ibm-clang / clang
├── .gitignore              # Git ignore rules for build artifacts
├── include/
│   ├── ussxmit.h           # Core declarations, options, and exit codes
│   ├── netdata.h           # Netdata structures, constants, and text unit keys
│   ├── dynalloc.h          # BPXWDYN / SVC 6 dynamic allocation interface
│   └── utils.h             # String, EBCDIC/ASCII conversions, and logging
├── src/
│   ├── main.c              # CLI argument parser and action dispatcher
│   ├── xmit.c              # Pure in-process XMIT streaming engine
│   ├── pds_xmit.c          # In-process PDS/PDSE IEBCOPY unload engine
│   ├── receive.c           # Pure in-process RECEIVE streaming engine
│   ├── inspect.c           # Archive inspection & metadata display
│   ├── netdata.c           # Netdata stream writer, reader, and text units
│   ├── dynalloc.c          # BPXWDYN dynamic dataset allocation and cleanup
│   └── utils.c             # Code page translation (IBM-1047) and logging
└── tests/
    ├── test_netdata.c      # Unit test suite (30/30 tests)
    └── test_integration.sh # End-to-end integration test runner on z/OS
```

---

## 2. Implementation Milestones

### Milestone 1: Project Scaffolding & CLI Dispatcher ✅
- [x] Create repository structure, header files, and [`Makefile`](Makefile:1).
- [x] Implement CLI argument parser in [`src/main.c`](src/main.c:1) supporting `xmit`, `receive`, `inspect`, and option flags (`-v`, `-q`, `-f`, `-l`, `-h`, `-V`).
- [x] Implement utility helpers in [`src/utils.c`](src/utils.c:1) (EBCDIC/ASCII translation, logging).

### Milestone 2: Dynamic Allocation & In-Process MVS I/O Layer ✅
- [x] Implement [`src/dynalloc.c`](src/dynalloc.c:1) wrapping `BPXWDYN` via SVC 6 for dynamic dataset allocation and deletion.
- [x] Implement native z/OS C record I/O (`fopen("DD:...", "rb/wb,type=record")`) and attribute extraction with `fldata()`.
- [x] Verify dynamic allocation and deletion under USS on `pok56`.

### Milestone 3: Netdata Protocol Encoder & Decoder Engine ✅
- [x] Implement continuous card-level segment encoder in [`src/netdata.c`](src/netdata.c:1).
- [x] Implement continuous card-level stream reader and logical record reassembly in [`src/netdata.c`](src/netdata.c:1).
- [x] Implement bidirectional text unit encoder and parser (`INMDSNAM`, `INMMBR`, `INMDSORG`, `INMRECFM`, `INMLRECL`, `INMBLKSIZE`, `INMUTILN`, `INMDIR`).
- [x] Support spanned large logical records (>253 bytes) with `0x80`/`0x00`/`0x40` flags across 80-byte card images.

### Milestone 4: Pure In-Process XMIT, RECEIVE & Inspect Engines ✅
- [x] Implement pure in-process `do_xmit()` in [`src/xmit.c`](src/xmit.c:1) (DASD -> fldata -> Netdata card stream -> stdout).
- [x] Implement pure in-process `do_receive()` in [`src/receive.c`](src/receive.c:1) (stdin -> Netdata reader -> BPXWDYN alloc -> DASD).
- [x] Implement `do_inspect()` in [`src/inspect.c`](src/inspect.c:1) supporting direct inspection from MVS datasets, Unix files, or stdin pipes.

### Milestone 5: Testing, Validation & Documentation ✅
- [x] Implement unit test suite in [`tests/test_netdata.c`](tests/test_netdata.c:1) (30/30 tests passing).
- [x] Implement end-to-end integration test runner in [`tests/test_integration.sh`](tests/test_integration.sh:1) (all 5 integration scenarios passing on `pok56`).
- [x] Verify live streaming pipeline: `ussxmit xmit "CCW.TESTSEQ" | ussxmit receive -f "CCW.TESTSEQ.INTTEST"` with byte-for-byte fidelity.
- [x] Update documentation in [`README.md`](README.md:1), [`DESIGN.md`](DESIGN.md:1), and [`PLAN.md`](PLAN.md:1).

### Milestone 6: In-Process PDS & PDSE Streaming Unload and Restore Engine ✅
- [x] Implement in-process directory parsing for PDS & PDSE containers in [`src/pds_xmit.c`](src/pds_xmit.c:1).
- [x] Implement standard IEBCOPY unloaded data stream generator (`COPYR1` header, 276-byte directory header, 288-byte directory blocks, 12-byte CKD member blocks).
- [x] Implement dynamic attribute probe on first member to refine `RECFM`, `LRECL`, and `BLKSIZE` for partitioned datasets.
- [x] Support `ALIAS` member preservation in directory blocks while skipping duplicate physical data block emission.
- [x] Implement in-process PDS/PDSE restoration in [`src/receive.c`](src/receive.c:1) with automatic container and member DD management.
- [x] Verify end-to-end PDS streaming and restore on `pok56` with byte fidelity (`CCW.TESTPDS` -> `CCW.TESTPDS.INTTEST` and `CCW.GO.PDSE`).

---

## 3. Build & Compilation Strategy

The build system automatically detects the 64-bit Clang compiler on z/OS USS (`~/clang` or `clang`):

```makefile
ifeq ($(origin CC),default)
  CC = $(shell if [ -x "$$HOME/clang" ]; then echo "$$HOME/clang"; else echo "clang"; fi)
endif
CFLAGS ?= -O2 -fzos-le-char-mode=ascii -D_EXT -D_XOPEN_SOURCE_EXTENDED=1 -I./include -Wall
```

---

## 4. Verification Summary

| Test Category | Target | Result |
|:---|:---|:---|
| Unit Tests | `make test` (`tests/test_netdata.c`) | 30 / 30 Passed ✅ |
| CLI Options | `ussxmit -V`, `ussxmit -h` | Passed ✅ |
| Dataset Archive Inspection | `ussxmit inspect "CCW.TESTPDS.XMI"` | Passed ✅ |
| Streaming Pipeline (XMIT -> Inspect) | `ussxmit xmit "CCW.TESTSEQ" \| ussxmit inspect` | Passed ✅ |
| Streaming Pipeline (XMIT -> Receive) | `ussxmit xmit "CCW.TESTSEQ" \| ussxmit receive -f ...` | Passed ✅ |
| Restored Dataset Integrity | Re-xmit & inspect restored dataset | Passed ✅ |
| PDS Streaming Pipeline (XMIT -> Receive) | `ussxmit xmit "CCW.TESTPDS" \| ussxmit receive -f ...` | Passed ✅ |
| Restored PDS Member Integrity | Re-xmit & verify members (`MBR1`, `MBR2`) | Passed ✅ |
| PDSE GOFF Program Object Stream | `ussxmit xmit "CCW.GO.PDSE" \| ussxmit inspect` | Passed ✅ |
