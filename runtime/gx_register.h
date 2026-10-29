/* Shared typed GX register boundary: live runtime and offline replay use
 * the same request validation and mmio forwarding. No second device model. */
#ifndef SOA_GX_REGISTER_H
#define SOA_GX_REGISTER_H
#include "cpu.h"
#include "soa_host.h"
static inline int gx_register_access(SoaHostCall* call, uint32_t address,
                                     unsigned size, unsigned flags, uint64_t* value,
                                     uint32_t pc, int guest)
{
    CpuState* s;
    const int half = size == 2 && !(address & 1u) &&
        ((address >= 0xCC000000u && address < 0xCC000080u) ||
         (address >= 0xCC001000u && address < 0xCC001010u));
    const int word = size == 4 && !(address & 3u) &&
        address >= 0xCC00300Cu && address <= 0xCC003014u;
    if (!call || call->after || !call->runtime || !guest || !value ||
        (flags & ~(SOA_HOST_GX_WRITE | SOA_HOST_GX_PROBE)) || (!half && !word) ||
        (pc && ((pc & 3u) || pc < 0x80000000u || pc >= 0x81800000u))) return 0;
    if (flags & SOA_HOST_GX_PROBE) return 1;
    s = (CpuState*)call->runtime;
    if (pc) s->pc = pc;
    if (flags & SOA_HOST_GX_WRITE) {
        if (half) mmio_write16(s, address, (uint16_t)*value);
        else mmio_write32(s, address, (uint32_t)*value);
    } else {
        *value = half ? mmio_read16(s, address) : mmio_read32(s, address);
    }
    return 1;
}
#endif
