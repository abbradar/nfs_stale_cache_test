#!/bin/bash
set -e

# rpcbind + NFS kernel server
rpcbind
exportfs -ra
rpc.nfsd 8
rpc.mountd

echo "NFS server ready, starting writer..."
exec writer /export/testfile
