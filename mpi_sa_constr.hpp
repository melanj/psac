/**
 * @file    mpi_sa_constr.hpp
 * @author  Patrick Flick <patrick.flick@gmail.com>
 * @brief   Implements methods for parallel, distributed (MPI) suffix array
 *          construction.
 *
 * Copyright (c) 2014 Georgia Institute of Technology. All Rights Reserved.
 *
 * TODO add Licence
 */

#ifndef MPI_SA_CONSTR_HPP
#define MPI_SA_CONSTR_HPP

/*
 * Idea:
 * - bucket by string index ( ~BISA?)
 *      o initially by having bucket-number = k-mer
 *      o 1-mer would be BISA[i] = char2int(S[i])
 *      // o initially by sorting tuples (string-index, k-mer number) [32-64 bits each]
 *
 * - BISA: 2^k -> 2^{k+1}
 *      o Read 2^k ahead (comm with one processor) to get next bucket no (BISA[i+2^k])
 *      o create tuple (BISA[i], BISA[i+2^k], i)
 *      o sort all tuples by first two entries to get next SA
 *      o prefix-scan to assign new bucket numbers (bn, i)
 *      o bucket-"sort" by `i` to get BISA_{k+1} (one all2all)
 *
 * - SA based
 *      o (assume we have suffix array after sorting first k characters [k-mers])
 *          - req: bucket sort (histogram and shuffle)
 *      o create tuples for each SA[i]:  (j=SA[i], i, b)
 *      o send to processor for j -> current ISA
 *      o copy and shift by 2^k
 *      o gives new combined bucket number tuple, prefix scan for new bucket numbers
 *      o inverse (j,i) by sending to processor for i: gives sorting by first
 *        bucket index
 *      o sort by second bucket number (which is potentially only a local sort!)
 *      o ENH: adjust bucket boundaries if close, so that it becomes local sort
 *
 *
 * - LCP while M&M:
 *      o TODO: this doesn't yet make sense and/or work properly :-/
 *      o TODO: see ya tmrw dude :)
 *      o when combining two buckets into tuple, we add the other buckets (+2^k)
 *        current LCP value to the tuple, after sorting into SA order, we can
 *        determine the new LCP value by adding of still in same bucket
 *        with neighbor
 *      o when B[i]
 *
 *
 *      ---------------------------
 * SA:  | | | ... |j|   ...   | | |
 *      ---------------------------
 *                 ^
 *                 i
 *
 *      ---------------------------
 * ISA: | |  ...      |i| ... | | |
 *      ---------------------------
 *                     ^
 *                     j
 *
 */

#include <mpi.h>

#include <iostream>
#include <vector>
#include <limits>
#include <type_traits>
#include <algorithm>

#include <assert.h>

#include "algos.hpp"
#include "parallel_utils.hpp"
#include "mpi_utils.hpp"
#include "mpi_samplesort.hpp"

#define PSAC_TAG_EDGE_KMER 1
#define PSAC_TAG_SHIFT 2
#define PSAC_TAG_EDGE_B 3

#include "timer.hpp"


/*********************************************************************
 *                 macros for debugging with distributed vectors     *
 *********************************************************************/
// whether to gather all vectors to rank 0 prior to debug output
// set both to `0` to disable debug output
#define DO_DEBUG_GLOBAL_VEC 0
#define DO_DEBUG_LOCAL_VEC 0

// print vector helpers
#define DEBUG_PRINT_STAGE(stage) \
    std::cerr << "========  " << stage << "  =======" << std::endl;\

