# patches/ — changes that live inside the `freeink-sdk` submodule

`freeink-sdk` is a git submodule whose `origin` is **upstream's**
`https://github.com/Free-Ink/freeink-sdk.git`, checked out detached at
`a485dc46`. We cannot commit to it, and anything edited in its working tree is
recorded nowhere: `git submodule update` discards it without a word.

A patch kept here is in the parent repository's history, so the work survives
even though the submodule's working tree does not.

## Why the parent repo still tells the truth about this

A modified submodule shows up in the parent as `M freeink-sdk`, which
`scripts/git_branch.py` counts as a dirty tree (it only ignores *untracked*
files). So a firmware built with any of these patches applied names itself
`1.5.0-jp-dev-<sha>-dirty`. That is correct and wanted: the binary contains code
that is in no commit. See spec section 8.8-34.

## Applying

    cd freeink-sdk
    git apply ../patches/<name>.patch

To check one still applies after a submodule move: `git apply --check`.

## The patches

| file | what it adds | why it has to be in the submodule |
|---|---|---|
| `freeink-sdk-tls-heap-probe.patch` | `SecureClient::setHeapProbe()` and four samples around a TLS session (`tls-before`, `tls-handshake`, `tls-record`, `tls-after`) | wolfSSL grows and frees its receive buffer *inside* `wolfSSL_read`, so the peak falls between two calls the application makes. It cannot be observed from outside `SecureClient`. See [docs-jp/g4-tls-memory.md](../docs-jp/g4-tls-memory.md) section 8 |

## The real fix, when it is wanted

Point the submodule at a fork of `freeink-sdk` under our own account, commit
there, and record the new pointer in this repository. Then these patches become
ordinary commits and the `-dirty` label goes away. That is a repository-topology
decision, not something to do quietly while fixing a crash.
