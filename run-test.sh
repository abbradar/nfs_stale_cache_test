#!/usr/bin/env bash
set -e

DURATION="${1:-60}"
cd "$(dirname "$0")"

cleanup() {
    echo "=== Cleaning up ==="
    docker compose exec nfs-client umount /mnt/nfs 2>/dev/null || true
    docker compose down -v 2>/dev/null || true
}
trap cleanup EXIT

echo "=== Tearing down any previous run ==="
docker compose down -v 2>/dev/null || true

echo "=== Building containers ==="
docker compose build

echo "=== Starting NFS server (writer runs inside) ==="
docker compose up -d --force-recreate nfs-server
sleep 5  # wait for NFS + initial file data

echo "=== Starting client container ==="
docker compose up -d --force-recreate nfs-client
sleep 1

echo "=== Mounting NFS with noac (no attribute caching) ==="
docker compose exec nfs-client \
    mount -t nfs4 -o noac \
    nfs-server:/ /mnt/nfs

echo "=== Running reader + stat hammer for ${DURATION}s ==="
docker compose exec nfs-client \
    reader /mnt/nfs/testfile "$DURATION"
