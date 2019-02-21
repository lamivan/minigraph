#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "kthread.h"
#include "kvec.h"
#include "kalloc.h"
#include "mgpriv.h"
#include "bseq.h"
#include "khash.h"

struct mg_tbuf_s {
	void *km;
	int rep_len, frag_gap;
};

mg_tbuf_t *mg_tbuf_init(void)
{
	mg_tbuf_t *b;
	b = (mg_tbuf_t*)calloc(1, sizeof(mg_tbuf_t));
	if (!(mg_dbg_flag & 1)) b->km = km_init();
	return b;
}

void mg_tbuf_destroy(mg_tbuf_t *b)
{
	if (b == 0) return;
	km_destroy(b->km);
	free(b);
}

void *mg_tbuf_get_km(mg_tbuf_t *b)
{
	return b->km;
}

static void collect_minimizers(void *km, const mg_mapopt_t *opt, const mg_idx_t *gi, int n_segs, const int *qlens, const char **seqs, mg128_v *mv)
{
	int i, n, sum = 0;
	mv->n = 0;
	for (i = n = 0; i < n_segs; ++i) {
		size_t j;
		mg_sketch(km, seqs[i], qlens[i], gi->w, gi->k, i, gi->flag&MG_I_HPC, mv);
		for (j = n; j < mv->n; ++j)
			mv->a[j].y += sum << 1;
		sum += qlens[i], n = mv->n;
	}
}

#include "ksort.h"
#define heap_lt(a, b) ((a).x > (b).x)
KSORT_INIT(heap, mg128_t, heap_lt)

typedef struct {
	uint32_t n;
	uint32_t q_pos, q_span;
	uint32_t seg_id:31, is_tandem:1;
	const uint64_t *cr;
} mg_match_t;

static mg_match_t *collect_matches(void *km, int *_n_m, int max_occ, const mg_idx_t *gi, const mg128_v *mv, int64_t *n_a, int *rep_len, int *n_mini_pos, uint64_t **mini_pos)
{
	int rep_st = 0, rep_en = 0, n_m;
	size_t i;
	mg_match_t *m;
	*n_mini_pos = 0;
	*mini_pos = (uint64_t*)kmalloc(km, mv->n * sizeof(uint64_t));
	m = (mg_match_t*)kmalloc(km, mv->n * sizeof(mg_match_t));
	for (i = 0, n_m = 0, *rep_len = 0, *n_a = 0; i < mv->n; ++i) {
		const uint64_t *cr;
		mg128_t *p = &mv->a[i];
		uint32_t q_pos = (uint32_t)p->y, q_span = p->x & 0xff;
		int t;
		cr = mg_idx_get(gi, p->x>>8, &t);
		if (t >= max_occ) {
			int en = (q_pos >> 1) + 1, st = en - q_span;
			if (st > rep_en) {
				*rep_len += rep_en - rep_st;
				rep_st = st, rep_en = en;
			} else rep_en = en;
		} else {
			mg_match_t *q = &m[n_m++];
			q->q_pos = q_pos, q->q_span = q_span, q->cr = cr, q->n = t, q->seg_id = p->y >> 32;
			q->is_tandem = 0;
			if (i > 0 && p->x>>8 == mv->a[i - 1].x>>8) q->is_tandem = 1;
			if (i < mv->n - 1 && p->x>>8 == mv->a[i + 1].x>>8) q->is_tandem = 1;
			*n_a += q->n;
			(*mini_pos)[(*n_mini_pos)++] = (uint64_t)q_span<<32 | q_pos>>1;
		}
	}
	*rep_len += rep_en - rep_st;
	*_n_m = n_m;
	return m;
}

static inline int skip_seed(int flag, uint64_t r, const mg_match_t *q, const char *qname, int qlen, const mg_idx_t *gi)
{
	if (flag & (MG_M_FOR_ONLY|MG_M_REV_ONLY)) {
		if ((r&1) == (q->q_pos&1)) { // forward strand
			if (flag & MG_M_REV_ONLY) return 1;
		} else {
			if (flag & MG_M_FOR_ONLY) return 1;
		}
	}
	return 0;
}

