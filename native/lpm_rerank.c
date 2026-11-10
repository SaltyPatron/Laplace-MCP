/* Laplace 4D S3 geometric and discrete Frechet trajectory reranker for code search reranking.
 * Computes the reranker score that corrects lexical overlap for document length using corpus
 * document frequency chance p = 1 - (1 - r)^(L/A) and discrete Frechet trajectory distance over
 * 4D S3 coordinates. */
#define _GNU_SOURCE
#include "lpm_internal.h"

static void unit_s3(const int64_t m[4], double p[4]) {
    double s2 = 0.0;
    for (int d = 0; d < 4; d++) {
        p[d] = ldexp((double)m[d], -53);
        s2 += p[d] * p[d];
    }
    if (s2 > 1e-30) {
        double inv = 1.0 / sqrt(s2);
        for (int d = 0; d < 4; d++) p[d] *= inv;
    } else {
        p[0] = 1.0; p[1] = 0.0; p[2] = 0.0; p[3] = 0.0;
    }
}

static double dist4(const double a[4], const double b[4]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], dw = a[3] - b[3];
    return sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
}

static double directed_hausdorff4(const double *a, size_t na, const double *b, size_t nb) {
    if (!na || !nb) return 2.0;
    double max_min = 0.0;
    for (size_t i = 0; i < na; i++) {
        double min_d = 1e9;
        const double *ai = a + i * 4;
        for (size_t j = 0; j < nb; j++) {
            double d = dist4(ai, b + j * 4);
            if (d < min_d) min_d = d;
        }
        if (min_d > max_min) max_min = min_d;
    }
    return max_min;
}

/* Discrete Frechet trajectory distance over 4D S3 coordinates for code search reranking. */
static double subtraj_frechet4(const double *q, size_t nq, const double *d, size_t nd) {
    if (!nq || !nd) return -1.0;
    if (nd <= nq) return lp_frechet4(q, nq, d, nd);

    const double match_thresh = 0.28;
    size_t first_match = nd, last_match = 0;
    int any_match = 0;
    for (size_t j = 0; j < nd; j++) {
        const double *dj = d + j * 4;
        for (size_t i = 0; i < nq; i++) {
            if (dist4(dj, q + i * 4) <= match_thresh) {
                if (!any_match) first_match = j;
                last_match = j;
                any_match = 1;
                break;
            }
        }
    }

    if (any_match) {
        size_t pad = nq <= 4 ? 1 : 2;
        size_t w_start = first_match > pad ? first_match - pad : 0;
        size_t w_end = last_match + pad < nd - 1 ? last_match + pad : nd - 1;
        size_t max_win = nq * 3 > 8 ? nq * 3 : 8;
        if (w_end - w_start + 1 > max_win) w_end = w_start + max_win - 1;

        size_t win_len = w_end - w_start + 1;
        double *core = malloc(win_len * 4 * sizeof(double));
        size_t ncore = 0;
        for (size_t j = w_start; j <= w_end; j++) {
            const double *dj = d + j * 4;
            for (size_t i = 0; i < nq; i++) {
                if (dist4(dj, q + i * 4) <= match_thresh) {
                    memcpy(core + ncore * 4, dj, 4 * sizeof(double));
                    ncore++;
                    break;
                }
            }
        }
        size_t matched_q = 0;
        for (size_t i = 0; i < nq; i++) {
            const double *qi = q + i * 4;
            for (size_t k = 0; k < ncore; k++) {
                if (dist4(qi, core + k * 4) <= match_thresh) {
                    matched_q++;
                    break;
                }
            }
        }
        double core_frechet = ncore ? lp_frechet4(q, nq, core, ncore) : lp_frechet4(q, nq, d + w_start * 4, win_len);
        double core_haus = ncore ? directed_hausdorff4(q, nq, core, ncore) : core_frechet;
        free(core);
        double core_shape = ncore >= 2 ? fmin(core_frechet, core_haus + 0.12) : core_frechet;
        double cov_pen = (1.0 - (double)matched_q / (double)nq) * 0.45;
        double win_frechet = lp_frechet4(q, nq, d + w_start * 4, win_len);
        return fmin(core_shape + cov_pen, win_frechet);
    }

    size_t w = nq + 2 < nd ? nq + 2 : nd;
    size_t stride = nq / 2 > 1 ? nq / 2 : 1;
    double best = lp_frechet4(q, nq, d, nd);
    for (size_t s = 0; s + w <= nd; s += stride) {
        double f = lp_frechet4(q, nq, d + s * 4, w);
        if (f < best) best = f;
    }
    return best;
}