#define DEBUG_PRINT_LOCAL_VEC(vec) \
    fprintf(stderr, "%-10s: ",#vec);print_vec(vec);

#define DEBUG_PRINT_GLOBAL_VEC(vec) \
    {\
        std::vector<index_t> gl_##vec = gather_vectors(vec,comm);\
        if (rank == 0) {\
            fprintf(stderr, "GLOBAL %-10s: ",#vec);print_vec(gl_##vec);\
    }}

// defining common macros for stage vector output
#if DO_DEBUG_GLOBAL_VEC
#define DEBUG_STAGE_VEC(stage, vec)\
        DEBUG_PRINT_STAGE(stage)\
        DEBUG_PRINT_GLOBAL_VEC(vec)
#define DEBUG_STAGE_VEC2(stage, vec, vec2)\
        DEBUG_PRINT_STAGE(stage)\
        DEBUG_PRINT_GLOBAL_VEC(vec)\
        DEBUG_PRINT_GLOBAL_VEC(vec2)
#define DEBUG_STAGE_VEC3(stage, vec, vec2, vec3)\
        DEBUG_PRINT_STAGE(stage)\
        DEBUG_PRINT_GLOBAL_VEC(vec)\
        DEBUG_PRINT_GLOBAL_VEC(vec2)\
        DEBUG_PRINT_GLOBAL_VEC(vec3)
#elif DO_DEBUG_LOCAL_VEC
#define DEBUG_STAGE_VEC(stage, vec)\
        DEBUG_PRINT_STAGE(stage)\
        DEBUG_PRINT_LOCAL_VEC(vec)
#define DEBUG_STAGE_VEC2(stage, vec, vec2)\
        DEBUG_PRINT_STAGE(stage)\
        DEBUG_PRINT_LOCAL_VEC(vec)\
        DEBUG_PRINT_LOCAL_VEC(vec2)
#define DEBUG_STAGE_VEC3(stage, vec, vec2, vec3)\
        DEBUG_PRINT_STAGE(stage)\
        DEBUG_PRINT_LOCAL_VEC(vec)\
        DEBUG_PRINT_LOCAL_VEC(vec2)\
        DEBUG_PRINT_LOCAL_VEC(vec3)
#else
#define DEBUG_STAGE_VEC(stage, vec)
#define DEBUG_STAGE_VEC2(stage, vec, vec2)
#define DEBUG_STAGE_VEC3(stage, vec, vec2, vec3)
#endif


/*********************************************************************
 *              Macros for timing sections in the code               *
 *********************************************************************/

#define SAC_ENABLE_TIMER 1
#if SAC_ENABLE_TIMER
#define SAC_TIMER_START() TIMER_START()
#define SAC_TIMER_END_SECTION(str) TIMER_END_SECTION(str)
#define SAC_TIMER_LOOP_START() TIMER_LOOP_START()
#define SAC_TIMER_END_LOOP_SECTION(iter, str) TIMER_END_LOOP_SECTION(iter, str)
#else
#define SAC_TIMER_START()
#define SAC_TIMER_END_SECTION(str)
#define SAC_TIMER_LOOP_START()
#define SAC_TIMER_END_LOOP_SECTION(iter, str)
#endif


template<typename T>
void print_vec(const std::vector<T>& vec)
{
    auto it = vec.begin();
    while (it != vec.end())
    {
        std::cerr << *(it++) << " ";
    }
    std::cerr << std::endl;
}

template<typename Iterator, typename T = typename std::iterator_traits<Iterator>::value_type>
T global_max_element(Iterator begin, Iterator end, MPI_Comm comm)
{
    // mpi datatype
    MPI_Datatype mpi_dt = get_mpi_dt<T>();
    // get local max
    T max = *std::max_element(begin, end);

    // get global max
    T gl_max;
    MPI_Allreduce(&max, &gl_max, 1, mpi_dt, MPI_MAX, comm);

    return gl_max;
}

template<typename T, typename Iterator>
std::vector<T> get_histogram(Iterator begin, Iterator end, std::size_t size = 0)
{
    if (size == 0)
        size = static_cast<std::size_t>(*std::max_element(begin, end)) + 1;
    std::vector<T> hist(size);

    while (begin != end)
    {
        ++hist[static_cast<std::size_t>(*(begin++))];
    }

    return hist;
}

// TODO: global character histogram + required bits per character + compression
template <typename index_t>
std::vector<index_t> alphabet_histogram(const std::string& local_str, MPI_Comm comm)
{
    // get local histogram of alphabet characters
    std::vector<index_t> hist = get_histogram<index_t>(local_str.begin(), local_str.end(), 256);

    std::vector<index_t> out_hist(256);

    // global all reduce to get global histogram
    MPI_Datatype mpi_dt = get_mpi_dt<index_t>();
    MPI_Allreduce(&hist[0], &out_hist[0], 256, mpi_dt, MPI_SUM, comm);

    return out_hist;
}

template <typename index_t>
std::vector<char> alphabet_mapping_tbl(const std::vector<index_t>& global_hist)
{
    std::vector<char> mapping(256, 0);

    char next = static_cast<char>(1);
    for (std::size_t c = 0; c < 256; ++c)
    {
        if (global_hist[c] != 0)
        {
            mapping[c] = next;
            ++next;
        }
    }
    return mapping;
}

template <typename index_t>
unsigned int alphabet_unique_chars(const std::vector<index_t>& global_hist)
{
    unsigned int unique_count = 0;
    for (std::size_t c = 0; c < 256; ++c)
    {
        if (global_hist[c] != 0)
        {
            ++unique_count;
        }
    }
    return unique_count;
}

unsigned int ceillog2(unsigned int x)
{
    unsigned int log_floor = 0;
    unsigned int n = x;
    for (;n != 0; n >>= 1)
    {
        ++log_floor;
    }
    --log_floor;
    // add one if not power of 2
    return log_floor + (((x&(x-1)) != 0) ? 1 : 0);
}

unsigned int alphabet_bits_per_char(unsigned int sigma)
{
    // since we have to account for the `0` character, we use ceil(log(unique_chars + 1))
    return ceillog2(sigma+1);
}

template<typename word_t>
unsigned int alphabet_chars_per_word(unsigned int bits_per_char)
{
    // using bit concatenation, NOT multiplication by base
    // TODO: try multiplication by base

    unsigned int bits_per_word = sizeof(word_t)*8;
    // TODO: this is currently a "work-around": if the type is signed, we
    //       can't use the msb, thus we need to subtract one
    if (std::is_signed<word_t>::value)
        --bits_per_word;
    return bits_per_word/bits_per_char;
}

template <typename index_t>
unsigned int initial_bucketing(const std::size_t n, const std::basic_string<char>& local_str, std::vector<index_t>& local_B, MPI_Comm comm)
{
    // get local size
    std::size_t local_size = local_str.size();

    // get communication parameters
    int rank, p;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &p);

    std::size_t min_local_size = block_partition_local_size(n, p, p-1);

    // get global alphabet histogram
    std::vector<index_t> alphabet_hist = alphabet_histogram<index_t>(local_str, comm);
    // get mapping table and alphabet sizes
    std::vector<char> alphabet_mapping = alphabet_mapping_tbl(alphabet_hist);
    unsigned int sigma = alphabet_unique_chars(alphabet_hist);
    // bits per character: set l=ceil(log(sigma))
    unsigned int l = alphabet_bits_per_char(sigma);
    // number of characters per word => the `k` in `k-mer`
    unsigned int k = alphabet_chars_per_word<index_t>(l);
    // if the input is too small for `k`, choose a smaller `k`
    if (k > min_local_size)
    {
        k = min_local_size;
    }

    if (rank == 0)
        std::cerr << "Detecting sigma=" << sigma << " => l=" << l << ", k=" << k << std::endl;

    // get k-mer mask
    index_t kmer_mask = ((static_cast<index_t>(1) << (l*k)) - static_cast<index_t>(1));
    if (kmer_mask == 0)
        kmer_mask = ~static_cast<index_t>(0);

    // sliding window k-mer (for prototype only using ASCII alphabet)

    // fill first k-mer (until k-1 positions) and send to left processor
    // filling k-mer with first character = MSBs for lexicographical ordering
    auto str_it = local_str.begin();
    index_t kmer = 0;
    for (unsigned int i = 0; i < k-1; ++i)
    {
        kmer <<= l;
        kmer |= alphabet_mapping[static_cast<index_t>(*str_it)];
        ++str_it;
    }


    // send this to left processor, start async receive from right processor
    // for last in seq
    // start receiving for end
    index_t last_kmer = 0;
    MPI_Request recv_req;
    MPI_Datatype mpi_dt = get_mpi_dt<index_t>();
    if (rank < p-1) // if not last processor
    {
        MPI_Irecv(&last_kmer, 1, mpi_dt, rank+1, PSAC_TAG_EDGE_KMER,
                  comm, &recv_req);
    }
    if (rank > 0) // if not first processor
    {
        // TODO: [ENH] use async send as well and start with the computation
        //             immediately
        MPI_Send(&kmer, 1, mpi_dt, rank-1, PSAC_TAG_EDGE_KMER, comm);
    }


    // init output
    if (local_B.size() != local_size)
        local_B.resize(local_size);
    auto buk_it = local_B.begin();
    // continue to create all k-mers and add into histogram count
    while (str_it != local_str.end())
    {
        // get next kmer
        kmer <<= l;
        kmer |= alphabet_mapping[static_cast<index_t>(*str_it)];
        kmer &= kmer_mask;
        // add to bucket number array
        *buk_it = kmer;
        // increase iterators
        ++str_it;
        ++buk_it;
    }

    // finish the receive to get the last k-1 k-kmers with string data from the
    // processor to the right
    if (rank < p-1) // if not last processor
    {
        // wait for the async receive to finish
        MPI_Wait(&recv_req, MPI_STATUS_IGNORE);
    }
    else
    {
        // in this case the last k-mers contains shifting `$` signs
        // we assume this to be the `\0` value
        // TODO: how can we solve this efficiently when using on 4 character
        // DNA strings? (using a 5 character alphabet might be unnessesary
        // overhead)
    }


    // construct last (k-1) k-mers
    for (unsigned int i = 0; i < k-1; ++i)
    {
        kmer <<= l;
        kmer |= (last_kmer >> (l*(k-i-2)));
        kmer &= kmer_mask;

        // add to bucket number array
        *buk_it = kmer;
        ++buk_it;
    }

    // return the number of characters which are part of each bucket number
    // (i.e., k-mer)
    return k;
}


// assumed sorted order (globally) by tuple (B1[i], B2[i])
// this reassigns new, unique bucket numbers in {0,...,n-1} globally
template <typename index_t>
std::size_t rebucket(std::size_t n, std::vector<index_t>& local_B1, std::vector<index_t>& local_B2, MPI_Comm comm, bool count_unfinished)
{
    // assert inputs are of equal size
    assert(local_B1.size() == local_B2.size() && local_B1.size() > 0);
    std::size_t local_size = local_B1.size();

    // init result
    std::size_t result = 0;

    // get communication parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);

    /*
     * send right most element to one processor to the right
     * so that that processor can determine whether the same bucket continues
     * or a new bucket starts with it's first element
     */
    MPI_Request recv_req;
    MPI_Datatype mpi_dt = get_mpi_dt<index_t>();
    index_t prevRight[2];
    if (rank > 0) // if not last processor
    {
        MPI_Irecv(prevRight, 2, mpi_dt, rank-1, PSAC_TAG_EDGE_KMER,
                  comm, &recv_req);
    }
    if (rank < p-1) // if not first processor
    {
        // send my most right element to the right
        index_t myRight[2] = {local_B1.back(), local_B2.back()};
        MPI_Send(myRight, 2, mpi_dt, rank+1, PSAC_TAG_EDGE_KMER, comm);
    }
    if (rank > 0)
    {
        // wait for the async receive to finish
        MPI_Wait(&recv_req, MPI_STATUS_IGNORE);
    }

    // get my global starting index
    std::size_t prefix = block_partition_excl_prefix_size(n, p, rank);

    /*
     * assign local zero or one, depending on whether the bucket is the same
     * as the previous one
     */
    bool firstDiff = false;
    if (rank == 0)
    {
        firstDiff = true;
    }
    else if (prevRight[0] != local_B1[0] || prevRight[1] != local_B2[0])
    {
        firstDiff = true;
    }

    // set local_B1 to `1` if previous entry is different:
    // i.e., mark start of buckets
    bool nextDiff = firstDiff;
    for (std::size_t i = 0; i+1 < local_B1.size(); ++i)
    {
        bool setOne = nextDiff;
        nextDiff = (local_B1[i] != local_B1[i+1] || local_B2[i] != local_B2[i+1]);
        local_B1[i] = setOne ? prefix+i : 0;
    }

    local_B1.back() = nextDiff ? prefix+local_size-1 : 0;

    if (count_unfinished)
    {
        // mark 1->0 transitions with 1, if i am the zero and previous is 1
        // (i.e. identical)
        // (i.e. `i` is the second equal element in a bucket)
        // which means counting unfinished buckets, then allreduce
        index_t local_unfinished_buckets = firstDiff ? 0 : 1;
        if (rank == 0)
            local_unfinished_buckets = 0;
        for (std::size_t i = 1; i < local_B1.size(); ++i)
        {
            if(local_B1[i-1] > 0 && local_B1[i] == 0)
                ++local_unfinished_buckets;
        }

        MPI_Allreduce(&local_unfinished_buckets, &result, 1,
                      mpi_dt, MPI_SUM, comm);
    }


    /*
     * Global prefix MAX:
     *  - such that for every item we have it's bucket number, where the
     *    bucket number is equal to the first index in the bucket
     *    this way buckets who are finished, will never receive a new
     *    number.
     */
    // 1.) find the max in the local sequence. since the max is the last index
    //     of a bucket, this should be somewhere at the end -> start scanning
    //     from the end
    auto rev_it = local_B1.rbegin();
    std::size_t local_max = 0;
    while (rev_it != local_B1.rend() && (local_max = *rev_it) == 0)
        ++rev_it;

    // 2.) distributed scan with max() to get starting max for each sequence
    std::size_t pre_max;
    MPI_Datatype mpi_size_t = get_mpi_dt<std::size_t>();
    MPI_Exscan(&local_max, &pre_max, 1, mpi_size_t, MPI_MAX, comm);
    if (rank == 0){
        std::vector<index_t> pre_maxs(p);
        MPI_Gather(&pre_max, 1, mpi_dt, &pre_maxs[0], 1, mpi_dt, 0, comm);
        std::cerr << "~~~~~~PRE-MAX: "; print_range(pre_maxs.begin(), pre_maxs.end());
        MPI_Gather(&local_max, 1, mpi_dt, &pre_maxs[0], 1, mpi_dt, 0, comm);
        std::cerr << "~~~~~~LOC-MAX: "; print_range(pre_maxs.begin(), pre_maxs.end());
        pre_max = 0;
    }
    else
    {
        MPI_Gather(&pre_max, 1, mpi_dt, NULL, 1, mpi_dt, 0, comm);
        MPI_Gather(&local_max, 1, mpi_dt, NULL, 1, mpi_dt, 0, comm);
    }

    // 3.) linear scan and assign bucket numbers
    for (std::size_t i = 0; i < local_B1.size(); ++i)
    {
        if (local_B1[i] == 0)
            local_B1[i] = pre_max;
        else
            pre_max = local_B1[i];
        assert(local_B1[i] <= i+prefix);
        // first element of bucket has id of it's own global index:
        assert(i == 0 || (local_B1[i-1] ==  local_B1[i] || local_B1[i] == i+prefix));
    }

    return result;
}

template <typename index_t>
void reorder_sa_to_isa(std::size_t n, std::vector<index_t>& local_SA, std::vector<index_t>& local_B, MPI_Comm comm)
{
    assert(local_SA.size() == local_B.size());
    // get processor id
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);
    MPI_Datatype mpi_dt = get_mpi_dt<index_t>();

    SAC_TIMER_START();
    // 1.) local bucketing for each processor
    //
    // counting the number of elements for each processor
    std::vector<int> send_counts(p, 0);
    for (index_t sa : local_SA)
    {
        int target_p = block_partition_target_processor(n, p, static_cast<std::size_t>(sa));
        assert(0 <= target_p && target_p < p);
        ++send_counts[target_p];
    }
    std::vector<int> send_displs = get_displacements(send_counts);
    std::vector<index_t> send_SA(local_SA.size());
    std::vector<index_t> send_B(local_B.size());
    // Reorder the SA and B arrays into buckets, one for each target processor.
    // The target processor is given by the value in the SA.
    for (std::size_t i = 0; i < local_SA.size(); ++i)
    {
        int target_p = block_partition_target_processor(n, p, static_cast<std::size_t>(local_SA[i]));
        assert(target_p < p && target_p >= 0);
        std::size_t out_idx = send_displs[target_p]++;
        assert(out_idx < local_SA.size());
        send_SA[out_idx] = local_SA[i];
        send_B[out_idx] = local_B[i];
    }
    SAC_TIMER_END_SECTION("sa2isa_bucketing");

    // get displacements again (since they were modified above)
    send_displs = get_displacements(send_counts);
    // get receive information
    std::vector<int> recv_counts = all2allv_get_recv_counts(send_counts, comm);
    std::vector<int> recv_displs = get_displacements(recv_counts);

    // perform the all2all communication
    MPI_Alltoallv(&send_B[0], &send_counts[0], &send_displs[0], mpi_dt,
                  &local_B[0], &recv_counts[0], &recv_displs[0], mpi_dt,
                  comm);
    MPI_Alltoallv(&send_SA[0], &send_counts[0], &send_displs[0], mpi_dt,
                  &local_SA[0], &recv_counts[0], &recv_displs[0], mpi_dt,
                  comm);
    SAC_TIMER_END_SECTION("sa2isa_all2all");

    // rearrange locally
    // TODO [ENH]: more cache efficient by sorting rather than random assignment
    for (std::size_t i = 0; i < local_SA.size(); ++i)
    {
        index_t out_idx = local_SA[i] - block_partition_excl_prefix_size(n, p, rank);
        assert(0 <= out_idx && out_idx < local_SA.size());
        send_B[out_idx] = local_B[i];
    }
    std::copy(send_B.begin(), send_B.end(), local_B.begin());

    // reassign the SA
    std::size_t global_offset = block_partition_excl_prefix_size(n, p, rank);
    for (std::size_t i = 0; i < local_SA.size(); ++i)
    {
        local_SA[i] = global_offset + i;
    }
    SAC_TIMER_END_SECTION("sa2isa_rearrange");
}

