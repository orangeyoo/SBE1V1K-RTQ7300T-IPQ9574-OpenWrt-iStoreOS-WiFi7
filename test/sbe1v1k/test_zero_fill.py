#!/usr/bin/env python3
"""Compile the real zero-fill functions with fault-injecting host block I/O.

No device access. The fake disk asserts all I/O remains inside the partition;
guards outside it must survive. This is not a hardware throughput benchmark.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    pos = brace + 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
typedef unsigned long ulong;
typedef unsigned long long lbaint_t;
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define ARCH_DMA_MINALIGN 64
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define min(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) ((int)(sizeof(a)/sizeof((a)[0])))
#define LBAF "%llu"
struct blk_desc { ulong blksz; };
struct disk_partition { ulong blksz; lbaint_t start, size; };
struct recovery_status_led_ctrl { int unused; };
static u32 prog_erase_done, prog_done, prog_write_done;
static unsigned int writes, reads, checks, services, allocations, frees, cases;
static unsigned int bad_write, bad_read, corrupt_read, fail_allocations;
static int corrupt_where;
static ulong clock_ms;
static lbaint_t base_lba, blocks, wpos[32], rpos[32], wlen[32], rlen[32];
static size_t block_bytes, allocated_bytes;
static u8 *disk, *allocated;
static char summary[1024];
static int summary_count;
static ulong get_timer(ulong base) { return clock_ms - base; }
static void recovery_service_runtime(struct recovery_status_led_ctrl *leds)
{
    (void)leds; services++; clock_ms += 5;
}
static void recovery_debug_printf(const char *fmt, ...) { (void)fmt; }
static int capture_printf(const char *fmt, ...)
{
    if (strstr(fmt, "eMMC zero-fill timing:")) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(summary, sizeof(summary), fmt, ap); va_end(ap);
        summary_count++;
    }
    return 0;
}
static void *test_memalign(size_t alignment, size_t size)
{
    void *ptr = NULL; allocations++;
    if (allocations <= fail_allocations) return NULL;
    assert(!allocated);
    assert(posix_memalign(&ptr, alignment, size) == 0);
    allocated = ptr; allocated_bytes = size; return ptr;
}
static void test_free(void *ptr)
{
    if (ptr) { assert(ptr == allocated); frees++; free(ptr); allocated = NULL; }
}
static void bounds(struct blk_desc *desc, lbaint_t start, lbaint_t count)
{
    assert(desc->blksz == block_bytes);
    assert(start >= base_lba && count && start <= base_lba + blocks);
    assert(count <= base_lba + blocks - start);
    assert(count * block_bytes <= allocated_bytes);
}
static lbaint_t blk_dwrite(struct blk_desc *desc, lbaint_t start,
                          lbaint_t count, const void *buffer)
{
    bounds(desc, start, count); assert(writes < 32);
    wpos[writes] = start; wlen[writes++] = count; clock_ms += 3;
    const u8 *p = buffer;
    for (size_t i=0; i<count*block_bytes; i++) assert(p[i] == 0);
    lbaint_t done = writes == bad_write ? count - 1 : count;
    memcpy(disk + block_bytes + (start-base_lba)*block_bytes,
           buffer, done*block_bytes);
    return done;
}
static lbaint_t blk_dread(struct blk_desc *desc, lbaint_t start,
                         lbaint_t count, void *buffer)
{
    bounds(desc, start, count); assert(reads < 32);
    rpos[reads] = start; rlen[reads++] = count; clock_ms += 2;
    lbaint_t done = reads == bad_read ? count - 1 : count;
    memcpy(buffer, disk + block_bytes + (start-base_lba)*block_bytes,
           done*block_bytes);
    if (reads == corrupt_read) {
        size_t pos = corrupt_where == 0 ? 0 :
            (corrupt_where == 1 ? count*block_bytes/2 : count*block_bytes-1);
        ((u8 *)buffer)[pos] = 0x80;
    }
    return done;
}
'''

TESTS = r'''
static void run(lbaint_t size, ulong blksz, bool use_desc, lbaint_t start,
                unsigned fail_alloc, unsigned fail_write, unsigned fail_read,
                unsigned poison_read, int poison_where)
{
    struct blk_desc desc = {blksz};
    struct disk_partition part = {use_desc ? 0 : blksz, start, size};
    struct recovery_status_led_ctrl leds = {0};
    base_lba = start; blocks = size; block_bytes = blksz;
    disk = malloc((size+2)*blksz); assert(disk);
    memset(disk, 0xa5, (size+2)*blksz);
    writes=reads=checks=services=allocations=frees=0;
    clock_ms=0; allocated=NULL; allocated_bytes=0;
    summary[0]=0; summary_count=0;
    bad_write=fail_write; bad_read=fail_read;
    corrupt_read=poison_read; corrupt_where=poison_where;
    fail_allocations=fail_alloc;
    prog_erase_done=3000; prog_write_done=7; prog_done=3007;
    int ret=recovery_mmc_zero_part("rootfs_data", &leds, &desc, &part, 3000);
    assert(!allocated);
    if (!size || !blksz) {
        assert(ret == -EINVAL && !writes && !reads && !allocations);
        assert(!summary_count);
    } else {
        assert(summary_count == 1);
        int want=fail_alloc >= 2 ? -ENOMEM : fail_write ? -EIO :
                 (fail_read || poison_read) ? -EBADMSG : 0;
        assert(ret == want);
        unsigned long long verified, total;
        ulong chunk, wm, rm, cm, sm, tm;
        int result;
        assert(sscanf(summary, "eMMC zero-fill timing: part=rootfs_data result=%d "
                      "verified=%llu/%llu chunk=%lu write_ms=%lu read_ms=%lu "
                      "check_ms=%lu service_ms=%lu total_ms=%lu",
                      &result,&verified,&total,&chunk,&wm,&rm,&cm,&sm,&tm)==9);
        assert(result == want && total == size*blksz);
        assert(wm == writes*3 && rm == reads*2 && cm == checks && sm == services*5);
        assert(tm == wm+rm+cm+sm);
        assert(allocations == (fail_alloc ? 2u : 1u));
        assert(frees == (fail_alloc >= 2 ? 0u : 1u));
        assert(chunk == (fail_alloc ? RECOVERY_MMC_WRITE_CHUNK : RECOVERY_MMC_ZERO_CHUNK));
        assert(prog_done == prog_erase_done + prog_write_done);
        if (want == -ENOMEM) {
            assert(!writes && !reads && !services && verified == 0);
        } else {
            lbaint_t cb=chunk/blksz, loops=(size+cb-1)/cb, serviced=0;
            for (unsigned j=0; j<writes; j++) {
                assert(wpos[j] == start+j*cb);
                assert(wlen[j] == min(cb, size-j*cb));
            }
            if (fail_write) {
                assert(writes == fail_write && reads == fail_write-1);
                assert(checks == reads && services == reads);
            } else if (fail_read || poison_read) {
                unsigned n=fail_read ? fail_read : poison_read;
                assert(reads == n && writes == min(n,loops));
                assert(checks == n-(fail_read ? 1u : 0u));
                assert(services == min(n-1,loops));
            } else {
                assert(writes == loops && reads == loops+2 && checks == reads);
                assert(services == loops && prog_erase_done == 4000);
                for (size_t j=0; j<size*blksz; j++) assert(disk[blksz+j] == 0);
            }
            for (unsigned j=0; j<services; j++) serviced += min(cb,size-j*cb);
            assert(verified == serviced*blksz);
            for (unsigned j=0; j<reads; j++) {
                if (j<loops) {
                    assert(rpos[j] == start+j*cb);
                    assert(rlen[j] == min(cb,size-j*cb));
                } else {
                    assert(rlen[j] == min(cb,size));
                    assert(rpos[j] == (j==loops ? start : start+size-min(cb,size)));
                }
            }
        }
    }
    for (size_t i=0; i<blksz; i++) {
        assert(disk[i] == 0xa5);
        assert(disk[(size+1)*blksz+i] == 0xa5);
    }
    free(disk); cases++;
}
int main(void)
{
    u8 buf[128];
    for (unsigned off=0; off<8; off++) {
        for (unsigned len=0; len<=80; len++) {
            memset(buf,0,sizeof(buf));
            assert(recovery_mmc_buffer_is_zero(buf+off,len));
            for (unsigned pos=0; pos<len; pos++) {
                buf[off+pos]=0xff;
                assert(!recovery_mmc_buffer_is_zero(buf+off,len));
                buf[off+pos]=0;
            }
        }
    }
    puts("PASS real memchr_inv: every offset/tail and nonzero byte (length 0..80, alignment 0..7)");
    lbaint_t sizes[]={1,2047,2048,8191,8192,8193,16387};
    for (unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        run(sizes[i],512,false,2252834,0,0,0,0,0);
        run(sizes[i],512,false,2252834,1,0,0,0,0);
    }
    run(2051,4096,false,2252834,0,0,0,0,0);
    run(16387,512,true,0x100000022ULL,0,0,0,0,0);
    run(16387,512,false,2252834,2,0,0,0,0);
    run(0,512,false,2252834,0,0,0,0,0);
    run(1,0,false,2252834,0,0,0,0,0);
    for (unsigned i=1;i<=3;i++) run(16387,512,false,2252834,0,i,0,0,0);
    for (unsigned i=1;i<=5;i++) {
        run(16387,512,false,2252834,0,0,i,0,0);
        for (int pos=0;pos<3;pos++) run(16387,512,false,2252834,0,0,0,i,pos);
    }
    /* Same injected faults on all 1 MiB fallback chunks and both endpoints. */
    for (unsigned i=1;i<=9;i++) run(16387,512,false,2252834,1,i,0,0,0);
    for (unsigned i=1;i<=11;i++) {
        run(16387,512,false,2252834,1,0,i,0,0);
        run(16387,512,false,2252834,1,0,0,i,2);
    }
    printf("PASS %u actual zero-fill cases: exact coverage, guards, fallback, faults, progress, timings\n",cases);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (args.source_root / "net/lwip/httpd_recovery.c").read_text()
    string = (args.source_root / "lib/string.c").read_text()
    macros = "\n".join(re.findall(
        r"^#define RECOVERY_MMC_(?:WRITE_CHUNK|ZERO_CHUNK|ERASE_PROGRESS_STEPS)\s+[^\n]+", source, re.M))
    assert len(macros.splitlines()) == 3
    for name, value in (("WRITE", 1), ("STREAM", 1), ("ERASE", 32)):
        line = re.search(rf"^#define RECOVERY_MMC_{name}_CHUNK\s+([^\n]+)",source,re.M)[1]
        assert eval(re.sub(r"(?:ULL|UL)", "", line)) == value*1024*1024
    zero_impl = function(string, "static void *check_bytes8") + "\n" + function(string,"void *memchr_inv")
    zero_impl = zero_impl.replace("void *memchr_inv(", "void *real_memchr_inv(")
    counted = """
static void *memchr_inv(const void *p, int c, size_t n) {
    checks++; clock_ms++; return real_memchr_inv(p,c,n);
}
"""
    code = PRELUDE + macros + "\n" + zero_impl + counted
    code += "\n#define memalign test_memalign\n#define free test_free\n#define printf capture_printf\n"
    code += "static void\n" + function(source,"recovery_mmc_update_erase_progress(") + "\n"
    code += function(source,"static bool recovery_mmc_buffer_is_zero") + "\n"
    code += function(source,"static int recovery_mmc_zero_part") + "\n"
    code += "#undef free\n#undef printf\n" + TESTS
    with tempfile.TemporaryDirectory(prefix="sbe-zero-host-") as temp:
        src, binary = Path(temp)/"test.c", Path(temp)/"test"
        src.write_text(code)
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-fno-strict-aliasing", "-Wall", "-Wextra", "-Werror", *flags, str(src), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("UBOOT_WEBFIX1_4_ZERO_FILL_TEST=PASS (host only, no device I/O)")


if __name__ == "__main__":
    main()
