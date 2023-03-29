// Copyright (c) 2017-2022, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/slate.hh"
#include "auxiliary/Debug.hh"
#include "slate/Matrix.hh"
#include "slate/Tile_blas.hh"
#include "slate/TriangularBandMatrix.hh"
#include "internal/internal.hh"

namespace slate {

//------------------------------------------------------------------------------
///
/// todo: document
///
/// @ingroup svd
///
/// Note A is passed by value, so we can transpose if needed
/// without affecting caller.
///
template <typename scalar_t>
void gesvd(
    Matrix<scalar_t> A,
    std::vector< blas::real_type<scalar_t> >& S,
    Matrix<scalar_t>& U,
    Matrix<scalar_t>& VT,
    Options const& opts)
{
    using real_t = blas::real_type<scalar_t>;
    using std::swap;

    // Constants
    scalar_t zero = 0;
    scalar_t one  = 1;

    const auto mpi_real_type = mpi_type< blas::real_type<scalar_t> >::value;

    Target target = get_option( opts, Option::Target, Target::HostTask );

    int64_t m = A.m();
    int64_t n = A.n();

    bool wantu  = (U.mt() > 0);
    bool wantvt = (VT.mt() > 0);

    bool flip = m < n; // Flip for fat matrix.
    if (flip) {
        slate_not_implemented("m < n not yet supported");
        swap(m, n);
        A = conj_transpose( A );
    }

    Job jobu  = Job::NoVec;
    Job jobvt = Job::NoVec;
    if (wantu) {
        jobu = Job::Vec;
    }

    if (wantvt) {
        jobvt = Job::Vec;
    }

    // Scale matrix to allowable range, if necessary.
    // todo

    // 0. If m >> n, use QR factorization to reduce to square.
    // Theoretical thresholds based on flops:
    // m >=  5/3 n for no vectors,
    // m >= 16/9 n for QR iteration with vectors,
    // m >= 10/3 n for Divide & Conquer with vectors.
    // Different in practice because stages have different flop rates.
    double threshold = 5/3.;
    bool qr_path = m > threshold*n;
    Matrix<scalar_t> Ahat;
    TriangularFactors<scalar_t> TQ;
    if (qr_path) {
        geqrf(A, TQ, opts);

        auto R_ = A.slice(0, n-1, 0, n-1);
        TriangularMatrix<scalar_t> R(Uplo::Upper, Diag::NonUnit, R_);

        Ahat = R_.emptyLike();
        Ahat.insertLocalTiles();
        set(zero, Ahat);  // todo: only lower

        TriangularMatrix<scalar_t> Ahat_tr(Uplo::Upper, Diag::NonUnit, Ahat);
        copy(R, Ahat_tr);
    }
    else {
        Ahat = A;
    }

    // 1. Reduce to band form.
    TriangularFactors<scalar_t> TU, TV;
    ge2tb(Ahat, TU, TV, opts);

    // Copy band.
    // Currently, gathers band matrix to rank 0.
    TriangularBandMatrix<scalar_t> Aband(Uplo::Upper, Diag::NonUnit,
                                         n, A.tileNb(0), A.tileNb(0),
                                         1, 1, A.mpiComm());
    Aband.insertLocalTiles();
    Aband.ge2tbGather(Ahat);

    // Currently, tb2bd and bdsqr run on a single node.
    //slate::Matrix<scalar_t> U2;
    //slate::Matrix<scalar_t> V2T;

    // Currently, tb2bd and bdsqr run on a single node.
    S.resize(n);
    std::vector<real_t> E(n - 1);

    if (A.mpiRank() == 0) {
        // 2. Reduce band to bi-diagonal.
        tb2bd(Aband, opts);

        // Copy diagonal and super-diagonal to vectors.
        internal::copytb2bd(Aband, S, E);
    }

    slate::set( zero, one, U );
    slate::set( zero, one, VT );

    // 3. Bi-diagonal SVD solver.
    if (wantu || wantvt) {
        // Bcast the Lambda and E vectors (diagonal and sup/super-diagonal).
        MPI_Bcast( &S[0], n,   mpi_real_type, 0, A.mpiComm() );
        MPI_Bcast( &E[0], n-1, mpi_real_type, 0, A.mpiComm() );

        // QR iteration
        bdsqr<scalar_t>(jobu, jobvt, S, E, U, VT, opts);

        int mpi_size;
        // Find the total number of processors.
        slate_mpi_call(
            MPI_Comm_size(A.mpiComm(), &mpi_size));

        //Matrix<scalar_t> U1d(U.m(), U.n(), U.tileNb(0), 1, mpi_size, U.mpiComm());
        //U1d.insertLocalTiles(target);
        //U1d.redistribute(U);

        //Matrix<scalar_t> VT1d(VT.m(), VT.n(), VT.tileNb(0), 1, mpi_size, VT.mpiComm());
        //VT1d.insertLocalTiles(target);
        //VT1d.redistribute(VT);

        // Back-transform: U = U1 * U2 * U.
        // U1 is the output of ge2tb and it is saved in A
        // U2 is the output of tb2bd
        // U initially has left singular vectors of the bidiagonal matrix
        //unmbr_tb2bd( Side::Left, Op::NoTrans, U2, U1d, opts );
        //U.redistribute(U1d);
        if (wantu) {
            //unmqr(Side::Left, Op::NoTrans, Ahat, TU, U, opts);
            unmbr_ge2tb( Side::Left, Op::NoTrans, Ahat, TU, U, opts );
        }

        // Back-transform: VT = VT * V2T * V1T.
        // V1T is the output of ge2tb and it is saved in A
        // V2T is the output of tb2bd
        // VT initially has right singular vectors of the bidiagonal matrix


        //unmbr_tb2bd( Side::Right, Op::NoTrans, V2T, VT1d, opts );
        //VT.redistribute(VT1d);
        //unmtr_he2hb( Side::Right, Op::NoTrans, A, TV, VT, opts );

        if (wantvt) {
            //auto A_sub = Ahat.sub(0, Ahat.mt()-1, 1, Ahat.nt()-1);
            //auto VT_sub = VT.sub(0, A.mt()-1, 1, A.nt()-1);
            //slate::TriangularFactors<scalar_t> TV_sub = {
            //    TV[ 0 ].sub( 0, A.mt()-1, 1, A.nt()-1 ),
            //    TV[ 1 ].sub( 0, A.mt()-1, 1, A.nt()-1 )
            //};
            //unmlq(Side::Right, Op::NoTrans, A_sub, TV_sub, VT_sub, opts);
            unmbr_ge2tb( Side::Right, Op::NoTrans, Ahat, TV, VT, opts );
        }
    }
    else {
        if (A.mpiRank() == 0) {
            // QR iteration
            bdsqr<scalar_t>(jobu, jobvt, S, E, U, VT, opts);
        }
        // Bcast singular nvalues.
        MPI_Bcast( &S[0], n, mpi_real_type, 0, A.mpiComm() );
    }

    // If matrix was scaled, then rescale singular values appropriately.
    // todo

    // todo: bcast S.

    if (qr_path) {
        // When initial QR was used.
        // U = Q*U;
    }

    if (flip) {
        // todo: swap(U, V);
    }
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
void gesvd<float>(
     Matrix<float> A,
     std::vector<float>& S,
     Matrix<float>& U,
     Matrix<float>& VT,
     Options const& opts);

template
void gesvd<double>(
     Matrix<double> A,
     std::vector<double>& S,
     Matrix<double>& U,
     Matrix<double>& VT,
     Options const& opts);

template
void gesvd< std::complex<float> >(
     Matrix< std::complex<float> > A,
     std::vector<float>& S,
     Matrix< std::complex<float> >& U,
     Matrix< std::complex<float> >& VT,
     Options const& opts);

template
void gesvd< std::complex<double> >(
     Matrix< std::complex<double> > A,
     std::vector<double>& S,
     Matrix< std::complex<double> >& U,
     Matrix< std::complex<double> >& VT,
     Options const& opts);

} // namespace slate