static mg128_t *collect_seed_hits_heap(void *km, const mg_mapopt_t *opt, int max_occ, const mg_idx_t *gi, const char *qname, const mg128_v *mv, int qlen, int64_t *n_a, int *rep_len,
								  int *n_mini_pos, uint64_t **mini_pos)
{
	int i, n_m, heap_size = 0;
	int64_t j, n_for = 0, n_rev = 0;
	mg_match_t *m;
	mg128_t *a, *heap;

	m = collect_matches(km, &n_m, max_occ, gi, mv, n_a, rep_len, n_mini_pos, mini_pos);

	heap = (mg128_t*)kmalloc(km, n_m * sizeof(mg128_t));
	a = (mg128_t*)kmalloc(km, *n_a * sizeof(mg128_t));

	for (i = 0, heap_size = 0; i < n_m; ++i) {
		if (m[i].n > 0) {
			heap[heap_size].x = m[i].cr[0];
			heap[heap_size].y = (uint64_t)i<<32;
			++heap_size;
		}
	}
	ks_heapmake_heap(heap_size, heap);
	while (heap_size > 0) {
		mg_match_t *q = &m[heap->y>>32];
		mg128_t *p;
		uint64_t r = heap->x;
		int32_t rpos = (uint32_t)r >> 1;
		if (!skip_seed(opt->flag, r, q, qname, qlen, gi)) {
			if ((r&1) == (q->q_pos&1)) { // forward strand
				p = &a[n_for++];
				p->x = (r&0xffffffff00000000ULL) | rpos;
				p->y = (uint64_t)q->q_span << 32 | q->q_pos >> 1;
			} else { // reverse strand
				p = &a[(*n_a) - (++n_rev)];
				p->x = 1ULL<<63 | (r&0xffffffff00000000ULL) | rpos;
				p->y = (uint64_t)q->q_span << 32 | (qlen - ((q->q_pos>>1) + 1 - q->q_span) - 1);
			}
			p->y |= (uint64_t)q->seg_id << MG_SEED_SEG_SHIFT;
			if (q->is_tandem) p->y |= MG_SEED_TANDEM;
		}
		// update the heap
		if ((uint32_t)heap->y < q->n - 1) {
			++heap[0].y;
			heap[0].x = m[heap[0].y>>32].cr[(uint32_t)heap[0].y];
		} else {
			heap[0] = heap[heap_size - 1];
			--heap_size;
		}
		ks_heapdown_heap(0, heap_size, heap);
	}
	kfree(km, m);
	kfree(km, heap);

	// reverse anchors on the reverse strand, as they are in the descending order
	for (j = 0; j < n_rev>>1; ++j) {
		mg128_t t = a[(*n_a) - 1 - j];
		a[(*n_a) - 1 - j] = a[(*n_a) - (n_rev - j)];
		a[(*n_a) - (n_rev - j)] = t;
	}
	if (*n_a > n_for + n_rev) {
		memmove(a + n_for, a + (*n_a) - n_rev, n_rev * sizeof(mg128_t));
		*n_a = n_for + n_rev;
	}
	return a;
}

static mg128_t *collect_seed_hits(void *km, const mg_mapopt_t *opt, int max_occ, const mg_idx_t *gi, const char *qname, const mg128_v *mv, int qlen, int64_t *n_a, int *rep_len,
								  int *n_mini_pos, uint64_t **mini_pos)
{
	int i, n_m;
	mg_match_t *m;
	mg128_t *a;
	m = collect_matches(km, &n_m, max_occ, gi, mv, n_a, rep_len, n_mini_pos, mini_pos);
	a = (mg128_t*)kmalloc(km, *n_a * sizeof(mg128_t));
	for (i = 0, *n_a = 0; i < n_m; ++i) {
		mg_match_t *q = &m[i];
		const uint64_t *r = q->cr;
		uint32_t k;
		for (k = 0; k < q->n; ++k) {
			int32_t rpos = (uint32_t)r[k] >> 1;
			mg128_t *p;
			if (skip_seed(opt->flag, r[k], q, qname, qlen, gi)) continue;
			p = &a[(*n_a)++];
			if ((r[k]&1) == (q->q_pos&1)) { // forward strand
				p->x = (r[k]&0xffffffff00000000ULL) | rpos;
				p->y = (uint64_t)q->q_span << 32 | q->q_pos >> 1;
			} else { // reverse strand
				p->x = 1ULL<<63 | (r[k]&0xffffffff00000000ULL) | rpos;
				p->y = (uint64_t)q->q_span << 32 | (qlen - ((q->q_pos>>1) + 1 - q->q_span) - 1);
			}
			p->y |= (uint64_t)q->seg_id << MG_SEED_SEG_SHIFT;
			if (q->is_tandem) p->y |= MG_SEED_TANDEM;
		}
	}
	kfree(km, m);
	radix_sort_128x(a, a + (*n_a));
	return a;
}