// in: 2^m, B1
// out: B2
template <typename index_t>
void shift_buckets(std::size_t n, std::size_t dist, std::vector<index_t>& local_B1, std::vector<index_t>& local_B2, MPI_Comm comm)
{
    // get MPI comm parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);
    MPI_Datatype mpi_dt = get_mpi_dt<index_t>();

    // get # elements to the left
    std::size_t prev_size = block_partition_excl_prefix_size(n, p, rank);
    std::size_t local_size = block_partition_local_size(n, p, rank);
    assert(local_size == local_B1.size());

    // init B2
    if (local_B2.size() != local_size){
        local_B2.clear();
        local_B2.resize(local_size, 0);
    }

    MPI_Request recv_reqs[2];
    int n_irecvs = 0;
    // receive elements from the right
    if (prev_size + dist < n)
    {
        std::size_t right_first_gl_idx = prev_size + dist;
        int p1 = block_partition_target_processor(n, p, right_first_gl_idx);

        std::size_t p1_gl_end = block_partition_prefix_size(n, p, p1);
        std::size_t p1_recv_cnt = p1_gl_end - right_first_gl_idx;

        if (p1 != rank)
        {
            // only receive if the source is not myself (i.e., `rank`)
            // [otherwise results are directly written instead of MPI_Sended]
            assert(p1_recv_cnt < std::numeric_limits<int>::max());
            int recv_cnt = p1_recv_cnt;
            // TODO: MPI_Datatype
            MPI_Irecv(&local_B2[0],recv_cnt, mpi_dt, p1,
                      PSAC_TAG_SHIFT, comm, &recv_reqs[n_irecvs++]);
        }

        if (p1_recv_cnt < local_size && p1 != p-1)
        {
            // also receive from one more processor
            int p2 = p1+1;
            // since p2 has at least local_size - 1 elements and at least
            // one element came from p1, we can assume that the receive count
            // is our local size minus the already received elements
            std::size_t p2_recv_cnt = local_size - p1_recv_cnt;

            assert(p2_recv_cnt < std::numeric_limits<int>::max());
            int recv_cnt = p2_recv_cnt;
            // send to `p1` (which is necessarily different from `rank`)
            // TODO: MPI_Datatype
            MPI_Irecv(&local_B2[0] + p1_recv_cnt, recv_cnt, mpi_dt, p2,
                      PSAC_TAG_SHIFT, comm, &recv_reqs[n_irecvs++]);
        }
    }

    // send elements to the left (split to at most 2 target processors)
    if (prev_size + local_size - 1 >= dist)
    {
        int p1 = -1;
        if (prev_size >= dist)
        {
            std::size_t first_gl_idx = prev_size - dist;
            p1 = block_partition_target_processor(n, p, first_gl_idx);
        }
        std::size_t last_gl_idx = prev_size + local_size - 1 - dist;
        int p2 = block_partition_target_processor(n, p, last_gl_idx);

        std::size_t local_split;
        if (p1 != p2)
        {
            // local start index of area for second processor
            if (p1 >= 0)
            {
                local_split = block_partition_prefix_size(n, p, p1) + dist - prev_size;
                // send to first processor
                assert(p1 != rank);
                MPI_Send(&local_B1[0], local_split,
                         mpi_dt, p1, PSAC_TAG_SHIFT, comm);
            }
            else
            {
                // p1 doesn't exist, then there is no prefix to add
                local_split = dist - prev_size;
            }
        }
        else
        {
            // only one target processor
            local_split = 0;
        }

        if (p2 != rank)
        {
            MPI_Send(&local_B1[0] + local_split, local_size - local_split,
                     mpi_dt, p2, PSAC_TAG_SHIFT, comm);
        }
        else
        {
            // in this case the split should be exactly at `dist`
            assert(local_split == dist);
            // locally reassign
            for (std::size_t i = local_split; i < local_size; ++i)
            {
                local_B2[i-local_split] = local_B1[i];
            }
        }
    }

    // wait for successful receive:
    MPI_Waitall(n_irecvs, recv_reqs, MPI_STATUS_IGNORE);
}


