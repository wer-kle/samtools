/*  bam_mate.c -- fix mate pairing information and clean up flags.

    Copyright (C) 2009, 2011-2017, 2019, 2022 Genome Research Ltd.
    Portions copyright (C) 2011 Broad Institute.
    Portions copyright (C) 2012 Peter Cock, The James Hutton Institute.

    Author: Heng Li <lh3@sanger.ac.uk>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notices and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#include <config.h>

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "htslib/thread_pool.h"
#include "sam_opts.h"
#include "htslib/kstring.h"
#include "htslib/sam.h"
#include "samtools.h"


#define MD_MIN_QUALITY 15

/*
 * This function calculates ct tag for two bams, it assumes they are from the same template and
 * writes the tag to the first read in position terms.
 */
static void bam_template_cigar(bam1_t *b1, bam1_t *b2, kstring_t *str)
{
    bam1_t *swap;
    int i;
    hts_pos_t end;
    uint32_t *cigar;
    str->l = 0;
    if (b1->core.tid != b2->core.tid || b1->core.tid < 0 || b1->core.pos < 0 || b2->core.pos < 0 || b1->core.flag&BAM_FUNMAP || b2->core.flag&BAM_FUNMAP) return; // coordinateless or not on the same chr; skip
    if (b1->core.pos > b2->core.pos) swap = b1, b1 = b2, b2 = swap; // make sure b1 has a smaller coordinate
    kputc((b1->core.flag & BAM_FREAD1)? '1' : '2', str); // segment index
    kputc((b1->core.flag & BAM_FREVERSE)? 'R' : 'F', str); // strand
    for (i = 0, cigar = bam_get_cigar(b1); i < b1->core.n_cigar; ++i) {
        kputw(bam_cigar_oplen(cigar[i]), str);
        kputc(bam_cigar_opchr(cigar[i]), str);
    }
    end = bam_endpos(b1);
    kputw(b2->core.pos - end, str);
    kputc('T', str);
    kputc((b2->core.flag & BAM_FREAD1)? '1' : '2', str); // segment index
    kputc((b2->core.flag & BAM_FREVERSE)? 'R' : 'F', str); // strand
    for (i = 0, cigar = bam_get_cigar(b2); i < b2->core.n_cigar; ++i) {
        kputw(bam_cigar_oplen(cigar[i]), str);
        kputc(bam_cigar_opchr(cigar[i]), str);
    }

    uint8_t* data;
    if ((data = bam_aux_get(b1,"ct")) != NULL) bam_aux_del(b1, data);
    if ((data = bam_aux_get(b2,"ct")) != NULL) bam_aux_del(b2, data);

    bam_aux_append(b1, "ct", 'Z', str->l+1, (uint8_t*)str->s);
}

/*
 * What This Program is Supposed To Do:
 * Fill in mate coordinates, ISIZE and mate related flags from a name-sorted
 * alignment.
 *
 * How We Handle Input
 *
 * Secondary and supplementary Reads:
 * -write to output unchanged
 * All Reads:
 * -if pos == 0 (1 based), tid == -1 set UNMAPPED flag
 * single Reads:
 * -if pos == 0 (1 based), tid == -1, or UNMAPPED then set UNMAPPED, pos = 0,
 *  tid = -1
 * -clear bad flags (PAIRED, MREVERSE, PROPER_PAIR)
 * -set mpos = 0 (1 based), mtid = -1 and isize = 0
 * -write to output
 * Paired Reads:
 * -if read is unmapped and mate is not, set pos and tid to equal that of mate
 * -sync mate flags (MREVERSE, MUNMAPPED), mpos, mtid
 * -recalculate ISIZE if possible, otherwise set it to 0
 * -optionally clear PROPER_PAIR flag from reads where mapping or orientation
 *  indicate this is not possible (Illumina orientation only)
 * -calculate ct and apply to lowest positioned read
 * -write to output
 * Limitations
 * -Does not handle tandem reads
 * -Should mark supplementary reads the same as primary.
 * Notes
 * -CT definition appears to be something else in spec, this was in here before
 *  I started tampering with it, anyone know what is going on here? To work
 *  around this I have demoted the CT this tool generates to ct.
 */

