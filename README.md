# ussxmit — UNIX System Services TSO XMIT & RECEIVE Utility

`ussxmit` is a high-performance, in-process command-line utility for IBM z/OS UNIX System Services (USS) that brings native Unix stream and pipe semantics (`stdout` / `stdin`) to standard MVS TSO XMIT (Netdata / TRANSMIT / RECEIVE) archives with **zero temporary DASD staging**.

---

## Overview

On z/OS, dataset interchange has historically required interactive TSO sessions, TMP background jobs, or complex JCL batch streams using `IKJEFT01` with `XMIT` and `RECEIVE` commands, which stage files to temporary DASD datasets.

`ussxmit` provides a unified C CLI operating directly in USS shells (`bash`, `zsh`, `sh`) that encodes datasets into compliant Netdata card streams to `stdout` and restores streams from `stdin` directly into target MVS datasets in memory.

### Key Capabilities
- **Single Unified Binary**: Invoked as `ussxmit <action> [options] [dataset-name]` with built-in symlink support (`xmit`, `receive`).
- **Zero Temporary DASD Staging**: Operates 100% in-process using dynamic allocation (`BPXWDYN` via SVC 6) and native z/OS C record I/O (`type=record`), eliminating temporary scratch datasets.
- **Full Standard XMIT / Netdata Compatibility**: Generates and decodes IBM standard Netdata card streams (`RECFM=FB`, `LRECL=80`, `INMR01`–`INMR06` control records and text units). Archives created by `ussxmit` can be restored by standard `TSO RECEIVE` and vice versa.
- **Direct Dataset & Stream Inspection**: `ussxmit inspect` reads metadata (DSN, DSORG, RECFM, LRECL, BLKSIZE, directory blocks, origin node/user, timestamp) directly from MVS datasets, Unix files, or stdin pipes without restoring.
- **Unix Pipeline Friendly**: Streams raw XMIT binary cards directly to `stdout` and consumes them from `stdin`, enabling direct composition with `gzip`, `ssh`, `nc`, `tar`, and `curl`.
- **Dual Char-Mode Portability**: Compiles and runs cleanly in either ASCII mode (`-fzos-le-char-mode=ascii`) or native EBCDIC mode.

---

## Building and Testing

### Prerequisites
- IBM Open XL C/C++ (`clang` / `ibm-clang64` / `~/clang`) on z/OS USS.
- GNU Make.

### Build
```bash
# Build binary and symlinks in bin/
make

# Run unit tests (30/30 tests)
make test

# Run end-to-end integration tests on z/OS
tests/test_integration.sh
```

---

## CLI Usage

### Synopsis

```bash
ussxmit <action> [options] [dataset-name]
```

### Actions

| Action | Description |
|:---|:---|
| `xmit` | Export an MVS dataset to standard XMIT stream on `stdout` |
| `receive` | Restore an XMIT stream from `stdin` to an MVS dataset |
| `inspect` | Display metadata from an XMIT archive (dataset, file, or `stdin`) without restoring |

### Options

| Option | Long Option | Description |
|:---|:---|:---|
| `-f` | `--force` | Overwrite target dataset if it already exists (receive) |
| `-l` | `--list` | List/inspect metadata only (synonym for `inspect`) |
| `-v` | `--verbose` | Enable verbose diagnostic logging to `stderr` |
| `-q` | `--quiet` | Suppress informational messages |
| `-h` | `--help` | Show synopsis and usage help |
| `-V` | `--version` | Show version information |

---

## Examples

### 1. `xmit` — Export Dataset to XMIT Stream (STDOUT)

```bash
# Stream sequential dataset to an XMIT file
ussxmit xmit "HLQ.PROD.DATA" > mydata.xmi

# Pipe directly through compression
ussxmit xmit "HLQ.SOURCE.DATA" | gzip -c > mydata.xmi.gz

# Stream across network over SSH directly to another z/OS host without local files
ussxmit xmit "HLQ.SOURCE.DATA" | ssh user@target-zos "ussxmit receive 'HLQ.RESTORED.DATA'"
```

### 2. `receive` — Restore XMIT Stream (STDIN) to Dataset

```bash
# Restore from stdin to original dataset name embedded in the archive header
ussxmit receive < mydata.xmi

# Restore from stdin into a user-specified target dataset name (with force overwrite)
ussxmit receive -f "HLQ.TARGET.NEWDATA" < mydata.xmi

# Decompress and receive in a single streaming pipeline
gzip -dc mydata.xmi.gz | ussxmit receive "HLQ.RESTORED.DATA"
```

### 3. `inspect` — Inspect Archive Metadata

```bash
# Inspect an MVS dataset archive directly
ussxmit inspect "HLQ.BACKUP.XMI"

# Inspect from stdin stream
cat mydata.xmi | ussxmit inspect

# Inspect generated stream from a live dataset
ussxmit xmit "HLQ.PROD.DATA" | ussxmit inspect
```

Sample inspection output:
```
========================================
 MVS TSO XMIT Archive Information
========================================
 Dataset Name : CCW.TESTPDS
 Organization : PO (Partitioned)
 Record Format: FB
 Record Length: 80
 Block Size   : 3120
 Utility Name : IEBCOPY
 Origin       : ZOSCAN56.CCW
 Timestamp    : 2026-11-04 16:37:29
========================================
```

---

## Symlink Shorthand

When `bin/xmit` and `bin/receive` are in your `PATH`:

```bash
# Direct xmit invocation
xmit "HLQ.SOURCE.DATA" > mydata.xmi

# Direct receive invocation
receive "HLQ.TARGET.DATA" < mydata.xmi
```

---

## Documentation

- Detailed architecture, Netdata card framing, and text unit specifications: [`DESIGN.md`](DESIGN.md)
- Implementation roadmap and milestone records: [`PLAN.md`](PLAN.md)