template <typename T>
struct TwoBSA
{
    T B1;
    T B2;
    T SA;

    inline bool operator<(const TwoBSA& other) const
    {
        // tuple comparison of (B1, B2) with precedence to B1
        return (this->B1 < other.B1)
            || (this->B1 == other.B1 && this->B2 < other.B2);
    }
};

// template specialization for MPI_Datatype for the two buckets and SA structure
// Needed for the samplesort implementation
template<>
MPI_Datatype get_mpi_dt<TwoBSA<unsigned int>>()
{
    // keep only one instance around
    // NOTE: this memory will not be destructed.
    MPI_Datatype dt;
    MPI_Datatype element_t = get_mpi_dt<unsigned int>();
    MPI_Type_contiguous(3, element_t, &dt);
    return dt;
}
template<>
MPI_Datatype get_mpi_dt<TwoBSA<int>>()
{
    // keep only one instance around
    // NOTE: this memory will not be destructed.
    MPI_Datatype dt;
    MPI_Datatype element_t = get_mpi_dt<int>();
    MPI_Type_contiguous(3, element_t, &dt);
    return dt;
}
template<>
MPI_Datatype get_mpi_dt<TwoBSA<std::size_t>>()
{
    // keep only one instance around
    // NOTE: this memory will not be destructed.
    MPI_Datatype dt;
    MPI_Datatype element_t = get_mpi_dt<std::size_t>();
    MPI_Type_contiguous(3, element_t, &dt);
    return dt;
}

// custom MPI data types for std::pair messages
template<>
MPI_Datatype get_mpi_dt<std::pair<std::size_t, std::size_t> >()
{
    // keep only one instance around
    // NOTE: this memory will not be destructed.
    MPI_Datatype dt;
    MPI_Datatype element_t = get_mpi_dt<std::size_t>();
    MPI_Type_contiguous(2, element_t, &dt);
    return dt;
}
template<>
MPI_Datatype get_mpi_dt<std::pair<unsigned int, unsigned int> >()
{
    // keep only one instance around
    // NOTE: this memory will not be destructed.
    MPI_Datatype dt;
    MPI_Datatype element_t = get_mpi_dt<unsigned int>();
    MPI_Type_contiguous(2, element_t, &dt);
    return dt;
}
template<>
MPI_Datatype get_mpi_dt<std::pair<int, int> >()
{
    // keep only one instance around
    // NOTE: this memory will not be destructed.
    MPI_Datatype dt;
    MPI_Datatype element_t = get_mpi_dt<int>();
    MPI_Type_contiguous(2, element_t, &dt);
    return dt;
}