static void sync_unmapped_pos_inner(bam1_t* src, bam1_t* dest) {
    if ((dest->core.flag & BAM_FUNMAP) && !(src->core.flag & BAM_FUNMAP)) {
        // Set unmapped read's RNAME and POS to those of its mapped mate
        // (recommended best practice, ensures if coord sort will be together)
        dest->core.tid = src->core.tid;
        dest->core.pos = src->core.pos;
    }
}

static void sync_mate_inner(bam1_t* src, bam1_t* dest)
{
    // sync mate pos information
    dest->core.mtid = src->core.tid; dest->core.mpos = src->core.pos;
    // sync flag info
    if (src->core.flag&BAM_FREVERSE)
        dest->core.flag |= BAM_FMREVERSE;
    else
        dest->core.flag &= ~BAM_FMREVERSE;
    if (src->core.flag & BAM_FUNMAP) {
        dest->core.flag |= BAM_FMUNMAP;
    }
}

// Is it plausible that these reads are properly paired?
// Can't really give definitive answer without checking isize
static bool plausibly_properly_paired(bam1_t* a, bam1_t* b)
{
    if ((a->core.flag & BAM_FUNMAP) || (b->core.flag & BAM_FUNMAP)) return false;
    assert(a->core.tid >= 0); // This should never happen if FUNMAP is set correctly

    if (a->core.tid != b->core.tid) return false;

    bam1_t* first = a;
    bam1_t* second = b;
    hts_pos_t a_pos = a->core.flag&BAM_FREVERSE ? bam_endpos(a) : a->core.pos;
    hts_pos_t  b_pos = b->core.flag&BAM_FREVERSE ? bam_endpos(b) : b->core.pos;
    if (a_pos > b_pos) {
        first = b;
        second = a;
    } else {
        first = a;
        second = b;
    }

    if (!(first->core.flag&BAM_FREVERSE) && (second->core.flag&BAM_FREVERSE))
        return true;
    else
        return false;
}

// Returns 0 on success, -1 on failure.
static int bam_format_cigar(const bam1_t* b, kstring_t* str)
{
    // An empty cigar is a special case return "*" rather than ""
    if (b->core.n_cigar == 0) {
        return (kputc('*', str) == EOF) ? -1 : 0;
    }

    const uint32_t *cigar = bam_get_cigar(b);
    uint32_t i;

    for (i = 0; i < b->core.n_cigar; ++i) {
        if (kputw(bam_cigar_oplen(cigar[i]), str) == EOF) return -1;
        if (kputc(bam_cigar_opchr(cigar[i]), str) == EOF) return -1;
    }

    return 0;
}

// Returns 0 on success, -1 on failure.
static int sync_mq_mc(bam1_t* src, bam1_t* dest)
{
    if ( (src->core.flag & BAM_FUNMAP) == 0 ) { // If mapped
        // Copy Mate Mapping Quality
        uint32_t mq = src->core.qual;
        uint8_t* data;
        if ((data = bam_aux_get(dest,"MQ")) != NULL) {
            bam_aux_del(dest, data);
        }

        bam_aux_append(dest, "MQ", 'i', sizeof(uint32_t), (uint8_t*)&mq);
    }
    // Copy mate cigar if either read is mapped
    if ( (src->core.flag & BAM_FUNMAP) == 0 || (dest->core.flag & BAM_FUNMAP) == 0 ) {
        uint8_t* data_mc;
        if ((data_mc = bam_aux_get(dest,"MC")) != NULL) {
            bam_aux_del(dest, data_mc);
        }

        // Convert cigar to string
        kstring_t mc = { 0, 0, NULL };
        if (bam_format_cigar(src, &mc) < 0) return -1;

        bam_aux_append(dest, "MC", 'Z', ks_len(&mc)+1, (uint8_t*)ks_str(&mc));
        free(mc.s);
    }
    return 0;
}

// Copy flags.
// Returns 0 on success, -1 on failure.
static int sync_mate(bam1_t* a, bam1_t* b)
{
    sync_unmapped_pos_inner(a,b);
    sync_unmapped_pos_inner(b,a);
    sync_mate_inner(a,b);
    sync_mate_inner(b,a);
    if (sync_mq_mc(a,b) < 0) return -1;
    if (sync_mq_mc(b,a) < 0) return -1;
    return 0;
}