void mg_map_frag(const mg_idx_t *gi, int n_segs, const int *qlens, const char **seqs, int *n_regs, mg_lchain1_t **regs, mg_tbuf_t *b, const mg_mapopt_t *opt, const char *qname)
{
	int i, j, rep_len, qlen_sum, n_regs0, n_mini_pos;
	int max_chain_gap_qry, max_chain_gap_ref, is_splice = !!(opt->flag & MG_M_SPLICE), is_sr = !!(opt->flag & MG_M_SR);
	uint32_t hash;
	int64_t n_a;
	uint64_t *u, *mini_pos;
	mg128_t *a;
	mg128_v mv = {0,0,0};
	mg_lchain1_t *regs0;
	km_stat_t kmst;

	for (i = 0, qlen_sum = 0; i < n_segs; ++i)
		qlen_sum += qlens[i], n_regs[i] = 0, regs[i] = 0;

	if (qlen_sum == 0 || n_segs <= 0 || n_segs > MG_MAX_SEG) return;
	if (opt->max_qlen > 0 && qlen_sum > opt->max_qlen) return;

	hash  = qname? __ac_X31_hash_string(qname) : 0;
	hash ^= __ac_Wang_hash(qlen_sum) + __ac_Wang_hash(opt->seed);
	hash  = __ac_Wang_hash(hash);

	collect_minimizers(b->km, opt, gi, n_segs, qlens, seqs, &mv);
	if (opt->flag & MG_M_HEAP_SORT) a = collect_seed_hits_heap(b->km, opt, opt->mid_occ, gi, qname, &mv, qlen_sum, &n_a, &rep_len, &n_mini_pos, &mini_pos);
	else a = collect_seed_hits(b->km, opt, opt->mid_occ, gi, qname, &mv, qlen_sum, &n_a, &rep_len, &n_mini_pos, &mini_pos);

	if (mg_dbg_flag & MG_DBG_PRINT_SEED) {
		fprintf(stderr, "RS\t%d\n", rep_len);
		for (i = 0; i < n_a; ++i)
			fprintf(stderr, "SD\t%s\t%d\t%c\t%d\t%d\t%d\n", gi->g->seg[a[i].x<<1>>33].name, (int32_t)a[i].x, "+-"[a[i].x>>63], (int32_t)a[i].y, (int32_t)(a[i].y>>32&0xff),
					i == 0? 0 : ((int32_t)a[i].y - (int32_t)a[i-1].y) - ((int32_t)a[i].x - (int32_t)a[i-1].x));
	}

	// set max chaining gap on the query and the reference sequence
	if (is_sr)
		max_chain_gap_qry = qlen_sum > opt->max_gap? qlen_sum : opt->max_gap;
	else max_chain_gap_qry = opt->max_gap;
	if (opt->max_gap_ref > 0) {
		max_chain_gap_ref = opt->max_gap_ref; // always honor mg_mapopt_t::max_gap_ref if set
	} else if (opt->max_frag_len > 0) {
		max_chain_gap_ref = opt->max_frag_len - qlen_sum;
		if (max_chain_gap_ref < opt->max_gap) max_chain_gap_ref = opt->max_gap;
	} else max_chain_gap_ref = opt->max_gap;

	a = mg_lchain_dp(max_chain_gap_ref, max_chain_gap_qry, opt->bw, opt->max_chain_skip, opt->min_lc_cnt, opt->min_lc_score, is_splice, n_segs, n_a, a, &n_regs0, &u, b->km);

	if (opt->max_occ > opt->mid_occ && rep_len > 0) {
		int rechain = 0;
		if (n_regs0 > 0) { // test if the best chain has all the segments
			int n_chained_segs = 1, max = 0, max_i = -1, max_off = -1, off = 0;
			for (i = 0; i < n_regs0; ++i) { // find the best chain
				if (max < (int)(u[i]>>32)) max = u[i]>>32, max_i = i, max_off = off;
				off += (uint32_t)u[i];
			}
			for (i = 1; i < (int32_t)u[max_i]; ++i) // count the number of segments in the best chain
				if ((a[max_off+i].y&MG_SEED_SEG_MASK) != (a[max_off+i-1].y&MG_SEED_SEG_MASK))
					++n_chained_segs;
			if (n_chained_segs < n_segs)
				rechain = 1;
		} else rechain = 1;
		if (rechain) { // redo chaining with a higher max_occ threshold
			kfree(b->km, a);
			kfree(b->km, u);
			kfree(b->km, mini_pos);
			if (opt->flag & MG_M_HEAP_SORT) a = collect_seed_hits_heap(b->km, opt, opt->max_occ, gi, qname, &mv, qlen_sum, &n_a, &rep_len, &n_mini_pos, &mini_pos);
			else a = collect_seed_hits(b->km, opt, opt->max_occ, gi, qname, &mv, qlen_sum, &n_a, &rep_len, &n_mini_pos, &mini_pos);
			a = mg_lchain_dp(max_chain_gap_ref, max_chain_gap_qry, opt->bw, opt->max_chain_skip, opt->min_lc_cnt, opt->min_lc_score, is_splice, n_segs, n_a, a, &n_regs0, &u, b->km);
		}
	}
	b->frag_gap = max_chain_gap_ref;
	b->rep_len = rep_len;

	regs0 = mg_lchain_gen(b->km, hash, qlen_sum, n_regs0, u, a);

	if (mg_dbg_flag & MG_DBG_PRINT_SEED)
		for (j = 0; j < n_regs0; ++j)
			for (i = regs0[j].as; i < regs0[j].as + regs0[j].cnt; ++i)
				fprintf(stderr, "CN\t%d\t%s\t%d\t%c\t%d\t%d\t%d\n", j, gi->g->seg[a[i].x<<1>>33].name, (int32_t)a[i].x, "+-"[a[i].x>>63], (int32_t)a[i].y, (int32_t)(a[i].y>>32&0xff),
						i == regs0[j].as? 0 : ((int32_t)a[i].y - (int32_t)a[i-1].y) - ((int32_t)a[i].x - (int32_t)a[i-1].x));
	//if (!is_sr) mg_est_err(gi, qlen_sum, n_regs0, regs0, a, n_mini_pos, mini_pos);

	kfree(b->km, mv.a);
	kfree(b->km, a);
	kfree(b->km, u);
	kfree(b->km, mini_pos);

	if (b->km) {
		km_stat(b->km, &kmst);
		if (mg_dbg_flag & MG_DBG_PRINT_QNAME)
			fprintf(stderr, "QM\t%s\t%d\tcap=%ld,nCore=%ld,largest=%ld\n", qname, qlen_sum, kmst.capacity, kmst.n_cores, kmst.largest);
		assert(kmst.n_blocks == kmst.n_cores); // otherwise, there is a memory leak
		if (kmst.largest > 1U<<28) {
			km_destroy(b->km);
			b->km = km_init();
		}
	}
}