// assumed sorted order (globally) by tuple (B1[i], B2[i])
// this reassigns new, unique bucket numbers in {0,...,n-1} globally
template <typename index_t>
void rebucket_tuples(std::vector<TwoBSA<index_t> >& tuples, MPI_Comm comm, std::size_t gl_offset)
{
    // assert inputs are of equal size
    std::size_t local_size = tuples.size();

    // get communication parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);

    /*
     * send right most element to one processor to the right
     * so that that processor can determine whether the same bucket continues
     * or a new bucket starts with it's first element
     */
    MPI_Request recv_req;
    MPI_Datatype mpi_dt = get_mpi_dt<index_t>();
    index_t prevRight[2];
    if (rank > 0) // if not last processor
    {
        MPI_Irecv(prevRight, 2, mpi_dt, rank-1, PSAC_TAG_EDGE_KMER,
                  comm, &recv_req);
    }
    if (rank < p-1) // if not first processor
    {
        // send my most right element to the right
        index_t myRight[2] = {tuples.back().B1, tuples.back().B2};
        MPI_Send(myRight, 2, mpi_dt, rank+1, PSAC_TAG_EDGE_KMER, comm);
    }
    if (rank > 0)
    {
        // wait for the async receive to finish
        MPI_Wait(&recv_req, MPI_STATUS_IGNORE);
    }

    // get my global starting index
    std::size_t prefix;
    MPI_Datatype mpi_size_t = get_mpi_dt<std::size_t>();
    MPI_Exscan(&local_size, &prefix, 1, mpi_size_t, MPI_SUM, comm);
    if (rank == 0)
        prefix = 0;
    prefix += gl_offset;

    /*
     * assign local zero or one, depending on whether the bucket is the same
     * as the previous one
     */
    bool firstDiff = false;
    if (rank == 0)
    {
        firstDiff = true;
    }
    else if (prevRight[0] != tuples[0].B1 || prevRight[1] != tuples[0].B2)
    {
        firstDiff = true;
    }

    // set local_B1 to `1` if previous entry is different:
    // i.e., mark start of buckets
    bool nextDiff = firstDiff;
    for (std::size_t i = 0; i+1 < local_size; ++i)
    {
        bool setOne = nextDiff;
        nextDiff = (tuples[i].B1 != tuples[i+1].B1 || tuples[i].B2 != tuples[i+1].B2);
        tuples[i].B1 = setOne ? prefix+i : 0;
    }

    tuples.back().B1 = nextDiff ? prefix+local_size-1 : 0;

    /*
     * Global prefix MAX:
     *  - such that for every item we have it's bucket number, where the
     *    bucket number is equal to the first index in the bucket
     *    this way buckets who are finished, will never receive a new
     *    number.
     */
    // 1.) find the max in the local sequence. since the max is the last index
    //     of a bucket, this should be somewhere at the end -> start scanning
    //     from the end
    auto rev_it = tuples.rbegin();
    std::size_t local_max = 0;
    while (rev_it != tuples.rend() && (local_max = rev_it->B1) == 0)
        ++rev_it;

    // 2.) distributed scan with max() to get starting max for each sequence
    std::size_t pre_max;
    MPI_Exscan(&local_max, &pre_max, 1, mpi_size_t, MPI_MAX, comm);
    if (rank == 0)
        pre_max = 0;

    // 3.) linear scan and assign bucket numbers
    for (std::size_t i = 0; i < local_size; ++i)
    {
        if (tuples[i].B1 == 0)
            tuples[i].B1 = pre_max;
        else
            pre_max = tuples[i].B1;
        assert(tuples[i].B1 <= i+prefix);
        // first element of bucket has id of it's own global index:
        assert(i == 0 || (tuples[i-1].B1 ==  tuples[i].B1 || tuples[i].B1 == i+prefix));
    }
}

// in: B1, B2
// out: reordered B1, B2; and SA
template <typename index_t>
void isa_2b_to_sa(std::size_t n, std::vector<index_t>& B1, std::vector<index_t>& B2, std::vector<index_t>& SA, MPI_Comm comm)
{
    // check input sizes
    std::size_t local_size = B1.size();
    assert(B2.size() == local_size);
    assert(SA.size() == local_size);

    // get comm parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);

    SAC_TIMER_START();

    // initialize tuple array
    std::vector<TwoBSA<index_t> > tuple_vec(local_size);

    // get global index offset
    std::size_t str_offset = block_partition_excl_prefix_size(n, p, rank);

    // fill tuple vector
    for (std::size_t i = 0; i < local_size; ++i)
    {
        tuple_vec[i].B1 = B1[i];
        tuple_vec[i].B2 = B2[i];
        assert(str_offset + i < std::numeric_limits<index_t>::max());
        tuple_vec[i].SA = str_offset + i;
    }

    // release memory of input (to remain at the minimum 6x words memory usage)
    B1.clear(); B1.shrink_to_fit();
    B2.clear(); B2.shrink_to_fit();
    SA.clear(); SA.shrink_to_fit();

    SAC_TIMER_END_SECTION("isa2sa_tupleize");

    // parallel, distributed sample-sorting of tuples (B1, B2, SA)
    if(rank == 0)
        std::cerr << "  sorting local size = " << tuple_vec.size() << std::endl;
    samplesort(tuple_vec.begin(), tuple_vec.end(), std::less<TwoBSA<index_t> >());

    SAC_TIMER_END_SECTION("isa2sa_samplesort");

    // reallocate output
    B1.resize(local_size);
    B2.resize(local_size);
    SA.resize(local_size);

    // read back into input vectors
    for (std::size_t i = 0; i < local_size; ++i)
    {
        B1[i] = tuple_vec[i].B1;
        B2[i] = tuple_vec[i].B2;
        SA[i] = tuple_vec[i].SA;
    }
    SAC_TIMER_END_SECTION("isa2sa_untupleize");
}

template <typename index_t>
bool gl_check_correct_SA(const std::vector<index_t> SA, const std::vector<index_t>& ISA, const std::string& str)
{
    std::size_t n = SA.size();
    bool success = true;

    for (std::size_t i = 0; i < n; ++i)
    {
        // check valid range
        if (SA[i] >= n || SA[i] < 0)
        {
            std::cerr << "[ERROR] SA[" << i << "] = " << SA[i] << " out of range 0 <= sa < " << n << std::endl;
            success = false;
        }

        // check SA conditions
        if (i >= 1 && SA[i-1] < n-1)
        {
            if (!(str[SA[i]] >= str[SA[i-1]]))
            {
                std::cerr << "[ERROR] wrong SA order: str[SA[i]] >= str[SA[i-1]]" << std::endl;
                success = false;
            }

            // if strings are equal, the ISA of these positions have to be
            // ordered
            if (str[SA[i]] == str[SA[i-1]])
            {
                if (!(ISA[SA[i-1]+1] < ISA[SA[i]+1]))
                {
                    std::cerr << "[ERROR] invalid SA order: ISA[SA[" << i-1 << "]+1] < ISA[SA[" << i << "]+1]" << std::endl;
                    std::cerr << "[ERROR] where SA[i-1]=" << SA[i-1] << ", SA[i]=" << SA[i] << ", ISA[SA[i-1]+1]=" << ISA[SA[i-1]+1] << ", ISA[SA[i]+1]=" << ISA[SA[i]+1] << std::endl;
                    success = false;
                }
            }
        }
    }

    return success;
}

template<typename T, typename _TargetP>
void msgs_all2all(std::vector<T>& msgs, _TargetP target_p_fun, MPI_Comm comm)
{
    // get comm parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);
    MPI_Datatype mpi_dt = get_mpi_dt<T>();
    MPI_Type_commit(&mpi_dt);

    // bucket input by their target processor
    std::vector<int> send_counts(p, 0);
    for (auto it = msgs.begin(); it != msgs.end(); ++it)
    {
        send_counts[target_p_fun(*it)]++;
    }
    std::vector<std::size_t> offset(send_counts.begin(), send_counts.end());
    excl_prefix_sum(offset.begin(), offset.end());
    std::vector<T> send_buffer;
    if (msgs.size() > 0)
        send_buffer.resize(msgs.size());
    for (auto it = msgs.begin(); it != msgs.end(); ++it)
    {
        send_buffer[offset[target_p_fun(*it)]++] = *it;
    }

    // get all2all params
    std::vector<int> recv_counts = all2allv_get_recv_counts(send_counts, comm);
    std::vector<int> send_displs = get_displacements(send_counts);
    std::vector<int> recv_displs = get_displacements(recv_counts);

    // resize messages to fit recv
    std::size_t recv_size = std::accumulate(recv_counts.begin(), recv_counts.end(), 0);
    msgs.resize(recv_size);

    // all2all
    MPI_Alltoallv(&send_buffer[0], &send_counts[0], &send_displs[0], mpi_dt,
                  &msgs[0], &recv_counts[0], &recv_displs[0], mpi_dt, comm);
    // done, result is returned in vector of input messages
}

