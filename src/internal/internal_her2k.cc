// Copyright (c) 2017-2020, University of Tennessee. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
// This program is free software: you can redistribute it and/or modify it under
// the terms of the BSD 3-Clause license. See the accompanying LICENSE file.

#include "slate/Matrix.hh"
#include "slate/HermitianMatrix.hh"
#include "slate/types.hh"
#include "slate/Tile_blas.hh"
#include "internal/internal.hh"
#include "internal/internal_batch.hh"

#ifdef SLATE_WITH_MKL
    #include <mkl_cblas.h>
#else
    #include <cblas.h>
#endif

namespace slate {
namespace internal {

//------------------------------------------------------------------------------
/// Hermitian rank-2k update of single block column (i.e., k = nb).
/// Dispatches to target implementations.
/// C is Lower, NoTrans or Upper, Trans/ConjTrans.
/// In complex case, A, B, and C cannot be Trans.
/// Requires op(A) and op(B) to be the same, either both NoTrans, or both Trans.
/// @ingroup her2k_internal
///
template <Target target, typename scalar_t>
void her2k(scalar_t alpha,                  Matrix<scalar_t>&& A,
                                            Matrix<scalar_t>&& B,
           blas::real_type<scalar_t> beta,  HermitianMatrix<scalar_t>&& C,
           int priority, int queue_index, Layout layout)
{
    if (! ((C.uplo() == Uplo::Lower)
           &&
           (C.is_real || (C.op() != Op::Trans &&
                          A.op() != Op::Trans))
           &&
           (A.op() == B.op())))
        throw std::exception();

    her2k(internal::TargetType<target>(),
          alpha, A,
                 B,
          beta,  C,
          priority, queue_index, layout);
}

//------------------------------------------------------------------------------
/// Hermitian rank-2k update of single block column (i.e., k = nb).
/// Host OpenMP task implementation.
/// Assumes A is NoTrans or ConjTrans; C is Lower, NoTrans or Upper, ConjTrans.
/// @ingroup her2k_internal
///
template <typename scalar_t>
void her2k(internal::TargetType<Target::HostTask>,
           scalar_t alpha,                 Matrix<scalar_t>& A,
                                           Matrix<scalar_t>& B,
           blas::real_type<scalar_t> beta, HermitianMatrix<scalar_t>& C,
           int priority, int queue_index, Layout layout)
{
    using blas::conj;

    // CPU assumes column major
    // todo: relax this assumption, by allowing Tile_blas.hh::her2k()
    //       to take layout param
    // todo: optimize for the number of layout conversions,
    //       by watching 'layout' and 'C(i, j).layout()'
    assert(layout == Layout::ColMajor);

    scalar_t beta_ = beta;
    int err = 0;
    for (int64_t j = 0; j < C.nt(); ++j) {
        for (int64_t i = j; i < C.mt(); ++i) { // lower
            if (C.tileIsLocal(i, j)) {
                if (i == j) {
                    #pragma omp task shared(A, B, C, err) priority(priority)
                    {
                        try {
                            A.tileGetForReading(j, 0, LayoutConvert(layout));
                            B.tileGetForReading(j, 0, LayoutConvert(layout));
                            C.tileGetForWriting(j, j, LayoutConvert(layout));
                            her2k(alpha, A(j, 0),
                                         B(j, 0),
                                  beta,  C(j, j));
                            // todo: should tileRelease()?
                            A.tileTick(j, 0);
                            B.tileTick(j, 0);
                        }
                        catch (std::exception& e) {
                            err = __LINE__;
                        }
                    }
                }
                else {
                    #pragma omp task shared(A, B, C, err) priority(priority)
                    {
                        try {
                            A.tileGetForReading(i, 0, LayoutConvert(layout));
                            A.tileGetForReading(j, 0, LayoutConvert(layout));
                            B.tileGetForReading(i, 0, LayoutConvert(layout));
                            B.tileGetForReading(j, 0, LayoutConvert(layout));
                            C.tileGetForWriting(i, j, LayoutConvert(layout));
                            auto Aj0 = A(j, 0);
                            auto Bj0 = B(j, 0);
                            gemm(alpha, A(i, 0),
                                        conjTranspose(Bj0),
                                 beta_, C(i, j));
                            gemm(conj(alpha),   B(i, 0),
                                                conjTranspose(Aj0),
                                 scalar_t(1.0), C(i, j));
                            // todo: should tileRelease()?
                            A.tileTick(i, 0);
                            A.tileTick(j, 0);
                            B.tileTick(i, 0);
                            B.tileTick(j, 0);
                        }
                        catch (std::exception& e) {
                            err = __LINE__;
                        }
                    }
                }
            }
        }
    }

    #pragma omp taskwait

    if (err)
        throw std::exception();
}

//------------------------------------------------------------------------------
/// Hermitian rank-2k update of single block column (i.e., k = nb).
/// Host nested OpenMP implementation.
/// Assumes A is NoTrans or ConjTrans; C is Lower, NoTrans or Upper, ConjTrans.
/// @ingroup her2k_internal
///
template <typename scalar_t>
void her2k(internal::TargetType<Target::HostNest>,
           scalar_t alpha,                 Matrix<scalar_t>& A,
                                           Matrix<scalar_t>& B,
           blas::real_type<scalar_t> beta, HermitianMatrix<scalar_t>& C,
           int priority, int queue_index, Layout layout)
{
    using blas::conj;

    // CPU assumes column major
    // todo: relax this assumption, by allowing Tile_blas.hh::her2k()
    //       to take layout param
    // todo: optimize for the number of layout conversions,
    //       by watching 'layout' and 'C(i, j).layout()'
    assert(layout == Layout::ColMajor);

    scalar_t beta_ = beta;
    int err = 0;
    for (int64_t j = 0; j < C.nt(); ++j) {
        if (C.tileIsLocal(j, j)) {
            #pragma omp task shared(A, B, C, err)
            {
                try {
                    A.tileGetForReading(j, 0, LayoutConvert(layout));
                    B.tileGetForReading(j, 0, LayoutConvert(layout));
                    C.tileGetForWriting(j, j, LayoutConvert(layout));
                    her2k(alpha, A(j, 0),
                                 B(j, 0),
                          beta,  C(j, j));
                    // todo: should tileRelease()?
                    A.tileTick(j, 0);
                    B.tileTick(j, 0);
                }
                catch (std::exception& e) {
                    err = __LINE__;
                }
            }
        }
    }

    int64_t C_mt = C.mt();
    int64_t C_nt = C.nt();

    //  #pragma omp parallel for collapse(2) schedule(dynamic, 1) num_threads(...)
    #pragma omp parallel for collapse(2) schedule(dynamic, 1)
    for (int64_t j = 0; j < C_nt; ++j) {
        for (int64_t i = 0; i < C_mt; ++i) {  // full
            if (i >= j+1) {                     // strictly lower
                if (C.tileIsLocal(i, j)) {
                    try {
                        A.tileGetForReading(i, 0, LayoutConvert(layout));
                        B.tileGetForReading(j, 0, LayoutConvert(layout));
                        C.tileGetForWriting(i, j, LayoutConvert(layout));
                        auto Aj0 = A(j, 0);
                        auto Bj0 = B(j, 0);
                        gemm(alpha, A(i, 0),
                                    conjTranspose(Bj0),
                             beta_, C(i, j));
                        gemm(conj(alpha),   B(i, 0),
                                            conjTranspose(Aj0),
                             scalar_t(1.0), C(i, j));
                        // todo: should tileRelease()?
                        A.tileTick(i, 0);
                        A.tileTick(j, 0);
                        B.tileTick(i, 0);
                        B.tileTick(j, 0);
                    }
                    catch (std::exception& e) {
                        err = __LINE__;
                    }
                }
            }
        }
    }

    #pragma omp taskwait

    if (err)
        throw std::exception();
}

//------------------------------------------------------------------------------
/// Hermitian rank-2k update of single block column (i.e., k = nb).
/// Host batched implementation.
/// Assumes A is NoTrans or ConjTrans; C is Lower, NoTrans or Upper, ConjTrans.
/// @ingroup her2k_internal
///
template <typename scalar_t>
void her2k(internal::TargetType<Target::HostBatch>,
           scalar_t alpha,                 Matrix<scalar_t>& A,
                                           Matrix<scalar_t>& B,
           blas::real_type<scalar_t> beta, HermitianMatrix<scalar_t>& C,
           int priority, int queue_index, Layout layout)
{
    using blas::conj;

    // CPU assumes column major
    // todo: relax this assumption, by allowing Tile_blas.hh::her2k() to
    //       take layout param
    // todo: optimize for the number of layout conversions,
    //       by watching 'layout' and 'C(i, j).layout()'
    assert(layout == Layout::ColMajor);

    // diagonal tiles by her2k on host
    int err = 0;
    for (int64_t j = 0; j < C.nt(); ++j) {
        if (C.tileIsLocal(j, j)) {
            #pragma omp task shared(A, B, C, err)
            {
                try {
                    A.tileGetForReading(j, 0, LayoutConvert(layout));
                    B.tileGetForReading(j, 0, LayoutConvert(layout));
                    C.tileGetForWriting(j, j, LayoutConvert(layout));
                    her2k(alpha, A(j, 0),
                                 B(j, 0),
                          beta,  C(j, j));
                    // todo: should tileRelease()?
                    A.tileTick(j, 0);
                    B.tileTick(j, 0);
                }
                catch (std::exception& e) {
                    err = __LINE__;
                }
            }
        }
    }

    // load off-diagonal tiles to host, if not there
    // also count tiles
    int batch_count = 0;
    for (int64_t j = 0; j < C.nt(); ++j) {
        for (int64_t i = j+1; i < C.mt(); ++i) {  // strictly lower
            if (C.tileIsLocal(i, j)) {
                A.tileGetForReading(i, 0, LayoutConvert(layout));
                B.tileGetForReading(j, 0, LayoutConvert(layout));
                C.tileGetForWriting(i, j, LayoutConvert(layout));
                ++batch_count;
            }
        }
    }
    if (batch_count > 0) {
        // off-diagonal tiles by batch gemm on host
        Op opA = A.op();
        if (C.op() != Op::NoTrans) {
            if (A.op() == Op::NoTrans)
                opA = C.op();
            else if (A.op() == C.op() || C.is_real) {
                // A and C are both Trans or both ConjTrans;
                // Trans == ConjTrans if real
                opA = Op::NoTrans;
            }
            else
                throw std::exception();
            alpha = conj(alpha);
        }

        Op opB = (opA == Op::NoTrans ? Op::ConjTrans : Op::NoTrans);

        // all same
        std::vector<CBLAS_TRANSPOSE> opA_array(batch_count,
                                               cblas_trans_const(opA));
        // all same
        std::vector<CBLAS_TRANSPOSE> opB_array(batch_count,
                                               cblas_trans_const(opB));
        std::vector<int> m_array(batch_count);
        std::vector<int> n_array(batch_count);
        std::vector<int> k_array(batch_count);
        std::vector<scalar_t> alpha_array(batch_count, alpha);  // all same
        std::vector<scalar_t>  beta_array(batch_count,  beta);  // all same
        std::vector<const scalar_t*> ai_array(batch_count);
        std::vector<const scalar_t*> aj_array(batch_count);
        std::vector<const scalar_t*> bi_array(batch_count);
        std::vector<const scalar_t*> bj_array(batch_count);
        std::vector<scalar_t*> c_array(batch_count);
        std::vector<int> ldai_array(batch_count);
        std::vector<int> ldaj_array(batch_count);
        std::vector<int> ldbi_array(batch_count);
        std::vector<int> ldbj_array(batch_count);
        std::vector<int> ldc_array(batch_count);
        std::vector<int> group_size(batch_count, 1);  // all same

        int index = 0;
        for (int64_t j = 0; j < C.nt(); ++j) {
            for (int64_t i = j+1; i < C.mt(); ++i) {  // strictly lower
                if (C.tileIsLocal(i, j)) {
                    m_array[ index ] = C(i, j).mb();
                    n_array[ index ] = C(i, j).nb();
                    k_array[ index ] = A(i, 0).nb();  // should be all same

                    assert(A(i, 0).mb() == m_array[ index ]);
                    assert(A(j, 0).mb() == n_array[ index ]);
                    assert(A(j, 0).nb() == k_array[ index ]);

                    ai_array[ index ] = A(i, 0).data();
                    aj_array[ index ] = A(j, 0).data();
                    bi_array[ index ] = B(i, 0).data();
                    bj_array[ index ] = B(j, 0).data();
                    c_array[ index ] = C(i, j).data();

                    ldai_array[ index ] = A(i, 0).stride();
                    ldaj_array[ index ] = A(j, 0).stride();
                    ldbi_array[ index ] = B(i, 0).stride();
                    ldbj_array[ index ] = B(j, 0).stride();
                    ldc_array[ index ] = C(i, j).stride();

                    ++index;
                }
            }
        }

        if (C.op() != Op::NoTrans) {
            // swap A <=> B; swap m <=> n
            // alpha conjugated above
            swap(opA_array,  opB_array );
            swap(ai_array,   bj_array  );
            swap(aj_array,   bi_array  );
            swap(ldai_array, ldbj_array);
            swap(ldaj_array, ldbi_array);
            swap(m_array,    n_array   );
        }

        {
            trace::Block trace_block("cblas_gemm_batch");
            #ifdef SLATE_WITH_MKL
                // mkl_set_num_threads_local(...);
                cblas_gemm_batch(CblasColMajor,
                                 opA_array.data(), opB_array.data(),
                                 m_array.data(), n_array.data(), k_array.data(),
                                 alpha_array.data(),
                                 ai_array.data(), ldai_array.data(),
                                 bj_array.data(), ldbj_array.data(),
                                 beta_array.data(),
                                 c_array.data(), ldc_array.data(),
                                 batch_count, group_size.data());

                // ai => bi, bj => aj, conjugate alpha, set beta = 1
                if (is_complex<scalar_t>::value) {
                    std::fill(alpha_array.begin(),
                              alpha_array.end(), conj(alpha));
                }
                std::fill(beta_array.begin(), beta_array.end(), scalar_t(1.0));
                cblas_gemm_batch(CblasColMajor,
                                 opA_array.data(), opB_array.data(),
                                 m_array.data(), n_array.data(), k_array.data(),
                                 alpha_array.data(),
                                 bi_array.data(), ldbi_array.data(),
                                 aj_array.data(), ldaj_array.data(),
                                 beta_array.data(),
                                 c_array.data(), ldc_array.data(),
                                 batch_count, group_size.data());
                // mkl_set_num_threads_local(1);
            #else
                slate_not_implemented(
                    "slate::Target::HostBatch needs Intel MKL.");
            #endif
        }

        for (int64_t j = 0; j < C.nt(); ++j) {
            for (int64_t i = j+1; i < C.mt(); ++i) {  // strictly lower
                if (C.tileIsLocal(i, j)) {
                    // todo: should tileRelease()?
                    A.tileTick(i, 0);
                    A.tileTick(j, 0);
                    B.tileTick(i, 0);
                    B.tileTick(j, 0);
                }
            }
        }
    }

    #pragma omp taskwait

    if (err)
        throw std::exception();
}

//------------------------------------------------------------------------------
/// Hermitian rank-2k update of single block column (i.e., k = nb).
/// GPU device batched cuBLAS implementation.
/// Assumes A is NoTrans or ConjTrans; C is Lower, NoTrans or Upper, ConjTrans.
/// @ingroup her2k_internal
///
template <typename scalar_t>
void her2k(internal::TargetType<Target::Devices>,
           scalar_t alpha,                 Matrix<scalar_t>& A,
                                           Matrix<scalar_t>& B,
           blas::real_type<scalar_t> beta, HermitianMatrix<scalar_t>& C,
           int priority, int queue_index, Layout layout)
{
    using std::swap;
    using blas::conj;
    using ij_tuple = typename BaseMatrix<scalar_t>::ij_tuple;

    assert(C.num_devices() > 0);

    int err = 0;

    // if single tile, avoid creating tasks for all devices
    if (C.nt() == 1) {
        if (C.tileIsLocal(0, 0)) {
            #pragma omp task shared(A, B, C, err) priority(priority)
            {
                auto device = C.tileDevice(0, 0);
                A.tileGetForReading(0, 0, device, LayoutConvert(layout));
                B.tileGetForReading(0, 0, device, LayoutConvert(layout));
                C.tileGetForWriting(0, 0, device, LayoutConvert(layout));

                blas::Queue* queue = C.compute_queue(device, queue_index);

                auto A00 = A(0, 0, device);
                auto B00 = B(0, 0, device);
                auto C00 = C(0, 0, device);

                blas::her2k(
                    layout, C00.uploPhysical(), A00.op(),
                    C00.nb(), A00.nb(),
                    alpha, A00.data(), A00.stride(),
                           B00.data(), B00.stride(),
                    beta,  C00.data(), C00.stride(), *queue);

                queue->sync();

                A.tileRelease(0, 0, device);
                B.tileRelease(0, 0, device);
                A.tileTick(0, 0);
                A.tileTick(0, 0);
                B.tileTick(0, 0);
                B.tileTick(0, 0);
            }
        }
    }
    else {
        // off-diagonal tiles by batch gemm on device
        // diagonal tiles by BLAS++ her2k on device
        for (int device = 0; device < C.num_devices(); ++device) {
            #pragma omp task shared(A, B, C, err) priority(priority)
            {
                try {
                    // if op(C) is NoTrans, invert opA, opB if possible
                    Op opA = A.op();
                    if (C.op() != Op::NoTrans) {
                        if (A.op() == Op::NoTrans)
                            opA = C.op();
                        else if (A.op() == C.op() || C.is_real) {
                            // A and C are both Trans or both ConjTrans;
                            // Trans == ConjTrans if real
                            opA = Op::NoTrans;
                        }
                        else
                            throw std::exception();
                        alpha = conj(alpha);
                    }

                    Op opB = (opA == Op::NoTrans ? Op::ConjTrans : Op::NoTrans);

                    std::set<ij_tuple> A_tiles_gemm, B_tiles_gemm, C_tiles_gemm;
                    std::set<ij_tuple> A_tiles_her2k, B_tiles_her2k, C_tiles_her2k;
                    for (int64_t j = 0; j < C.nt(); ++j) {
                        for (int64_t i = j; i < C.mt(); ++i) {
                            if (C.tileIsLocal(i, j)
                                && device == C.tileDevice(i, j)) {
                                if (i == j) {
                                    A_tiles_her2k.insert({j, 0});
                                    B_tiles_her2k.insert({j, 0});
                                    C_tiles_her2k.insert({i, j});
                                }
                                else {
                                    A_tiles_gemm.insert({i, 0});
                                    A_tiles_gemm.insert({j, 0});
                                    B_tiles_gemm.insert({i, 0});
                                    B_tiles_gemm.insert({j, 0});
                                    C_tiles_gemm.insert({i, j});
                                }
                            }
                        }
                    }
                    #pragma omp task default(shared)
                    {
                        A.tileGetForReading(A_tiles_gemm, device, LayoutConvert(layout));
                    }
                    #pragma omp task default(shared)
                    {
                        B.tileGetForReading(B_tiles_gemm, device, LayoutConvert(layout));
                    }
                    #pragma omp task default(shared)
                    {
                        C.tileGetForWriting(C_tiles_gemm, device, LayoutConvert(layout));
                    }
                    #pragma omp taskwait

                    int64_t batch_size_gemm = C_tiles_gemm.size();

                    //----------------------------------------
                    // A * B^T
                    std::vector<scalar_t*> a_array_host_gemm_0(batch_size_gemm);
                    std::vector<scalar_t*> b_array_host_gemm_0(batch_size_gemm);
                    std::vector<scalar_t*> c_array_host_gemm_0(batch_size_gemm);
                    int64_t batch_count_gemm_0 = 0;
                    int64_t lda00 = 0;
                    int64_t ldb00 = 0;
                    int64_t ldc00 = 0;
                    int64_t mb00 = C.tileMb(0);
                    int64_t nb00 = C.tileNb(0);
                    int64_t kb   = A.tileNb(0);
                    for (int64_t j = 0; j < C.nt()-1; ++j) {
                        // strictly lower
                        for (int64_t i = j+1; i < C.mt()-1; ++i) {
                            if (C.tileIsLocal(i, j)) {
                                if (device == C.tileDevice(i, j)) {
                                    a_array_host_gemm_0[batch_count_gemm_0]
                                        = A(i, 0, device).data();
                                    b_array_host_gemm_0[batch_count_gemm_0]
                                        = B(j, 0, device).data();
                                    c_array_host_gemm_0[batch_count_gemm_0]
                                        = C(i, j, device).data();
                                    lda00 = A(i, 0, device).stride();
                                    ldb00 = B(j, 0, device).stride();
                                    ldc00 = C(i, j, device).stride();
                                    ++batch_count_gemm_0;
                                }
                            }
                        }
                    }

                    std::vector<scalar_t*> a_array_host_gemm_1(batch_size_gemm);
                    std::vector<scalar_t*> b_array_host_gemm_1(batch_size_gemm);
                    std::vector<scalar_t*> c_array_host_gemm_1(batch_size_gemm);
                    int64_t batch_count_gemm_1 = 0;
                    int64_t lda10 = 0;
                    int64_t ldb10 = 0;
                    int64_t ldc10 = 0;
                    int64_t mb10 = C.tileMb(C.mt()-1);
                    int64_t nb10 = C.tileNb(0);
                    // same kb as above
                    {
                        int64_t i = C.mt()-1;
                        for (int64_t j = 0; j < C.nt()-1; ++j) {
                            if (C.tileIsLocal(i, j)) {
                                if (device == C.tileDevice(i, j)) {
                                    a_array_host_gemm_1[batch_count_gemm_1]
                                        = A(i, 0, device).data();
                                    b_array_host_gemm_1[batch_count_gemm_1]
                                        = B(j, 0, device).data();
                                    c_array_host_gemm_1[batch_count_gemm_1]
                                        = C(i, j, device).data();
                                    lda10 = A(i, 0, device).stride();
                                    ldb10 = B(j, 0, device).stride();
                                    ldc10 = C(i, j, device).stride();
                                    ++batch_count_gemm_1;
                                }
                            }
                        }
                    }

                    if (C.op() != Op::NoTrans) {
                        // swap A <=> B; swap m <=> n
                        swap(opA, opB);
                        swap(a_array_host_gemm_0, b_array_host_gemm_0);
                        swap(a_array_host_gemm_1, b_array_host_gemm_1);
                        swap(lda00, ldb00);
                        swap(lda10, ldb10);
                        swap(mb00, nb00);
                        swap(mb10, nb10);
                    }

                    std::vector<Op> transA(1, opA);
                    std::vector<Op> transB(1, opB);
                    std::vector<int64_t> k(1, kb);

                    blas::Queue* queue = C.compute_queue(device, queue_index);

                    {
                        trace::Block trace_block("blas::batch::gemm");

                        std::vector<scalar_t> alpha_(1, alpha);
                        std::vector<scalar_t> beta_(1, scalar_t(beta));

                        if (batch_count_gemm_0 > 0) {
                            std::vector<int64_t> m(1, mb00);
                            std::vector<int64_t> n(1, nb00);
                            std::vector<int64_t> ldda(1, lda00);
                            std::vector<int64_t> lddb(1, ldb00);
                            std::vector<int64_t> lddc(1, ldc00);
                            std::vector<int64_t> info(batch_count_gemm_0);
                            blas::batch::gemm(
                                layout, transA, transB,
                                m, n, k,
                                alpha_, a_array_host_gemm_0, ldda,
                                        b_array_host_gemm_0, lddb,
                                beta_,  c_array_host_gemm_0, lddc,
                                batch_count_gemm_0, info, *queue);
                        }

                        if (batch_count_gemm_1 > 0) {
                            std::vector<int64_t> m(1, mb10);
                            std::vector<int64_t> n(1, nb10);
                            std::vector<int64_t> ldda(1, lda10);
                            std::vector<int64_t> lddb(1, ldb10);
                            std::vector<int64_t> lddc(1, ldc10);
                            std::vector<int64_t> info(batch_count_gemm_1);
                            blas::batch::gemm(
                                layout, transA, transB,
                                m, n, k,
                                alpha_, a_array_host_gemm_1, ldda,
                                        b_array_host_gemm_1, lddb,
                                beta_,  c_array_host_gemm_1, lddc,
                                batch_count_gemm_1, info, *queue);
                        }
                    }

                    //----------------------------------------
                    // B * A^T
                    // ai => bi, bj => aj, set beta = 1
                    batch_count_gemm_0 = 0;
                    for (int64_t j = 0; j < C.nt()-1; ++j) {
                        // strictly lower
                        for (int64_t i = j+1; i < C.mt()-1; ++i) {
                            if (C.tileIsLocal(i, j)) {
                                if (device == C.tileDevice(i, j)) {
                                    a_array_host_gemm_0[batch_count_gemm_0]
                                        = A(j, 0, device).data();
                                    b_array_host_gemm_0[batch_count_gemm_0]
                                        = B(i, 0, device).data();
                                    lda00 = A(j, 0, device).stride();
                                    ldb00 = B(i, 0, device).stride();
                                    ++batch_count_gemm_0;
                                }
                            }
                        }
                    }

                    batch_count_gemm_1 = 0;
                    {
                        int i = C.mt()-1;
                        for (int64_t j = 0; j < C.nt()-1; ++j) {
                            if (C.tileIsLocal(i, j)) {
                                if (device == C.tileDevice(i, j)) {
                                    a_array_host_gemm_1[batch_count_gemm_1]
                                        = A(j, 0, device).data();
                                    b_array_host_gemm_1[batch_count_gemm_1]
                                        = B(i, 0, device).data();
                                    lda10 = A(j, 0, device).stride();
                                    ldb10 = B(i, 0, device).stride();
                                    ++batch_count_gemm_1;
                                }
                            }
                        }
                    }

                    if (C.op() != Op::NoTrans) {
                        // swap A <=> B; swap m <=> n
                        //swap(opA, opB);  // already done above
                        swap(a_array_host_gemm_0, b_array_host_gemm_0);
                        swap(a_array_host_gemm_1, b_array_host_gemm_1);
                        swap(lda00, ldb00);
                        swap(lda10, ldb10);
                        //swap(mb00, nb00);  // already done above
                        //swap(mb10, nb10);  // already done above
                    }

                    {
                        trace::Block trace_block("blas::batch::gemm");

                        std::vector<scalar_t> alpha_(1, conj(alpha));
                        std::vector<scalar_t> beta_(1, scalar_t(1));

                        if (batch_count_gemm_0 > 0) {
                            std::vector<int64_t> m(1, mb00);
                            std::vector<int64_t> n(1, nb00);
                            std::vector<int64_t> ldda(1, lda00);
                            std::vector<int64_t> lddb(1, ldb00);
                            std::vector<int64_t> lddc(1, ldc00);
                            std::vector<int64_t> info(batch_count_gemm_0);
                            blas::batch::gemm(
                                layout, transA, transB,
                                m, n, k,
                                alpha_, b_array_host_gemm_0, lddb,
                                        a_array_host_gemm_0, ldda,
                                beta_,  c_array_host_gemm_0, lddc,
                                batch_count_gemm_0, info, *queue);
                        }

                        if (batch_count_gemm_1 > 0) {
                            std::vector<int64_t> m(1, mb10);
                            std::vector<int64_t> n(1, nb10);
                            std::vector<int64_t> ldda(1, lda10);
                            std::vector<int64_t> lddb(1, ldb10);
                            std::vector<int64_t> lddc(1, ldc10);
                            std::vector<int64_t> info(batch_count_gemm_1);
                            blas::batch::gemm(
                                layout, transA, transB,
                                m, n, k,
                                alpha_, b_array_host_gemm_1, lddb,
                                        a_array_host_gemm_1, ldda,
                                beta_,  c_array_host_gemm_1, lddc,
                                batch_count_gemm_1, info, *queue);
                        }
                    }

                    #pragma omp task default(shared)
                    {
                        A.tileGetForReading(A_tiles_her2k, device, LayoutConvert(layout));
                    }
                    #pragma omp task default(shared)
                    {
                        B.tileGetForReading(B_tiles_her2k, device, LayoutConvert(layout));
                    }
                    #pragma omp task default(shared)
                    {
                        C.tileGetForWriting(C_tiles_her2k, device, LayoutConvert(layout));
                    }
                    #pragma omp taskwait

                    int64_t batch_size_her2k = C_tiles_her2k.size();

                    std::vector<scalar_t*> a_array_host_her2k_0(batch_size_her2k);
                    std::vector<scalar_t*> b_array_host_her2k_0(batch_size_her2k);
                    std::vector<scalar_t*> c_array_host_her2k_0(batch_size_her2k);

                    int64_t batch_count_her2k_0 = 0;
                    int64_t lda_her2k_0 = 0;
                    int64_t ldb_her2k_0 = 0;
                    int64_t ldc_her2k_0 = 0;

                    int64_t nb_her2k_0 = C.tileNb(0);

                    for (int64_t j = 0; j < C.nt()-1; ++j) {
                        for (int64_t i = j; i < C.mt()-1; ++i) {
                            if (C.tileIsLocal(i, j)) {
                                if (device == C.tileDevice(i, j)) {
                                    if (i == j) {
                                        a_array_host_her2k_0[batch_count_her2k_0]
                                            = A(j, 0, device).data();
                                        b_array_host_her2k_0[batch_count_her2k_0]
                                            = B(j, 0, device).data();
                                        c_array_host_her2k_0[batch_count_her2k_0]
                                            = C(j, j, device).data();
                                        lda_her2k_0 = A(j, 0, device).stride();
                                        ldb_her2k_0 = B(j, 0, device).stride();
                                        ldc_her2k_0 = C(j, j, device).stride();
                                        ++batch_count_her2k_0;
                                    }
                                }
                            }
                        }
                    }

                    std::vector<scalar_t*> a_array_host_her2k_1(batch_size_her2k);
                    std::vector<scalar_t*> b_array_host_her2k_1(batch_size_her2k);
                    std::vector<scalar_t*> c_array_host_her2k_1(batch_size_her2k);

                    int64_t batch_count_her2k_1 = 0;
                    int64_t lda_her2k_1 = 0;
                    int64_t ldb_her2k_1 = 0;
                    int64_t ldc_her2k_1 = 0;

                    int64_t nb_her2k_1 = C.tileNb(C.nt()-1);

                    {
                        int i = C.mt()-1;
                        int j = C.nt()-1;
                        if (C.tileIsLocal(i, j)) {
                            if (device == C.tileDevice(i, j)) {
                                a_array_host_her2k_1[batch_count_her2k_1]
                                    = A(j, 0, device).data();
                                b_array_host_her2k_1[batch_count_her2k_1]
                                    = B(j, 0, device).data();
                                c_array_host_her2k_1[batch_count_her2k_1]
                                    = C(j, j, device).data();
                                lda_her2k_1 = A(j, 0, device).stride();
                                ldb_her2k_1 = B(j, 0, device).stride();
                                ldc_her2k_1 = C(j, j, device).stride();
                                ++batch_count_her2k_1;
                            }
                        }
                    }

                    {
                        trace::Block trace_block("blas::batch::her2k");

                        std::vector<Uplo> uplo(1, C.uploPhysical());
                        std::vector<scalar_t> alpha_(1, alpha);
                        std::vector<blas::real_type<scalar_t>> beta_(1, beta);

                        if (batch_count_her2k_0 > 0) {
                            std::vector<int64_t> n(1, nb_her2k_0);
                            std::vector<int64_t> ldda(1, lda_her2k_0);
                            std::vector<int64_t> lddb(1, ldb_her2k_0);
                            std::vector<int64_t> lddc(1, ldc_her2k_0);
                            std::vector<int64_t> info(batch_count_her2k_0);
                            blas::batch::her2k(
                                layout, uplo, transA,
                                n, k,
                                alpha_, a_array_host_her2k_0, ldda,
                                        b_array_host_her2k_0, lddb,
                                beta_,  c_array_host_her2k_0, lddc,
                                batch_count_her2k_0, info, *queue);
                        }

                        if (batch_count_her2k_1 > 0) {
                            std::vector<int64_t> n(1, nb_her2k_1);
                            std::vector<int64_t> ldda(1, lda_her2k_1);
                            std::vector<int64_t> lddb(1, ldb_her2k_1);
                            std::vector<int64_t> lddc(1, ldc_her2k_1);
                            std::vector<int64_t> info(batch_count_her2k_1);
                            blas::batch::her2k(
                                layout, uplo, transA,
                                n, k,
                                alpha_, a_array_host_her2k_1, ldda,
                                        b_array_host_her2k_1, lddb,
                                beta_,  c_array_host_her2k_1, lddc,
                                batch_count_her2k_1, info, *queue);
                        }
                    }

                    queue->sync();

                    for (int64_t j = 0; j < C.nt(); ++j) {
                        for (int64_t i = j; i < C.mt(); ++i) {
                            if (C.tileIsLocal(i, j)) {
                                if (device == C.tileDevice(i, j)) {
                                    // erase tmp local and remote device tiles;
                                    A.tileRelease(i, 0, device);
                                    A.tileRelease(j, 0, device);
                                    B.tileRelease(i, 0, device);
                                    B.tileRelease(j, 0, device);
                                    // decrement life for remote tiles
                                    A.tileTick(i, 0);
                                    A.tileTick(j, 0);
                                    B.tileTick(i, 0);
                                    B.tileTick(j, 0);
                                }
                            }
                        }
                    }
                }
                catch (std::exception& e) {
                    err = __LINE__;
                }
            }
        }
    }

    #pragma omp taskwait

    if (err)
        throw std::exception();
}

//------------------------------------------------------------------------------
// Explicit instantiations.
// ----------------------------------------
template
void her2k<Target::HostTask, float>(
    float alpha, Matrix<float>&& A,
                 Matrix<float>&& B,
    float beta,  HermitianMatrix<float>&& C,
    int priority, int queue_index, Layout layout);

template
void her2k<Target::HostNest, float>(
    float alpha, Matrix<float>&& A,
                 Matrix<float>&& B,
    float beta,  HermitianMatrix<float>&& C,
    int priority, int queue_index, Layout layout);

template
void her2k<Target::HostBatch, float>(
    float alpha, Matrix<float>&& A,
                 Matrix<float>&& B,
    float beta,  HermitianMatrix<float>&& C,
    int priority, int queue_index, Layout layout);

template
void her2k<Target::Devices, float>(
    float alpha, Matrix<float>&& A,
                 Matrix<float>&& B,
    float beta,  HermitianMatrix<float>&& C,
    int priority, int queue_index, Layout layout);

// ----------------------------------------
template
void her2k<Target::HostTask, double>(
    double alpha, Matrix<double>&& A,
                  Matrix<double>&& B,
    double beta,  HermitianMatrix<double>&& C,
    int priority, int queue_index, Layout layout);

template
void her2k<Target::HostNest, double>(
    double alpha, Matrix<double>&& A,
                  Matrix<double>&& B,
    double beta,  HermitianMatrix<double>&& C,
    int priority, int queue_index, Layout layout);

template
void her2k<Target::HostBatch, double>(
    double alpha, Matrix<double>&& A,
                  Matrix<double>&& B,
    double beta,  HermitianMatrix<double>&& C,
    int priority, int queue_index, Layout layout);

template
void her2k<Target::Devices, double>(
    double alpha, Matrix<double>&& A,
                  Matrix<double>&& B,
    double beta,  HermitianMatrix<double>&& C,
    int priority, int queue_index, Layout layout);

// ----------------------------------------
template
void her2k< Target::HostTask, std::complex<float> >(
    std::complex<float> alpha, Matrix< std::complex<float> >&& A,
                               Matrix< std::complex<float> >&& B,
    float beta,                HermitianMatrix< std::complex<float> >&& C,
    int priority, int queue_index, Layout layout);

template
void her2k< Target::HostNest, std::complex<float> >(
    std::complex<float> alpha, Matrix< std::complex<float> >&& A,
                               Matrix< std::complex<float> >&& B,
    float beta,                HermitianMatrix< std::complex<float> >&& C,
    int priority, int queue_index, Layout layout);

template
void her2k< Target::HostBatch, std::complex<float> >(
    std::complex<float> alpha, Matrix< std::complex<float> >&& A,
                               Matrix< std::complex<float> >&& B,
    float beta,                HermitianMatrix< std::complex<float> >&& C,
    int priority, int queue_index, Layout layout);

template
void her2k< Target::Devices, std::complex<float> >(
    std::complex<float> alpha, Matrix< std::complex<float> >&& A,
                               Matrix< std::complex<float> >&& B,
    float beta,                HermitianMatrix< std::complex<float> >&& C,
    int priority, int queue_index, Layout layout);

// ----------------------------------------
template
void her2k< Target::HostTask, std::complex<double> >(
    std::complex<double> alpha, Matrix< std::complex<double> >&& A,
                                Matrix< std::complex<double> >&& B,
    double beta,                HermitianMatrix< std::complex<double> >&& C,
    int priority, int queue_index, Layout layout);

template
void her2k< Target::HostNest, std::complex<double> >(
    std::complex<double> alpha, Matrix< std::complex<double> >&& A,
                                Matrix< std::complex<double> >&& B,
    double beta,                HermitianMatrix< std::complex<double> >&& C,
    int priority, int queue_index, Layout layout);

template
void her2k< Target::HostBatch, std::complex<double> >(
    std::complex<double> alpha, Matrix< std::complex<double> >&& A,
                                Matrix< std::complex<double> >&& B,
    double beta,                HermitianMatrix< std::complex<double> >&& C,
    int priority, int queue_index, Layout layout);

template
void her2k< Target::Devices, std::complex<double> >(
    std::complex<double> alpha, Matrix< std::complex<double> >&& A,
                                Matrix< std::complex<double> >&& B,
    double beta,                HermitianMatrix< std::complex<double> >&& C,
    int priority, int queue_index, Layout layout);

} // namespace internal
} // namespace slate