mg_lchain1_t *mg_map(const mg_idx_t *gi, int qlen, const char *seq, int *n_regs, mg_tbuf_t *b, const mg_mapopt_t *opt, const char *qname)
{
	mg_lchain1_t *regs;
	mg_map_frag(gi, 1, &qlen, &seq, n_regs, &regs, b, opt, qname);
	return regs;
}

/**************************
 * Multi-threaded mapping *
 **************************/

typedef struct {
	int mini_batch_size, n_processed, n_threads, n_fp;
	const mg_mapopt_t *opt;
	mg_bseq_file_t **fp;
	const mg_idx_t *gi;
	kstring_t str;

	int n_parts;
	uint32_t *rid_shift;
} pipeline_t;

typedef struct {
	const pipeline_t *p;
    int n_seq, n_frag;
	mg_bseq1_t *seq;
	int *n_reg, *seg_off, *n_seg, *rep_len, *frag_gap;
	mg_lchain1_t **reg;
	mg_tbuf_t **buf;
} step_t;

static void worker_for(void *_data, long i, int tid) // kt_for() callback
{
    step_t *s = (step_t*)_data;
	int qlens[MG_MAX_SEG], j, off = s->seg_off[i], pe_ori = s->p->opt->pe_ori;
	const char *qseqs[MG_MAX_SEG];
	mg_tbuf_t *b = s->buf[tid];
	assert(s->n_seg[i] <= MG_MAX_SEG);
	if (mg_dbg_flag & MG_DBG_PRINT_QNAME)
		fprintf(stderr, "QR\t%s\t%d\t%d\n", s->seq[off].name, tid, s->seq[off].l_seq);
	for (j = 0; j < s->n_seg[i]; ++j) {
		if (s->n_seg[i] == 2 && ((j == 0 && (pe_ori>>1&1)) || (j == 1 && (pe_ori&1))))
			mg_revcomp_bseq(&s->seq[off + j]);
		qlens[j] = s->seq[off + j].l_seq;
		qseqs[j] = s->seq[off + j].seq;
	}
	if (s->p->opt->flag & MG_M_INDEPEND_SEG) {
		for (j = 0; j < s->n_seg[i]; ++j) {
			mg_map_frag(s->p->gi, 1, &qlens[j], &qseqs[j], &s->n_reg[off+j], &s->reg[off+j], b, s->p->opt, s->seq[off+j].name);
			s->rep_len[off + j] = b->rep_len;
			s->frag_gap[off + j] = b->frag_gap;
		}
	} else {
		mg_map_frag(s->p->gi, s->n_seg[i], qlens, qseqs, &s->n_reg[off], &s->reg[off], b, s->p->opt, s->seq[off].name);
		for (j = 0; j < s->n_seg[i]; ++j) {
			s->rep_len[off + j] = b->rep_len;
			s->frag_gap[off + j] = b->frag_gap;
		}
	}
	for (j = 0; j < s->n_seg[i]; ++j) // flip the query strand and coordinate to the original read strand
		if (s->n_seg[i] == 2 && ((j == 0 && (pe_ori>>1&1)) || (j == 1 && (pe_ori&1)))) {
			int k, t;
			mg_revcomp_bseq(&s->seq[off + j]);
			for (k = 0; k < s->n_reg[off + j]; ++k) {
				mg_lchain1_t *r = &s->reg[off + j][k];
				t = r->qs;
				r->qs = qlens[j] - r->qe;
				r->qe = qlens[j] - t;
				r->rev = !r->rev;
			}
		}
}

