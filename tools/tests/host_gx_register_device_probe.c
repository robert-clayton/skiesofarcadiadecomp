#define _GNU_SOURCE
#define SOA_HOST 1
#define SOA_SPLIT 1
#include "../../runtime/host.c"
#include "gx_state.h"
#include <assert.h>
#include <dlfcn.h>
static uint16_t (*read16)(CpuState*,uint32_t);
static uint32_t (*read32)(CpuState*,uint32_t);
static void (*write16)(CpuState*,uint32_t,uint16_t);
static void (*write32)(CpuState*,uint32_t,uint32_t);
static void (*write_pipe)(CpuState*,unsigned,uint64_t);
uint32_t g_watch_addr, g_watch_len;
void watch_hit(CpuState* s,uint32_t a,unsigned n,uint64_t v){(void)s;(void)a;(void)n;(void)v;abort();}
void gx_pipe_write(CpuState* s,unsigned n,uint64_t v){write_pipe(s,n,v);}
uint16_t mmio_read16(CpuState* s,uint32_t a){return read16(s,a);}
uint32_t mmio_read32(CpuState* s,uint32_t a){return read32(s,a);}
void mmio_write16(CpuState* s,uint32_t a,uint16_t v){write16(s,a,v);}
void mmio_write32(CpuState* s,uint32_t a,uint32_t v){write32(s,a,v);}
static uint64_t read_reg(SoaHostCall* c,uint32_t a,unsigned n)
{ uint64_t v=0; assert(soa_host_gx_register(c,a,n,0,&v));return v; }
static void write_reg(SoaHostCall* c,uint32_t a,unsigned n,uint64_t v)
{ assert(soa_host_gx_register(c,a,n,SOA_HOST_GX_WRITE,&v)); }
int main(void)
{
    const char* path=getenv("SOA_TEST_GX_RUNTIME");assert(path);
    void* lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);assert(lib);
    read16=dlsym(lib,"mmio_read16");read32=dlsym(lib,"mmio_read32");
    write16=dlsym(lib,"mmio_write16");write32=dlsym(lib,"mmio_write32");write_pipe=dlsym(lib,"gx_pipe_write");
    assert(read16&&read32&&write16&&write32&&write_pipe);
    CpuState cpu={0};SoaHostCall c={0};c.runtime=&cpu;t_guest=1;
    cpu.mem=calloc(1,MEM_IMAGE_SIZE);assert(cpu.mem);cpu.gpr[13]=0x8034e720u;
    assert(read_reg(&c,0xcc000000,2)==0xC);
    write_reg(&c,0xcc000002,2,0x12345);assert(read_reg(&c,0xcc000002,2)==0x2345);
    write_reg(&c,0xcc000034,2,0x4567);write_reg(&c,0xcc000036,2,0x89ab);
    assert(read_reg(&c,0xcc000038,2)==0x4567);assert(read_reg(&c,0xcc00003a,2)==0x89ab);
    write_reg(&c,0xcc000030,2,0xffff);assert(read_reg(&c,0xcc000030,2)==0);
    mem_w32(&cpu,cpu.gpr[13]+(uint32_t)GX_CPU_FIFO_SDA,GX_DL_FIFO);
    write_reg(&c,0xcc00300c,4,0x80400000);write_reg(&c,0xcc003010,4,0x8040001c);write_reg(&c,0xcc003014,4,0x80400000);
    assert(read_reg(&c,0xcc00300c,4)==0x00400000);
    write_pipe(&cpu,8,0x1122334455667788ull);
    assert(mem_r64(&cpu,0x80400000)==0x1122334455667788ull);
    assert(read_reg(&c,0xcc003014,4)==0x00400008);
    write_reg(&c,0xcc003010,4,0x80400004);write_reg(&c,0xcc003014,4,0x80400004);
    write_pipe(&cpu,8,0xaabbccdd12345678ull);
    assert(mem_r32(&cpu,0x80400004)==0xaabbccdd);assert(mem_r32(&cpu,0x80400000)==0x12345678);
    assert(read_reg(&c,0xcc003014,4)==(0x00400004|GX_PI_WRAP));
    mem_w32(&cpu,cpu.gpr[13]+(uint32_t)GX_CPU_FIFO_SDA,0);
    write_reg(&c,0xcc00300c,4,0x80400100);
    free(cpu.mem);puts("real GX model: CP status/control/read-pointer/distance, PI masks, display-list entry, bytes, pointer advance and wrap PASS");
}