static uint32_t calc_mate_score(bam1_t *b)
{
    uint32_t score = 0;
    uint8_t  *qual = bam_get_qual(b);
    int i;

    for (i = 0; i < b->core.l_qseq; i++) {
        if (qual[i] >= MD_MIN_QUALITY) score += qual[i];
    }

    return score;
}


static int add_mate_score(bam1_t *src, bam1_t *dest)
{
    uint8_t *data_ms;
    uint32_t mate_score = calc_mate_score(src);

    if ((data_ms = bam_aux_get(dest, "ms")) != NULL) {
        bam_aux_del(dest, data_ms);
    }

    if (bam_aux_append(dest, "ms", 'i', sizeof(uint32_t), (uint8_t*)&mate_score) == -1) {
        return -1;
    }

    return 0;
}

// Completely delete the CIGAR field
static void clear_cigar(bam1_t *b) {
    memmove(bam_get_cigar(b), bam_get_seq(b),
            b->data + b->l_data - bam_get_seq(b));
    b->l_data -= 4*b->core.n_cigar;
    b->core.n_cigar = 0;
}

// Trim a CIGAR field to end on reference position "end".  Remaining bases
// are turned to soft clips.
static int bam_trim(bam1_t *b, hts_pos_t end) {
    hts_pos_t pos = b->core.pos;
    int n_cigar = b->core.n_cigar, i;
    uint32_t new_cigar_a[1024];
    uint32_t *new_cigar = new_cigar_a;
    uint32_t *cigar = bam_get_cigar(b);

    // Find end of alignment or end of ref
    int op = 0, oplen = 0;
    for (i = 0; i < n_cigar; i++) {
        op = bam_cigar_op(cigar[i]);
        oplen = bam_cigar_oplen(cigar[i]);
        if (!(bam_cigar_type(op) & 2))
            continue;
        pos += oplen;
        if (pos > end)
            break;
    }

    if (i == n_cigar)
        // looks fine already
        return 0;

    int old_i = i, j = 0;
    // At worst we grow by 1 element (eg 100M -> 70M30S)
    if (n_cigar-i >= 1024-1) {
        new_cigar = malloc(4*(n_cigar-i+1));
        if (!new_cigar)
            return -1;
    }

    // We fill out to new_cigar from here on.
    if (pos-oplen < end) {
        // Partial CIGAR op?  Split existing tag.
        cigar[old_i++] = bam_cigar_gen(end - (pos-oplen), op);
        new_cigar[j++] = bam_cigar_gen(pos-end, BAM_CSOFT_CLIP);
    } else if (pos > end) {
        // entirely off the chromosome; this will trigger CIGAR *, MQUAL 0
        b->core.flag |= BAM_FUNMAP;
        b->core.flag &= ~BAM_FPROPER_PAIR;
    } else {
        // CIGAR op started on the trim junction
        new_cigar[j++] = bam_cigar_gen(oplen, BAM_CSOFT_CLIP);
    }

    // Replace trailing elements.
    for (i++; i < n_cigar; i++) {
        op = bam_cigar_op(cigar[i]);
        oplen = bam_cigar_oplen(cigar[i]);
        if (op == BAM_CHARD_CLIP) {
            new_cigar[j++] = cigar[i];
        } else {
            new_cigar[j-1] =
                bam_cigar_gen(bam_cigar_oplen(new_cigar[j-1]) + oplen,
                              BAM_CSOFT_CLIP);
        }
    }

    // We now have cigar[0..old_i-1] for existing CIGAR
    // and new_cigar[0..j-1] for new CIGAR trailing component.

    if (old_i+j == n_cigar) {
        // Fits and no data move needed
        memcpy(&cigar[old_i], new_cigar, j*4);
    } else {
        uint8_t *seq_old = bam_get_seq(b);
        uint8_t *aux_end = b->data + b->l_data;
        int nshift;
        if (old_i+j < n_cigar) {
            // Smaller, and can move data down
            nshift = -4*(n_cigar - (old_i+j));
        } else {
            // Bigger, so grow BAM and move data up
            nshift = 4*(old_i+j - n_cigar);
            // FIXME: make htslib's sam_realloc_bam_data public
            if (b->l_data + nshift > b->m_data) {
                uint8_t *new_data = realloc(b->data, b->l_data + nshift);
                if (!new_data) {
                    if (new_cigar != new_cigar_a)
                        free(new_cigar);
                    return -1;
                }
                b->m_data = b->l_data + nshift;
                if (b->data != new_data) {
                    b->data = new_data;
                    seq_old = bam_get_seq(b);
                    aux_end = b->data + b->l_data;
                    cigar = bam_get_cigar(b);
                }
            }
        }
        memmove(seq_old+nshift, seq_old, aux_end - seq_old);
        b->l_data += nshift;
        memcpy(&cigar[old_i], new_cigar, j*4);
        b->core.n_cigar = old_i+j;
    }

    if (new_cigar != new_cigar_a)
        free(new_cigar);

    return 0;
}

