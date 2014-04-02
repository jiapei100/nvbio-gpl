/*
 * nvbio
 * Copyright (C) 2011-2014, NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#pragma once

#include <nvbio/sufsort/sufsort_priv.h>
#include <nvbio/basic/string_set.h>
#include <nvbio/basic/thrust_view.h>
#include <thrust/host_vector.h>
#include <thrust/device_vector.h>
#include <thrust/iterator/constant_iterator.h>
#include <thrust/iterator/counting_iterator.h>

namespace nvbio {

/// A data structure to hold a Difference Cover Sample
///
struct DCSView
{
    DCSView(
        const uint32  _size     = 0,
              uint32* _dc       = NULL,
              uint32* _lut      = NULL,
              uint32* _pos      = NULL,
              uint8*  _bitmask  = NULL,
              uint32* _ranks    = NULL) :
        dc      ( _dc ),
        lut     ( _lut ),
        pos     ( _pos ),
        bitmask ( _bitmask ),
        ranks   ( _ranks ),
        size    ( _size ) {}


    /// return the sampled position of a given suffix index
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 index(const uint32 i) const;

    uint32*         dc;
    uint32*         lut;
    uint32*         pos;
    uint8*          bitmask;
    uint32*         ranks;
    const uint32    size;
};

/// A data structure to hold a Difference Cover Sample
///
struct DCS
{
    static const uint32 Q = 64;         // DC period
    static const uint32 N = 9;          // DC quorum

    typedef DCSView     plain_view_type;

    /// constructor
    ///
    DCS();

    thrust::device_vector<uint32> d_dc;         ///< difference cover table
    thrust::device_vector<uint32> d_lut;        ///< the (i,j) -> l LUT
    thrust::device_vector<uint32> d_pos;        ///< the DC -> pos mapping
    thrust::device_vector<uint8>  d_bitmask;    ///< difference cover bitmask
    thrust::device_vector<uint32> d_ranks;      ///< ordered DCS ranks
};

/// return the plain view of a DCS
///
inline DCSView plain_view(DCS& dcs)
{
    return DCSView(
        uint32( dcs.d_ranks.size() ),
        nvbio::plain_view( dcs.d_dc ),
        nvbio::plain_view( dcs.d_lut ),
        nvbio::plain_view( dcs.d_pos ),
        nvbio::plain_view( dcs.d_bitmask ),
        nvbio::plain_view( dcs.d_ranks ) );
}

/// return the plain view of a DCS
///
inline DCSView plain_view(const DCS& dcs)
{
    return plain_view( const_cast<DCS&>( dcs ) );
}

namespace priv {

/// A functor to evaluate whether an index is in a Difference Cover Sample
///
template <uint32 Q> // DC cover period
struct DCS_predicate
{
    typedef uint32 argument_type;
    typedef uint32 result_type;

    /// constructor
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    DCS_predicate(const uint8* _dc_bitmask) : dc_bitmask(_dc_bitmask) {}

    /// return whether the given integer is the DC
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    result_type operator() (const uint32 suffix) const { return (dc_bitmask[ suffix % Q ] != 0); }

    const uint8* dc_bitmask;
};

/// A functor to transform a global suffix index into a DCS-local index
///
struct DCS_string_suffix_index
{
    typedef uint32   argument_type;
    typedef uint32   result_type;

    DCS_string_suffix_index(const DCSView _dcs) : dcs( _dcs ) {}

    /// return true if the first suffix is lexicographically smaller than the second, false otherwise
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    result_type operator() (const uint32 suffix_idx) const
    {
        return nvbio::min( dcs.index( suffix_idx ), dcs.size );
    }

    const DCSView dcs;
};

/// A binary functor comparing two suffixes lexicographically using a Difference Cover Sample
///
template <uint32 SYMBOL_SIZE, typename string_type>
struct DCS_string_suffix_less
{
    typedef uint32   first_argument_type;
    typedef uint32   second_argument_type;
    typedef uint32   result_type;

    /// constructor
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    DCS_string_suffix_less(
        const uint64        _string_len,
        const string_type   _string,
        const DCSView       _dcs) :
        string_len(_string_len),
        string(_string),
        dcs( _dcs ) {}

    /// return true if the first suffix is lexicographically smaller than the second, false otherwise
    ///
    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    result_type operator() (const uint64 suffix_idx1, const uint64 suffix_idx2) const
    {
      //#define DCS_CHECKS
      #if defined(DCS_CHECKS)
        const string_suffix_less<SYMBOL_SIZE,string_type> less( string_len, string );
        const bool r  = less( suffix_idx1, suffix_idx2 );
      #endif

        const uint32 WORD_BITS   = 32u; // use 32-bit words
        const uint32 DOLLAR_BITS = 4u;  // 4 is the minimum number needed to encode up to 16 symbols per word
        const uint32 SYMBOLS_PER_WORD = symbols_per_word<SYMBOL_SIZE,WORD_BITS,DOLLAR_BITS>();

        const uint32 suffix_len1 = string_len - suffix_idx1;
        const uint32 suffix_len2 = string_len - suffix_idx2;
        const uint32 q_words = (DCS::Q + SYMBOLS_PER_WORD-1) / SYMBOLS_PER_WORD;
        const uint32 n_words = nvbio::min(
            uint32( nvbio::min(
                suffix_len1,
                suffix_len2 ) + SYMBOLS_PER_WORD-1 ) / SYMBOLS_PER_WORD,
                q_words );

        // loop through all string-words
        for (uint32 w = 0; w < n_words; ++w)
        {
            string_suffix_word_functor<SYMBOL_SIZE,WORD_BITS,DOLLAR_BITS,string_type,uint32> word_functor( string_len, string, w );

            const uint32 w1 = word_functor( suffix_idx1 );
            const uint32 w2 = word_functor( suffix_idx2 );
            if (w1 < w2) return true;
            if (w1 > w2) return false;
        }

        // check whether the suffixes are shorter than Q - this should never happen...
        if (suffix_len1 < DCS::Q ||
            suffix_len2 < DCS::Q)
        {
            #if defined(DCS_CHECKS)
            const bool r2 = suffix_len1 < suffix_len2;
            if (r != r2)
                printf("short suffixes %u, %u\n", suffix_len1, suffix_len2 );
            #endif

            return suffix_len1 < suffix_len2;
        }

        // compare the DCS ranks
        {
            const uint32 i_mod_Q = suffix_idx1 % DCS::Q;
            const uint32 j_mod_Q = suffix_idx2 % DCS::Q;

            // lookup the smallest number l such that (i + l) and (j + l) are in the DCS
            const uint32 l = dcs.lut[ i_mod_Q * DCS::Q + j_mod_Q ];

            // by construction (suffix_idx1 + l) and (suffix_idx2 + l) are both in the DCS,
            // we just need to find exactly where...
            const uint32 pos_i = dcs.index( suffix_idx1 + l );
            const uint32 pos_j = dcs.index( suffix_idx2 + l );

            // now we can lookup the ranks of the suffixes in the DCS
            const uint32 rank_i = dcs.ranks[ pos_i ];
            const uint32 rank_j = dcs.ranks[ pos_j ];

            #if defined(DCS_CHECKS)
            const bool r2 = rank_i < rank_j;
            if (r != r2)
            {
                printf("(%u,%u) : %u != %u, l[%u], pos[%u,%u], rank[%u,%u]\n",
                    (uint32)suffix_idx1, (uint32)suffix_idx2,
                    (uint32)r,
                    (uint32)r2,
                    (uint32)l,
                    (uint32)pos_i,
                    (uint32)pos_j,
                    (uint32)rank_i,
                    (uint32)rank_j);
            }
            #endif

            return rank_i < rank_j;
        }
    }

    const uint64        string_len;
    const string_type   string;
    const DCSView       dcs;
};

} // namespace priv
} // namespace nvbio

#include <nvbio/sufsort/dcs_inl.h>
