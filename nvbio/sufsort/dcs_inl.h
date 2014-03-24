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

namespace nvbio {

// a utility SuffixHandler to rank the sorted suffixes
//
struct DCSSuffixRanker
{
    // constructor
    //
    DCSSuffixRanker(DCSView _dcs) : dcs( _dcs ), n_output(0) {}

    // process the next batch of suffixes
    //
    void process_batch(
        const uint32  n_suffixes,
        const uint32* d_suffixes)
    {
        // essentially, invert the suffix array
        thrust::scatter(
            thrust::make_counting_iterator<uint32>( n_output ),
            thrust::make_counting_iterator<uint32>( n_output ) + n_suffixes,
            thrust::make_transform_iterator(
                thrust::device_ptr<const uint32>( d_suffixes ),
                priv::DCS_string_suffix_index( dcs ) ),         // localize the index to the DCS
            thrust::device_ptr<uint32>( dcs.ranks ) );

        n_output += n_suffixes;
    }

    // process a sparse set of suffixes
    //
    void process_scattered(
        const uint32  n_suffixes,
        const uint32* d_suffixes,
        const uint32* d_slots)
    {
        // essentially, invert the suffix array
        thrust::scatter(
            thrust::device_ptr<const uint32>( d_slots ),
            thrust::device_ptr<const uint32>( d_slots ) + n_suffixes,
            thrust::make_transform_iterator(
                thrust::device_ptr<const uint32>( d_suffixes ),
                priv::DCS_string_suffix_index( dcs ) ),         // localize the index to the DCS
            thrust::device_ptr<uint32>( dcs.ranks ) );
    }

    const DCSView   dcs;
    uint32          n_output;
};

// return the sampled position of a given suffix index
//
NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
uint32 DCSView::index(const uint32 i) const
{
    const uint32 block_i = i / DCS::Q;
    const uint32 mod_i   = i % DCS::Q;

    return block_i * DCS::N + pos[ mod_i ];
}

// constructor
//
inline DCS::DCS()
{
    // build a table for our Difference Cover
    //const uint32 h_dc[]  = { 1, 2, 3, 6, 13, 28, 37, 39, 45, 53, 66, 94 };   // q = 111
    const uint32  h_dc[] = { 1, 2, 3, 6, 15, 17, 35, 43, 60 };                // q = 64

    thrust::host_vector<uint8>    h_bitmask( Q, 0u );
    thrust::host_vector<uint32>   h_lut( Q*Q, 0u );
    thrust::host_vector<uint32>   h_pos( Q, 0u );

    // build the DC bitmask
    thrust::scatter(
        thrust::make_constant_iterator<uint32>(1u),
        thrust::make_constant_iterator<uint32>(1u) + N,
        h_dc,
        h_bitmask.begin() );

    // build the DC position table, mapping each entry in DC to its position (q -> i | DC[i] = q)
    thrust::scatter(
        thrust::make_counting_iterator<uint32>(0u),
        thrust::make_counting_iterator<uint32>(0u) + N,
        h_dc,
        h_pos.begin() );

    // build the LUT (i,j) -> l | [(i + l) in DC && (j + l) in DC]
    for (uint32 i = 0; i < Q; ++i)
    {
        for (uint32 j = 0; j < Q; ++j)
        {
            for (uint32 l = 0; l < Q; ++l)
            {
                if (h_bitmask[ (i + l) % Q ] &&
                    h_bitmask[ (j + l) % Q ])
                {
                    h_lut[ i * Q + j ] = l;
                    break;
                }
                if (l == Q-1)
                    throw nvbio::logic_error("DCS: could not find a period for (%u,%u)!\n", i, j);
            }
        }
    }

    // copy all tables to the device
    d_dc.resize( N );
    thrust::copy(
        h_dc,
        h_dc + N,
        d_dc.begin() );

    d_lut     = h_lut;
    d_pos     = h_pos;
    d_bitmask = h_bitmask;
}

} // namespace nvbio