# ussxmit — UNIX System Services TSO XMIT & RECEIVE Utility

`ussxmit` is a unified command-line utility for IBM z/OS UNIX System Services (USS) that brings native Unix stream and pipe semantics to standard MVS TSO XMIT (Netdata / TRANSMIT / RECEIVE) archives.

---

## Overview

On z/OS, dataset interchange has historically required interactive TSO sessions or complex JCL batch jobs using `IKJEFT01` with `XMIT` and `RECEIVE` commands. `ussxmit` provides a unified C CLI operating directly in USS shells (bash, zsh, sh) with standard Unix `stdin` and `stdout` streaming.

### Key Capabilities
- **Single Unified Binary**: Invoked as [`ussxmit xmit <dsname>`](ussxmit/README.md:27) or [`ussxmit receive [target-dsname]`](ussxmit/README.md:37). Symlinking `xmit` and `receive` to `ussxmit` allows shorthand execution.
- **Full Standard XMIT / Netdata Compatibility**: Generates and unpacks byte-identical IBM TSO XMIT streams (`RECFM=FB`, `LRECL=80`, `INMR01`–`INMR06` control records). Files created by `ussxmit` can be unpacked by standard `TSO RECEIVE` and tools like `recv370`, and vice versa.
- **Dataset Support**: Supports Sequential (`DSORG=PS`), Partitioned (`DSORG=PO`), PDSE (`DSORG=PO-E`), load modules, and program objects.
- **Unix Pipeline Friendly**: Writes raw XMIT binary streams to `stdout` and consumes them from `stdin`, enabling direct piping through tools like `gzip`, `ssh`, `tar`, and `curl`.

---

## CLI Usage

### Synopsis

```bash
ussxmit <action> [options] [dataset-name]
```

### Actions

#### 1. `xmit` — Export Dataset to XMIT Stream (STDOUT)

```bash
# Stream dataset to an XMIT file
ussxmit xmit "HLQ.SOURCE.PDS" > mypds.xmi

# Pipe directly through compression
ussxmit xmit "HLQ.SOURCE.DATA" | gzip -c > mydata.xmi.gz

# Stream across network over SSH directly to another z/OS host
ussxmit xmit "HLQ.PROD.LOADLIB" | ssh user@target-zos "ussxmit receive 'HLQ.TEST.LOADLIB'"
```

#### 2. `receive` — Restore XMIT Stream (STDIN) to Dataset

```bash
# Restore from stdin to original dataset name embedded in the archive
ussxmit receive < mypds.xmi

# Restore from stdin into a user-specified target dataset name
ussxmit receive "HLQ.TARGET.NEWPDS" < mypds.xmi

# Decompress and receive in a single pipeline
gzip -dc mypds.xmi.gz | ussxmit receive "HLQ.RESTORED.PDS"
```

---

## Symlink Shorthand

When installed, symlinks can be created pointing to `ussxmit`:

```bash
ln -s /usr/local/bin/ussxmit /usr/local/bin/xmit
ln -s /usr/local/bin/ussxmit /usr/local/bin/receive

# Direct invocation
xmit "HLQ.SOURCE.PDS" > mypds.xmi
receive "HLQ.TARGET.PDS" < mypds.xmi
```

---

## Documentation

- Detailed architecture, record layouts, and MVS system interfaces: [`DESIGN.md`](DESIGN.md)
- Implementation roadmap, milestones, and build instructions: [`PLAN.md`](PLAN.md)
