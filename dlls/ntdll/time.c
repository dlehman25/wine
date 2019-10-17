/*
 * Nt time functions.
 *
 * RtlTimeToTimeFields, RtlTimeFieldsToTime and defines are taken from ReactOS and
 * adapted to wine with special permissions of the author. This code is
 * Copyright 2002 Rex Jolliff (rex@lvcablemodem.com)
 *
 * Copyright 1999 Juergen Schmied
 * Copyright 2007 Dmitry Timoshkov
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef __APPLE__
# include <mach/mach_time.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/exception.h"
#include "wine/unicode.h"
#include "wine/debug.h"
#include "wine/library.h"
#include "wine/list.h"
#include "ntdll_misc.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntdll);

#define SHORT_TZ_NAME_MAX 8
struct tz_name_map {
    WCHAR key_name[128];
    char short_name[SHORT_TZ_NAME_MAX];
};

static int init_tz_info(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi);

static RTL_CRITICAL_SECTION TIME_tz_section;
static RTL_CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &TIME_tz_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": TIME_tz_section") }
};
static RTL_CRITICAL_SECTION TIME_tz_section = { &critsect_debug, -1, 0, 0, 0, 0 };

#define TICKSPERSEC        10000000
#define TICKSPERMSEC       10000
#define SECSPERDAY         86400
#define SECSPERHOUR        3600
#define SECSPERMIN         60
#define MINSPERHOUR        60
#define HOURSPERDAY        24
#define EPOCHWEEKDAY       1  /* Jan 1, 1601 was Monday */
#define DAYSPERWEEK        7
#define MONSPERYEAR        12
#define DAYSPERQUADRICENTENNIUM (365 * 400 + 97)
#define DAYSPERNORMALQUADRENNIUM (365 * 4 + 1)

/* 1601 to 1970 is 369 years plus 89 leap days */
#define SECS_1601_TO_1970  ((369 * 365 + 89) * (ULONGLONG)SECSPERDAY)
#define TICKS_1601_TO_1970 (SECS_1601_TO_1970 * TICKSPERSEC)
/* 1601 to 1980 is 379 years plus 91 leap days */
#define SECS_1601_TO_1980  ((379 * 365 + 91) * (ULONGLONG)SECSPERDAY)
#define TICKS_1601_TO_1980 (SECS_1601_TO_1980 * TICKSPERSEC)


static const int MonthLengths[2][MONSPERYEAR] =
{
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};

static inline BOOL IsLeapYear(int Year)
{
    return Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
}

/* return a monotonic time counter, in Win32 ticks */
static inline ULONGLONG monotonic_counter(void)
{
    struct timeval now;

#ifdef __APPLE__
    static mach_timebase_info_data_t timebase;

    if (!timebase.denom) mach_timebase_info( &timebase );
    return mach_absolute_time() * timebase.numer / timebase.denom / 100;
#elif defined(HAVE_CLOCK_GETTIME)
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    if (!clock_gettime( CLOCK_MONOTONIC_RAW, &ts ))
        return ts.tv_sec * (ULONGLONG)TICKSPERSEC + ts.tv_nsec / 100;
#endif
    if (!clock_gettime( CLOCK_MONOTONIC, &ts ))
        return ts.tv_sec * (ULONGLONG)TICKSPERSEC + ts.tv_nsec / 100;
#endif

    gettimeofday( &now, 0 );
    return now.tv_sec * (ULONGLONG)TICKSPERSEC + now.tv_usec * 10 + TICKS_1601_TO_1970 - server_start_time;
}

/******************************************************************************
 *       RtlTimeToTimeFields [NTDLL.@]
 *
 * Convert a time into a TIME_FIELDS structure.
 *
 * PARAMS
 *   liTime     [I] Time to convert.
 *   TimeFields [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
VOID WINAPI RtlTimeToTimeFields(
	const LARGE_INTEGER *liTime,
	PTIME_FIELDS TimeFields)
{
	int SecondsInDay;
        long int cleaps, years, yearday, months;
	long int Days;
	LONGLONG Time;

	/* Extract millisecond from time and convert time into seconds */
	TimeFields->Milliseconds =
            (CSHORT) (( liTime->QuadPart % TICKSPERSEC) / TICKSPERMSEC);
	Time = liTime->QuadPart / TICKSPERSEC;

	/* The native version of RtlTimeToTimeFields does not take leap seconds
	 * into account */

	/* Split the time into days and seconds within the day */
	Days = Time / SECSPERDAY;
	SecondsInDay = Time % SECSPERDAY;

	/* compute time of day */
	TimeFields->Hour = (CSHORT) (SecondsInDay / SECSPERHOUR);
	SecondsInDay = SecondsInDay % SECSPERHOUR;
	TimeFields->Minute = (CSHORT) (SecondsInDay / SECSPERMIN);
	TimeFields->Second = (CSHORT) (SecondsInDay % SECSPERMIN);

	/* compute day of week */
	TimeFields->Weekday = (CSHORT) ((EPOCHWEEKDAY + Days) % DAYSPERWEEK);

        /* compute year, month and day of month. */
        cleaps=( 3 * ((4 * Days + 1227) / DAYSPERQUADRICENTENNIUM) + 3 ) / 4;
        Days += 28188 + cleaps;
        years = (20 * Days - 2442) / (5 * DAYSPERNORMALQUADRENNIUM);
        yearday = Days - (years * DAYSPERNORMALQUADRENNIUM)/4;
        months = (64 * yearday) / 1959;
        /* the result is based on a year starting on March.
         * To convert take 12 from Januari and Februari and
         * increase the year by one. */
        if( months < 14 ) {
            TimeFields->Month = months - 1;
            TimeFields->Year = years + 1524;
        } else {
            TimeFields->Month = months - 13;
            TimeFields->Year = years + 1525;
        }
        /* calculation of day of month is based on the wonderful
         * sequence of INT( n * 30.6): it reproduces the 
         * 31-30-31-30-31-31 month lengths exactly for small n's */
        TimeFields->Day = yearday - (1959 * months) / 64 ;
        return;
}

/******************************************************************************
 *       RtlTimeFieldsToTime [NTDLL.@]
 *
 * Convert a TIME_FIELDS structure into a time.
 *
 * PARAMS
 *   ftTimeFields [I] TIME_FIELDS structure to convert.
 *   Time         [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE.
 */