// Parses a comma-separated list of "pos", "mqual", "unmap", "cigar", and "aux"
// keywords for the bam sanitizer.
int bam_sanitize_options(const char *str) {
    int opt = 0;

    while (str && *str) {
        const char *str_start;
        while(*str && *str == ',')
            str++;

        for (str_start = str; *str && *str != ','; str++);
        int len = str - str_start;
        if (strncmp(str_start, "all", 3) == 0 || *str_start == '*')
            opt = FIX_ALL;
        else if (strncmp(str_start, "none", 4) == 0 ||
                 strncmp(str_start, "off", 3) == 0)
            opt = 0;
        else if (strncmp(str_start, "on", 2) == 0)
            // default for position sorted data
            opt = FIX_MQUAL | FIX_UNMAP | FIX_CIGAR | FIX_AUX;
        else if (strncmp(str_start, "pos", 3) == 0)
            opt |= FIX_POS;
        else if (strncmp(str_start, "mqual", 5) == 0)
            opt |= FIX_MQUAL;
        else if (strncmp(str_start, "unmap", 5) == 0)
            opt |= FIX_UNMAP;
        else if (strncmp(str_start, "cigar", 5) == 0)
            opt |= FIX_CIGAR;
        else if (strncmp(str_start, "aux", 3) == 0)
            opt |= FIX_AUX;
        else {
            print_error("sanitize", "Unrecognised keyword %.*s\n",
                        len, str_start);
            return -1;
        }
    }

    return opt;
}

int bam_sanitize(sam_hdr_t *h, bam1_t *b, int flags) {
    if ((flags & FIX_POS) && b->core.tid < 0) {
        // RNAME * => pos 0. NB can break alignment chr/pos sort order
        b->core.pos = -1;
        if (flags & FIX_UNMAP)
            b->core.flag |= BAM_FUNMAP;
    }

    if ((flags & FIX_CIGAR) && !(b->core.flag & BAM_FUNMAP)) {
        // Mapped => unmapped correction
        if (b->core.pos < 0 && (flags & FIX_UNMAP)) {
            b->core.flag |= BAM_FUNMAP;
        } else {
            hts_pos_t cur_end, rlen = sam_hdr_tid2len(h, b->core.tid);
            if (b->core.pos >= rlen && (flags & FIX_UNMAP)) {
                b->core.flag |= BAM_FUNMAP;
                if (flags & FIX_POS)
                    b->core.tid = b->core.pos = -1;
            } else if ((cur_end = bam_endpos(b)) > rlen) {
                if (bam_trim(b, rlen) < 0)
                    return -1;
            }
        }
    }

    if (b->core.flag & BAM_FUNMAP) {
        // Unmapped -> cigar/qual correctoins
        if ((flags & FIX_CIGAR) && b->core.n_cigar > 0)
            clear_cigar(b);

        if (flags & FIX_MQUAL)
            b->core.qual = 0;

        // Remove NM, MD, CG, SM tags.
        if (flags & FIX_AUX) {
            uint8_t *from = bam_aux_first(b);
            uint8_t *end = b->data + b->l_data;
            uint8_t *to = from ? from-2 : end;

#define XTAG(a) (((a)[0]<<8) + (a)[1])
            while (from) {
                uint8_t *next = bam_aux_next(b, from);
                if (!next && errno != ENOENT)
                    return -1;

                // Keep tag unless one of a specific set.
                // NB "to" always points to an aux tag start, while
                // "from" is after key.
                from -= 2;
                int key = (int)from[0]<<8 | from[1];
                if (key != XTAG("NM") && key != XTAG("MD") &&
                    key != XTAG("CG") && key != XTAG("SM")) {
                    ptrdiff_t len = (next ? next-2 : end) - from;
                    if (from != to)
                        memmove(to, from, len);
                    to += len;
                }
                from = next;
            }
            b->l_data = to - b->data;
        }
    }

    return 0;
}

