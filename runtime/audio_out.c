/*
 * Sound output: the AI DMA's blocks, as the console would send them to the
 * DAC. Blocks arrive as big-endian right/left 16-bit pairs at 32 kHz (or 48
 * kHz). The meter, the arrival rate, SOA_WAV's file, the mute and the report
 * are every platform's (portability L9: a headless Linux run writes its
 * WAV); the device is a backend, waveOut on Windows, SDL3's audio stream
 * where the build has SDL (L10's audio_sdl.c), the host's queue in a host
 * build (host.c), and none otherwise.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "plat.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_BYTES 4096

static int g_ready = -1;
static uint64_t g_pushed, g_dropped;
/* Drops come in runs: a burst after a stall the clock counted, which the
 * queue cannot hold, is one run; a device slower than its rate is many
 * short ones. The report tells them apart. */
static uint64_t g_drop_runs, g_drop_run, g_drop_longest;
static void note_drop(int dropped)
{
    if (!dropped) { g_drop_run = 0; return; }
    g_dropped++;
    if (!g_drop_run++) g_drop_runs++;
    if (g_drop_run > g_drop_longest) g_drop_longest = g_drop_run;
}

/* Silence while another window is in front (`unfocused = mute`, M5b): the
 * samples handed to the device are zeroed, and the WAV and the meter still
 * hear the game. The test sink (the self test's) takes the samples in place
 * of the device, so the check needs no audio hardware. */
static volatile int g_muted;
static void (*g_test_sink)(const int16_t* lr, unsigned frames);

/* Big-endian right/left pairs to little-endian left/right, or zeros. */
static void to_device(const uint8_t* be_rl, unsigned n, int16_t* out)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        int16_t r = (int16_t)(((uint16_t)be_rl[4 * i] << 8) | be_rl[4 * i + 1]);
        int16_t l = (int16_t)(((uint16_t)be_rl[4 * i + 2] << 8) | be_rl[4 * i + 3]);
        out[2 * i] = g_muted ? 0 : l;
        out[2 * i + 1] = g_muted ? 0 : r;
    }
}

/* ---- the device: waveOut on Windows --------------------------------------- */
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

#define BLOCKS 24

static HWAVEOUT g_wo;
static WAVEHDR g_hdr[BLOCKS];
static uint8_t g_buf[BLOCKS][BLOCK_BYTES];
static int g_next;

static int audio_open(unsigned rate)
{
    WAVEFORMATEX fmt;
    int i;
    if (getenv("SOA_NOSOUND")) return 0;
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[audio] waveOutOpen failed; no sound\n");
        return 0;
    }
    for (i = 0; i < BLOCKS; i++) {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = BLOCK_BYTES;
        g_hdr[i].dwFlags = WHDR_DONE;
    }
    fprintf(stderr, "[audio] output open at %u Hz\n", rate);
    return 1;
}

/* One block to the device, at most BLOCK_BYTES of it. */
static void audio_play(const uint8_t* be_rl, unsigned bytes)
{
    WAVEHDR* h = &g_hdr[g_next];
    unsigned n;
    if (!(h->dwFlags & WHDR_DONE)) { note_drop(1); return; } /* the device is behind; drop */
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, h, sizeof *h);
    n = bytes / 4;
    to_device(be_rl, n, (int16_t*)g_buf[g_next]);
    h->dwBufferLength = n * 4;
    h->dwFlags = 0;
    if (waveOutPrepareHeader(g_wo, h, sizeof *h) == MMSYSERR_NOERROR && waveOutWrite(g_wo, h, sizeof *h) == MMSYSERR_NOERROR) {
        g_pushed++;
        g_next = (g_next + 1) % BLOCKS;
        note_drop(0);
    } else {
        h->dwFlags = WHDR_DONE;
        note_drop(1);
    }
}
#elif defined(SOA_SDL)
/* SDL3's audio stream (portability L10, audio_sdl.c). A block goes when the
 * device has fallen behind by waveOut's 24 of them, as it does there. */
int audio_sdl_open(unsigned rate);
unsigned audio_sdl_queued(void);
int audio_sdl_put(const int16_t* lr, unsigned bytes);

#define BLOCKS 24

static int16_t g_out[BLOCK_BYTES / 2];

static int audio_open(unsigned rate)
{
    if (getenv("SOA_NOSOUND")) return 0;
    return audio_sdl_open(rate);
}

/* One block to the device, at most BLOCK_BYTES of it. */
static void audio_play(const uint8_t* be_rl, unsigned bytes)
{
    unsigned n = bytes / 4;
    if (audio_sdl_queued() + n * 4 > BLOCKS * n * 4) { note_drop(1); return; } /* the device is behind; drop */
    to_device(be_rl, n, g_out);
    if (audio_sdl_put(g_out, n * 4)) {
        g_pushed++;
        note_drop(0);
    } else {
        note_drop(1);
    }
}
#elif defined(SOA_HOST)
/* A queue the host empties (host.c, soa_host_audio), as long as SDL's 24
 * blocks: a block goes when it is full, as it does there. */
int audio_host_open(unsigned rate);
int audio_host_put(const int16_t* lr, unsigned bytes);

static int16_t g_out[BLOCK_BYTES / 2];

static int audio_open(unsigned rate)
{
    if (getenv("SOA_NOSOUND")) return 0;
    return audio_host_open(rate);
}