template <typename index_t>
void sa_bucket_chaising_constr(std::size_t n, std::vector<index_t>& local_SA, std::vector<index_t>& local_B, std::vector<index_t>& local_ISA, MPI_Comm comm, int dist)
{
    /*
     * Algorithm for few remaining buckets (more communication overhead per
     * element but sends only unfinished buckets -> less data in total if few
     * buckets remaining)
     *
     * INPUT:
     *  - SA in SA order
     *  - B in SA order
     *  - ISA in ISA order (<=> B in ISA order)
     *  - dist: the current dist=2^k, gets doubled after every iteration
     *
     * ALGO:
     * 1.) on i:            send tuple (`to:` Sa[i]+2^k, `from:` i)
     * 2.) on SA[i]+2^k:    return tuple (`to:` i, ISA[SA[i]+2^k])
     * 3.) on i:            for each unfinished bucket:
     *                          sort by new bucket index (2-stage across
     *                          processor boundaries using MPI subcommunicators)
     *                          rebucket into `B`
     * 4.) on i:            send tuple (`to:` SA[i], B[i]) // update bucket numbers in ISA order
     * 5.) on SA[i]:        update ISA[SA[i]] to new B[i]
     *
     */
    // get input size
    std::size_t local_size = local_SA.size();
    assert(local_B.size() == local_size);
    assert(local_ISA.size() == local_size);

    // get comm parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);
    MPI_Datatype mpi_dt = get_mpi_dt<index_t>();

    for (std::size_t shift_by = dist<<1; shift_by < n; shift_by <<= 1)
    {
        /*
         * 0.) Preparation: need unfinished buckets (info accross proc. boundaries)
         */
        // exchange border elements (left and right)
        index_t left_B, right_B;
        int reqc = 0;
        MPI_Request recv_reqs[2];
        if (rank > 0)
        {
            // receive from left
            MPI_Irecv(&left_B, 1, mpi_dt, rank-1, PSAC_TAG_EDGE_B,
                      comm, &recv_reqs[reqc++]);
        }
        if (rank < p-1)
        {
            // receive from right
            MPI_Irecv(&right_B, 1, mpi_dt, rank+1, PSAC_TAG_EDGE_B,
                      comm, &recv_reqs[reqc++]);
        }
        if (rank > 0)
        {
            // send to left
            MPI_Send(&local_B[0], 1, mpi_dt, rank-1, PSAC_TAG_EDGE_B, comm);
        }
        if (rank < p-1)
        {
            // send to right
            MPI_Send(&local_B.back(), 1, mpi_dt, rank+1, PSAC_TAG_EDGE_B, comm);
        }
        MPI_Waitall(reqc, recv_reqs, MPI_STATUS_IGNORE);
        bool right_bucket_crosses_proc = (rank < p-1 && local_B.back() == right_B);

        /*
         * 1.) on i: send tuple (`to:` Sa[i]+2^k, `from:` i)
         */
        std::vector<std::pair<index_t, index_t> > msgs;
        //std::vector<std::pair<index_t, index_t> > out_of_bounds_msgs;
        // linear scan for bucket boundaries
        // and create tuples/pairs
        std::size_t prefix = block_partition_excl_prefix_size(n, p, rank);
        std::size_t unresolved = 0;
        for (std::size_t j = 0; j < local_B.size(); ++j)
        {
            // get global index for each local index
            std::size_t i =  prefix + j;
            // check if this is a unresolved bucket
            // relying on the property that for resolved buckets:
            //   B[i] == i and B[i+1] == i+1
            //   (where `i' is the global index)
            if (local_B[j] != i || (local_B[j] == i
                && ((j < local_size-1 && local_B[j+1] == i)
                    || (j == local_size-1 && right_B == i))))
            {
                // add tuple
                if (local_SA[j] + shift_by >= n)
                    unresolved--;
         //           out_of_bounds_msgs.push_back(std::make_pair<index_t,index_t>(0, static_cast<index_t>(i)));
                else
                    msgs.push_back(std::make_pair<index_t,index_t>(local_SA[j]+shift_by, static_cast<index_t>(i)));
                unresolved++;
            }
        }

        // check if all resolved
        std::size_t gl_unresolved;
        MPI_Datatype mpi_size_t = get_mpi_dt<std::size_t>();
        MPI_Allreduce(&unresolved, &gl_unresolved, 1, mpi_size_t, MPI_SUM, comm);
        if (rank == 0)
            std::cerr << "==== chaising iteration " << shift_by << " unresolved = " << gl_unresolved << std::endl;
        if (gl_unresolved == 0)
            // finished!
            break;

        // message exchange to processor which contains first index
        msgs_all2all(msgs, [&](const std::pair<index_t, index_t>& x){return block_partition_target_processor(n,p,static_cast<std::size_t>(x.first));}, comm);

        // for each message, add the bucket no. into the `first` field
        for (auto it = msgs.begin(); it != msgs.end(); ++it)
        {
            it->first = local_ISA[it->first - prefix];
        }


        /*
         * 2.)
         */
        // send messages back to originator
        msgs_all2all(msgs, [&](const std::pair<index_t, index_t>& x){return block_partition_target_processor(n,p,static_cast<std::size_t>(x.second));}, comm);

        // append the previous out-of-bounds messages (since they all have B2 = 0)
        //if (out_of_bounds_msgs.size() > 0)
        //    msgs.insert(msgs.end(), out_of_bounds_msgs.begin(), out_of_bounds_msgs.end());
        //out_of_bounds_msgs.clear();

        // sort received messages by the target index to enable consecutive
        // scanning of local buckets and messages
        std::sort(msgs.begin(), msgs.end(), [&](const std::pair<index_t, index_t>& x, const std::pair<index_t, index_t>& y){ return x.second < y.second;});


        /*
         * 3.)
         */
        // building sequence of triplets for each unfinished bucket and sort
        // then rebucket, buckets which spread accross boundaries, sort via
        // MPI sub communicators and samplesort in two phases
        std::vector<TwoBSA<index_t> > bucket;
        std::vector<TwoBSA<index_t> > left_bucket;
        std::vector<TwoBSA<index_t> > right_bucket;
        // find bucket boundaries:
        auto msgit = msgs.begin();
        // overlap type:    0: no overlaps, 1: left overlap, 2:right overlap,
        //                  3: separate overlaps on left and right
        //                  4: contiguous overlap with both sides
        int overlap_type = 0; // init to no overlaps
        std::size_t bucket_begin = local_B[0];
        std::cerr << " on rank " << rank <<  ", left_B=" << left_B << ", B[0..] = "; print_range(local_B.begin(), local_B.begin()+10);
        std::cerr << " on rank " << rank << " B[end]=" << local_B.back() << ", right_B=" << right_B << std::endl;
        std::size_t first_bucket_begin = bucket_begin;
        std::size_t right_bucket_offset = 0;
        do
        {
            if (msgit != msgs.end())
                bucket_begin = local_B[msgit->second - prefix];
            // find end of bucket
            while (msgit != msgs.end() && local_B[msgit->second - prefix] == bucket_begin)
            {
                TwoBSA<index_t> tuple;
                tuple.SA = local_SA[msgit->second - prefix];
                tuple.B1 = local_B[msgit->second - prefix];
                tuple.B2 = msgit->first;
                bucket.push_back(tuple);
                msgit++;
            }
            // get bucket end (could be on other processor)
            std::size_t bucket_end = 0;
            if (msgit == msgs.end() && right_bucket_crosses_proc)
            {
                if (bucket_begin >= prefix)
                {
                    overlap_type += 2;
                    right_bucket = bucket;
                    right_bucket_offset = bucket_begin - prefix;
                }
                else
                {
                    // bucket extends to left AND right
                    left_bucket = bucket;
                    overlap_type = 4;
                }
            } else {
                if (msgit != msgs.end())
                    bucket_end = msgit->second;
                if (bucket_begin >= prefix)
                {
                    // this is a local bucket => sort by B2, rebucket, and save
                    // TODO custom comparison that only sorts by B2, not by B1 as well
                    std::sort(bucket.begin(), bucket.end());
                    // local rebucket
                    // save back into local_B, local_SA, etc
                    index_t cur_b = bucket_begin;
                    std::size_t out_idx = bucket_begin - prefix;
                    for (auto it = bucket.begin(); it != bucket.end(); ++it)
                    {
                        // if this is a new bucket, then update number
                        if (it != bucket.begin() && (it-1)->B2 != it->B2)
                        {
                            cur_b = out_idx + prefix;
                        }
                        local_SA[out_idx] = it->SA;
                        local_B[out_idx] = cur_b;
                        out_idx++;
                    }
                }
                else
                {
                    std::cerr << "+++++++ on rank " << rank << " bucket_begin=" << bucket_begin << ", prefix =" << prefix << std::endl;
                    overlap_type += 1;
                    left_bucket = bucket;
                }
            }
            bucket.clear();
            // quit loop
            if (msgit == msgs.end())
                break;
            bucket_begin = bucket_end;
            // sort bucket (if local), otherwise remember for interleaved parallel
            // sorting
        } while(1);

        // if we have left/right/both/or double buckets, do global comm in two phases
        int my_schedule = -1;
        if (rank == 0)
        {
            // gather all types to first processor
            std::vector<int> overlaps(p);
            MPI_Gather(&overlap_type, 1, MPI_INT, &overlaps[0], 1, MPI_INT, 0, comm);
            std::cerr << "HAVE OVERLAPS: "; print_range(overlaps.begin(), overlaps.end());

            // create schedule using linear scan over the overlap types
            std::vector<int> schedule(p);
            int phase = 0; // start in first phase
            for (int i = 0; i < p; ++i)
            {
                switch (overlaps[i])
                {
                    case 0:
                        schedule[i] = -1; // doesn't matter
                        break;
                    case 1:
                        // only left overlap -> participate in current phase
                        schedule[i] = phase;
                        break;
                    case 2:
                        // only right overlap, start with phase 0
                        phase = 0;
                        schedule[i] = phase;
                        break;
                    case 3:
                        // separate overlaps left and right -> switch phase
                        schedule[i] = phase; // left overlap starts with current phase
                        phase = 1 - phase;
                        break;
                    case 4:
                        // overlap with both: left and right => keep phase
                        schedule[i] = phase;
                        break;
                    default:
                        assert(false);
                        break;
                }
            }

            std::cerr << "FOUND SCHEDULE: "; print_range(schedule.begin(), schedule.end());
            // scatter the schedule to the processors
            MPI_Scatter(&schedule[0], 1, MPI_INT, &my_schedule, 1, MPI_INT, 0, comm);
        }
        else
        {
            // send out my overlap type
            MPI_Gather(&overlap_type, 1, MPI_INT, NULL, 1, MPI_INT, 0, comm);

            // ... let master processor solve the schedule

            // receive schedule:
            MPI_Scatter(NULL, 1, MPI_INT, &my_schedule, 1, MPI_INT, 0, comm);
        }


        // two phase sorting across boundaries using sub communicators
        for (int phase = 0; phase <= 1; ++phase)
        {
            std::vector<TwoBSA<index_t> >& border_bucket = left_bucket;
            // the leftmost processor of a group will be used as split
            int left_p = block_partition_target_processor(n, p, first_bucket_begin);
            bool participate = (overlap_type != 0 && my_schedule == phase);
            std::size_t bucket_offset = 0; // left bucket starts from beginning
            std::size_t rebucket_offset = first_bucket_begin;
            if ((my_schedule != phase && overlap_type == 3) || (my_schedule == phase && overlap_type == 2))
            {
                // starting a bucket at the end
                border_bucket = right_bucket;
                left_p = rank;
                participate = true;
                bucket_offset = right_bucket_offset;
                rebucket_offset = prefix + bucket_offset;
            }

            MPI_Comm subcomm;
            if (participate)
            {
                std::cerr << "++++++ rank = " << rank << " participates in " << left_p << std::endl;
                // split communicator to `left_p`
                MPI_Comm_split(comm, left_p, 0, &subcomm);

                // sample sort the bucket with arbitrary distribution
                samplesort(border_bucket.begin(), border_bucket.end(), std::less<TwoBSA<index_t> >(), subcomm, false);

                // rebucket with global offset of first -> in tuple form
                rebucket_tuples(border_bucket, subcomm, rebucket_offset);

                // save into full array (if this was left -> save to beginning)
                // (else, need offset of last)
                for (std::size_t i = 0; i < border_bucket.size(); ++i)
                {
                    local_SA[i+bucket_offset] = border_bucket[i].SA;
                    local_B[i+bucket_offset] = border_bucket[i].B1;
                }
            }
            else
            {
                // split communicator to don't care (since this processor doesn't
                // participate)
                MPI_Comm_split(comm, MPI_UNDEFINED, 0, &subcomm);
            }


            MPI_Barrier(comm);
        }


        /*
         * 4.)
         */
        // message new bucket numbers to new SA[i] for all previously unfinished
        // buckets
        // since the message array is still available with the indices of unfinished
        // buckets -> reuse that information => no need to rescan the whole 
        // local array
        // TODO: if i keep this information (active buffer indeces) around,
        // i can reuse it in every iteration and thus never have to scan the full
        // array ever again!
        for (auto it = msgs.begin(); it != msgs.end(); ++it)
        {
            it->first = local_SA[it->second - prefix]; // SA[i]
            it->second = local_B[it->second - prefix]; // B[i]
        }

        // message exchange to processor which contains first index
        msgs_all2all(msgs, [&](const std::pair<index_t, index_t>& x){return block_partition_target_processor(n,p,static_cast<std::size_t>(x.first));}, comm);

        // update local ISA with new bucket numbers
        for (auto it = msgs.begin(); it != msgs.end(); ++it)
        {
            local_ISA[it->first-prefix] = it->second;
        }
    }
}