// Ensure the b[] array is at least n.
// Returns 0 on success,
//        -1 on failure
static int grow_b_array(bam1_t **b, int *ba, int n) {
    if (n < *ba)
        return 0;

    bam1_t *bnew = realloc(*b, (n+10) * sizeof(**b));
    if (!bnew)
        return -1;
    *b = bnew;

    // bam_init1 equivalent
    int i;
    for (i = *ba; i < n; i++)
        memset(&(*b)[i], 0, sizeof(bam1_t));

    *b = bnew;
    *ba = n;

    return 0;
}

// We have b[0]..b[bn-1] entries all from the same template (qname)
typedef struct {
    bam1_t *b;
    int n, ba;  // number used and number allocated
    int b_next; // b[b_next] for start of next set, -1 if unset
    int eof;    // marker for having seen eof
} bam_set;

// Fetches a new batch of BAM records all containing the same name.
// NB: we cache the last (non-matching) name in b[n], so we can use it to
// start the next batch.
// Returns the number of records on success,
//         <0 on failure or EOF (sam_read1 return vals)
static int next_template(samFile *in, sam_hdr_t *header, bam_set *bs,
                         int sanitize_flags) {
    int result;

    if (bs->eof)
        return -1;

    // First time through, prime the template name
    if (bs->b_next < 0) {
        if (grow_b_array(&bs->b, &bs->ba, 1) < 0)
            return -2;
        result = sam_read1(in, header, &bs->b[0]);
        if (result < 0)
            return result;
        if (bam_sanitize(header, &bs->b[0], sanitize_flags) < 0)
            return -2;
    } else {
        // Otherwise use the previous template name read
        bam1_t btmp = bs->b[0];
        bs->b[0] = bs->b[bs->b_next];
        bs->b[bs->b_next] = btmp; // For ->{,l_,m_}data
    }
    bs->n = 1;

    // Now keep reading until we find a read that mismatches or we hit eof.
    char *name = bam_get_qname(&bs->b[0]);
    for (;;) {
        if (grow_b_array(&bs->b, &bs->ba, bs->n+1) < 0)
            return -2;

        result = sam_read1(in, header, &bs->b[bs->n]);
        if (result < -1)
            return result;
        if (bam_sanitize(header, &bs->b[0], sanitize_flags) < 0)
            return -2;

        if (result < 0) {
            bs->eof = 1;
            bs->b_next = -1;
            break;
        } else {
            bs->b_next = bs->n;
            if (strcmp(name, bam_get_qname(&bs->b[bs->n])) != 0)
                break;
        }

        bs->n++;
    }

    return bs->n;
}

