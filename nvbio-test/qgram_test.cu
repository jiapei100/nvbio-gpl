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

// qgram_test.cu
//
//#define CUFMI_CUDA_DEBUG
//#define CUFMI_CUDA_ASSERTS

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <nvbio/basic/timer.h>
#include <nvbio/basic/console.h>
#include <nvbio/basic/vector_wrapper.h>
#include <nvbio/basic/packedstream.h>
#include <nvbio/basic/shared_pointer.h>
#include <nvbio/io/reads/reads.h>
#include <nvbio/io/fmi.h>
#include <nvbio/qgram/qgram.h>
#include <nvbio/qgram/qgroup.h>

namespace nvbio {

// return the size of a given range
struct range_size
{
    typedef uint2  argument_type;
    typedef uint32 result_type;

    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 operator() (const uint2 range) const { return range.y - range.x; }
};

// return 1 for non-empty ranges, 0 otherwise
struct valid_range
{
    typedef uint2  argument_type;
    typedef uint32 result_type;

    NVBIO_FORCEINLINE NVBIO_HOST_DEVICE
    uint32 operator() (const uint2 range) const { return range.y - range.x > 0 ? 1u : 0u; }
};

int qgram_test(int argc, char* argv[])
{
    uint32 n_qgrams  = 10000000;
    uint32 n_queries = 10000000;
    char*  reads = "./data/SRR493095_1.fastq.gz";
    char*  index = "./data/human.NCBI36/Homo_sapiens.NCBI36.53.dna.toplevel.fa";

    for (int i = 0; i < argc; ++i)
    {
        if (strcmp( argv[i], "-qgrams" ) == 0)
            n_qgrams = atoi( argv[++i] )*1000;
        else if (strcmp( argv[i], "-queries" ) == 0)
            n_queries = atoi( argv[++i] )*1000;
        else if (strcmp( argv[i], "-reads" ) == 0)
            reads = argv[++i];
        else if (strcmp( argv[i], "-index" ) == 0)
            index = argv[++i];
    }

    log_info(stderr, "q-gram test... started\n");

    const io::QualityEncoding qencoding = io::Phred33;

    log_info(stderr, "  loading reads... started\n");

    SharedPointer<io::ReadDataStream> read_data_file(
        io::open_read_file(
            reads,
            qencoding,
            uint32(-1),
            uint32(-1) ) );

    if (read_data_file == NULL || read_data_file->is_ok() == false)
    {
        log_error(stderr, "    failed opening file \"%s\"\n", reads);
        return 1u;
    }

    const uint32 batch_size = uint32(-1);
    const uint32 batch_bps  = n_qgrams;

    // load a batch of reads
    SharedPointer<io::ReadData> h_read_data( read_data_file->next( batch_size, batch_bps ) );
    
    // build its device version
    io::ReadDataCUDA d_read_data( *h_read_data );

    log_info(stderr, "  loading reads... done\n");

    // fetch the actual string
    typedef io::ReadData::const_read_stream_type string_type;

    const uint32      string_len = d_read_data.bps();
    const string_type string     = string_type( d_read_data.read_stream() );

    log_info(stderr, "    symbols: %.1f M symbols\n", 1.0e-6f * float(string_len));

    io::FMIndexDataRAM fmi;
    if (!fmi.load( index, io::FMIndexData::GENOME ))
    {
        log_error(stderr, "    failed loading index \"%s\"\n", index);
        return 1u;
    }

    // build its device version
    const io::FMIndexDataCUDA fmi_cuda( fmi, io::FMIndexDataCUDA::GENOME );

    typedef io::FMIndexData::stream_type genome_type;

    const uint32      genome_len = fmi_cuda.genome_length();
    const genome_type genome( fmi_cuda.genome_stream() );

    // build the q-grams
    thrust::device_vector<uint64> d_qgrams( n_queries );
    thrust::transform(
        thrust::make_counting_iterator<uint32>(0u),
        thrust::make_counting_iterator<uint32>(0u) + n_queries,
        d_qgrams.begin(),
        string_qgram_functor<genome_type>( 16u, 2u, genome_len, genome ) );

    // sort the q-grams
    thrust::device_vector<uint64> d_sorted_qgrams( d_qgrams );
    thrust::sort( d_sorted_qgrams.begin(), d_sorted_qgrams.end() );

    // prepare a vector to store the query results
    thrust::device_vector<uint2>  d_ranges( n_queries );

    // and start testing...
    {
        log_info(stderr, "  building q-gram index... started\n");

        // build the q-gram index
        QGramIndexDevice qgram_index;

        Timer timer;
        timer.start();

        qgram_index.build(
            16u,            // q-gram size
            2u,             // implicitly convert N to A
            string_len,
            string,
            12u );

        cudaDeviceSynchronize();
        timer.stop();
        const float time = timer.seconds();

        log_info(stderr, "  building q-gram index... done\n");
        log_info(stderr, "    unique q-grams : %.2f M q-grams\n", 1.0e-6f * float( qgram_index.n_unique_qgrams ));
        log_info(stderr, "    throughput     : %.1f M q-grams/s\n", 1.0e-6f * float( string_len ) / time);
        log_info(stderr, "    memory usage   : %.1f MB\n", float( qgram_index.used_device_memory() ) / float(1024*1024) );

        log_info(stderr, "  querying q-gram index... started\n");

        timer.start();

        // and search the genome q-grams in the index
        thrust::transform(
            d_qgrams.begin(),
            d_qgrams.begin() + n_queries,
            d_ranges.begin(),
            nvbio::plain_view( qgram_index ) );

        cudaDeviceSynchronize();
        timer.stop();
        const float unsorted_time = timer.seconds();

        // and now repeat the same operation with the sorted q-grams
        timer.start();

        // and search the genome q-grams in the index
        thrust::transform(
            d_sorted_qgrams.begin(),
            d_sorted_qgrams.begin() + n_queries,
            d_ranges.begin(),
            nvbio::plain_view( qgram_index ) );

        cudaDeviceSynchronize();
        timer.stop();
        const float sorted_time = timer.seconds();

        const uint32 n_occurrences = thrust::reduce(
            thrust::make_transform_iterator( d_ranges.begin(), range_size() ),
            thrust::make_transform_iterator( d_ranges.begin(), range_size() ) + n_queries );

        const uint32 n_matches = thrust::reduce(
            thrust::make_transform_iterator( d_ranges.begin(), valid_range() ),
            thrust::make_transform_iterator( d_ranges.begin(), valid_range() ) + n_queries );

        log_info(stderr, "  querying q-gram index... done\n");
        log_info(stderr, "    unsorted throughput : %.2f B q-grams/s\n", (1.0e-9f * float( n_queries )) / unsorted_time);
        log_info(stderr, "    sorted   throughput : %.2f B q-grams/s\n", (1.0e-9f * float( n_queries )) / sorted_time);
        log_info(stderr, "    matches             : %.2f M\n", 1.0e-6f * float( n_matches ) );
        log_info(stderr, "    occurrences         : %.2f M\n", 1.0e-6f * float( n_occurrences ) );
    }
    {
        log_info(stderr, "  building q-group index... started\n");

        // build the q-group index
        QGroupIndexDevice qgroup_index;

        Timer timer;
        timer.start();

        qgroup_index.build(
            16u,            // q-gram size
            2u,             // implicitly convert N to A
            string_len,
            string );

        cudaDeviceSynchronize();
        timer.stop();
        const float time = timer.seconds();

        log_info(stderr, "  building q-group index... done\n");
        log_info(stderr, "    unique q-grams : %.2f M q-grams\n", 1.0e-6f * float( qgroup_index.n_unique_qgrams ));
        log_info(stderr, "    throughput     : %.1f M q-grams/s\n", 1.0e-6f * float( string_len ) / time);
        log_info(stderr, "    memory usage   : %.1f MB\n", float( qgroup_index.used_device_memory() ) / float(1024*1024) );

        log_info(stderr, "  querying q-group index... started\n");

        timer.start();

        // and search the genome q-grams in the index
        thrust::transform(
            d_qgrams.begin(),
            d_qgrams.begin() + n_queries,
            d_ranges.begin(),
            nvbio::plain_view( qgroup_index ) );

        cudaDeviceSynchronize();
        timer.stop();
        const float unsorted_time = timer.seconds();

        // and now repeat the same operation with the sorted q-grams
        timer.start();

        // and search the genome q-grams in the index
        thrust::transform(
            d_sorted_qgrams.begin(),
            d_sorted_qgrams.begin() + n_queries,
            d_ranges.begin(),
            nvbio::plain_view( qgroup_index ) );

        cudaDeviceSynchronize();
        timer.stop();
        const float sorted_time = timer.seconds();

        const uint32 n_occurrences = thrust::reduce(
            thrust::make_transform_iterator( d_ranges.begin(), range_size() ),
            thrust::make_transform_iterator( d_ranges.begin(), range_size() ) + n_queries );

        const uint32 n_matches = thrust::reduce(
            thrust::make_transform_iterator( d_ranges.begin(), valid_range() ),
            thrust::make_transform_iterator( d_ranges.begin(), valid_range() ) + n_queries );

        log_info(stderr, "  querying q-group index... done\n");
        log_info(stderr, "    unsorted throughput : %.2f B q-grams/s\n", (1.0e-9f * float( n_queries )) / unsorted_time);
        log_info(stderr, "    sorted   throughput : %.2f B q-grams/s\n", (1.0e-9f * float( n_queries )) / sorted_time);
        log_info(stderr, "    matches             : %.2f M\n", 1.0e-6f * float( n_matches ) );
        log_info(stderr, "    occurrences         : %.2f M\n", 1.0e-6f * float( n_occurrences ) );
    }

    log_info(stderr, "q-gram test... done\n" );
    return 0;
}

} // namespace nvbio