template <typename index_t>
void sa_construction_impl(std::size_t n, const std::string& local_str, std::vector<index_t>& local_SA, std::vector<index_t>& local_B, MPI_Comm comm)
{
    // get comm parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);

    SAC_TIMER_START();

    /***********************
     *  Initial bucketing  *
     ***********************/

    // create initial k-mers and use these as the initial bucket numbers
    // for each character position
    // `k` depends on the alphabet size and the word size of each suffix array
    // element. `k` is choosen to maximize the number of alphabet characters
    // that fit into one machine word
    unsigned int k = initial_bucketing(n, local_str, local_B, comm);
    DEBUG_STAGE_VEC("after initial bucketing", local_B);
#if 0
    std::cerr << "========  After initial bucketing  ========" << std::endl;
    std::cerr << "On processor rank = " << rank << std::endl;
    std::cerr << "B : "; print_vec(local_B);
#endif

    SAC_TIMER_END_SECTION("initial-bucketing");

    // init local_SA
    if (local_SA.size() != local_B.size())
    {
        local_SA.resize(local_B.size());
    }

    std::vector<index_t> local_B_SA;
    std::size_t unfinished_buckets = 1<<k;
    std::size_t shift_by;

    /*******************************
     *  Prefix Doubling main loop  *
     *******************************/


    for (shift_by = k; shift_by < n; shift_by <<= 1)
    {
        SAC_TIMER_LOOP_START();
        /**************************************************
         *  Pairing buckets by shifting `shift_by` = 2^k  *
         **************************************************/
        // shift the B1 buckets by 2^k to the left => equals B2
        std::vector<index_t> local_B2;
        shift_buckets(n, shift_by, local_B, local_B2, comm);
        DEBUG_STAGE_VEC2("after shift by " << shift_by, local_B, local_B2);
#if 0
        std::cerr << "========  After shift by " << shift_by << "  ========" << std::endl;
        std::cerr << "On processor rank = " << rank << std::endl;
        std::cerr << "B : "; print_vec(local_B);
        std::cerr << "B2: "; print_vec(local_B2);
#endif
        SAC_TIMER_END_LOOP_SECTION(shift_by, "shift-buckets");

        /*************
         *  ISA->SA  *
         *************/
        // by using sample sort on tuples (B1,B2)
        isa_2b_to_sa(n, local_B, local_B2, local_SA, comm);
        DEBUG_STAGE_VEC3("after reorder ISA->SA", local_B, local_B2, local_SA);
#if 0
        std::cerr << "========  After reorder ISA->SA  ========" << std::endl;
        std::cerr << "On processor rank = " << rank << std::endl;
        std::cerr << "B : "; print_vec(local_B);
        std::cerr << "B2: "; print_vec(local_B2);
        std::cerr << "SA: "; print_vec(local_SA);
#endif
        SAC_TIMER_END_LOOP_SECTION(shift_by, "ISA-to-SA");

        /*******************************
         *  Assign new bucket numbers  *
         *******************************/
        unfinished_buckets = rebucket(n, local_B, local_B2, comm ,true);
        if (rank == 0)
            std::cerr << "iteration " << shift_by << ": unfinished buckets = " << unfinished_buckets << std::endl;
        DEBUG_STAGE_VEC("after rebucket", local_B);
#if 0
        std::cerr << "========  After rebucket  ========" << std::endl;
        std::cerr << "On processor rank = " << rank << std::endl;
        std::cerr << "B : "; print_vec(local_B);
#endif
        SAC_TIMER_END_LOOP_SECTION(shift_by, "rebucket");

        /*************
         *  SA->ISA  *
         *************/
        // by bucketing to correct target processor using the `SA` array
        // // TODO by number of unresolved elements rather than buckets!!
        if (unfinished_buckets < n/10)
        {
            // prepare for bucket chaising (needs SA, and bucket arrays in both
            // SA and ISA order)
            std::vector<index_t> cpy_SA(local_SA);
            local_B_SA = local_B; // copy
            reorder_sa_to_isa(n, cpy_SA, local_B, comm);
            SAC_TIMER_END_LOOP_SECTION(shift_by, "SA-to-ISA");
            break;
        }
        else if ((shift_by << 1) >= n || unfinished_buckets == 0)
        {
            // if last iteration, use copy of local_SA for reorder and keep
            // original SA
            std::vector<index_t> cpy_SA(local_SA);
            reorder_sa_to_isa(n, cpy_SA, local_B, comm);
            SAC_TIMER_END_LOOP_SECTION(shift_by, "SA-to-ISA");
        }
        else
        {
            reorder_sa_to_isa(n, local_SA, local_B, comm);
            SAC_TIMER_END_LOOP_SECTION(shift_by, "SA-to-ISA");
        }
        DEBUG_STAGE_VEC2("after reorder SA->ISA", local_B, local_SA);
#if 0
        std::cerr << "========  After reorder SA->ISA  ========" << std::endl;
        std::cerr << "On processor rank = " << rank << std::endl;
        std::cerr << "B : "; print_vec(local_B);
        std::cerr << "SA: "; print_vec(local_SA);
#endif

        if (unfinished_buckets == 0)
            break;

        SAC_TIMER_END_SECTION("sac-iteration");
    }

    if (unfinished_buckets > 0)
    {
        if (rank == 0)
            std::cerr << "Starting Bucket chasing algorithm" << std::endl;
        sa_bucket_chaising_constr(n, local_SA, local_B_SA, local_B, comm, shift_by);
    }

    // now local_SA is actual block decomposed SA and local_B is actual ISA
}