// currently, this function ONLY works if each read has one hit
static int bam_mating_core(samFile *in, samFile *out, int remove_reads,
                           int proper_pair_check, int add_ct,
                           int do_mate_scoring, char *arg_list, int no_pg,
                           int sanitize_flags)
{
    sam_hdr_t *header;
    int result, n;
    kstring_t str = KS_INITIALIZE;
    bam_set bs = {NULL, 0, 0, -1, 0};

    header = sam_hdr_read(in);
    if (header == NULL) {
        fprintf(stderr, "[bam_mating_core] ERROR: Couldn't read header\n");
        return 1;
    }

    // Accept unknown, unsorted, or queryname sort order, but error on coordinate sorted.
    if (!sam_hdr_find_tag_hd(header, "SO", &str) && str.s && !strcmp(str.s, "coordinate")) {
        fprintf(stderr, "[bam_mating_core] ERROR: Coordinate sorted, require grouped/sorted by queryname.\n");
        goto fail;
    }
    ks_free(&str);

    if (!no_pg && sam_hdr_add_pg(header, "samtools",
                                 "VN", samtools_version(),
                                 arg_list ? "CL": NULL,
                                 arg_list ? arg_list : NULL,
                                 NULL))
        goto fail;

    if (sam_hdr_write(out, header) < 0) goto write_fail;

    // Iterate template by template fetching bs->n records at a time
    while ((result = next_template(in, header, &bs, sanitize_flags)) >= 0) {
        bam1_t *cur = NULL, *pre = NULL;
        int prev = -1, curr = -1;
        hts_pos_t pre_end = 0, cur_end = 0;

        // Find and fix up primary alignments
        for (n = 0; n < bs.n; n++) {
            if (bs.b[n].core.flag & (BAM_FSECONDARY | BAM_FSUPPLEMENTARY))
                continue;

            if (!pre) {
                pre = &bs.b[prev = n];
                pre_end = (pre->core.flag & BAM_FUNMAP) == 0
                    ? bam_endpos(pre) : 0;
                continue;
            }

            // Note, more than 2 primary alignments will use 'curr' as last
            cur = &bs.b[curr = n];
            cur_end = (cur->core.flag & BAM_FUNMAP) == 0
                ? bam_endpos(cur) : 0;

            pre->core.flag |= BAM_FPAIRED;
            cur->core.flag |= BAM_FPAIRED;
            if (sync_mate(pre, cur))
                goto fail;

            // If safe set TLEN/ISIZE
            if (pre->core.tid == cur->core.tid
                && !(cur->core.flag & (BAM_FUNMAP | BAM_FMUNMAP))
                && !(pre->core.flag & (BAM_FUNMAP | BAM_FMUNMAP))) {
                hts_pos_t cur5, pre5;
                cur5 = (cur->core.flag & BAM_FREVERSE)
                    ? cur_end
                    : cur->core.pos;
                pre5 = (pre->core.flag & BAM_FREVERSE)
                    ? pre_end
                    : pre->core.pos;
                cur->core.isize = pre5 - cur5;
                pre->core.isize = cur5 - pre5;
            } else {
                cur->core.isize = pre->core.isize = 0;
            }

            if (add_ct)
                bam_template_cigar(pre, cur, &str);

            // TODO: Add code to properly check if read is in a proper
            // pair based on ISIZE distribution
            if (proper_pair_check && !plausibly_properly_paired(pre,cur)) {
                pre->core.flag &= ~BAM_FPROPER_PAIR;
                cur->core.flag &= ~BAM_FPROPER_PAIR;
            }

            if (do_mate_scoring) {
                if ((add_mate_score(pre, cur) == -1) ||
                    (add_mate_score(cur, pre) == -1)) {
                    fprintf(stderr, "[bam_mating_core] ERROR: "
                            "unable to add mate score.\n");
                    goto fail;
                }
            }

            // If we have to remove reads make sure we do it in a way that
            // doesn't create orphans with bad flags
            if (remove_reads) {
                if (pre->core.flag&BAM_FUNMAP)
                    cur->core.flag &=
                        ~(BAM_FPAIRED|BAM_FMREVERSE|BAM_FPROPER_PAIR);
                if (cur->core.flag&BAM_FUNMAP)
                    pre->core.flag &=
                        ~(BAM_FPAIRED|BAM_FMREVERSE|BAM_FPROPER_PAIR);
            }
        }

        // Handle unpaired primary data
        if (!cur) {
            pre->core.mtid = -1;
            pre->core.mpos = -1;
            pre->core.isize = 0;
            pre->core.flag &= ~(BAM_FPAIRED|BAM_FMREVERSE|BAM_FPROPER_PAIR);
        }

        // Now process secondary and supplementary alignments.
        // TODO: we could reject secondaries / supplementaries that have no
        // known primary.  For now though we don't have anything to do here.
        // (However see fix MM in subsequent commits)

        // Finally having curated everything, write out all records in their
        // original ordering
        for (n = 0; n < bs.n; n++) {
            bam1_t *cur = &bs.b[n];
            // We may remove unmapped and secondary alignments
            if (remove_reads && (cur->core.flag & (BAM_FSECONDARY|BAM_FUNMAP)))
                continue;

            if (sam_write1(out, header, cur) < 0)
                goto write_fail;
        }
    }
    if (result < -1)
        goto read_fail;

    sam_hdr_destroy(header);
    for (n = 0; n < bs.ba; n++)
        free(bs.b[n].data);
    free(bs.b);
    ks_free(&str);
    return 0;

 read_fail:
    print_error("fixmate", "Couldn't read from input file");
    goto fail;

 write_fail:
    print_error_errno("fixmate", "Couldn't write to output file");
 fail:
    sam_hdr_destroy(header);
    for (n = 0; n < bs.ba; n++)
        free(bs.b[n].data);
    free(bs.b);
    ks_free(&str);
    return 1;
}

