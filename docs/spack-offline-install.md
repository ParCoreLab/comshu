# Installing pcomm with spack on an offline cluster

The normal path is in the [README](../README.md): `spack repo add` the repo URL
and `spack install pcomm`. That needs a network and a recent spack. This file is
for a cluster with neither. It was verified on MareNostrum 5 on 2026-09-16.

Two things rule out the obvious approach there:

- MN5's own `spack/0.22.0` module predates the v2.0 package API, so it cannot
  load this repo's `parcore` package repository at all.
- MN5 has no outbound network, so spack can fetch nothing, including the clingo
  it needs before it can concretize anything.

## 1. Bring your own spack

Copy a spack 1.3 tree and the builtin package repository to the cluster:

```bash
rsync -az --exclude '.git' spack/ user@cluster:~/spack-1.3/
rsync -az --exclude '.git' ~/.spack/package_repos/<hash>/ user@cluster:~/spack-builtin/
```

Point spack at both by **path**. Git URLs make spack try to clone, which hangs
with no network:

```yaml
# ~/.spack/repos.yaml
repos:
  builtin: /home/you/spack-builtin/repos/spack_repo/builtin
  parcore: /home/you/pcomm-src/spack_repo/parcore
```

## 2. Give it clingo

Spack cannot concretize without clingo and cannot bootstrap it offline. Copying
your own bootstrap store does not work: that clingo is a Python extension built
for your interpreter, and the cluster's Python is usually older.

Download wheels matching the cluster's Python (3.9 here), ship them, unpack:

```bash
pip download clingo cffi pycparser --only-binary=:all: \
    --python-version 39 --implementation cp \
    --platform manylinux2014_x86_64 --no-deps -d wheels/
```

All three are needed: PyPI's clingo binds through CFFI, so clingo alone fails
with `No module named '_cffi_backend'`. Unzip them into one directory and run
spack with `PYTHONPATH` set to it. Also disable bootstrapping, so a mistake
fails immediately instead of hanging:

```bash
spack bootstrap disable
env PYTHONPATH=$HOME/clingo39 spack spec pcomm
```

## 3. Register the toolchain and every dependency as external

Load modules one command at a time. Lmod aborts a combined load atomically, so
one unsatisfiable module leaves you with none of them:

```bash
module load gcc/13.2.0
module load cmake/3.30.5
spack compiler find
spack external find cmake python rdma-core gmake pkgconf ncurses zlib perl
```

Do this **before** concretizing. A spec solved earlier will happily pick a
version that has to be built, and building it means downloading it.

## 4. Install and check

Since the repo is private for now, add a version pointing at a local clone:

```python
version("local", git="file:///home/you/pcomm-src", branch="main")
```

```bash
env PYTHONPATH=$HOME/clingo39 spack install pcomm@local +ibverbs
```

Then confirm it captures something real rather than merely running:

```bash
P=$(spack location -i "pcomm@local +ibverbs")
ib_write_bw -s 1024 -n 200 &                     # server
$P/bin/pcomm run -p ib -o cap -- ib_write_bw -s 1024 -n 200 localhost
$P/bin/pcomm read cap
```

A good capture shows `ib_post_send` with one record per iteration, next to
`ib_queue_pair`, `ib_completion_queue`, `ib_modify_qp` and `ib_poll_cq`.
