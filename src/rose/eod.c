/*
 * Copyright (c) 2015-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "catchup.h"
#include "match.h"
#include "program_runtime.h"
#include "rose.h"
#include "util/fatbit.h"

static really_inline
void initContext(const struct RoseEngine *t, char *state, u64a offset,
                 struct hs_scratch *scratch, RoseCallback callback,
                 RoseCallbackSom som_callback) {
    struct RoseContext *tctxt = &scratch->tctxt;
    tctxt->groups = loadGroups(t, state); /* TODO: diff groups for eod */
    tctxt->lit_offset_adjust = scratch->core_info.buf_offset
                             - scratch->core_info.hlen
                             + 1; // index after last byte
    tctxt->delayLastEndOffset = offset;
    tctxt->lastEndOffset = offset;
    tctxt->filledDelayedSlots = 0;
    tctxt->cb = callback;
    tctxt->cb_som = som_callback;
    tctxt->lastMatchOffset = 0;
    tctxt->minMatchOffset = offset;
    tctxt->minNonMpvMatchOffset = offset;
    tctxt->next_mpv_offset = offset;
    tctxt->curr_anchored_loc = MMB_INVALID;
    tctxt->curr_row_offset = 0;

    scratch->catchup_pq.qm_size = 0;
    scratch->al_log_sum = 0; /* clear the anchored logs */

    fatbit_clear(scratch->aqa);
}

static rose_inline
hwlmcb_rv_t roseEodRunMatcher(const struct RoseEngine *t, u64a offset,
                              struct hs_scratch *scratch,
                              const char is_streaming) {
    assert(t->ematcherOffset);

    size_t eod_len;
    const u8 *eod_data;
    if (!is_streaming) { /* Block */
        eod_data = scratch->core_info.buf;
        eod_len = scratch->core_info.len;
    } else { /* Streaming */
        eod_len = scratch->core_info.hlen;
        eod_data = scratch->core_info.hbuf;
    }

    assert(eod_data);
    assert(eod_len);

    // If we don't have enough bytes to produce a match from an EOD table scan,
    // there's no point scanning.
    if (eod_len < t->eodmatcherMinWidth) {
        DEBUG_PRINTF("len=%zu < eodmatcherMinWidth=%u\n", eod_len,
                     t->eodmatcherMinWidth);
        return HWLM_CONTINUE_MATCHING;
    }

    // Ensure that we only need scan the last N bytes, where N is the length of
    // the eod-anchored matcher region.
    size_t adj = eod_len - MIN(eod_len, t->ematcherRegionSize);

    DEBUG_PRINTF("eod offset=%llu, eod length=%zu\n", offset, eod_len);

    struct RoseContext *tctxt = &scratch->tctxt;
    const struct HWLM *etable = getELiteralMatcher(t);

    hwlmExec(etable, eod_data, eod_len, adj, roseCallback, tctxt, tctxt->groups);

    // We may need to fire delayed matches
    return cleanUpDelayed(0, offset, scratch);
}

static rose_inline
int roseEodRunIterator(const struct RoseEngine *t, u64a offset,
                       struct hs_scratch *scratch) {
    if (!t->eodIterProgramOffset) {
        return MO_CONTINUE_MATCHING;
    }

    DEBUG_PRINTF("running eod program at offset %u\n", t->eodIterProgramOffset);

    const size_t match_len = 0;
    if (roseRunProgram(t, t->eodIterProgramOffset, offset, match_len,
                       &(scratch->tctxt), 0) == HWLM_TERMINATE_MATCHING) {
        return MO_HALT_MATCHING;
    }

    return MO_CONTINUE_MATCHING;
}

/**
 * \brief Adapts an NfaCallback to the rose callback specified in the
 * RoseContext.
 */
static
int eodNfaCallback(u64a offset, ReportID report, void *context) {
    struct hs_scratch *scratch = context;
    assert(scratch->magic == SCRATCH_MAGIC);
    return scratch->tctxt.cb(offset, report, scratch);
}

/**
 * \brief Adapts a SomNfaCallback to the rose SOM callback specified in the
 * RoseContext.
 */
static
int eodNfaSomCallback(u64a from_offset, u64a to_offset, ReportID report,
                      void *context) {
    struct hs_scratch *scratch = context;
    assert(scratch->magic == SCRATCH_MAGIC);
    return scratch->tctxt.cb_som(from_offset, to_offset, report, scratch);
}