BOOLEAN WINAPI RtlTimeFieldsToTime(
	PTIME_FIELDS tfTimeFields,
	PLARGE_INTEGER Time)
{
        int month, year, cleaps, day;

	/* FIXME: normalize the TIME_FIELDS structure here */
        /* No, native just returns 0 (error) if the fields are not */
        if( tfTimeFields->Milliseconds< 0 || tfTimeFields->Milliseconds > 999 ||
                tfTimeFields->Second < 0 || tfTimeFields->Second > 59 ||
                tfTimeFields->Minute < 0 || tfTimeFields->Minute > 59 ||
                tfTimeFields->Hour < 0 || tfTimeFields->Hour > 23 ||
                tfTimeFields->Month < 1 || tfTimeFields->Month > 12 ||
                tfTimeFields->Day < 1 ||
                tfTimeFields->Day > MonthLengths
                    [ tfTimeFields->Month ==2 || IsLeapYear(tfTimeFields->Year)]
                    [ tfTimeFields->Month - 1] ||
                tfTimeFields->Year < 1601 )
            return FALSE;

        /* now calculate a day count from the date
         * First start counting years from March. This way the leap days
         * are added at the end of the year, not somewhere in the middle.
         * Formula's become so much less complicate that way.
         * To convert: add 12 to the month numbers of Jan and Feb, and 
         * take 1 from the year */
        if(tfTimeFields->Month < 3) {
            month = tfTimeFields->Month + 13;
            year = tfTimeFields->Year - 1;
        } else {
            month = tfTimeFields->Month + 1;
            year = tfTimeFields->Year;
        }
        cleaps = (3 * (year / 100) + 3) / 4;   /* nr of "century leap years"*/
        day =  (36525 * year) / 100 - cleaps + /* year * dayperyr, corrected */
                 (1959 * month) / 64 +         /* months * daypermonth */
                 tfTimeFields->Day -          /* day of the month */
                 584817 ;                      /* zero that on 1601-01-01 */
        /* done */
        
        Time->QuadPart = (((((LONGLONG) day * HOURSPERDAY +
            tfTimeFields->Hour) * MINSPERHOUR +
            tfTimeFields->Minute) * SECSPERMIN +
            tfTimeFields->Second ) * 1000 +
            tfTimeFields->Milliseconds ) * TICKSPERMSEC;

        return TRUE;
}

/***********************************************************************
 *       TIME_GetBias [internal]
 *
 * Helper function calculates delta local time from UTC. 
 *
 * PARAMS
 *   utc [I] The current utc time.
 *   pdaylight [I] Local daylight.
 *
 * RETURNS
 *   The bias for the current timezone.
 */
static LONG TIME_GetBias(void)
{
    static time_t last_utc;
    static LONG last_bias;
    LONG ret;
    time_t utc;

    utc = time( NULL );

    RtlEnterCriticalSection( &TIME_tz_section );
    if (utc != last_utc)
    {
        RTL_DYNAMIC_TIME_ZONE_INFORMATION tzi;
        int is_dst = init_tz_info( &tzi );

	last_utc = utc;
        last_bias = tzi.Bias;
        last_bias += is_dst ? tzi.DaylightBias : tzi.StandardBias;
        last_bias *= SECSPERMIN;
    }

    ret = last_bias;

    RtlLeaveCriticalSection( &TIME_tz_section );
    return ret;
}

/******************************************************************************
 *        RtlLocalTimeToSystemTime [NTDLL.@]
 *
 * Convert a local time into system time.
 *
 * PARAMS
 *   LocalTime  [I] Local time to convert.
 *   SystemTime [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlLocalTimeToSystemTime( const LARGE_INTEGER *LocalTime,
                                          PLARGE_INTEGER SystemTime)
{
    LONG bias;

    TRACE("(%p, %p)\n", LocalTime, SystemTime);

    bias = TIME_GetBias();
    SystemTime->QuadPart = LocalTime->QuadPart + bias * (LONGLONG)TICKSPERSEC;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *       RtlSystemTimeToLocalTime [NTDLL.@]
 *
 * Convert a system time into a local time.
 *
 * PARAMS
 *   SystemTime [I] System time to convert.
 *   LocalTime  [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlSystemTimeToLocalTime( const LARGE_INTEGER *SystemTime,
                                          PLARGE_INTEGER LocalTime )
{
    LONG bias;

    TRACE("(%p, %p)\n", SystemTime, LocalTime);

    bias = TIME_GetBias();
    LocalTime->QuadPart = SystemTime->QuadPart - bias * (LONGLONG)TICKSPERSEC;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *       RtlTimeToSecondsSince1970 [NTDLL.@]
 *
 * Convert a time into a count of seconds since 1970.
 *
 * PARAMS
 *   Time    [I] Time to convert.
 *   Seconds [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE, if the resulting value will not fit in a DWORD.
 */
BOOLEAN WINAPI RtlTimeToSecondsSince1970( const LARGE_INTEGER *Time, LPDWORD Seconds )
{
    ULONGLONG tmp = Time->QuadPart / TICKSPERSEC - SECS_1601_TO_1970;
    if (tmp > 0xffffffff) return FALSE;
    *Seconds = tmp;
    return TRUE;
}

/******************************************************************************
 *       RtlTimeToSecondsSince1980 [NTDLL.@]
 *
 * Convert a time into a count of seconds since 1980.
 *
 * PARAMS
 *   Time    [I] Time to convert.
 *   Seconds [O] Destination for the converted time.
 *
 * RETURNS
 *   Success: TRUE.
 *   Failure: FALSE, if the resulting value will not fit in a DWORD.
 */
BOOLEAN WINAPI RtlTimeToSecondsSince1980( const LARGE_INTEGER *Time, LPDWORD Seconds )
{
    ULONGLONG tmp = Time->QuadPart / TICKSPERSEC - SECS_1601_TO_1980;
    if (tmp > 0xffffffff) return FALSE;
    *Seconds = tmp;
    return TRUE;
}

/******************************************************************************
 *       RtlSecondsSince1970ToTime [NTDLL.@]
 *
 * Convert a count of seconds since 1970 to a time.
 *
 * PARAMS
 *   Seconds [I] Time to convert.
 *   Time    [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlSecondsSince1970ToTime( DWORD Seconds, LARGE_INTEGER *Time )
{
    Time->QuadPart = Seconds * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
}

/******************************************************************************
 *       RtlSecondsSince1980ToTime [NTDLL.@]
 *
 * Convert a count of seconds since 1980 to a time.
 *
 * PARAMS
 *   Seconds [I] Time to convert.
 *   Time    [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlSecondsSince1980ToTime( DWORD Seconds, LARGE_INTEGER *Time )
{
    Time->QuadPart = Seconds * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1980;
}

/******************************************************************************
 *       RtlTimeToElapsedTimeFields [NTDLL.@]
 *
 * Convert a time to a count of elapsed seconds.
 *
 * PARAMS
 *   Time       [I] Time to convert.
 *   TimeFields [O] Destination for the converted time.
 *
 * RETURNS
 *   Nothing.
 */
void WINAPI RtlTimeToElapsedTimeFields( const LARGE_INTEGER *Time, PTIME_FIELDS TimeFields )
{
    LONGLONG time;
    INT rem;

    time = Time->QuadPart / TICKSPERSEC;
    TimeFields->Milliseconds = (Time->QuadPart % TICKSPERSEC) / TICKSPERMSEC;

    /* time is now in seconds */
    TimeFields->Year  = 0;
    TimeFields->Month = 0;
    TimeFields->Day   = time / SECSPERDAY;

    /* rem is now the remaining seconds in the last day */
    rem = time % SECSPERDAY;
    TimeFields->Second = rem % 60;
    rem /= 60;
    TimeFields->Minute = rem % 60;
    TimeFields->Hour = rem / 60;
}