/* One block to the queue, at most BLOCK_BYTES of it. */
static void audio_play(const uint8_t* be_rl, unsigned bytes)
{
    unsigned n = bytes / 4;
    to_device(be_rl, n, g_out);
    if (audio_host_put(g_out, n * 4)) {
        g_pushed++;
        note_drop(0);
    } else {
        note_drop(1);
    }
}
#else
/* No device here: the meter, the rate and the WAV still run. */
static int audio_open(unsigned rate)
{
    (void)rate;
    return 0;
}
static void audio_play(const uint8_t* be_rl, unsigned bytes)
{
    (void)be_rl;
    (void)bytes;
    (void)note_drop; /* a device that falls behind counts its drops there */
}
#endif

/* ---- every platform's ------------------------------------------------------ */

static int g_peak;
static uint64_t g_blocks_seen;
/* The rate blocks arrive at in wall time, for turbo's check (M11a): at any
 * game speed the DSP should feed 128,000 bytes a second, and 2x audio is a
 * clock running fast. */
static uint64_t g_rate_bytes;
static uint64_t g_rate_first, g_rate_last;

/* SOA_WAV=path: every block also goes to a WAV file (stereo 16-bit, little-endian),
 * so a headless run's sound can be listened to or compared afterwards. */
static FILE* g_wav;
static uint32_t g_wav_bytes;
static unsigned g_wav_rate;
static int g_wav_tried;

static void wav_u32(FILE* f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; fwrite(b, 1, 4, f); }
static void wav_u16(FILE* f, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; fwrite(b, 1, 2, f); }

static void wav_header(FILE* f, unsigned rate, uint32_t data_bytes)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); wav_u32(f, 36 + data_bytes); fwrite("WAVEfmt ", 1, 8, f);
    wav_u32(f, 16); wav_u16(f, 1); wav_u16(f, 2); wav_u32(f, rate); wav_u32(f, rate * 4); wav_u16(f, 4); wav_u16(f, 16);
    fwrite("data", 1, 4, f); wav_u32(f, data_bytes);
}

static void wav_append(const uint8_t* be_rl, unsigned bytes, unsigned rate)
{
    unsigned i;
    if (!g_wav_tried) {
        const char* path = getenv("SOA_WAV");
        g_wav_tried = 1;
        if (path) {
            g_wav = fopen(path, "wb");
            g_wav_rate = rate;
            if (g_wav) wav_header(g_wav, rate, 0);
            else fprintf(stderr, "[audio] cannot write %s\n", path);
        }
    }
    if (!g_wav) return;
    for (i = 0; i + 3 < bytes; i += 4) { /* right then left on the wire; left then right in the file */
        uint8_t s[4] = {be_rl[i + 3], be_rl[i + 2], be_rl[i + 1], be_rl[i]};
        fwrite(s, 1, 4, g_wav);
        g_wav_bytes += 4;
    }
}

void audio_set_muted(int on)
{
    g_muted = on;
}

void audio_set_test_sink(void (*fn)(const int16_t* lr, unsigned frames))
{
    g_test_sink = fn;
}

/* Queue one AI DMA block (big-endian R/L pairs). */
void audio_push_block(const uint8_t* be_rl, unsigned bytes, unsigned rate)
{
    unsigned i, n;
    uint64_t now = plat_mono_raw();
    g_blocks_seen++;
    if (!g_rate_first) g_rate_first = now ? now : 1;
    else g_rate_bytes += bytes; /* the bytes after the first block, over the time since it */
    g_rate_last = now;
    for (i = 0; i + 1 < bytes; i += 2) { /* a meter, so silence is visible in the report */
        int v = (int16_t)(((uint16_t)be_rl[i] << 8) | be_rl[i + 1]);
        if (v < 0) v = -v;
        if (v > g_peak) g_peak = v;
    }
    wav_append(be_rl, bytes, rate);
    if (g_test_sink) {
        static int16_t test[BLOCK_BYTES / 2];
        n = (bytes > BLOCK_BYTES ? BLOCK_BYTES : bytes) / 4;
        to_device(be_rl, n, test);
        g_test_sink(test, n);
        return;
    }
    if (g_ready < 0) g_ready = audio_open(rate);
    if (!g_ready) return;
    audio_play(be_rl, bytes > BLOCK_BYTES ? BLOCK_BYTES : bytes);
}

void audio_report(void)
{
    fprintf(stderr, "[audio] %llu DMA blocks, peak sample %d%s; %llu played, %llu dropped", (unsigned long long)g_blocks_seen, g_peak,
            g_ready > 0 ? "" : " (no output device)", (unsigned long long)g_pushed, (unsigned long long)g_dropped);
    if (g_dropped)
        fprintf(stderr, " in %llu run(s), the longest %llu", (unsigned long long)g_drop_runs, (unsigned long long)g_drop_longest);
    fprintf(stderr, "\n");
    if (g_rate_last > g_rate_first) {
        double secs = (double)(g_rate_last - g_rate_first) / plat_mono_hz();
        fprintf(stderr, "[audio] %llu bytes over %.1f s of wall time between the first block and the last: %.0f bytes a second\n",
                (unsigned long long)g_rate_bytes, secs, (double)g_rate_bytes / secs);
    }
    if (g_wav) { /* finish the file: the header carries the data size */
        wav_header(g_wav, g_wav_rate, g_wav_bytes);
        fclose(g_wav);
        g_wav = NULL;
        fprintf(stderr, "[audio] wrote %u bytes of samples to the WAV file\n", g_wav_bytes);
    }
}
