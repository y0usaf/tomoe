#include "mtdev_priv.h"

#include <string.h>

typedef unsigned int tmt_col[1];
typedef unsigned int tmt_mat[TMT_FINGERS];

#define TMT_GET1(m, x) ((m[0] >> (x)) & 1U)
#define TMT_SET1(m, x) (m[0] |= (1U << (x)))
#define TMT_CLEAR1(m, x) (m[0] &= ~(1U << (x)))

#define TMT_GET2(m, row, col) ((m[col] >> (row)) & 1U)
#define TMT_SET2(m, row, col) (m[col] |= (1U << (row)))
#define TMT_CLEAR2(m, row, col) (m[col] &= ~(1U << (row)))

static void build_index(int *ix, tmt_mat mstar, int nrows, int ncols) {
    int row, col;
    for (row = 0; row < nrows; row++) {
        for (col = 0; col < ncols; col++) {
            if (TMT_GET2(mstar, row, col)) {
                ix[row] = col;
                break;
            }
        }
    }
}

static void step2a(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin);
static void step2b(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin);
static void step3(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin);
static void step4(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin, int row, int col);
static void step5(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin);

static void ixoptimal(int *ix, int *mdist, int nrows, int ncols) {
    int *temp, *end, *colend, value, minValue;
    int dmin, row, col;
    tmt_col ccol, crow;
    tmt_mat mstar, mprime, nmstar;

    memset(ccol, 0, sizeof(tmt_col));
    memset(crow, 0, sizeof(tmt_col));
    memset(mstar, 0, sizeof(tmt_mat));
    memset(mprime, 0, sizeof(tmt_mat));
    memset(nmstar, 0, sizeof(tmt_mat));

    for (row = 0; row < nrows; row++) ix[row] = -1;

    end = mdist + nrows * ncols;

    if (nrows <= ncols) {
        dmin = nrows;
        for (row = 0; row < nrows; row++) {
            temp = mdist + row;
            minValue = *temp;
            temp += nrows;
            while (temp < end) {
                value = *temp;
                if (value < minValue) minValue = value;
                temp += nrows;
            }
            temp = mdist + row;
            while (temp < end) {
                *temp -= minValue;
                temp += nrows;
            }
        }
        for (row = 0; row < nrows; row++) {
            for (col = 0; col < ncols; col++) {
                if (mdist[row + nrows * col] != 0) continue;
                if (TMT_GET1(ccol, col)) continue;
                TMT_SET2(mstar, row, col);
                TMT_SET1(ccol, col);
                break;
            }
        }
    } else {
        dmin = ncols;
        for (col = 0; col < ncols; col++) {
            temp = mdist + nrows * col;
            colend = temp + nrows;
            minValue = *temp++;
            while (temp < colend) {
                value = *temp++;
                if (value < minValue) minValue = value;
            }
            temp = mdist + nrows * col;
            while (temp < colend) *temp++ -= minValue;
        }
        for (col = 0; col < ncols; col++) {
            for (row = 0; row < nrows; row++) {
                if (mdist[row + nrows * col] != 0) continue;
                if (TMT_GET1(crow, row)) continue;
                TMT_SET2(mstar, row, col);
                TMT_SET1(ccol, col);
                TMT_SET1(crow, row);
                break;
            }
        }
        memset(crow, 0, sizeof(tmt_col));
    }

    step2b(ix, mdist, mstar, nmstar, mprime, ccol, crow, nrows, ncols, dmin);
}

static void step2a(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin) {
    int col, row;
    for (col = 0; col < ncols; col++) {
        for (row = 0; row < nrows; row++) {
            if (!TMT_GET2(mstar, row, col)) continue;
            TMT_SET1(ccol, col);
            break;
        }
    }
    step2b(ix, mdist, mstar, nmstar, mprime, ccol, crow, nrows, ncols, dmin);
}

static void step2b(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin) {
    int col, ncc;
    ncc = 0;
    for (col = 0; col < ncols; col++)
        if (TMT_GET1(ccol, col)) ncc++;
    if (ncc == dmin)
        build_index(ix, mstar, nrows, ncols);
    else
        step3(ix, mdist, mstar, nmstar, mprime, ccol, crow, nrows, ncols, dmin);
}

static void step3(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin) {
    int zerosFound;
    int row, col, cstar;

    zerosFound = 1;
    while (zerosFound) {
        zerosFound = 0;
        for (col = 0; col < ncols; col++) {
            if (TMT_GET1(ccol, col)) continue;
            for (row = 0; row < nrows; row++) {
                if (mdist[row + nrows * col] != 0) continue;
                if (TMT_GET1(crow, row)) continue;
                TMT_SET2(mprime, row, col);
                for (cstar = 0; cstar < ncols; cstar++)
                    if (TMT_GET2(mstar, row, cstar)) break;
                if (cstar == ncols) {
                    step4(ix, mdist, mstar, nmstar, mprime, ccol, crow, nrows, ncols, dmin, row, col);
                    return;
                } else {
                    TMT_SET1(crow, row);
                    TMT_CLEAR1(ccol, cstar);
                    zerosFound = 1;
                    break;
                }
            }
        }
    }

    step5(ix, mdist, mstar, nmstar, mprime, ccol, crow, nrows, ncols, dmin);
}

static void step4(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin, int row, int col) {
    int rstar, cstar, primeRow, primeCol;

    memcpy(nmstar, mstar, sizeof(tmt_mat));
    TMT_SET2(nmstar, row, col);
    cstar = col;
    for (rstar = 0; rstar < nrows; rstar++)
        if (TMT_GET2(mstar, rstar, cstar)) break;

    while (rstar < nrows) {
        TMT_CLEAR2(nmstar, rstar, cstar);
        primeRow = rstar;
        for (primeCol = 0; primeCol < ncols; primeCol++)
            if (TMT_GET2(mprime, primeRow, primeCol)) break;
        TMT_SET2(nmstar, primeRow, primeCol);
        cstar = primeCol;
        for (rstar = 0; rstar < nrows; rstar++)
            if (TMT_GET2(mstar, rstar, cstar)) break;
    }

    memcpy(mstar, nmstar, sizeof(tmt_mat));
    memset(mprime, 0, sizeof(tmt_mat));
    memset(crow, 0, sizeof(tmt_col));

    step2a(ix, mdist, mstar, nmstar, mprime, ccol, crow, nrows, ncols, dmin);
}

static void step5(int *ix, int *mdist, tmt_mat mstar, tmt_mat nmstar, tmt_mat mprime,
        tmt_col ccol, tmt_col crow, int nrows, int ncols, int dmin) {
    int h = 0, value;
    int row, col, found = 0;

    for (row = 0; row < nrows; row++) {
        if (TMT_GET1(crow, row)) continue;
        for (col = 0; col < ncols; col++) {
            if (TMT_GET1(ccol, col)) continue;
            value = mdist[row + nrows * col];
            if (!found || value < h) {
                h = value;
                found = 1;
            }
        }
    }

    if (!found) return;

    for (row = 0; row < nrows; row++) {
        if (!TMT_GET1(crow, row)) continue;
        for (col = 0; col < ncols; col++) mdist[row + nrows * col] += h;
    }

    for (col = 0; col < ncols; col++) {
        if (TMT_GET1(ccol, col)) continue;
        for (row = 0; row < nrows; row++) mdist[row + nrows * col] -= h;
    }

    step3(ix, mdist, mstar, nmstar, mprime, ccol, crow, nrows, ncols, dmin);
}

void tmt_match(int *ix, int *dist, int nrow, int ncol) {
    ixoptimal(ix, dist, nrow, ncol);
}