/***********************************************************************
 *       NtQuerySystemTime [NTDLL.@]
 *       ZwQuerySystemTime [NTDLL.@]
 *
 * Get the current system time.
 *
 * PARAMS
 *   time [O] Destination for the current system time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI NtQuerySystemTime( LARGE_INTEGER *time )
{
#ifdef HAVE_CLOCK_GETTIME
    struct timespec ts;
    static clockid_t clock_id = CLOCK_MONOTONIC; /* placeholder */

    if (clock_id == CLOCK_MONOTONIC)
    {
#ifdef CLOCK_REALTIME_COARSE
        struct timespec res;

        /* Use CLOCK_REALTIME_COARSE if it has 1 ms or better resolution */
        if (!clock_getres( CLOCK_REALTIME_COARSE, &res ) && res.tv_sec == 0 && res.tv_nsec <= 1000000)
            clock_id = CLOCK_REALTIME_COARSE;
        else
#endif /* CLOCK_REALTIME_COARSE */
            clock_id = CLOCK_REALTIME;
    }

    if (!clock_gettime( clock_id, &ts ))
    {
        time->QuadPart = ts.tv_sec * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
        time->QuadPart += (ts.tv_nsec + 50) / 100;
    }
    else
#endif /* HAVE_CLOCK_GETTIME */
    {
        struct timeval now;

        gettimeofday( &now, 0 );
        time->QuadPart = now.tv_sec * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
        time->QuadPart += now.tv_usec * 10;
    }
    return STATUS_SUCCESS;
}

/***********************************************************************
 *       RtlGetSystemTimePrecise [NTDLL.@]
 *
 * Get a more accurate current system time.
 *
 * RETURNS
 *   The current system time.
 */
LONGLONG WINAPI RtlGetSystemTimePrecise( void )
{
    LONGLONG time;

#ifdef HAVE_CLOCK_GETTIME
    struct timespec ts;

    if (!clock_gettime( CLOCK_REALTIME, &ts ))
    {
        time = ts.tv_sec * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
        time += (ts.tv_nsec + 50) / 100;
    }
    else
#endif
    {
        struct timeval now;

        gettimeofday( &now, 0 );
        time = now.tv_sec * (ULONGLONG)TICKSPERSEC + TICKS_1601_TO_1970;
        time += now.tv_usec * 10;
    }

    return time;
}

/******************************************************************************
 *  NtQueryPerformanceCounter	[NTDLL.@]
 */
NTSTATUS WINAPI NtQueryPerformanceCounter( LARGE_INTEGER *counter, LARGE_INTEGER *frequency )
{
    __TRY
    {
        counter->QuadPart = monotonic_counter();
        if (frequency) frequency->QuadPart = TICKSPERSEC;
    }
    __EXCEPT_PAGE_FAULT
    {
        return STATUS_ACCESS_VIOLATION;
    }
    __ENDTRY

    return STATUS_SUCCESS;
}

/******************************************************************************
 *  RtlQueryPerformanceCounter   [NTDLL.@]
 */
BOOL WINAPI DECLSPEC_HOTPATCH RtlQueryPerformanceCounter( LARGE_INTEGER *counter )
{
    counter->QuadPart = monotonic_counter();
    return TRUE;
}

/******************************************************************************
 *  RtlQueryPerformanceFrequency   [NTDLL.@]
 */
BOOL WINAPI DECLSPEC_HOTPATCH RtlQueryPerformanceFrequency( LARGE_INTEGER *frequency )
{
    frequency->QuadPart = TICKSPERSEC;
    return TRUE;
}

/******************************************************************************
 * NtGetTickCount   (NTDLL.@)
 * ZwGetTickCount   (NTDLL.@)
 */
ULONG WINAPI NtGetTickCount(void)
{
    return monotonic_counter() / TICKSPERMSEC;
}

/* calculate the mday of dst change date, so that for instance Sun 5 Oct 2007
 * (last Sunday in October of 2007) becomes Sun Oct 28 2007
 *
 * Note: year, day and month must be in unix format.
 */
static int weekday_to_mday(int year, int day, int mon, int day_of_week)
{
    struct tm date;
    time_t tmp;
    int wday, mday;

    /* find first day in the month matching week day of the date */
    memset(&date, 0, sizeof(date));
    date.tm_year = year;
    date.tm_mon = mon;
    date.tm_mday = -1;
    date.tm_wday = -1;
    do
    {
        date.tm_mday++;
        tmp = mktime(&date);
    } while (date.tm_wday != day_of_week || date.tm_mon != mon);

    mday = date.tm_mday;

    /* find number of week days in the month matching week day of the date */
    wday = 1; /* 1 - 1st, ...., 5 - last */
    while (wday < day)
    {
        struct tm *tm;

        date.tm_mday += 7;
        tmp = mktime(&date);
        tm = localtime(&tmp);
        if (tm->tm_mon != mon)
            break;
        mday = tm->tm_mday;
        wday++;
    }

    return mday;
}

static BOOL match_tz_date(const RTL_SYSTEM_TIME *st, const RTL_SYSTEM_TIME *reg_st)
{
    WORD wDay;

    if (st->wMonth != reg_st->wMonth) return FALSE;

    if (!st->wMonth) return TRUE; /* no transition dates */

    wDay = reg_st->wDay;
    if (!reg_st->wYear) /* date in a day-of-week format */
        wDay = weekday_to_mday(st->wYear - 1900, reg_st->wDay, reg_st->wMonth - 1, reg_st->wDayOfWeek);

    if (st->wDay != wDay ||
        st->wHour != reg_st->wHour ||
        st->wMinute != reg_st->wMinute ||
        st->wSecond != reg_st->wSecond ||
        st->wMilliseconds != reg_st->wMilliseconds) return FALSE;

    return TRUE;
}

static BOOL match_tz_info(const RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi, const RTL_DYNAMIC_TIME_ZONE_INFORMATION *reg_tzi)
{
    if (tzi->Bias == reg_tzi->Bias &&
        match_tz_date(&tzi->StandardDate, &reg_tzi->StandardDate) &&
        match_tz_date(&tzi->DaylightDate, &reg_tzi->DaylightDate))
        return TRUE;

    return FALSE;
}

static int compare_tz_key(const void *a, const void *b)
{
    const struct tz_name_map *map_a, *map_b;
    map_a = (const struct tz_name_map *)a;
    map_b = (const struct tz_name_map *)b;
    return strcmpW(map_a->key_name, map_b->key_name);
}

static BOOL match_tz_name(const char* tz_name,
                          const RTL_DYNAMIC_TIME_ZONE_INFORMATION *reg_tzi)
{
    static const struct tz_name_map mapping[] = {
        { {'K','o','r','e','a',' ','S','t','a','n','d','a','r','d',' ','T','i',
           'm','e',0 },
          "KST" },
        { {'T','o','k','y','o',' ','S','t','a','n','d','a','r','d',' ','T','i',
           'm','e',0 },
          "JST" },
        { {'Y','a','k','u','t','s','k',' ','S','t','a','n','d','a','r','d',' ',
           'T','i','m','e',0 },
          "+09" }, /* YAKST was used until tzdata 2016f */
    };
    struct tz_name_map *match, key;

    if (reg_tzi->DaylightDate.wMonth)
        return TRUE;

    strcpyW(key.key_name, reg_tzi->TimeZoneKeyName);
    match = bsearch(&key, mapping, ARRAY_SIZE(mapping), sizeof(mapping[0]), compare_tz_key);
    if (!match)
        return TRUE;

    return !strcmp(match->short_name, tz_name);
}

