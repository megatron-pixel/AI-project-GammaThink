# PHASE 7 — IPFS VIRTUAL DRIVE

Components: [`virtual-drive`](../../components/virtual-drive/).

## Original section 47 requirements

- IPFS backend
- CID resolution
- Lazy block retrieval
- SSD caching
- Hash verification
- Offline pinning

## Evidence and acceptance boundary

Checks: virtual-drive, remote-query, remote-mount, gateway, block-cache, block-cid, block-pin, retention. See [verification](../../verification/README.md).

Local HTTP/IPFS-compatible fixtures verify protocols, integrity and offline pinning; external network availability is not guaranteed.
