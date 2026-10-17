/*
 * soa-compare: a recording's calls replayed and held (replay.h,
 * docs/RECORD.md). Built by tools/replay.py with any candidates linked in.
 *
 *   soa-compare REC                    the translation's self check, then every candidate
 *                                      linked in whose function the recording has
 *   soa-compare REC --original         the self check alone
 *   soa-compare REC --candidate NAME   one candidate (repeatable)
 *   soa-compare REC --info             what the recording holds
 *   soa-compare REC --calls N --only 0xADDR
 *                                      the calls the game's call N of ADDR made
 *   soa-compare --list                 the candidates linked in
 *
 * Options: --game PATH (default gen/linux-split/libsoa_game.so), --only
 * 0xADDR (the self check of one function), --show N (mismatching calls told
 * in full, default 5), --stack BYTES (the stack under r1 a candidate may use
 * as scratch, default 65536), --with-flagged (replay calls the recording
 * flagged too), --partial (a byte the game's call wrote that the candidate
 * left as it was is counted as left undone, not held: for a rule that
 * answers part of a function; the stretches are listed). Exit 0 when every
 * call replayed agreed, 1 when one did not, 2 for a usage or a read error.
 */
#include "replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr, "usage: soa-compare REC [--original] [--candidate NAME]... [--info] [--game PATH] [--only 0xADDR] "
                    "[--show N] [--stack BYTES] [--with-flagged] [--partial] [--calls N]\n       soa-compare --list\n");
    return 2;
}

static int one(SoaReplayRun* run, const char* label)
{
    SoaReplayTotals t;
    printf("%s\n", label);
    fflush(stdout);
    if (!soa_replay(run, &t)) return -1;
    printf("  %llu calls: %llu agree, %llu do not, %llu not replayable; %llu with a flag skipped\n",
           (unsigned long long)t.calls, (unsigned long long)t.matched, (unsigned long long)t.mismatched,
           (unsigned long long)t.unreplayable, (unsigned long long)t.flagged_skipped);
    if (run->partial && !run->original)
        printf("  --partial: %llu more agree on every byte they wrote and every result, leaving undone %llu bytes the game's "
               "call wrote\n",
               (unsigned long long)t.partial, (unsigned long long)t.undone_bytes);
    if (run->candidate)
        printf("  the candidate answered %llu (%llu with the results left live), ran beside the game's own on %llu, left %llu to the game\n",
               (unsigned long long)(t.answered + t.live), (unsigned long long)t.live, (unsigned long long)t.both,
               (unsigned long long)t.games);
    printf("  held %llu bytes and %llu registers in %.3f s (%.0f calls a second)\n", (unsigned long long)t.bytes_held,
           (unsigned long long)t.regs_held, t.seconds, t.seconds > 0 ? (double)t.calls / t.seconds : 0.0);
    fflush(stdout);
    return t.mismatched || t.unreplayable ? 1 : 0; /* (calls agreeing but for what they left undone are not a failure
                                                      under --partial, which asked for them) */
}

int main(int argc, char** argv)
{
    SoaReplayRun run;
    const char* names[64];
    unsigned nnames = 0, n, k, j;
    int original = 0, info = 0, list = 0, bad = 0, ran = 0, rc;
    int64_t calls_of = -1;
    const SoaReplayCandidate* const* cands = soa_replay_candidates(&n);
    char label[256];
    memset(&run, 0, sizeof run);
    run.game = "gen/linux-split/libsoa_game.so";
    run.show = 5;
    for (k = 1; k < (unsigned)argc; k++) {
        const char* a = argv[k];
        const char* v = k + 1 < (unsigned)argc ? argv[k + 1] : NULL;
        if (!strcmp(a, "--original")) original = 1;
        else if (!strcmp(a, "--info")) info = 1;
        else if (!strcmp(a, "--list")) list = 1;
        else if (!strcmp(a, "--with-flagged")) run.with_flagged = 1;
        else if (!strcmp(a, "--partial")) run.partial = 1;
        else if (!strcmp(a, "--calls") && v) calls_of = strtoll(v, NULL, 0), info = 1, k++;
        else if (!strcmp(a, "--candidate") && v && nnames < 64) names[nnames++] = v, k++;
        else if (!strcmp(a, "--game") && v) run.game = v, k++;
        else if (!strcmp(a, "--only") && v) run.only = (uint32_t)strtoul(v, NULL, 0), k++;
        else if (!strcmp(a, "--show") && v) run.show = atoi(v), k++;
        else if (!strcmp(a, "--stack") && v) run.stack_scratch = (uint32_t)strtoul(v, NULL, 0), k++;
        else if (a[0] != '-' && !run.recording) run.recording = a;
        else return usage();
    }
    if (list) {
        for (k = 0; k < n; k++)
            printf("%08X %s (results held:%s%s%s%s)\n", cands[k]->address, cands[k]->name,
                   cands[k]->results & SOA_REPLAY_R3 ? " r3" : "", cands[k]->results & SOA_REPLAY_R4 ? " r4" : "",
                   cands[k]->results & SOA_REPLAY_F1 ? " f1" : "", cands[k]->results ? "" : " none");
        return 0;
    }
    if (!run.recording) return usage();
    if (info) return soa_replay_info(run.recording, run.only, calls_of) ? 0 : 2;
    for (j = 0; j < nnames; j++) {
        for (k = 0; k < n && strcmp(cands[k]->name, names[j]); k++) {}
        if (k == n) {
            fprintf(stderr, "soa-compare: no candidate named %s (--list)\n", names[j]);
            return 2;
        }
    }
    if (original || !nnames) {
        run.original = 1;
        snprintf(label, sizeof label, "the translation%s, held to the recording", run.only ? " of one function" : "");
        rc = one(&run, label);
        if (rc < 0) return 2;
        bad |= rc;
        ran++;
        run.original = 0;
    }
    if (!original)
        for (k = 0; k < n; k++) {
            int asked = !nnames;
            for (j = 0; j < nnames; j++) asked |= !strcmp(cands[k]->name, names[j]);
            if (!asked) continue;
            run.candidate = cands[k];
            snprintf(label, sizeof label, "candidate %s (%08X), held to the recording", cands[k]->name, cands[k]->address);
            rc = one(&run, label);
            if (rc < 0) return 2;
            bad |= rc;
            ran++;
        }
    return bad ? 1 : 0;
}