/**
 * \brief Check for (and deliver) reports from active output-exposed (suffix
 * or outfix) NFAs.
 *
 * \return MO_HALT_MATCHING if the user instructs us to stop.
 */
static rose_inline
int roseCheckNfaEod(const struct RoseEngine *t, char *state,
                     struct hs_scratch *scratch, u64a offset,
                     const char is_streaming) {
    if (!t->eodNfaIterOffset) {
        DEBUG_PRINTF("no engines that report at EOD\n");
        return MO_CONTINUE_MATCHING;
    }

    /* data, len is used for state decompress, should be full available data */
    u8 key = 0;
    if (is_streaming) {
        const u8 *eod_data = scratch->core_info.hbuf;
        size_t eod_len = scratch->core_info.hlen;
        key = eod_len ? eod_data[eod_len - 1] : 0;
    }

    const u8 *aa = getActiveLeafArray(t, state);
    const u32 aaCount = t->activeArrayCount;

    const struct mmbit_sparse_iter *it = getByOffset(t, t->eodNfaIterOffset);
    assert(ISALIGNED(it));

    u32 idx = 0;
    struct mmbit_sparse_state si_state[MAX_SPARSE_ITER_STATES];

    for (u32 qi = mmbit_sparse_iter_begin(aa, aaCount, &idx, it, si_state);
         qi != MMB_INVALID;
         qi = mmbit_sparse_iter_next(aa, aaCount, qi, &idx, it, si_state)) {
        const struct NfaInfo *info = getNfaInfoByQueue(t, qi);
        const struct NFA *nfa = getNfaByInfo(t, info);

        DEBUG_PRINTF("checking nfa %u\n", qi);
        assert(nfaAcceptsEod(nfa));

        char *fstate = scratch->fullState + info->fullStateOffset;
        const char *sstate = (const char *)state + info->stateOffset;

        if (is_streaming) {
            // Decompress stream state.
            nfaExpandState(nfa, fstate, sstate, offset, key);
        }

        if (nfaCheckFinalState(nfa, fstate, sstate, offset, eodNfaCallback,
                               eodNfaSomCallback,
                               scratch) == MO_HALT_MATCHING) {
            DEBUG_PRINTF("user instructed us to stop\n");
            return MO_HALT_MATCHING;
        }
    }

    return MO_CONTINUE_MATCHING;
}

static rose_inline
void cleanupAfterEodMatcher(const struct RoseEngine *t, char *state,
                            u64a offset, struct hs_scratch *scratch) {
    struct RoseContext *tctxt = &scratch->tctxt;

    // Flush history to make sure it's consistent.
    roseFlushLastByteHistory(t, state, offset, tctxt);
}

static rose_inline
void roseCheckEodSuffixes(const struct RoseEngine *t, char *state, u64a offset,
                          struct hs_scratch *scratch) {
    const u8 *aa = getActiveLeafArray(t, state);
    const u32 aaCount = t->activeArrayCount;
    UNUSED u32 qCount = t->queueCount;

    for (u32 qi = mmbit_iterate(aa, aaCount, MMB_INVALID); qi != MMB_INVALID;
         qi = mmbit_iterate(aa, aaCount, qi)) {
        const struct NfaInfo *info = getNfaInfoByQueue(t, qi);
        const struct NFA *nfa = getNfaByInfo(t, info);

        assert(nfaAcceptsEod(nfa));

        DEBUG_PRINTF("checking nfa %u\n", qi);

        assert(fatbit_isset(scratch->aqa, qCount, qi)); /* we have just been
                                                           triggered */

        char *fstate = scratch->fullState + info->fullStateOffset;
        const char *sstate = (const char *)state + info->stateOffset;

        struct mq *q = scratch->queues + qi;

        pushQueueNoMerge(q, MQE_END, scratch->core_info.len);

        q->context = NULL;
        /* rose exec is used as we don't want to / can't raise matches in the
         * history buffer. */
        char rv = nfaQueueExecRose(q->nfa, q, MO_INVALID_IDX);
        if (rv) { /* nfa is still alive */
            if (nfaCheckFinalState(nfa, fstate, sstate, offset, eodNfaCallback,
                                   eodNfaSomCallback,
                                   scratch) == MO_HALT_MATCHING) {
                DEBUG_PRINTF("user instructed us to stop\n");
                return;
            }
        }
    }
}

