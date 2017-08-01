/*
 *   BSD LICENSE
 *
 *   Copyright (C) 2016 LightBits Labs Ltd. - All Rights Reserved
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of LightBits Labs Ltd nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "percentile.h"
#include <memory.h>
#include <string.h>
#include <assert.h>

/*
 * Given a number, return the index of the corresponding bucket in
 * the structure tracking percentiles.
 *
 * (1) find the group (and error bits) that the value
 * belongs to by looking at its MSB. (2) find the bucket number in the
 * group by looking at the index bits.
 *
 */
static unsigned int percentile_value_to_index(uint32_t val)
{
	unsigned int msb, error_bits, base, offset, idx;

	/* Find MSB starting from bit 0 */
	if (val == 0)
		msb = 0;
	else
		msb = (sizeof(val)*8) - __builtin_clz(val) - 1;

	/*
	 * MSB <= (PROCSTAT_BUCKET_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index
	 */
	if (msb <= PROCSTAT_BUCKET_BITS)
		return (unsigned int)val;

	/* Compute the number of error bits to discard*/
	error_bits = msb - PROCSTAT_BUCKET_BITS;

	/* Compute the number of buckets before the group */
	base = (error_bits + 1) << PROCSTAT_BUCKET_BITS;

	/*
	 * Discard the error bits and apply the mask to find the
	 * index for the buckets in the group
	 */
	offset = (PROCSTAT_BUCKET_VALUES - 1) & (val >> error_bits);

	/* Make sure the index does not exceed (array size - 1) */
	idx = (base + offset) < (PROCSTAT_PERCENTILE_ARR_NR - 1) ?
		(base + offset) : (PROCSTAT_PERCENTILE_ARR_NR - 1);

	return idx;
}


/*
 * Convert the given index of the bucket array to the value
 * represented by the bucket
 */
uint32_t procstat_percentile_idx_to_val(unsigned int idx)
{
	unsigned int error_bits, k, base;

	assert(idx < PROCSTAT_PERCENTILE_ARR_NR);

	/* MSB <= (FIO_IO_U_PLAT_BITS-1), cannot be rounded off. Use
	 * all bits of the sample as index */
	if (idx < (PROCSTAT_BUCKET_VALUES << 1))
		return idx;

	/* Find the group and compute the minimum value of that group */
	error_bits = (idx >> PROCSTAT_BUCKET_BITS) - 1;
	base = 1 << (error_bits + PROCSTAT_BUCKET_BITS);

	/* Find its bucket number of the group */
	k = idx % PROCSTAT_BUCKET_VALUES;

	/* Return the mean of the range of the bucket */
	return base + ((k + 0.5) * (1 << error_bits));
}


void procstat_hist_add_point(uint32_t *histogram, uint32_t value)
{
	unsigned int index = percentile_value_to_index(value);

	++histogram[index];
}

void procstat_percentile_calculate(uint32_t *histogram,
				   uint64_t samples_count,
				   struct procstat_percentile_result *result,
				   unsigned result_len)
{
	unsigned long num_points = 0;
	unsigned int  i, j = 0;


	for (i = 0; i < PROCSTAT_PERCENTILE_ARR_NR && j < result_len; ++i) {
		num_points += histogram[i];

		/* several percentiles might be anwered with same bucket*/
		while (num_points >= result[j].fraction * samples_count) {
			assert(result[j].fraction <= 1.0);
			result[j].value = procstat_percentile_idx_to_val(i);

			++j;
			if (j == result_len)
				break;
		}
	}
}