static BOOL reg_query_value(HKEY hkey, LPCWSTR name, DWORD type, void *data, DWORD count)
{
    UNICODE_STRING nameW;
    char buf[256];
    KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buf;

    if (count > sizeof(buf) - sizeof(KEY_VALUE_PARTIAL_INFORMATION))
        return FALSE;

    RtlInitUnicodeString(&nameW, name);

    if (NtQueryValueKey(hkey, &nameW, KeyValuePartialInformation,
                        buf, sizeof(buf), &count))
        return FALSE;

    if (info->Type != type) return FALSE;

    memcpy(data, info->Data, info->DataLength);
    return TRUE;
}

static void find_reg_tz_info(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi, const char* tz_name, int year)
{
    static const WCHAR Time_ZonesW[] = { 'M','a','c','h','i','n','e','\\',
        'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\',
        'W','i','n','d','o','w','s',' ','N','T','\\',
        'C','u','r','r','e','n','t','V','e','r','s','i','o','n','\\',
        'T','i','m','e',' ','Z','o','n','e','s',0 };
    static const WCHAR Dynamic_DstW[] = { 'D','y','n','a','m','i','c',' ','D','S','T',0 };
    static const WCHAR fmtW[] = { '%','d',0 };
    HANDLE hkey;
    ULONG idx;
    OBJECT_ATTRIBUTES attr, attrDynamic;
    UNICODE_STRING nameW, nameDynamicW;
    WCHAR buf[128], yearW[16];

    sprintfW(yearW, fmtW, year);

    attrDynamic.Length = sizeof(attrDynamic);
    attrDynamic.RootDirectory = 0; /* will be replaced later */
    attrDynamic.ObjectName = &nameDynamicW;
    attrDynamic.Attributes = 0;
    attrDynamic.SecurityDescriptor = NULL;
    attrDynamic.SecurityQualityOfService = NULL;
    RtlInitUnicodeString(&nameDynamicW, Dynamic_DstW);

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    RtlInitUnicodeString(&nameW, Time_ZonesW);
    if (NtOpenKey(&hkey, KEY_READ, &attr))
    {
        WARN("Unable to open the time zones key\n");
        return;
    }

    idx = 0;
    nameW.Buffer = buf;
    nameW.Length = sizeof(buf);
    nameW.MaximumLength = sizeof(buf);

    while (!RtlpNtEnumerateSubKey(hkey, &nameW, idx++))
    {
        static const WCHAR stdW[] = { 'S','t','d',0 };
        static const WCHAR dltW[] = { 'D','l','t',0 };
        static const WCHAR mui_stdW[] = { 'M','U','I','_','S','t','d',0 };
        static const WCHAR mui_dltW[] = { 'M','U','I','_','D','l','t',0 };
        static const WCHAR tziW[] = { 'T','Z','I',0 };
        RTL_DYNAMIC_TIME_ZONE_INFORMATION reg_tzi;
        HANDLE hSubkey, hSubkeyDynamicDST;
        BOOL is_dynamic = FALSE;

        struct tz_reg_data
        {
            LONG bias;
            LONG std_bias;
            LONG dlt_bias;
            RTL_SYSTEM_TIME std_date;
            RTL_SYSTEM_TIME dlt_date;
        } tz_data;

        attr.Length = sizeof(attr);
        attr.RootDirectory = hkey;
        attr.ObjectName = &nameW;
        attr.Attributes = 0;
        attr.SecurityDescriptor = NULL;
        attr.SecurityQualityOfService = NULL;
        if (NtOpenKey(&hSubkey, KEY_READ, &attr))
        {
            WARN("Unable to open subkey %s\n", debugstr_wn(nameW.Buffer, nameW.Length/sizeof(WCHAR)));
            continue;
        }

#define get_value(hkey, name, type, data, len) \
    if (!reg_query_value(hkey, name, type, data, len)) \
    { \
        WARN("can't read data from %s\n", debugstr_w(name)); \
        NtClose(hkey); \
        continue; \
    }

        if (!reg_query_value(hSubkey, mui_stdW, REG_SZ, reg_tzi.StandardName, sizeof(reg_tzi.StandardName)))
            get_value(hSubkey, stdW, REG_SZ, reg_tzi.StandardName, sizeof(reg_tzi.StandardName));
        if (!reg_query_value(hSubkey, mui_dltW, REG_SZ, reg_tzi.DaylightName, sizeof(reg_tzi.DaylightName)))
            get_value(hSubkey, dltW, REG_SZ, reg_tzi.DaylightName, sizeof(reg_tzi.DaylightName));
        memcpy(reg_tzi.TimeZoneKeyName, nameW.Buffer, nameW.Length);
        reg_tzi.TimeZoneKeyName[nameW.Length/sizeof(WCHAR)] = 0;

        /* Check for Dynamic DST entry first */
        attrDynamic.RootDirectory = hSubkey;
        if (!NtOpenKey(&hSubkeyDynamicDST, KEY_READ, &attrDynamic))
        {
            is_dynamic = reg_query_value(hSubkeyDynamicDST, yearW, REG_BINARY, &tz_data, sizeof(tz_data));
            NtClose(hSubkeyDynamicDST);
        }

        if (!is_dynamic)
            get_value(hSubkey, tziW, REG_BINARY, &tz_data, sizeof(tz_data));

#undef get_value

        reg_tzi.Bias = tz_data.bias;
        reg_tzi.StandardBias = tz_data.std_bias;
        reg_tzi.DaylightBias = tz_data.dlt_bias;
        reg_tzi.StandardDate = tz_data.std_date;
        reg_tzi.DaylightDate = tz_data.dlt_date;

        TRACE("%s: bias %d\n", debugstr_wn(nameW.Buffer, nameW.Length/sizeof(WCHAR)), reg_tzi.Bias);
        TRACE("std (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            reg_tzi.StandardDate.wDay, reg_tzi.StandardDate.wMonth,
            reg_tzi.StandardDate.wYear, reg_tzi.StandardDate.wDayOfWeek,
            reg_tzi.StandardDate.wHour, reg_tzi.StandardDate.wMinute,
            reg_tzi.StandardDate.wSecond, reg_tzi.StandardDate.wMilliseconds,
            reg_tzi.StandardBias);
        TRACE("dst (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            reg_tzi.DaylightDate.wDay, reg_tzi.DaylightDate.wMonth,
            reg_tzi.DaylightDate.wYear, reg_tzi.DaylightDate.wDayOfWeek,
            reg_tzi.DaylightDate.wHour, reg_tzi.DaylightDate.wMinute,
            reg_tzi.DaylightDate.wSecond, reg_tzi.DaylightDate.wMilliseconds,
            reg_tzi.DaylightBias);

        NtClose(hSubkey);

        if (match_tz_info(tzi, &reg_tzi)
            && match_tz_name(tz_name, &reg_tzi))
        {
            *tzi = reg_tzi;
            NtClose(hkey);
            return;
        }

        /* reset len */
        nameW.Length = sizeof(buf);
        nameW.MaximumLength = sizeof(buf);
    }

    NtClose(hkey);

    FIXME("Can't find matching timezone information in the registry for "
          "%s, bias %d, std (d/m/y): %u/%02u/%04u, dlt (d/m/y): %u/%02u/%04u\n",
          tz_name, tzi->Bias,
          tzi->StandardDate.wDay, tzi->StandardDate.wMonth, tzi->StandardDate.wYear,
          tzi->DaylightDate.wDay, tzi->DaylightDate.wMonth, tzi->DaylightDate.wYear);
}