static void traj_walk(const Lpm *ix, const uint8_t id[16], double **pts, size_t *n, size_t *cap, int depth) {
    Ent *e = lpm_ent_find(ix, id);
    if (!e || depth > 64) return;
    if (!e->nkids || e->tier <= 2) {
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *pts = lpm_grow(*pts, *cap * 4 * sizeof(double));
        }
        unit_s3(e->m, *pts + *n * 4);
        (*n)++;
        return;
    }
    for (uint32_t i = 0; i < e->nkids; i++) traj_walk(ix, e->kids + (size_t)i * 16, pts, n, cap, depth + 1);
}

double lpm_frechet(const Lpm *ix, const uint8_t a[16], const uint8_t b[16]) {
    if (!lpm_ent_find(ix, a) || !lpm_ent_find(ix, b)) return -1;
    if (!memcmp(a, b, 16)) return 0.0;
    double *pa = NULL, *pb = NULL;
    size_t na = 0, nb = 0, ca = 0, cb = 0;
    traj_walk(ix, a, &pa, &na, &ca, 0);
    traj_walk(ix, b, &pb, &nb, &cb, 0);
    double d = (na && nb) ? subtraj_frechet4(pa, na, pb, nb) : -1;
    free(pa);
    free(pb);
    return d;
}

static void extract_word_traj(Lpm *ix, const uint8_t id[16], const int64_t cent_m[4],
                              double **pts_out, size_t *npts_out, double cent_out[4]) {
    double *pts = NULL;
    size_t npts = 0, cap = 0;
    traj_walk(ix, id, &pts, &npts, &cap, 0);
    *pts_out = pts;
    *npts_out = npts;
    unit_s3(cent_m, cent_out);
}

static size_t dedup_feat(uint8_t *buf, size_t n) {
    if (n <= 1) return n;
    qsort(buf, n, 16, lpm_cmp16);
    size_t w = 1;
    for (size_t k = 1; k < n; k++) {
        if (memcmp(buf + k * 16, buf + (w - 1) * 16, 16)) {
            memcpy(buf + w * 16, buf + k * 16, 16);
            w++;
        }
    }
    return w;
}

/* Reranker score that corrects lexical overlap for document length and combines it with
 * discrete Frechet trajectory distance over 4D S3 coordinates for code search reranking. */
