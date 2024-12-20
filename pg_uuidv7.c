#include "postgres.h"

#include "fmgr.h"
#include "port/pg_bswap.h"
#include "utils/uuid.h"
#include "utils/timestamp.h"

#include <time.h>

/*
 * Number of microseconds between unix and postgres epoch
 */
#define EPOCH_DIFF_USECS ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY)

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(uuid_generate_v7);

Datum uuid_generate_v7(PG_FUNCTION_ARGS)
{
	pg_uuid_t *uuid = palloc(UUID_LEN);
	struct timespec ts;
	uint64_t tms;

	/*
	 * Set first 48 bits to unix epoch timestamp
	 */
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not get CLOCK_REALTIME")));

	tms = ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000);

	/*
	 * Generate first 64 bits of UUID
	 * 48 bits = unix epoch timestamp in milliseconds
	 *  4 bits = version 7 [0111]
	 * 12 bits = ~250 nanoseconds precision
	 *
	 * Using an OPTIONAL sub-millisecond timestamp fraction (12 bits at maximum) as per Section 6.2 (Method 3).
	 * https://datatracker.ietf.org/doc/rfc9562/
	*/
	tms = pg_hton64(
	    (tms << 16) |
	    0x7000 |
	    /*
	     * ((((uint64_t)ts.tv_nsec << 12) / 1000000) & 0x0fff) is equivalent to
	     * ((((uint64_t)ts.tv_nsec % 1000000) * 4096) / 1000000)
	     * but faster, because it replaces a MOD function with an & operation with the same result.
	     * (<< 12) is used for a faster 4096 multiplication
	    */
	    ((((uint64_t)ts.tv_nsec << 12) / 1000000) & 0x0fff));
	memcpy(&uuid->data[0], &tms, 8);

	/*
	 * Generate last 62 random bits
	*/
	if (!pg_strong_random(&uuid->data[8], UUID_LEN - 8))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random values")));

	/*
	 * Set magic numbers for a "version 7" UUID, see
	 * https://www.ietf.org/archive/id/draft-ietf-uuidrev-rfc4122bis-00.html#name-uuid-version-7
	 */
	uuid->data[8] = (uuid->data[8] & 0x3f) | 0x80; /* 2 bit variant [10]   */

	PG_RETURN_UUID_P(uuid);
}

static uint64_t uuid_v7_to_uint64(pg_uuid_t *uuid)
{
	uint64_t ts;

	memcpy(&ts, &uuid->data[0], 6);
	ts = pg_ntoh64(ts) >> 16;
	ts = 1000 * ts - EPOCH_DIFF_USECS;

	return ts;
}

PG_FUNCTION_INFO_V1(uuid_v7_to_timestamptz);

Datum uuid_v7_to_timestamptz(PG_FUNCTION_ARGS)
{
	pg_uuid_t *uuid = PG_GETARG_UUID_P(0);
	uint64_t ts = uuid_v7_to_uint64(uuid);

	PG_RETURN_TIMESTAMPTZ(ts);
}

PG_FUNCTION_INFO_V1(uuid_v7_to_timestamp);

Datum uuid_v7_to_timestamp(PG_FUNCTION_ARGS)
{
	pg_uuid_t *uuid = PG_GETARG_UUID_P(0);
	uint64_t ts = uuid_v7_to_uint64(uuid);

	PG_RETURN_TIMESTAMP(ts);
}

static Datum uuid_uint64_to_v7(uint64_t ts, bool zero)
{
	pg_uuid_t *uuid = palloc(UUID_LEN);
	uint64_t tms;

	tms = (ts + EPOCH_DIFF_USECS) / 1000;
	tms = pg_hton64(tms << 16);
	memcpy(&uuid->data[0], &tms, 6);

	if (zero)
		memset(&uuid->data[6], 0, UUID_LEN - 6);
	else if (!pg_strong_random(&uuid->data[6], UUID_LEN - 6))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random values")));

	/*
	 * Set magic numbers for a "version 7" UUID, see
	 * https://www.ietf.org/archive/id/draft-ietf-uuidrev-rfc4122bis-00.html#name-uuid-version-7
	 */
	uuid->data[6] = (uuid->data[6] & 0x0f) | 0x70; /* 4 bit version [0111] */
	uuid->data[8] = (uuid->data[8] & 0x3f) | 0x80; /* 2 bit variant [10]   */

	PG_RETURN_UUID_P(uuid);
}

PG_FUNCTION_INFO_V1(uuid_timestamptz_to_v7);

Datum uuid_timestamptz_to_v7(PG_FUNCTION_ARGS)
{
	TimestampTz ts = PG_GETARG_TIMESTAMPTZ(0);
	bool zero = false;

	if (!PG_ARGISNULL(1))
		zero = PG_GETARG_BOOL(1);

	return uuid_uint64_to_v7(ts, zero);
}

PG_FUNCTION_INFO_V1(uuid_timestamp_to_v7);

Datum uuid_timestamp_to_v7(PG_FUNCTION_ARGS)
{
	Timestamp ts = PG_GETARG_TIMESTAMP(0);
	bool zero = false;

	if (!PG_ARGISNULL(1))
		zero = PG_GETARG_BOOL(1);

	return uuid_uint64_to_v7(ts, zero);
}