static void *worker_pipeline(void *shared, int step, void *in)
{
	int i, j, k;
    pipeline_t *p = (pipeline_t*)shared;
    if (step == 0) { // step 0: read sequences
		int with_qual = !(p->opt->flag & MG_M_NO_QUAL);
		int with_comment = !!(p->opt->flag & MG_M_COPY_COMMENT);
		int frag_mode = (p->n_fp > 1 || !!(p->opt->flag & MG_M_FRAG_MODE));
        step_t *s;
        s = (step_t*)calloc(1, sizeof(step_t));
		if (p->n_fp > 1) s->seq = mg_bseq_read_frag(p->n_fp, p->fp, p->mini_batch_size, with_qual, with_comment, &s->n_seq);
		else s->seq = mg_bseq_read(p->fp[0], p->mini_batch_size, with_qual, with_comment, frag_mode, &s->n_seq);
		if (s->seq) {
			s->p = p;
			for (i = 0; i < s->n_seq; ++i)
				s->seq[i].rid = p->n_processed++;
			s->buf = (mg_tbuf_t**)calloc(p->n_threads, sizeof(mg_tbuf_t*));
			for (i = 0; i < p->n_threads; ++i)
				s->buf[i] = mg_tbuf_init();
			s->n_reg = (int*)calloc(5 * s->n_seq, sizeof(int));
			s->seg_off = s->n_reg + s->n_seq; // seg_off, n_seg, rep_len and frag_gap are allocated together with n_reg
			s->n_seg = s->seg_off + s->n_seq;
			s->rep_len = s->n_seg + s->n_seq;
			s->frag_gap = s->rep_len + s->n_seq;
			s->reg = (mg_lchain1_t**)calloc(s->n_seq, sizeof(mg_lchain1_t*));
			for (i = 1, j = 0; i <= s->n_seq; ++i)
				if (i == s->n_seq || !frag_mode || !mg_qname_same(s->seq[i-1].name, s->seq[i].name)) {
					s->n_seg[s->n_frag] = i - j;
					s->seg_off[s->n_frag++] = j;
					j = i;
				}
			return s;
		} else free(s);
    } else if (step == 1) { // step 1: map
		kt_for(p->n_threads, worker_for, in, ((step_t*)in)->n_frag);
		return in;
    } else if (step == 2) { // step 2: output
		void *km = 0;
        step_t *s = (step_t*)in;
		for (i = 0; i < p->n_threads; ++i) mg_tbuf_destroy(s->buf[i]);
		free(s->buf);
		if (!(mg_dbg_flag & MG_DBG_NO_KALLOC)) km = km_init();
		for (k = 0; k < s->n_frag; ++k) {
			int seg_st = s->seg_off[k], seg_en = s->seg_off[k] + s->n_seg[k];
			for (i = seg_st; i < seg_en; ++i) {
				free(s->reg[i]);
				free(s->seq[i].seq); free(s->seq[i].name);
				if (s->seq[i].qual) free(s->seq[i].qual);
				if (s->seq[i].comment) free(s->seq[i].comment);
			}
		}
		free(s->reg); free(s->n_reg); free(s->seq); // seg_off, n_seg, rep_len and frag_gap were allocated with reg; no memory leak here
		if (km) km_destroy(km);
		if (mg_verbose >= 3)
			fprintf(stderr, "[M::%s::%.3f*%.2f] mapped %d sequences\n", __func__, realtime() - mg_realtime0, cputime() / (realtime() - mg_realtime0), s->n_seq);
		free(s);
	}
    return 0;
}