void usage(FILE* where)
{
    fprintf(where,
"Usage: samtools fixmate <in.nameSrt.bam> <out.nameSrt.bam>\n"
"Options:\n"
"  -r           Remove unmapped reads and secondary alignments\n"
"  -p           Disable FR proper pair check\n"
"  -c           Add template cigar ct tag\n"
"  -m           Add mate score tag\n"
"  -u           Uncompressed output\n"
"  -z, --sanitize FLAG[,FLAG]\n"
"               Sanitize alignment fields [defaults to all types]\n"
"  --no-PG      do not add a PG line\n");

    sam_global_opt_help(where, "-.O..@-.");

    fprintf(where,
"\n"
"As elsewhere in samtools, use '-' as the filename for stdin/stdout. The input\n"
"file must be grouped by read name (e.g. sorted by name). Coordinated sorted\n"
"input is not accepted.\n");
}

int bam_mating(int argc, char *argv[])
{
    htsThreadPool p = {NULL, 0};
    samFile *in = NULL, *out = NULL;
    int c, remove_reads = 0, proper_pair_check = 1, add_ct = 0, res = 1,
        mate_score = 0, no_pg = 0, sanitize_flags = FIX_ALL;
    sam_global_args ga = SAM_GLOBAL_ARGS_INIT;
    char wmode[4] = {'w', 'b', 0, 0};
    static const struct option lopts[] = {
        SAM_OPT_GLOBAL_OPTIONS('-', 0, 'O', 0, 0, '@'),
        {"no-PG", no_argument, NULL, 1},
        { NULL, 0, NULL, 0 }
    };
    char *arg_list = NULL;

    // parse args
    if (argc == 1) { usage(stdout); return 0; }
    while ((c = getopt_long(argc, argv, "rpcmO:@:uz:", lopts, NULL)) >= 0) {
        switch (c) {
        case 'r': remove_reads = 1; break;
        case 'p': proper_pair_check = 0; break;
        case 'c': add_ct = 1; break;
        case 'm': mate_score = 1; break;
        case 'u': wmode[2] = '0'; break;
        case 1: no_pg = 1; break;
        default:  if (parse_sam_global_opt(c, optarg, lopts, &ga) == 0) break;
            /* else fall-through */
        case '?': usage(stderr); goto fail;
        case 'z':
            if ((sanitize_flags = bam_sanitize_options(optarg)) < 0)
                exit(1);
            break;
        }
    }
    if (optind+1 >= argc) { usage(stderr); goto fail; }

    if (!no_pg && !(arg_list =  stringify_argv(argc+1, argv-1)))
        goto fail;

    // init
    if ((in = sam_open_format(argv[optind], "rb", &ga.in)) == NULL) {
        print_error_errno("fixmate", "cannot open input file");
        goto fail;
    }
    sam_open_mode(wmode+1, argv[optind+1], NULL);
    if ((out = sam_open_format(argv[optind+1], wmode, &ga.out)) == NULL) {
        print_error_errno("fixmate", "cannot open output file");
        goto fail;
    }

    if (ga.nthreads > 0) {
        if (!(p.pool = hts_tpool_init(ga.nthreads))) {
            fprintf(stderr, "Error creating thread pool\n");
            goto fail;
        }
        hts_set_opt(in,  HTS_OPT_THREAD_POOL, &p);
        hts_set_opt(out, HTS_OPT_THREAD_POOL, &p);
    }

    // run
    res = bam_mating_core(in, out, remove_reads, proper_pair_check, add_ct,
                          mate_score, arg_list, no_pg, sanitize_flags);

    // cleanup
    sam_close(in);
    if (sam_close(out) < 0) {
        fprintf(stderr, "[bam_mating] error while closing output file\n");
        res = 1;
    }

    if (p.pool) hts_tpool_destroy(p.pool);
    free(arg_list);
    sam_global_args_free(&ga);
    return res;

 fail:
    if (in) sam_close(in);
    if (out) sam_close(out);
    if (p.pool) hts_tpool_destroy(p.pool);
    free(arg_list);
    sam_global_args_free(&ga);
    return 1;
}


