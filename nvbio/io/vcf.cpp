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

// loader for variant call format files, version 4.2

#include <nvbio/basic/console.h>
#include <nvbio/io/vcf.h>
#include <nvbio/io/bufferedtextfile.h>
#include <nvbio/basic/dna.h>

#include <stdlib.h>
#include <string.h>

namespace nvbio {
namespace io {

// parse the INFO field looking for an END tag
// INFO is a set of ID=val entries separated by semicolons
// returns false if a parse error occurs
static bool get_end_position(uint32 *out, char *info)
{
    char *sc, *eq;

    do {
        // search for the next semi-colon
        sc = strchr(info, ';');
        if (sc)
        {
            // null it out
            *sc = '\0';
        }

        // now search for the next equal sign
        eq = strchr(info, '=');
        if (!eq)
        {
            // no equal sign, malformed header
            return false;
        }

        // zero out the equal sign
        *eq = 0;

        // check the key name
        if (strcmp(info, "END") == 0)
        {
            // parse the END value
            char *endptr = NULL;
            uint32 position = strtoll(eq + 1, &endptr, 10);
            if (!endptr || endptr == eq || *endptr != '\0')
            {
                return false;
            }

            *out = position;
            return true;
        }

        if (sc)
        {
            info = sc + 1;
        } else {
            info = NULL;
        }
    } while (info && *info);

    return true;
}

// loads a VCF 4.2 file, appending the data to output
bool loadVCF(SNPDatabase& output, const char *file_name)
{
    BufferedTextFile file(file_name);
    char *line, *end;
    uint32 line_counter = 0;

    while((line = file.next_record(&end)))
    {
        line_counter++;
        *end = '\0';

        // strip out comments
        char *comment = strchr(line, '#');
        if (comment)
            *comment = '\0';

        // skip all leading whitespace
        while (*line == ' ' || *line == '\t' || *line == '\r')
        {
            line++;
        }

        if (*line == '\0')
        {
            // empty line, skip
            continue;
        }

        // parse the entries in each record
        char *chrom  = NULL;
        char *pos    = NULL;
        char *id     = NULL;
        char *ref    = NULL;
        char *alt    = NULL;
        char *qual   = NULL;
        char *filter = NULL;
        char *info   = NULL;

// ugly macro to tokenize the string based on strchr
#define NEXT(prev, next)                        \
    {                                           \
        if (prev)                               \
        {                                       \
            next = strchr(prev, '\t');          \
            if (next)                           \
            {                                   \
                *next = '\0';                   \
                next++;                         \
            }                                   \
        }                                       \
    }

        chrom = line;
        NEXT(chrom, pos);
        NEXT(pos, id);
        NEXT(id, ref);
        NEXT(ref, alt);
        NEXT(alt, qual);
        NEXT(qual, filter);
        NEXT(filter, info);

        if (!chrom || !pos || !id || !ref || !alt || !qual || !filter)
        {
            log_error(stderr, "Error parsing VCF file (line %d): incomplete variant\n", line_counter);
            return false;
        }

#undef NEXT

        // convert position and quality
        char *endptr = NULL;
        uint32 position = strtoll(pos, &endptr, 10);
        if (!endptr || endptr == pos || *endptr != '\0')
        {
            log_error(stderr, "VCF file error (line %d): invalid position\n", line_counter);
            return false;
        }

        uint8 quality;
        if (*qual == '.')
        {
            quality = 0xff;
        } else {
            quality = (uint8) strtol(qual, &endptr, 10);
            if (!endptr || endptr == qual || *endptr != '\0')
            {
                log_warning(stderr, "VCF file error (line %d): invalid quality\n", line_counter);
                quality = 0xff;
            }
        }

        uint32 stop = position + strlen(ref);
        // parse the info header looking for a stop position
        if (info)
        {
            bool ret;
            ret = get_end_position(&stop, info);
            if (ret == false)
            {
                log_warning(stderr, "VCF file error (line %d): error parsing INFO line\n", line_counter);
                return false;
            }
        }

        // add an entry for each possible variant listed in this record
        do {
            char *next_base = strchr(alt, ',');
            if (next_base)
                *next_base = '\0';

            char *var;
            // if this is a called monomorphic variant (i.e., a site which has been identified as always having the same allele)
            // we store the reference string as the variant
            if (strcmp(alt, ".") == 0)
                var = ref;
            else
                var = alt;

            const uint32 ref_len = strlen(ref);
            const uint32 var_len = strlen(var);

            SNP_sequence_index index(output.reference_sequences.size(), ref_len,
                                     output.variants.size(), var_len);
            output.ref_variant_index.push_back(index);

            output.reference_sequence_names.push_back(std::string(chrom));
            output.sequence_positions.push_back(make_uint2(position, stop));

            output.reference_sequences.resize(index.reference_start + ref_len);
            string_to_iupac16(ref, output.reference_sequences.begin() + index.reference_start);

            output.variants.resize(index.variant_start + var_len);
            string_to_iupac16(var, output.variants.begin() + index.variant_start);

            output.variant_qualities.push_back(quality);

            if (next_base)
                alt = next_base + 1;
            else
                alt = NULL;
        } while (alt && *alt != '\0');
    }

    return true;
}

} // namespace io
} // namespace nvbio