void lpm_rerank_parts(Lpm *ix, const char *query, size_t qn, const char *doc, size_t dn, LpmRerank *out) {
    memset(out, 0, sizeof *out);
    out->frechet = -1.0;
    uint32_t *shared_out = &out->shared;
    double *frechet_out = &out->frechet;
    if (!ix || !qn || !dn) return;
    /* Corpus statistics before the query and the candidate are decomposed into the index's entities. */
    double avg;
    size_t live = lpm_live_stats(ix, &avg);
    size_t all_docs = ix->ndocs;

    uint64_t saved_files = ix->files, saved_bytes = ix->bytes, saved_comp = ix->compositions;
    size_t saved_ndocs = ix->ndocs;

    ix->nfeat = 0;
    ix->collecting = 2;
    lp_ref qr = {0};
    if (lpm_is_utf8((const uint8_t *)query, qn)) {
        qr = lp_text_decompose(ix->text, (const uint8_t *)query, qn, lpm_on_compose, ix);
    }
    if (qr.tier) lpm_add_feat(ix, qr.id.b);
    size_t qfn = dedup_feat(ix->feat, ix->nfeat);
    uint8_t *qfeat = qfn ? malloc(qfn * 16) : NULL;
    if (qfn && qfeat) memcpy(qfeat, ix->feat, qfn * 16);

    ix->nfeat = 0;
    ix->collecting = 1;
    lp_ref dr = {0};
    if (lpm_is_utf8((const uint8_t *)doc, dn)) {
        dr = lp_text_decompose(ix->text, (const uint8_t *)doc, dn, lpm_on_compose, ix);
    }
    if (dr.tier) lpm_add_feat(ix, dr.id.b);
    size_t dfn = dedup_feat(ix->feat, ix->nfeat);
    ix->collecting = 0;

    ix->files = saved_files;
    ix->bytes = saved_bytes;
    ix->compositions = saved_comp;
    ix->ndocs = saved_ndocs;

    /* Lexical overlap corrected for document length: a query constituent the candidate holds counts with its
     * 4th-power IDF specificity weight times the complement chance (1 - p) that holding it is not an accident of
     * document length: a document of L bytes holds a constituent found in a fraction r of the index's documents
     * (mean length A bytes) with probability p = 1 - (1 - r)^(L/A), so the match is worth w * (1 - p). */
    uint32_t shared = 0;
    double overlap_w = 0.0, total_w = 0.0;
    double len_ratio = live ? (double)dn / avg : 0.0;
    size_t i = 0, j = 0;
    while (i < qfn) {
        Ent *qe = lpm_ent_find(ix, qfeat + i * 16);
        int known = qe && qe->nd && all_docs;
        double rate = known ? (double)qe->nd / (double)all_docs : 0.0;
        if (rate > 1.0) rate = 1.0;
        double idf = known ? log(1.0 + (double)(all_docs + 1) / (double)qe->nd) : 1.0;
        double idf2 = idf * idf;
        double w = known ? idf2 * idf2 * (1.0 - rate * 0.90) : 0.25;
        double chance = rate > 0.0 && len_ratio > 0.0 ? 1.0 - pow(1.0 - rate, len_ratio) : 0.0;
        total_w += w;
        while (j < dfn && memcmp(ix->feat + j * 16, qfeat + i * 16, 16) < 0) j++;
        if (j < dfn && !memcmp(ix->feat + j * 16, qfeat + i * 16, 16)) {
            shared++;
            overlap_w += w * (1.0 - chance);
        }
        i++;
    }
    free(qfeat);

    double *q_pts = NULL, *d_pts = NULL;
    size_t nq_pts = 0, nd_pts = 0;
    double q_cent[4], d_cent[4];
    extract_word_traj(ix, qr.id.b, qr.c.m, &q_pts, &nq_pts, q_cent);
    extract_word_traj(ix, dr.id.b, dr.c.m, &d_pts, &nd_pts, d_cent);

    double fr = (nq_pts && nd_pts)
        ? subtraj_frechet4(q_pts, nq_pts, d_pts, nd_pts)
        : ((qr.tier && dr.tier) ? lpm_frechet(ix, qr.id.b, dr.id.b) : -1.0);
    free(q_pts);
    free(d_pts);

    if (!memcmp(qr.id.b, dr.id.b, 16) && qr.tier) {
        if (shared_out) *shared_out = shared;
        if (frechet_out) *frechet_out = 0.0;
        out->score = out->lexical = out->geometric = out->centroid = 1.0;
        return;
    }

    if (shared_out) *shared_out = shared;
    if (frechet_out) *frechet_out = fr;

    double lex = total_w > 0.0 ? (overlap_w / total_w) : 0.0;
    double geom = fr >= 0.0 ? (1.0 / (1.0 + fr)) : 0.0;
    double cent_dist = dist4(q_cent, d_cent);
    double cent_sim = 1.0 / (1.0 + cent_dist);
    /* Shape and centroid say how well the evidence is arranged, so they scale it: with no lexical evidence the
     * score is 0 whatever the length, instead of the constant the shape terms give any text. */
    double score = lex * (0.55 + 0.35 * geom + 0.10 * cent_sim);
    out->lexical = lex;
    out->geometric = geom;
    out->centroid = cent_sim;
    out->score = score > 1.0 ? 1.0 : score;
}

double lpm_rerank(Lpm *ix, const char *query, size_t qn, const char *doc, size_t dn, uint32_t *shared_out, double *frechet_out) {
    LpmRerank r;
    lpm_rerank_parts(ix, query, qn, doc, dn, &r);
    if (shared_out) *shared_out = r.shared;
    if (frechet_out) *frechet_out = r.frechet;
    return r.score;
}
