# nfs_stale_cache_test

Demonstrates a possible NFS race condition where readers see zero-filled bytes
when an NFS-served file grows externally, with no userspace writes. The issue
was originally encountered while running
[JuiceFS](https://github.com/juicedata/juicefs/issues/5038) in production and
researched in FUSE, but it may actually be present in NFS and CIFS as well.
The FUSE sister repo is [here](https://github.com/abbradar/fuse_growtest).

## LLM disclosure

LLMs were used to generate the code of these tests, which was then manually
reviewed. LLMs were also used to research the Linux kernel source code to
(hopefully) understand the original distributed FUSE FS issue. This test was
created when I researched whether the same bug happens in other network
filesystems, which should be the case if I understand it correctly.

Any issues found here are mine to blame on.

## Running

```
$ sudo modprobe nfsv4
$ ./run-test.sh s00  # run for 300 seconds
```

## The writer

`writer.c` runs on the NFS server and writes directly to the exported
filesystem. It creates (or truncates) the target file and then appends
`0xAA` bytes one byte at a time in an infinite loop. The file is opened
with `O_SYNC` so that the NFS client's GETATTR RPCs always see the
up-to-date file size.

## The checker

`reader.c` runs on the NFS client. It spawns 16 reader threads and
4 stat-hammer threads. Each reader thread sequentially `read()`s the
file one byte at a time; when it hits EOF it retries. Any `0x00` byte is reported as a
stale-cache hit. The stat-hammer threads continuously call `stat()` on
the file path to force GETATTR RPCs, which update `i_size` on the
client and stale the old cache pages.

## Test results

The results were obtained on Linux 6.19.7, x86_64.

```
=== Mounting NFS with noac (no attribute caching) ===
=== Running reader + stat hammer for 300s ===
opened /mnt/nfs/testfile (fd 3), file ready
running for 300 seconds (16 readers, 4 stat hammers)...
[517770.940] reader 3: STALE ZERO at offset 2350
[517774.952] reader 9: STALE ZERO at offset 2435
[517776.727] reader 12: STALE ZERO at offset 2492
[517778.327] reader 7: STALE ZERO at offset 2537
[517782.521] reader 12: STALE ZERO at offset 2644
[517784.477] reader 2: STALE ZERO at offset 2685
[517802.044] reader 6: STALE ZERO at offset 2953
[517804.749] reader 7: STALE ZERO at offset 2943
[517817.333] reader 14: STALE ZERO at offset 3136
[517843.201] reader 11: STALE ZERO at offset 3518
[517850.938] reader 11: STALE ZERO at offset 3622
[517856.455] reader 4: STALE ZERO at offset 3680
[517856.909] reader 15: STALE ZERO at offset 3723
[517864.988] reader 15: STALE ZERO at offset 3839
[517867.111] reader 9: STALE ZERO at offset 3764
[517879.556] reader 12: STALE ZERO at offset 3933
[517895.401] reader 15: STALE ZERO at offset 4264
[517905.291] reader 10: STALE ZERO at offset 4276
[517908.358] reader 0: STALE ZERO at offset 4322
[517909.986] reader 14: STALE ZERO at offset 4381
[517915.972] reader 11: STALE ZERO at offset 4486
[517923.885] reader 15: STALE ZERO at offset 4658
[517937.375] reader 9: STALE ZERO at offset 4627
[517937.567] reader 5: STALE ZERO at offset 4736
[517944.166] reader 11: STALE ZERO at offset 4845
[517953.804] reader 7: STALE ZERO at offset 4906
[517976.017] reader 0: STALE ZERO at offset 5157
[517978.209] reader 4: STALE ZERO at offset 5366
[517983.375] reader 8: STALE ZERO at offset 5381
[517994.900] reader 12: STALE ZERO at offset 5426
[518031.446] reader 4: STALE ZERO at offset 6074
[518034.808] reader 15: STALE ZERO at offset 6110
[518035.656] reader 9: STALE ZERO at offset 5922
[518044.322] reader 1: STALE ZERO at offset 6274
[518049.330] reader 9: STALE ZERO at offset 6216
[518051.791] reader 3: STALE ZERO at offset 6512
[518062.647] reader 0: STALE ZERO at offset 6573
reader 13: 6800 checks, 0 stale hits
reader 8: 6805 checks, 1 stale hits
reader 10: 6838 checks, 1 stale hits
reader 5: 6760 checks, 1 stale hits
reader 11: 6912 checks, 4 stale hits
reader 2: 6861 checks, 1 stale hits
reader 1: 6903 checks, 1 stale hits
reader 4: 6952 checks, 3 stale hits
reader 7: 6836 checks, 3 stale hits
reader 6: 6864 checks, 1 stale hits
reader 3: 6920 checks, 2 stale hits
reader 0: 6703 checks, 3 stale hits
reader 12: 6699 checks, 4 stale hits
reader 15: 6876 checks, 5 stale hits
reader 9: 6695 checks, 5 stale hits
reader 14: 6704 checks, 2 stale hits

Bug reproduced: 37 stale-zero hit(s)
```

## Possible explanation

When a file grows remotely, the page before the old EOF in the read cache contains zero-fill beyond the old size. Those zeroes are valid while new size <= old size (they are beyond EOF), but become stale once the new size is updated to reflect the remote growth: the remote host wrote real data there, but the local cache still has the old zero-fill.

In filemap_read() (mm/filemap.c) we have:

```
do {
    ...
    error = filemap_get_pages(iocb, ...);  // (1) get cached folios
    ...
    isize = i_size_read(inode);            // (2) get file size
    ...
    // (3) copy from folio to user, capped at isize
} while (...);
```

If we grow the inode size in-between (1) and (2), the race happens; the old page gets capped at the new size, so the userspace reads zeroes where there should be actual data.

To trigger this bug, something must change the inode size in parallel with a read and not come from a user's `write()` since writes are coherent with reads via the cache layer. In a network FS this may happen on getattr when we discover that the remote file has grown, and update the inode's size. When this happens we need to mark the cache pages as stale, but there is no way to "lock" the page and the inode size simultaneously, so the race cannot be fixed just by stalling the cache in getattr.

NFS does stall the cache already — it sets NFS_INO_INVALID_DATA, then before we read invalidates the cache as needed. However the window between (1) and (2) is still there.