static rose_inline
int roseRunEodProgram(const struct RoseEngine *t, u64a offset,
                      struct hs_scratch *scratch) {
    if (!t->eodProgramOffset) {
        return MO_CONTINUE_MATCHING;
    }

    DEBUG_PRINTF("running eod program at %u\n", t->eodProgramOffset);

    // There should be no pending delayed literals.
    assert(!scratch->tctxt.filledDelayedSlots);

    const size_t match_len = 0;
    if (roseRunProgram(t, t->eodProgramOffset, offset, match_len,
                       &scratch->tctxt, 0) == HWLM_TERMINATE_MATCHING) {
        return MO_HALT_MATCHING;
    }

    return MO_CONTINUE_MATCHING;
}

static really_inline
void roseEodExec_i(const struct RoseEngine *t, char *state, u64a offset,
                   struct hs_scratch *scratch, const char is_streaming) {
    assert(t);
    assert(scratch->core_info.buf || scratch->core_info.hbuf);
    assert(!scratch->core_info.buf || !scratch->core_info.hbuf);
    assert(!can_stop_matching(scratch));

    // Run the unconditional EOD program.
    if (roseRunEodProgram(t, offset, scratch) == MO_HALT_MATCHING) {
        return;
    }

    if (roseCheckNfaEod(t, state, scratch, offset, is_streaming) ==
        MO_HALT_MATCHING) {
        return;
    }

    if (!t->eodIterProgramOffset && !t->ematcherOffset) {
        DEBUG_PRINTF("no eod accepts\n");
        return;
    }

    // Handle pending EOD reports.
    if (roseEodRunIterator(t, offset, scratch) == MO_HALT_MATCHING) {
        return;
    }

    // Run the EOD anchored matcher if there is one.
    if (t->ematcherOffset) {
        assert(t->ematcherRegionSize);
        // Unset the reports we just fired so we don't fire them again below.
        mmbit_clear(getRoleState(state), t->rolesWithStateCount);
        mmbit_clear(getActiveLeafArray(t, state), t->activeArrayCount);

        if (roseEodRunMatcher(t, offset, scratch, is_streaming) ==
            HWLM_TERMINATE_MATCHING) {
            return;
        }

        cleanupAfterEodMatcher(t, state, offset, scratch);

        // Fire any new EOD reports.
        if (roseEodRunIterator(t, offset, scratch) == MO_HALT_MATCHING) {
            return;
        }

        roseCheckEodSuffixes(t, state, offset, scratch);
    }
}

void roseEodExec(const struct RoseEngine *t, u64a offset,
                 struct hs_scratch *scratch, RoseCallback callback,
                 RoseCallbackSom som_callback) {
    assert(scratch);
    assert(callback);
    assert(t->requiresEodCheck);
    DEBUG_PRINTF("ci buf %p/%zu his %p/%zu\n", scratch->core_info.buf,
                 scratch->core_info.len, scratch->core_info.hbuf,
                 scratch->core_info.hlen);

    if (t->maxBiAnchoredWidth != ROSE_BOUND_INF
        && offset > t->maxBiAnchoredWidth) {
        DEBUG_PRINTF("bailing, we are beyond max width\n");
        /* also some of the history/state may be stale */
        return;
    }

    char *state = scratch->core_info.state;
    assert(state);

    initContext(t, state, offset, scratch, callback, som_callback);

    roseEodExec_i(t, state, offset, scratch, 1);
}

static rose_inline
void prepForEod(const struct RoseEngine *t, char *state, size_t length,
                struct RoseContext *tctxt) {
    roseFlushLastByteHistory(t, state, length, tctxt);
    tctxt->lastEndOffset = length;
}

void roseBlockEodExec(const struct RoseEngine *t, u64a offset,
                      struct hs_scratch *scratch) {
    assert(t->requiresEodCheck);
    assert(t->maxBiAnchoredWidth == ROSE_BOUND_INF
           || offset <= t->maxBiAnchoredWidth);

    assert(!can_stop_matching(scratch));

    char *state = scratch->core_info.state;

    // Ensure that history is correct before we look for EOD matches
    prepForEod(t, state, scratch->core_info.len, &scratch->tctxt);

    roseEodExec_i(t, state, offset, scratch, 0);
}
