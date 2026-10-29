#define _GNU_SOURCE
#define SOA_HOST 1
#define SOA_SPLIT 1
#include "../../runtime/host.c"
#include <assert.h>
static unsigned reads, writes;
static uint32_t expected;
static unsigned expected_width;
static uint64_t value;
uint16_t mmio_read16(CpuState* s, uint32_t a) { (void)s; assert(a==expected && expected_width==2); ++reads; return (uint16_t)value; }
uint32_t mmio_read32(CpuState* s, uint32_t a) { (void)s; assert(a==expected && expected_width==4); ++reads; return (uint32_t)value; }
void mmio_write16(CpuState* s, uint32_t a, uint16_t v) { (void)s; assert(a==expected && expected_width==2); ++writes; value=v; }
void mmio_write32(CpuState* s, uint32_t a, uint32_t v) { (void)s; assert(a==expected && expected_width==4); ++writes; value=v; }
int main(void)
{
    CpuState cpu={0}; SoaHostCall c={0}; c.runtime=&cpu; uint64_t v=0x123456789abcdef0ull;
    assert(!soa_host_gx_register(&c,0xcc000002,2,0,&v)); t_guest=1;
    unsigned cases=0;
    for (unsigned region=0;region<3;++region) {
        const uint32_t base=region==0?0xcc000000u:region==1?0xcc001000u:0xcc00300cu;
        const unsigned len=region==0?128:region==1?16:12, width=region==2?4:2;
        for(unsigned off=0;off<len;off+=width) {
            expected=base+off;expected_width=width;
            unsigned before=reads+writes;uint64_t old=v;
            assert(soa_host_gx_register(&c,base+off,width,SOA_HOST_GX_PROBE,&v));assert(v==old&&reads+writes==before);
            assert(soa_host_gx_register(&c,base+off,width,SOA_HOST_GX_PROBE|SOA_HOST_GX_WRITE,&v));assert(reads+writes==before);
            v=0x123456789abcdef0ull;assert(soa_host_gx_register(&c,base+off,width,SOA_HOST_GX_WRITE,&v));
            v=0;assert(soa_host_gx_register(&c,base+off,width,0,&v));assert(v==(width==2?0xdef0u:0x9abcdef0u));++cases;
        }
    }
    const uint32_t bad[]={0xcc000080u,0xcc000001u,0xcc001010u,0xcc003008u,0xcc003018u,0xcc008000u,0x80000000u};
    for(unsigned k=0;k<sizeof bad/sizeof *bad;++k)for(unsigned width=1;width<=8;++width) {
        unsigned before=reads+writes;uint64_t old=v;assert(!soa_host_gx_register(&c,bad[k],width,0,&v));assert(v==old&&reads+writes==before);
    }
    assert(!soa_host_gx_register(&c,0xcc000002,4,0,&v));assert(!soa_host_gx_register(&c,0xcc00300c,2,0,&v));
    assert(!soa_host_gx_register(&c,0xcc000002,2,4,&v));assert(!soa_host_gx_register(&c,0xcc000002,2,0,NULL));
    c.after=1;assert(!soa_host_gx_register(&c,0xcc000002,2,0,&v));
    printf("prototype: %u valid registers, %u reads, %u writes; all probe/refusal tests passed\n",cases,reads,writes);
}
