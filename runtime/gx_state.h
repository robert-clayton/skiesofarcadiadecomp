/*
 * GX's state as gx.c keeps it, in one plain image, and the one rule of the
 * command stream a reader of it needs: how long a command is. Shared by
 * gx.c, the recorder (runtime/record.c keeps the image at each end of a
 * recorded call that drew) and the replay (tools/replay/replay.c walks a
 * call's pipe bytes from the image it began with and holds the image it
 * leaves), so that the three agree on what a command is.
 */
#ifndef SOA_GX_STATE_H
#define SOA_GX_STATE_H
#include <stddef.h>
#include <stdint.h>

#define GX_PIPE_CAP 65536u /* the bytes of a command not yet whole that gx.c keeps */
#define GX_CPU_FIFO_SDA (-27520) /* r13-relative: GXSetCPUFifo's current CPU FIFO object */
#define GX_DL_FIFO 0x80318B18u   /* DisplayListFifo (GXBeginDisplayList, 0x80251D94) */
#define GX_PI_WRAP 0x04000000u   /* PI_FIFO_WPTR's wrap bit; GXEndDisplayList reads it as overflow */
#define GX_PI_FIFO_BASE 0xCC00300Cu
#define GX_PI_FIFO_WPTR 0xCC003014u

/* The registers the command stream loads (CP, XF with its matrix memory, BP),
 * the PI FIFO's three registers, whether the CPU FIFO is the display-list
 * one (the pipe's bytes then go into guest memory, not to the parser), and
 * how many bytes of a command not yet whole are waiting. Host byte order. */
typedef struct {
    uint32_t cp[0x100];
    uint32_t xf[0x1100]; /* 0x000-0xFFF matrix memory, 0x1000-0x10FF registers */
    uint32_t bp[0x100];
    uint32_t pi[3];      /* PI_FIFO_BASE, _TOP, _WPTR */
    uint32_t dl_rec;     /* 1: recording a display list */
    uint32_t pipe_len;   /* bytes waiting for the rest of their command */
    uint32_t reserved[3];
    uint32_t cp_mmio[0x40]; /* raw halfword bank, zero extended; recording version 3 */
    uint32_t pe_mmio[8];
} GxImage;

/* gx.c: the image now, and the waiting bytes themselves (*len of them). */
void gx_image(GxImage* out);
const uint8_t* gx_pipe_left(uint32_t* len);

static inline unsigned gx_comp_size(unsigned fmt)
{
    return fmt == 4 ? 4 : (fmt >= 2 ? 2 : 1); /* u8 s8 u16 s16 f32 */
}

static inline unsigned gx_color_size(unsigned comp)
{
    static const unsigned t[8] = {2, 3, 4, 2, 3, 4, 4, 4}; /* 565 888 888x 4444 6666 8888 */
    return t[comp & 7];
}

static inline unsigned gx_attr_size(unsigned vcd, unsigned direct_size)
{
    switch (vcd & 3) {
    case 0: return 0;           /* not present */
    case 1: return direct_size; /* inline */
    case 2: return 1;           /* 8-bit index */
    default: return 2;          /* 16-bit index */
    }
}

/* A vertex's bytes in format `vat`, from the CP's VCD and VAT registers. */
static inline unsigned gx_vertex_size(const uint32_t* cp, unsigned vat)
{
    uint32_t lo = cp[0x50], hi = cp[0x60];
    uint32_t a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    unsigned size = 0, i;
    unsigned tc[8][2] = {
        {(a >> 21) & 1, (a >> 22) & 7}, {(b >> 0) & 1, (b >> 1) & 7},
        {(b >> 9) & 1, (b >> 10) & 7},  {(b >> 18) & 1, (b >> 19) & 7},
        {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
        {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7},
    };

    size += lo & 1; /* position matrix index */
    for (i = 0; i < 8; i++) size += (lo >> (1 + i)) & 1; /* texture matrix indices */

    size += gx_attr_size((lo >> 9) & 3, ((a & 1) ? 3 : 2) * gx_comp_size((a >> 1) & 7));

    {
        unsigned vcd = (lo >> 11) & 3, elems = (a >> 9) & 1, fmt = (a >> 10) & 7;
        if (vcd >= 2 && elems && ((a >> 31) & 1))
            size += 3 * (vcd == 2 ? 1 : 2); /* NBT with three separate indices */
        else
            size += gx_attr_size(vcd, (elems ? 9 : 3) * gx_comp_size(fmt));
    }

    size += gx_attr_size((lo >> 13) & 3, gx_color_size((a >> 14) & 7));
    size += gx_attr_size((lo >> 15) & 3, gx_color_size((a >> 18) & 7));

    for (i = 0; i < 8; i++)
        size += gx_attr_size((hi >> (2 * i)) & 3, (tc[i][0] ? 2 : 1) * gx_comp_size(tc[i][1]));
    return size;
}

/* The bytes of the command at p (len of them there), as gx.c's parse reads
 * it: 0 when it is not whole yet. An unknown byte is one byte long (the
 * parse resyncs by one). cp gives a draw's vertex size. */
static inline size_t gx_command_bytes(const uint8_t* p, size_t len, const uint32_t* cp)
{
    uint8_t op;
    size_t need;
    if (!len) return 0;
    op = p[0];
    if (op == 0x00 || op == 0x48) need = 1;
    else if (op == 0x08) need = 6;
    else if (op == 0x10) {
        if (len < 5) return 0;
        need = 5 + 4 * (size_t)(((unsigned)p[1] << 8 | p[2]) + 1u);
    } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) need = 5;
    else if (op == 0x40) need = 9;
    else if (op == 0x61) need = 5;
    else if (op >= 0x80 && op < 0xC0) {
        if (len < 3) return 0;
        need = 3 + (size_t)((unsigned)p[1] << 8 | p[2]) * gx_vertex_size(cp, op & 7);
    } else need = 1;
    return need <= len ? need : 0;
}

#endif