void sa_construction(const std::string& local_str, std::vector<std::size_t>& local_SA, std::vector<std::size_t>& local_ISA, MPI_Comm comm = MPI_COMM_WORLD)
{
    // get comm parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);

    // get local and global size
    std::size_t local_size = local_str.size();
    std::size_t n = 0;
    MPI_Datatype mpi_size_t = get_mpi_dt<std::size_t>();
    MPI_Allreduce(&local_size, &n, 1, mpi_size_t, MPI_SUM, comm);
    // assert the input is distributed as a block decomposition
    assert(local_size == block_partition_local_size(n, p, rank));

    // check if the input size fits in 32 bits (< 4 GiB input)
    if (sizeof(std::size_t) != sizeof(std::uint32_t))
    {
        if (n >= std::numeric_limits<std::uint32_t>::max())
        {
            // use 64 bits for the suffix array and the ISA
            // -> Suffix array construction needs 49x Bytes
            sa_construction_impl<std::size_t>(n, local_str, local_SA, local_ISA, comm);
        }
        else
        {
            // 32 bits is enough -> Suffix array construction needs 25x Bytes
            std::vector<std::uint32_t> local_SA32;
            std::vector<std::uint32_t> local_ISA32;
            // call the suffix array construction implementation
            sa_construction_impl<std::uint32_t>(n, local_str, local_SA32, local_ISA32, comm);
            // transform back into std::size_t
            local_SA.resize(local_size);
            std::copy(local_SA32.begin(), local_SA32.end(), local_SA.begin());
            local_ISA.resize(local_size);
            std::copy(local_ISA32.begin(), local_ISA32.end(), local_ISA.begin());
        }
    }
    else
    {
        assert(n < std::numeric_limits<std::uint32_t>::max());
        sa_construction_impl<std::size_t>(n, local_str, local_SA, local_ISA, comm);
    }
}


template<typename index_t>
void sa_construction_gl(std::string& global_str, std::vector<index_t>& SA, std::vector<index_t>& ISA, MPI_Comm comm = MPI_COMM_WORLD)
{
    // get comm parameters
    int p, rank;
    MPI_Comm_size(comm, &p);
    MPI_Comm_rank(comm, &rank);

    // distribute input
    std::string local_str = scatter_string_block_decomp(global_str, comm);

    // distribute the global size
    std::size_t n = global_str.size();
    MPI_Datatype mpi_size_t = get_mpi_dt<std::size_t>();
    MPI_Bcast(&n, 1, mpi_size_t, 0, comm);

    // allocate local output
    std::vector<index_t> local_SA;
    std::vector<index_t> local_ISA;

    // call sa implementation
    sa_construction_impl<index_t>(n, local_str, local_SA, local_ISA, comm);

    // gather output to processor 0
    SA = gather_vectors(local_SA, comm);
    ISA = gather_vectors(local_ISA, comm);
}

#endif // MPI_SA_CONSTR_HPP