static time_t find_dst_change(unsigned long min, unsigned long max, int *is_dst)
{
    time_t start;
    struct tm *tm;

    start = min;
    tm = localtime(&start);
    *is_dst = !tm->tm_isdst;
    TRACE("starting date isdst %d, %s", !*is_dst, ctime(&start));

    while (min <= max)
    {
        time_t pos = (min + max) / 2;
        tm = localtime(&pos);

        if (tm->tm_isdst != *is_dst)
            min = pos + 1;
        else
            max = pos - 1;
    }
    return min;
}

static int init_tz_info(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi)
{
    static RTL_DYNAMIC_TIME_ZONE_INFORMATION cached_tzi;
    static int current_year = -1, current_bias = 65535;
    struct tm *tm;
    char tz_name[SHORT_TZ_NAME_MAX];
    time_t year_start, year_end, tmp, dlt = 0, std = 0;
    int is_dst, current_is_dst, bias;

    RtlEnterCriticalSection( &TIME_tz_section );

    year_start = time(NULL);
    tm = gmtime(&year_start);
    bias = (LONG)(mktime(tm) - year_start) / 60;

    tm = localtime(&year_start);
    current_is_dst = tm->tm_isdst;
    if (current_year == tm->tm_year && current_bias == bias)
    {
        *tzi = cached_tzi;
        RtlLeaveCriticalSection( &TIME_tz_section );
        return current_is_dst;
    }

    memset(tzi, 0, sizeof(*tzi));
    if (!strftime(tz_name, sizeof(tz_name), "%Z", tm)) {
        /* not enough room or another error */
        tz_name[0] = '\0';
    }

    TRACE("tz data will be valid through year %d, bias %d\n", tm->tm_year + 1900, bias);
    current_year = tm->tm_year;
    current_bias = bias;

    tzi->Bias = bias;

    tm->tm_isdst = 0;
    tm->tm_mday = 1;
    tm->tm_mon = tm->tm_hour = tm->tm_min = tm->tm_sec = tm->tm_wday = tm->tm_yday = 0;
    year_start = mktime(tm);
    TRACE("year_start: %s", ctime(&year_start));

    tm->tm_mday = tm->tm_wday = tm->tm_yday = 0;
    tm->tm_mon = 12;
    tm->tm_hour = 23;
    tm->tm_min = tm->tm_sec = 59;
    year_end = mktime(tm);
    TRACE("year_end: %s", ctime(&year_end));

    tmp = find_dst_change(year_start, year_end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;

    tmp = find_dst_change(tmp, year_end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;

    TRACE("std: %s", ctime(&std));
    TRACE("dlt: %s", ctime(&dlt));

    if (dlt == std || !dlt || !std)
        TRACE("there is no daylight saving rules in this time zone\n");
    else
    {
        tmp = dlt - tzi->Bias * 60;
        tm = gmtime(&tmp);
        TRACE("dlt gmtime: %s", asctime(tm));

        tzi->DaylightBias = -60;
        tzi->DaylightDate.wYear = tm->tm_year + 1900;
        tzi->DaylightDate.wMonth = tm->tm_mon + 1;
        tzi->DaylightDate.wDayOfWeek = tm->tm_wday;
        tzi->DaylightDate.wDay = tm->tm_mday;
        tzi->DaylightDate.wHour = tm->tm_hour;
        tzi->DaylightDate.wMinute = tm->tm_min;
        tzi->DaylightDate.wSecond = tm->tm_sec;
        tzi->DaylightDate.wMilliseconds = 0;

        TRACE("daylight (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            tzi->DaylightDate.wDay, tzi->DaylightDate.wMonth,
            tzi->DaylightDate.wYear, tzi->DaylightDate.wDayOfWeek,
            tzi->DaylightDate.wHour, tzi->DaylightDate.wMinute,
            tzi->DaylightDate.wSecond, tzi->DaylightDate.wMilliseconds,
            tzi->DaylightBias);

        tmp = std - tzi->Bias * 60 - tzi->DaylightBias * 60;
        tm = gmtime(&tmp);
        TRACE("std gmtime: %s", asctime(tm));

        tzi->StandardBias = 0;
        tzi->StandardDate.wYear = tm->tm_year + 1900;
        tzi->StandardDate.wMonth = tm->tm_mon + 1;
        tzi->StandardDate.wDayOfWeek = tm->tm_wday;
        tzi->StandardDate.wDay = tm->tm_mday;
        tzi->StandardDate.wHour = tm->tm_hour;
        tzi->StandardDate.wMinute = tm->tm_min;
        tzi->StandardDate.wSecond = tm->tm_sec;
        tzi->StandardDate.wMilliseconds = 0;

        TRACE("standard (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            tzi->StandardDate.wDay, tzi->StandardDate.wMonth,
            tzi->StandardDate.wYear, tzi->StandardDate.wDayOfWeek,
            tzi->StandardDate.wHour, tzi->StandardDate.wMinute,
            tzi->StandardDate.wSecond, tzi->StandardDate.wMilliseconds,
            tzi->StandardBias);
    }

    find_reg_tz_info(tzi, tz_name, current_year + 1900);
    cached_tzi = *tzi;

    RtlLeaveCriticalSection( &TIME_tz_section );

    return current_is_dst;
}

/***********************************************************************
 *      RtlQueryTimeZoneInformation [NTDLL.@]
 *
 * Get information about the current timezone.
 *
 * PARAMS
 *   tzinfo [O] Destination for the retrieved timezone info.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlQueryTimeZoneInformation(RTL_TIME_ZONE_INFORMATION *ret)
{
    RTL_DYNAMIC_TIME_ZONE_INFORMATION tzinfo;

    init_tz_info( &tzinfo );
    memcpy( ret, &tzinfo, sizeof(*ret) );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *      RtlQueryDynamicTimeZoneInformation [NTDLL.@]
 *
 * Get information about the current timezone.
 *
 * PARAMS
 *   tzinfo [O] Destination for the retrieved timezone info.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI RtlQueryDynamicTimeZoneInformation(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzinfo)
{
    init_tz_info( tzinfo );

    return STATUS_SUCCESS;
}

/***********************************************************************
 *       RtlSetTimeZoneInformation [NTDLL.@]
 *
 * Set the current time zone information.
 *
 * PARAMS
 *   tzinfo [I] Timezone information to set.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 *
 */
NTSTATUS WINAPI RtlSetTimeZoneInformation( const RTL_TIME_ZONE_INFORMATION *tzinfo )
{
    return STATUS_PRIVILEGE_NOT_HELD;
}

/***********************************************************************
 *        NtSetSystemTime [NTDLL.@]
 *        ZwSetSystemTime [NTDLL.@]
 *
 * Set the system time.
 *
 * PARAMS
 *   NewTime [I] The time to set.
 *   OldTime [O] Optional destination for the previous system time.
 *
 * RETURNS
 *   Success: STATUS_SUCCESS.
 *   Failure: An NTSTATUS error code indicating the problem.
 */
NTSTATUS WINAPI NtSetSystemTime(const LARGE_INTEGER *NewTime, LARGE_INTEGER *OldTime)
{
    struct timeval tv;
    time_t tm_t;
    DWORD sec, oldsec;
    LARGE_INTEGER tm;

    /* Return the old time if necessary */
    if (!OldTime) OldTime = &tm;

    NtQuerySystemTime( OldTime );
    if (!RtlTimeToSecondsSince1970( OldTime, &oldsec )) return STATUS_INVALID_PARAMETER;
    if (!RtlTimeToSecondsSince1970( NewTime, &sec )) return STATUS_INVALID_PARAMETER;

    /* fake success if time didn't change */
    if (oldsec == sec)
        return STATUS_SUCCESS;

    /* set the new time */
    tv.tv_sec = sec;
    tv.tv_usec = 0;

#ifdef HAVE_SETTIMEOFDAY
    tm_t = sec;
    if (!settimeofday(&tv, NULL)) /* 0 is OK, -1 is error */
    {
        TRACE("OS time changed to %s\n", ctime(&tm_t));
        return STATUS_SUCCESS;
    }
    ERR("Cannot set time to %s, time adjustment %ld: %s\n",
        ctime(&tm_t), (long)(sec-oldsec), strerror(errno));
    if (errno == EPERM)
        return STATUS_PRIVILEGE_NOT_HELD;
    else
        return STATUS_INVALID_PARAMETER;
#else
    tm_t = sec;
    FIXME("setting time to %s not implemented for missing settimeofday\n",
        ctime(&tm_t));
    return STATUS_NOT_IMPLEMENTED;
#endif
}

/***********************************************************************
 *        RtlQueryUnbiasedInterruptTime [NTDLL.@]
 */
NTSTATUS WINAPI RtlQueryUnbiasedInterruptTime(ULONGLONG *time)
{
    *time = monotonic_counter();
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           wine_update_timezones_from_tzdata (NTDLL.@) Not a Windows API
 *
 * Update time zones in registry using information from tzdata.
 * $WINEPREFIX/win_unix_map provides the mapping between Windows and Olson IDs.
 */

/* similar to above functions of similar name
    - cache is removed
    - can pass specific year
    - dynamic dst is computed using relative/day-of-week instead of absolute date
*/
static inline void convert_to_non_absolute(RTL_SYSTEM_TIME *st)
{
    /* see dlls/kernel32/time.c TIME_DayLightCompareDate */
    WORD first;

    /* if on 4th week of a 4-week month, there's no way to determine
       if the rule is always for the 4th week, or if for the 'last'
       week (usually 5) and it happens to be the 4th week this year */
    st->wYear = 0;
    first = (6 + st->wDay) % 7 + 1;
    st->wDay = (st->wDay - first) / 7 + 1;
}

static int init_tz_info_year(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi, int year)
{
    struct tm *tm;
    time_t year_start, year_end, tmp, dlt = 0, std = 0;
    int is_dst, current_is_dst;

    year_start = time(NULL);
    tm = localtime(&year_start);
    current_is_dst = tm->tm_isdst;

    memset(tzi, 0, sizeof(*tzi));

    if (year)
        tm->tm_year = year - 1900;
    TRACE("tz data will be valid through year %d\n", tm->tm_year + 1900);

    tm->tm_isdst = 0;
    tm->tm_mday = 1;
    tm->tm_mon = tm->tm_hour = tm->tm_min = tm->tm_sec = tm->tm_wday = tm->tm_yday = 0;
    year_start = mktime(tm);
    TRACE("year_start: %s", ctime(&year_start));

    tm->tm_mday = tm->tm_wday = tm->tm_yday = 0;
    tm->tm_mon = 12;
    tm->tm_hour = 23;
    tm->tm_min = tm->tm_sec = 59;
    year_end = mktime(tm);
    TRACE("year_end: %s", ctime(&year_end));

    tm = gmtime(&year_start);
    tzi->Bias = (LONG)(mktime(tm) - year_start) / 60;

    tmp = find_dst_change(year_start, year_end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;

    tmp = find_dst_change(tmp, year_end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;

    TRACE("std: %s", ctime(&std));
    TRACE("dlt: %s", ctime(&dlt));

    if (dlt == std || !dlt || !std)
        TRACE("there is no daylight saving rules in this time zone\n");
    else
    {
        tmp = dlt - tzi->Bias * 60;
        tm = gmtime(&tmp);
        TRACE("dlt gmtime: %s", asctime(tm));

        tzi->DaylightBias = -60;
        tzi->DaylightDate.wYear = tm->tm_year + 1900;
        tzi->DaylightDate.wMonth = tm->tm_mon + 1;
        tzi->DaylightDate.wDayOfWeek = tm->tm_wday;
        tzi->DaylightDate.wDay = tm->tm_mday;
        tzi->DaylightDate.wHour = tm->tm_hour;
        tzi->DaylightDate.wMinute = tm->tm_min;
        tzi->DaylightDate.wSecond = tm->tm_sec;
        tzi->DaylightDate.wMilliseconds = 0;

        TRACE("daylight (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            tzi->DaylightDate.wDay, tzi->DaylightDate.wMonth,
            tzi->DaylightDate.wYear, tzi->DaylightDate.wDayOfWeek,
            tzi->DaylightDate.wHour, tzi->DaylightDate.wMinute,
            tzi->DaylightDate.wSecond, tzi->DaylightDate.wMilliseconds,
            tzi->DaylightBias);

        tmp = std - tzi->Bias * 60 - tzi->DaylightBias * 60;
        tm = gmtime(&tmp);
        TRACE("std gmtime: %s", asctime(tm));

        tzi->StandardBias = 0;
        tzi->StandardDate.wYear = tm->tm_year + 1900;
        tzi->StandardDate.wMonth = tm->tm_mon + 1;
        tzi->StandardDate.wDayOfWeek = tm->tm_wday;
        tzi->StandardDate.wDay = tm->tm_mday;
        tzi->StandardDate.wHour = tm->tm_hour;
        tzi->StandardDate.wMinute = tm->tm_min;
        tzi->StandardDate.wSecond = tm->tm_sec;
        tzi->StandardDate.wMilliseconds = 0;

        TRACE("standard (d/m/y): %u/%02u/%04u day of week %u %u:%02u:%02u.%03u bias %d\n",
            tzi->StandardDate.wDay, tzi->StandardDate.wMonth,
            tzi->StandardDate.wYear, tzi->StandardDate.wDayOfWeek,
            tzi->StandardDate.wHour, tzi->StandardDate.wMinute,
            tzi->StandardDate.wSecond, tzi->StandardDate.wMilliseconds,
            tzi->StandardBias);

        convert_to_non_absolute( &tzi->DaylightDate );
        convert_to_non_absolute( &tzi->StandardDate );
    }

    return current_is_dst;
}

struct tzmap
{
    struct list entry;
    WCHAR *win_tz;
    char *unix_tz;
};

static inline void free_timezones(struct list *tzs)
{
    struct list *head;
    struct tzmap *tzentry;

    while ((head = list_head( tzs )))
    {
        list_remove( head );
        tzentry = LIST_ENTRY( head, struct tzmap, entry );
        RtlFreeHeap( GetProcessHeap(), 0, tzentry );
    }
}

#define TIME_ZONE_KEY_LENGTH    (ARRAY_SIZE(((RTL_DYNAMIC_TIME_ZONE_INFORMATION*)0)->TimeZoneKeyName))

static int read_timezones(struct list *tzs)
{
    char buffer[TIME_ZONE_KEY_LENGTH +1];
    const char *config_dir = wine_get_config_dir();
    size_t win_len, unix_len, buf_len;
    char *win_unix_map, *unix_tz;
    struct tzmap *tzentry;
    FILE *file;

    list_init( tzs );
    if (!(win_unix_map = RtlAllocateHeap( GetProcessHeap(), 0, strlen(config_dir) + sizeof("/win_unix_map") )))
        return FALSE;

    strcpy( win_unix_map, config_dir );
    strcat( win_unix_map, "/win_unix_map" );
    if (!(file = fopen( win_unix_map, "rt" )))
    {
        WARN("wine: failed to open %s, time zones will not be updated\n", win_unix_map );
        goto error;
    }

    while (fgets( buffer, TIME_ZONE_KEY_LENGTH, file ))
    {
        buffer[TIME_ZONE_KEY_LENGTH] = 0;
        if (!(unix_tz = strchr( buffer, ',' )))
        {
            WARN("wine: invalid time zone map entry %s\n", debugstr_a(buffer));
            continue;
        }

        *unix_tz++ = 0;
        unix_len = strlen( unix_tz );
        if (!unix_len)
        {
            WARN("wine: invalid time zone map entry %s\n", debugstr_a(buffer));
            continue;
        }
        unix_tz[--unix_len] = 0; /* chop \n */

        buf_len = strlen(buffer);
        win_len = ntdll_umbstowcs( 0, buffer, buf_len, NULL, 0 );
        if (!win_len)
        {
            WARN("wine: invalid time zone map entry %s (%d)\n", debugstr_a(buffer), GetLastError());
            continue;
        }

        tzentry = RtlAllocateHeap( GetProcessHeap(), 0,
                    sizeof(*tzentry) + unix_len + 1 + 1 + /* extra +1 for ':' */ (win_len + 1) * sizeof(WCHAR) );
        if (!tzentry)
        {
            ERR("wine: out of memory updating time zone %s\n", debugstr_a(buffer));
            goto error;
        }

        tzentry->win_tz = (WCHAR *)((char *)tzentry + sizeof(*tzentry));
        tzentry->unix_tz = ((char *)tzentry->win_tz) + (win_len + 1) * sizeof(WCHAR);

        ntdll_umbstowcs( 0, buffer, buf_len, tzentry->win_tz, win_len );
        tzentry->win_tz[win_len] = 0;
        tzentry->unix_tz[0] = ':';
        strcpy( tzentry->unix_tz+1, unix_tz );
        list_add_tail( tzs, &tzentry->entry );
    }
    RtlFreeHeap( GetProcessHeap(), 0, win_unix_map );
    fclose( file );
    return TRUE;

error:
    free_timezones( tzs );
    RtlFreeHeap( GetProcessHeap(), 0, win_unix_map );
    if (file) fclose( file );
    return FALSE;
}

static inline struct tzmap *find_timezone(struct list *tzs, const WCHAR *win_tz)
{
    struct tzmap *tzentry;
    LIST_FOR_EACH_ENTRY( tzentry, tzs, struct tzmap, entry )
    {
        if (!strcmpW( tzentry->win_tz, win_tz ))
            return tzentry;
    }
    return NULL;
}

static void fill_timezone(HKEY tzkey, struct tzmap *tzentry)
{
    const DWORD dynamic_start = 2004;
    static const WCHAR displayW[] = { 'D','i','s','p','l','a','y',0 };
    static const WCHAR firstW[] = { 'F','i','r','s','t','E','n','t','r','y',0 };
    static const WCHAR lastW[] = { 'L','a','s','t','E','n','t','r','y',0 };
    static const WCHAR ddstW[] = { 'D','y','n','a','m','i','c',' ','D','S','T',0 };
    static const WCHAR tziW[] = { 'T','Z','I',0 };
    static const WCHAR fmtW[] = { '%','u',0 };
    WCHAR tznameW[TIME_ZONE_KEY_LENGTH+1];
    WCHAR valname[5];
    RTL_DYNAMIC_TIME_ZONE_INFORMATION dtzi, last;
    DWORD last_year, first_year, cur_year;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    char *unix_tz;
    struct tm *tm;
    time_t today;
    HANDLE dstkey;
    size_t len;
    struct tz_reg_data
    {
        LONG bias;
        LONG std_bias;
        LONG dlt_bias;
        RTL_SYSTEM_TIME std_date;
        RTL_SYSTEM_TIME dlt_date;
    } tz_data;

    setenv("TZ", tzentry->unix_tz, TRUE);
    tzset();

    init_tz_info_year( &dtzi, 0 );

    tz_data.bias = dtzi.Bias;
    tz_data.std_bias = dtzi.StandardBias;
    tz_data.dlt_bias = dtzi.DaylightBias;
    tz_data.std_date = dtzi.StandardDate;
    tz_data.dlt_date = dtzi.DaylightDate;
    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = 0;
    attr.SecurityQualityOfService = 0;
    RtlInitUnicodeString( &nameW, tziW );
    NtSetValueKey( tzkey, &nameW, 0, REG_BINARY, &tz_data, sizeof(tz_data) );

    unix_tz = tzentry->unix_tz + 1; /* skip : */
    len = ntdll_umbstowcs( 0, unix_tz, strlen(unix_tz), tznameW, TIME_ZONE_KEY_LENGTH );
    tznameW[len] = 0;
    RtlInitUnicodeString( &nameW, displayW );
    NtSetValueKey( tzkey, &nameW, 0, REG_SZ, tznameW, (len + 1) * sizeof(WCHAR) );

    attr.RootDirectory = tzkey;
    RtlInitUnicodeString( &nameW, ddstW );
    if (!NtOpenKey( &dstkey, DELETE, &attr ))
    {
        NtDeleteKey( dstkey );
        NtClose( dstkey );
    }

    today = time( NULL );
    tm = localtime( &today );
    cur_year = tm->tm_year + 1900;

    init_tz_info_year( &last, dynamic_start );
    for (first_year = dynamic_start + 1; first_year <= cur_year; first_year++)
    {
        init_tz_info_year( &dtzi, first_year );
        if (memcmp( &dtzi, &last, sizeof(last) ))
        {
            --first_year;
            break;
        }
    }

    if (first_year > cur_year)
    {
        TRACE("time zone %s has no dynamic dst\n", debugstr_w(tzentry->win_tz));
        return;
    }

    init_tz_info_year( &last, cur_year );
    for (last_year = cur_year - 1; last_year >= first_year; last_year--)
    {
        init_tz_info_year( &dtzi, last_year );
        if (memcmp( &dtzi, &last, sizeof(last) ))
        {
            ++last_year;
            break;
        }
    }

    attr.RootDirectory = tzkey;
    RtlInitUnicodeString( &nameW, ddstW );
    if (NtCreateKey( &dstkey, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL ))
    {
        WARN("failed to create '%s\\%s'\n", debugstr_w(tzentry->win_tz), debugstr_w(ddstW));
        return;
    }

    RtlInitUnicodeString( &nameW, firstW );
    NtSetValueKey( dstkey, &nameW, 0, REG_DWORD, &first_year, sizeof(first_year) );
    RtlInitUnicodeString( &nameW, lastW );
    NtSetValueKey( dstkey, &nameW, 0, REG_DWORD, &last_year, sizeof(last_year) );

    while (first_year <= last_year)
    {
        init_tz_info_year( &dtzi, first_year );

        snprintfW( valname, sizeof(valname), fmtW, first_year );
        tz_data.bias = dtzi.Bias;
        tz_data.std_bias = dtzi.StandardBias;
        tz_data.dlt_bias = dtzi.DaylightBias;
        tz_data.std_date = dtzi.StandardDate;
        tz_data.dlt_date = dtzi.DaylightDate;
        RtlInitUnicodeString( &nameW, valname );
        NtSetValueKey( dstkey, &nameW, 0, REG_BINARY, &tz_data, sizeof(tz_data) );

        first_year++;
    }

    NtClose( dstkey );
}

BOOL CDECL wine_update_timezones_from_tzdata(void)
{
    static const WCHAR timezonesW[] = {
        'M','a','c','h','i','n','e','\\',
        'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\',
        'W','i','n','d','o','w','s',' ','N','T','\\',
        'C','u','r','r','e','n','t','V','e','r','s','i','o','n','\\',
        'T','i','m','e',' ','Z','o','n','e','s',0 };
    RTL_DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    struct list tzs, *head;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    char buffer[TIME_ZONE_KEY_LENGTH * sizeof(WCHAR) + sizeof(KEY_NODE_INFORMATION)];
    WCHAR keyname[TIME_ZONE_KEY_LENGTH+1];
    KEY_NODE_INFORMATION *info = (KEY_NODE_INFORMATION *)buffer;
    DWORD total_size, index;
    struct tzmap *tzentry;
    HANDLE hkey, subkey;
    char *orig_tz;
    BOOL ret;

    if (!read_timezones( &tzs ))
        return FALSE;

    ret = FALSE;
    orig_tz = getenv( "TZ" );

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;
    RtlInitUnicodeString( &nameW, timezonesW );
    if (NtOpenKey( &hkey, KEY_ENUMERATE_SUB_KEYS, &attr ))
        goto error;

    index = 0;
    while (!NtEnumerateKey( hkey, index++, KeyNodeInformation, buffer, sizeof(buffer), &total_size ))
    {
        memcpy(keyname, info->Name, info->NameLength);
        keyname[info->NameLength/sizeof(WCHAR)] = 0;

        if (!(tzentry = find_timezone( &tzs, keyname )))
        {
            const DWORD obsolete = 1;
            static const WCHAR obsoleteW[] = {'I','s','O','b','s','o','l','e','t','e',0};

            TRACE("marking time zone %s obsolete\n", debugstr_w(keyname));
            attr.RootDirectory = hkey;
            RtlInitUnicodeString( &nameW, keyname );
            if (NtOpenKey( &subkey, KEY_SET_VALUE, &attr ))
            {
                WARN("failed to open time zone %s to mark obsolete\n", debugstr_w(keyname));
                continue;
            }

            attr.RootDirectory = subkey;
            RtlInitUnicodeString( &nameW, obsoleteW );
            NtSetValueKey( subkey, &nameW, 0, REG_DWORD, &obsolete, sizeof(obsolete) );
            NtClose( subkey );
        }
        else
        {
            TRACE("updating time zone %s\n", debugstr_w(keyname));
            list_remove( &tzentry->entry );

            attr.RootDirectory = hkey;
            RtlInitUnicodeString( &nameW, keyname );
            if (!NtOpenKey( &subkey, KEY_SET_VALUE, &attr ))
            {
                fill_timezone( subkey, tzentry );
                NtClose( subkey );
            }
            RtlFreeHeap( GetProcessHeap(), 0, tzentry );
        }
    }

    attr.RootDirectory = hkey;
    while ((head = list_head( &tzs )))
    {
        static const WCHAR stdW[] = { 'S','t','d',0 };
        static const WCHAR dltW[] = { 'D','l','t',0 };
        static const WCHAR mui_stdW[] = { 'M','U','I','_','S','t','d',0 };
        static const WCHAR mui_dltW[] = { 'M','U','I','_','D','l','t',0 };
        static const WCHAR stdnameW[] = { 'S','t','a','n','d','a','r','d',0 };
        static const WCHAR dltnameW[] = { 'D','a','y','l','i','g','h','t' };
        WCHAR *stdstr;

        list_remove( head );
        tzentry = LIST_ENTRY( head, struct tzmap, entry );
        TRACE("adding new time zone %s\n", debugstr_w(tzentry->win_tz));

        RtlInitUnicodeString( &nameW, tzentry->win_tz );
        if (NtCreateKey( &subkey, KEY_ALL_ACCESS, &attr, 0, NULL, 0, NULL ))
        {
            WARN("failed to add new time zone %s\n", debugstr_w(tzentry->win_tz));
            RtlFreeHeap( GetProcessHeap(), 0, tzentry );
            continue;
        }

        lstrcpynW( dtzi.StandardName, tzentry->win_tz, sizeof(dtzi.StandardName) );
        RtlInitUnicodeString( &nameW, stdW );
        NtSetValueKey( subkey, &nameW, 0, REG_SZ, dtzi.StandardName,
                       (strlenW(dtzi.StandardName) + 1) * sizeof(WCHAR) );
        RtlInitUnicodeString( &nameW, mui_stdW );
        NtSetValueKey( subkey, &nameW, 0, REG_SZ, dtzi.StandardName,
                       (strlenW(dtzi.StandardName) + 1) * sizeof(WCHAR) );

        lstrcpynW( dtzi.DaylightName, tzentry->win_tz, sizeof(dtzi.DaylightName) );
        if ((stdstr = strstrW( dtzi.DaylightName, stdnameW )))
            memcpy( stdstr, dltnameW, sizeof(dltnameW) );
        RtlInitUnicodeString( &nameW, dltW );
        NtSetValueKey( subkey, &nameW, 0, REG_SZ, dtzi.DaylightName,
                       (strlenW(dtzi.DaylightName) + 1) * sizeof(WCHAR) );
        RtlInitUnicodeString( &nameW, mui_dltW );
        NtSetValueKey( subkey, &nameW, 0, REG_SZ, dtzi.DaylightName,
                       (strlenW(dtzi.DaylightName) + 1) * sizeof(WCHAR) );

        fill_timezone( subkey, tzentry );
        NtClose( subkey );
        RtlFreeHeap( GetProcessHeap(), 0, tzentry );
    }

    NtClose( hkey );
    ret = TRUE;

error:
    if (orig_tz)
        setenv( "TZ", orig_tz, TRUE );
    else
        unsetenv( "TZ" );
    tzset();
    free_timezones( &tzs );
    return ret;
}
