/*
 * nvbio
 * Copyright (C) 2012-2014, NVIDIA Corporation
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

#include <nvbio/basic/types.h>
#include <nvbio/io/alignments.h>

#include <nvbio/io/sequence/sequence.h>
#include <nvbio/basic/vector_array.h>

#include <thrust/host_vector.h>
#include <thrust/device_vector.h>

#include <nvbio/io/output/output_utils.h>

namespace nvbio {
namespace io {

/**
   \page output_io_page Output
   This module implements alignment data output in various formats.

   The interface consists of a single method call that takes GPU buffers post-alignment and accumulates the results in host memory until we have enough data to write out a batch to disk. Additional method calls are used to signal the start/end of a batch of alignment data, as well as to configure the output. The main interface for this module is the OutputFile class, which exposes most of the functionality.

   The following classes are exposed as the interface to this module:

   - OutputFile
   - DeviceCigarArray
   - MapQEvaluator
   - GPUOutputBatch


   @addtogroup IO
   @{

   @addtogroup Output
   @{
*/

// this was used by mapq.h in bowtie2, but seems to no longer be used anywhere
//struct MapQInfo
//{
//    int32 best;
//    int32 second_best;
//};

/// Encapsulates all sequence data required for alignment.
/// This is a simple convenience wrapper for FMIndexData.
struct BNT
{
    uint32          n_seqs;
    const char*     names;
    const uint32*   names_index;
    const uint32*   sequence_index;

    BNT(const io::ConstSequenceDataView& reference)
        : n_seqs( reference.size() ),
          names( reference.name_stream() ),
          names_index( reference.name_index() ),
          sequence_index( reference.sequence_index() )
    {}
};

/// Helper enum to identify the type of alignment we're doing
typedef enum {
    SINGLE_END,
    PAIRED_END
} AlignmentType;

/// Helper enum to identify a mate in an alignment.
/// Note that these are used as indices within BestAlignments.
typedef enum {
    MATE_1 = 0,
    MATE_2 = 1,
} AlignmentMate;

/// Helper enum to identify a scoring pass.
typedef enum {
    BEST_SCORE,
    SECOND_BEST_SCORE,
} AlignmentScore;

/// Encapsulates the best and second-best alignments.
struct AlignmentResult
{
    /// Best alignment for both mates. Index 0 is the first mate, index 1 is the second mate
    Alignment best[2];
    /// Second-best alignment for both mates (same indexing)
    Alignment second_best[2];

    /// Set to true if this is a paired-end alignment
    bool is_paired_end;

    AlignmentResult();
};

/// Wrapper struct to keep CIGAR arrays and CIGAR coords in one place. This is the device version.
struct DeviceCigarArray
{
    nvbio::DeviceVectorArray<io::Cigar>&       array;
    thrust::device_vector<uint2>&              coords;

    DeviceCigarArray(nvbio::DeviceVectorArray<io::Cigar>& array,
                     thrust::device_vector<uint2>&        coords)
        : array(array),
          coords(coords)
    { }
};

/// Wrapper struct to keep CIGAR arrays and CIGAR coords in one place (host version).
struct HostCigarArray
{
    nvbio::HostVectorArray<io::Cigar> array;
    thrust::host_vector<uint2>              coords;
};

/// The type for the MDS array in the host
typedef nvbio::HostVectorArray<uint8> HostMdsArray;

/// Utility struct to gather all data related to a given alignment.
/// This breaks out the alignment data for a given alignment by setting up
/// pointers to the relevant bits of data pulled from the GPU buffers.
struct AlignmentData
{
    typedef io::SequenceDataHost                            read_data_type;
    typedef io::SequenceDataAccess<DNA_N>                   read_access_type;
    typedef read_access_type::sequence_stream_type          read_type;

    /// Set to true if this is a valid alignment
    bool valid;

    /// The alignment itself
    const Alignment *best;
    /// The second-best alignment
    const Alignment *second_best;
    /// The read id of this alignment within the batch
    uint32 read_id_p;

    // Pointers to the read_data, cigar and mds arrays for this read.
    // These are not really meant to be used outside AlignmentData and
    // should probably be removed

    const io::SequenceDataHost          *read_data_batch_p;
    const HostCigarArray                *cigar_array_p;
    const HostMdsArray                  *mds_array_p;

    // the remaining fields are derived from best, read_data_batch,
    // cigar_array and mds_array in the ctor
    // they are commonly used when writing alignment data out to disk

    /// the offset of the read from the start of read_data
    uint32 read_offset;
    /// length of the read
    uint32 read_len;
    /// read name
    const char *read_name;

    /// The iterator for the read data, acts as an array of uint8
    read_type read_data;
    /// quality data
    const char *qual;

    /// CIGAR for this alignment
    const Cigar *cigar;
    /// The position of the cigar in the cigar array for this batch
    /// (should really not be here, only used to get the BNT)
    uint32 cigar_pos;
    /// CIGAR length (in BPs? need to clarify)
    uint32 cigar_len;

    /// MDS vector
    const uint8 *mds_vec;

    AlignmentData()
        : valid(false),
          best(NULL),
          second_best(NULL),
          read_id_p(0xffffffff),
          read_data_batch_p(NULL),
          cigar_array_p(NULL),
          mds_array_p(NULL),
          read_offset(0xffffffff),
          read_len(0xffffffff),
          read_name(NULL),
          qual(NULL),
          cigar(NULL),
          cigar_pos(0xffffffff),
          cigar_len(0xffffffff)
    {
    }

    AlignmentData(const Alignment *best,
                  const Alignment *second_best,
                  uint32 read_id,
                  const io::SequenceDataHost *read_data_batch,
                  const HostCigarArray *cigar_array,
                  const HostMdsArray *mds_array)
        : valid(true),
          best(best),
          second_best(second_best),
          read_id_p(read_id),
          read_data_batch_p(read_data_batch),
          cigar_array_p(cigar_array),
          mds_array_p(mds_array)
    {
        uint2 cigar_coord;

        read_access_type read_data_access( *read_data_batch );

        read_offset = read_data_access.sequence_index()[read_id_p];
        read_len    = read_data_access.sequence_index()[read_id_p + 1] - read_offset;
        read_name   = read_data_access.name_stream() + read_data_access.name_index()[read_id];

        read_data   = read_data_access.sequence_stream() + read_offset;
        qual        = read_data_access.qual_stream() + read_offset;

        cigar       = cigar_array_p->array[read_id_p];
        cigar_coord = cigar_array_p->coords[read_id_p];
        cigar_pos   = compute_cigar_pos(cigar_coord.x, best->alignment());
        cigar_len   = cigar_coord.y;

        mds_vec = (*mds_array)[read_id_p];
    }

    static AlignmentData invalid(void)
    {
        return AlignmentData();
    }
};

/// MapQ evaluator interface
struct MapQEvaluator
{
    virtual ~MapQEvaluator() { }

    /// Callback to evaluate the mapping quality for a given alignment. Note that this could be called
    /// concurrently from multiple threads.
    /// \param [in] alignment The alignment to compute mapping quality for
    /// \param [in] mate The mate of alignment (can be invalid for single end reads)
    /// \return An int representing the mapping quality for this alignment
    virtual int compute_mapq(const AlignmentData& alignment,
                             const AlignmentData& mate) const = 0;

};

/**
   @} // Output
   @} // IO
*/

} // namespace io
} // namespace nvbio