static mg_bseq_file_t **open_bseqs(int n, const char **fn)
{
	mg_bseq_file_t **fp;
	int i, j;
	fp = (mg_bseq_file_t**)calloc(n, sizeof(mg_bseq_file_t*));
	for (i = 0; i < n; ++i) {
		if ((fp[i] = mg_bseq_open(fn[i])) == 0) {
			if (mg_verbose >= 1)
				fprintf(stderr, "ERROR: failed to open file '%s'\n", fn[i]);
			for (j = 0; j < i; ++j)
				mg_bseq_close(fp[j]);
			free(fp);
			return 0;
		}
	}
	return fp;
}

int mg_map_file_frag(const mg_idx_t *idx, int n_segs, const char **fn, const mg_mapopt_t *opt, int n_threads)
{
	int i, pl_threads;
	pipeline_t pl;
	if (n_segs < 1) return -1;
	memset(&pl, 0, sizeof(pipeline_t));
	pl.n_fp = n_segs;
	pl.fp = open_bseqs(pl.n_fp, fn);
	if (pl.fp == 0) return -1;
	pl.opt = opt, pl.gi = idx;
	pl.n_threads = n_threads > 1? n_threads : 1;
	pl.mini_batch_size = opt->mini_batch_size;
	pl_threads = n_threads == 1? 1 : (opt->flag&MG_M_2_IO_THREADS)? 3 : 2;
	kt_pipeline(pl_threads, worker_pipeline, &pl, 3);

	free(pl.str.s);
	for (i = 0; i < pl.n_fp; ++i)
		mg_bseq_close(pl.fp[i]);
	free(pl.fp);
	return 0;
}

int mg_map_file(const mg_idx_t *idx, const char *fn, const mg_mapopt_t *opt, int n_threads)
{
	return mg_map_file_frag(idx, 1, &fn, opt, n_threads);
}