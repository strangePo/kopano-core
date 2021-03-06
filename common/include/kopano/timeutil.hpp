/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright 2005 - 2016 Zarafa and its licensors
 */
#pragma once
#include <kopano/zcdefs.h>
#include <chrono>
#include <ctime>

namespace KC {

using time_point = std::chrono::time_point<std::chrono::steady_clock>;
using time_duration = std::chrono::steady_clock::duration;

/* MAPI TimeZoneStruct named property */
struct TIMEZONE_STRUCT {
	// The bias values (bias, stdbias and dstbias) are the opposite of what you expect.
	// Thus +1 hour becomes -60, +2 hours becomes -120, -3 becomes +180
	LONG lBias;					/* nl: -1*60, jp: -9*60 */
	LONG lStdBias;				/* nl: 0, jp: 0 (wintertijd) */
	LONG lDstBias;				/* nl: -1*60: jp: 0 (zomertijd) */
	WORD wStdYear;
	SYSTEMTIME stStdDate;		/* 2->3, dus 3 in wHour */
	WORD wDstYear;
	SYSTEMTIME stDstDate;		/* 3->2, dus 2 in wHour */

	void le_to_cpu()
	{
		lBias = le32_to_cpu(lBias);
		lStdBias = le32_to_cpu(lStdBias);
		lDstBias = le32_to_cpu(lDstBias);
		wStdYear = le16_to_cpu(wStdYear);
		KC::le_to_cpu(stStdDate);
		wDstYear = le16_to_cpu(wDstYear);
		KC::le_to_cpu(stDstDate);
	}
};

extern KC_EXPORT time_t LocalToUTC(time_t local, const TIMEZONE_STRUCT &);
extern KC_EXPORT time_t UTCToLocal(time_t utc, const TIMEZONE_STRUCT &);
extern KC_EXPORT FILETIME UnixTimeToFileTime(time_t);
extern KC_EXPORT time_t FileTimeToUnixTime(const FILETIME &);
extern KC_EXPORT void UnixTimeToFileTime(time_t, int *hi, unsigned int *lo);
extern KC_EXPORT LONG FileTimeToRTime(const FILETIME &);
extern KC_EXPORT int FileTimeToTimestamp(const FILETIME &, time_t &, char *, size_t);
extern KC_EXPORT LONG UnixTimeToRTime(time_t);
extern KC_EXPORT time_t RTimeToUnixTime(int rtime);
extern KC_EXPORT struct tm *gmtime_safe(time_t, struct tm *);
extern KC_EXPORT double timespec2dbl(const struct timespec &);
extern bool operator==(const FILETIME &, const FILETIME &) noexcept;
extern KC_EXPORT bool operator>(const FILETIME &, const FILETIME &) noexcept;
extern bool operator>=(const FILETIME &, const FILETIME &) noexcept;
extern KC_EXPORT bool operator<(const FILETIME &, const FILETIME &) noexcept;
extern bool operator<=(const FILETIME &, const FILETIME &) noexcept;
#ifndef KC_USES_TIMEGM
/* convert struct tm to time_t in timezone UTC0 (GM time) */
extern time_t timegm(struct tm *t);
#endif

template<typename T> static constexpr inline double dur2dbl(const T &t)
{
	return std::chrono::duration_cast<std::chrono::duration<double>>(t).count();
}

} /* namespace */
