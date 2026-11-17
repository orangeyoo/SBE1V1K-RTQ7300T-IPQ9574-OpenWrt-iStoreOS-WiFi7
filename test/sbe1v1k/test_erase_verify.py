#!/usr/bin/env python3
"""Fault-inject actual native erase/verify/repair C. Host-only; no device I/O."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
import test_zero_fill as base

EXTRA = r'''
struct mmc { u32 erase_grp_size; bool can_trim; };
static unsigned erases, bad_erase, native_mode, last_progress;
static unsigned below_complete_checks;
static int fallback_seen;
static lbaint_t blk_derase(struct blk_desc *desc, lbaint_t start, lbaint_t count)
{
    assert(desc->blksz == block_bytes && allocated);
    assert(start >= base_lba && count && start <= base_lba+blocks);
    assert(count <= base_lba+blocks-start);
    erases++; clock_ms += 7;
    if (erases == bad_erase) return count-1;
    u8 *p=disk+block_bytes+(start-base_lba)*block_bytes;
    size_t bytes=count*block_bytes;
    if (native_mode==0 || native_mode==3) memset(p,0,bytes);
    if (native_mode==1) memset(p,0xff,bytes);
    /* mode 2 leaves stale data untouched despite a successful command. */
    if (native_mode==3) {
        lbaint_t offsets[]={RECOVERY_MMC_ZERO_CHUNK/block_bytes, blocks-1};
        for (unsigned i=0;i<2;i++)
            if(base_lba+offsets[i]>=start && base_lba+offsets[i]<start+count)
                disk[block_bytes+offsets[i]*block_bytes]=0x81;
    }
    return count;
}
'''
TESTS = r'''
static void run(lbaint_t size, ulong bs, lbaint_t start, bool trim, u32 group,
                unsigned mode, unsigned fa, unsigned fe, unsigned fr,
                unsigned fw, unsigned poison, int want, int invalid)
{
    struct blk_desc desc={bs,start+size+5};
    struct disk_partition part={bs,start,size};
    struct mmc mmc={group,trim};
    struct recovery_status_led_ctrl leds={0};
    base_lba=start; blocks=size; block_bytes=bs;
    disk=malloc((size+2)*bs);assert(disk);memset(disk,0xa5,(size+2)*bs);
    writes=reads=checks=services=allocations=frees=erases=0;
    allocated=NULL;allocated_bytes=0;clock_ms=0;
    bad_erase=fe;bad_read=fr;bad_write=fw;corrupt_read=poison;corrupt_where=2;
    fail_allocations=fa;native_mode=mode;summary_count=0;summary[0]=0;
    fallback_seen=0;below_complete_checks=0;
    prog_erase_done=3000;prog_write_done=7;prog_done=3007;last_progress=3000;
    if(invalid==1)part.blksz=bs*2;
    if(invalid==2)desc.lba=start-1;
    if(invalid==3)desc.lba=start+size-1;
    if(invalid==4)part.size=0;
    if(invalid==5)part.size=~0ULL/512+1, desc.lba=~0ULL;
    if(invalid==6)part.blksz=desc.blksz=0;
    int ret=recovery_mmc_erase_verify_part("rootfs_data",&leds,&desc,&part,&mmc,3000);
    if(ret!=want)fprintf(stderr,"case %u ret=%d want=%d reads=%u writes=%u\n",cases,ret,want,reads,writes);
    assert(ret==want && !allocated);
    bool fallback=!group || (!trim && (start%group || size%group));
    if(invalid){assert(!reads&&!writes&&!erases&&!allocations);}
    else if(fa>=2){assert(!reads&&!writes&&!erases&&allocations==2&&frees==0);}
    else {
        assert(allocations==(fa?2u:1u) && frees==1 && summary_count==1);
        assert(!!fallback_seen==fallback);
        if(fallback)assert(!erases);
        else assert(erases);
        if(!ret){
            for(size_t i=0;i<size*bs;i++)assert(disk[bs+i]==0);
            assert(prog_erase_done==4000);
            lbaint_t cb=(fa?RECOVERY_MMC_WRITE_CHUNK:RECOVERY_MMC_ZERO_CHUNK)/bs;
            unsigned loops=(size+cb-1)/cb;
            if(!fallback){
                if(mode==0)assert(writes==0&&reads==loops+2);
                if(mode==1||mode==2)assert(writes==loops&&reads==loops*2+2);
                if(mode==3)assert(writes==2&&reads==loops+4);
                /* Every native success scans all chunks; final two reads are endpoints. */
                lbaint_t seen=0;
                for(unsigned i=0;i<reads-2;i++)
                    if(rpos[i]==start+seen)seen+=rlen[i];
                assert(seen==size);
                assert(rpos[reads-2]==start&&rpos[reads-1]==start+size-min(cb,size));
            }
        }else if(!fallback)assert(prog_erase_done<4000);
    }
    for(size_t i=0;i<bs;i++)assert(disk[i]==0xa5&&disk[(size+1)*bs+i]==0xa5);
    free(disk);cases++;
}
int main(void)
{
    lbaint_t sizes[]={1,2047,2048,8191,8192,8193,16387,65539};
    for(unsigned i=0;i<ARRAY_SIZE(sizes);i++)
        for(unsigned mode=0;mode<3;mode++)
            for(unsigned fa=0;fa<2;fa++)
                run(sizes[i],512,2252834,true,1024,mode,fa,0,0,0,0,0,0);
    run(16387,512,2252834,true,1024,3,0,0,0,0,0,0,0);
    run(16387,512,0x100000022ULL,true,1024,0,0,0,0,0,0,0,0);
    run(2051,4096,2252834,true,128,1,1,0,0,0,0,0,0);
    run(16384,512,2252800,false,1024,0,0,0,0,0,0,0,0);
    run(16384,512,2252800,false,1024,1,0,0,0,0,0,0,0);
    /* No trim: unaligned start or length must use original zero-fill. */
    run(16384,512,2252834,false,1024,0,0,0,0,0,0,0,0);
    run(16387,512,2252800,false,1024,0,1,0,0,0,0,0,0);
    run(16387,512,2252834,true,0,0,0,0,0,0,0,0,0);
    run(16387,512,2252834,true,1024,0,2,0,0,0,0,-ENOMEM,0);
    for(unsigned i=1;i<=2;i++)run(65539,512,2252834,true,1024,0,0,i,0,0,0,-EIO,0);
    /* Three verification chunks, including tail, plus both endpoint reads. */
    for(unsigned i=1;i<=5;i++) {
        run(16387,512,2252834,true,1024,0,0,0,i,0,0,-EBADMSG,0);
        /* Initial false nonzero is repairable; endpoint corruption is not. */
        if(i>=4)run(16387,512,2252834,true,1024,0,0,0,0,0,i,-EBADMSG,0);
    }
    /* All repair writes and all initial/post-repair/end reads. */
    for(unsigned i=1;i<=3;i++)run(16387,512,2252834,true,1024,1,0,0,0,i,0,-EIO,0);
    for(unsigned i=1;i<=8;i++)run(16387,512,2252834,true,1024,1,0,0,i,0,0,-EBADMSG,0);
    for(unsigned i=2;i<=6;i+=2)run(16387,512,2252834,true,1024,1,0,0,0,0,i,-EBADMSG,0);
    /* Allocation fallback: nine chunks + two endpoints, and repair variants. */
    for(unsigned i=1;i<=11;i++)run(16387,512,2252834,true,1024,0,1,0,i,0,0,-EBADMSG,0);
    for(unsigned i=1;i<=9;i++)run(16387,512,2252834,true,1024,1,1,0,0,i,0,-EIO,0);
    for(unsigned i=1;i<=20;i++)run(16387,512,2252834,true,1024,1,1,0,i,0,0,-EBADMSG,0);
    for(unsigned i=1;i<=6;i++)run(17,512,2252834,true,1024,0,0,0,0,0,0,i==5?-EOVERFLOW:-EINVAL,i);
    printf("PASS %u actual erase-verify cases: zero/FF/stale/mixed, fallback, faults, bounds, tails, high LBA, progress\n",cases);
}
'''

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--source-root',type=Path,default=base.ROOT)
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    source=(args.source_root/'net/lwip/httpd_recovery.c').read_text()
    string=(args.source_root/'lib/string.c').read_text()
    pre=base.PRELUDE.replace('struct blk_desc { ulong blksz; };','struct blk_desc { ulong blksz; lbaint_t lba; };')
    pre=pre.replace('[32]','[256]').replace('< 32','< 256')
    pre=pre.replace('static ulong get_timer','static unsigned erases, last_progress, below_complete_checks;\nstatic int fallback_seen;\nstatic ulong get_timer')
    pre=pre.replace('(void)leds; services++; clock_ms += 5;', '''(void)leds;
    assert(prog_erase_done>=last_progress && prog_erase_done<=4000);
    if(erases && prog_erase_done==4000)assert(below_complete_checks>=2);
    if(erases && prog_erase_done==3999)below_complete_checks++;
    last_progress=prog_erase_done;services++;clock_ms+=5;''')
    pre=pre.replace('if (strstr(fmt, "eMMC zero-fill timing:")) {','if (strstr(fmt,"eMMC zero-fill timing:")) fallback_seen=1;\n    if (strstr(fmt, "timing:")) {')
    macros='\n'.join(re.findall(r'^#define RECOVERY_MMC_(?:WRITE_CHUNK|ZERO_CHUNK|ERASE_CHUNK|ERASE_PROGRESS_STEPS)\s+[^\n]+',source,re.M))
    assert len(macros.splitlines())==4
    impl=base.function(string,'static void *check_bytes8')+'\n'+base.function(string,'void *memchr_inv')
    extra=EXTRA.replace('static unsigned erases, bad_erase, native_mode, last_progress;','static unsigned bad_erase, native_mode;').replace('static unsigned below_complete_checks;','').replace('static int fallback_seen;','')
    code=pre+macros+'\n'+extra+impl
    code+='\n#define memalign test_memalign\n#define free test_free\n#define printf capture_printf\n'
    code+='static void\n'+base.function(source,'recovery_mmc_update_erase_progress(')+'\n'
    for name in ['static bool recovery_mmc_buffer_is_zero','static int recovery_mmc_zero_part','static int recovery_mmc_erase_verify_part']:
        code+=base.function(source,name)+'\n'
    code+='#undef free\n#undef printf\n'+TESTS
    with tempfile.TemporaryDirectory(prefix='sbe-erase-verify-') as temp:
        c,b=Path(temp)/'test.c',Path(temp)/'test'
        c.write_text(code)
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run(['gcc','-std=gnu11','-O2','-fno-strict-aliasing','-Wall','-Wextra','-Werror',*flags,str(c),'-o',str(b)],check=True)
        subprocess.run([str(b)],check=True)
    print('UBOOT_WEBFIX1_5_ERASE_VERIFY_TEST=PASS (host only, not hardware acceptance)')

if __name__=='__main__':main()
