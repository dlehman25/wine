/*
 * Modified version of tzinfgen.c that uses relative times
 *
 * tzinfgen2.c - find the daylight savings transition times
 * Compile with: gcc -Wall -Iinclude -o tzinfgen2 tzinfgen2.c
 *
 * Time Zone IDs
 * msdn.microsoft.com/en-us/library/gg154758.aspx
 *
 * also see:
 * https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml
 */

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winbase.h"
#include "winternl.h"

typedef struct
{
    const char *wintz;
    const char *unixtz;
    unsigned int tzid;
} tzdata;

#include "zones.c"

#if 0
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time,"Display",,"America/Santiago"
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time,"Dlt",,"Pacific SA Daylight Time"
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time,"MUI_Dlt",,"@tzres.dll,-65009"
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time,"MUI_Std",,"@tzres.dll,-65008"
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time,"Std",,"Pacific SA Standard Time"
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time,"TZI",1,b4,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time\Dynamic DST,"2015",1,b4,00,00,00,00,00,00,00,c4,ff,ff,ff,df,07,04,00,00,00,1a,00,01,00,00,00,00,00,00,00,e0,07,01,00,05,00,01,00,00,00,00,00,00,00,00,00
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time\Dynamic DST,"FirstEntry",0x10001,2015
HKLM,%CurrentVersionNT%\Time Zones\Pacific SA Standard Time\Dynamic DST,"LastEntry",0x10001,2015
#endif

typedef struct tagREGTIMEZONEINFORMATION
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    RTL_SYSTEM_TIME StandardDate;
    RTL_SYSTEM_TIME DaylightDate;
} REGTIMEZONEINFORMATION;

static void print_tzi(const REGTIMEZONEINFORMATION *tzi)
{
    size_t i;

    for (i = 0; i < sizeof(*tzi); i++)
    {
        printf("%02x", ((const BYTE*)tzi)[i]);
        if (i == (sizeof(*tzi)-1))
            printf("\n");
        else
            printf(",");
    }
}

/* copied from dlls/ntdll/time.c, with the following changes
    - cache is removed
    - can pass specific year
    - dynamic dst is computed using relative/day-of-week instead of absolute date
*/
static time_t find_dst_change(time_t min, time_t max, int *is_dst)
{
    time_t start;
    struct tm *tm;

    start = min;
    tm = localtime(&start);
    *is_dst = !tm->tm_isdst;

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

static inline void convert_to_non_absolute(RTL_SYSTEM_TIME *st)
{
    /* see dlls/kernelbase/time.c compare_tzdate */
    WORD first;

    /* if on 4th week of a 4-week month, there's no way to determine
       if the rule is always for the 4th week, or if for the 'last'
       week (usually 5) and it happens to be the 4th week this year */
    st->wYear = 0;
    first = (6 + st->wDay) % 7 + 1;
    st->wDay = (st->wDay - first) / 7 + 1;
}

static time_t find_dst_change2(time_t min, time_t max, int *is_dst)
{
    time_t start, pos;
    struct tm tm;

    start = min;
    localtime_r(&start, &tm);
    *is_dst = !tm.tm_isdst;

    while (min <= max)
    {
        pos = (min + max) / 2;
        localtime_r(&pos, &tm);

        if (tm.tm_isdst != *is_dst)
            min = pos + 1;
        else
            max = pos - 1;
    }

    return min;
}

static inline int is_leap_year(int year)
{
    return !(year % 4) && ((year % 100) || !(year % 400));
}

static int init_tz_info2(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi, int year)
{
    static int mdays[2][12] =
    {
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
        { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    };
    int is_dst;
    int week1st;
    int weeklast;
    int weeknum;
    struct tm dlttm;
    struct tm stdtm;
    struct tm local;
    struct tm mo1st;
    struct tm molast;
    struct tm jan1st;
    struct tm dec31st;
    time_t start, end, tmp, dlt, std;

    memset(&local, 0, sizeof(local));
    memset(&jan1st, 0, sizeof(jan1st));
    memset(&dec31st, 0, sizeof(dec31st));
    if (year)
    {
        jan1st.tm_year = year - 1900;
        dec31st.tm_year = year - 1900;
    }

    jan1st.tm_mday = 1;
    start = mktime(&jan1st);

    dec31st.tm_mday = 31;
    dec31st.tm_mon  = 11;
    dec31st.tm_hour = 23;
    dec31st.tm_min  = 59;
    dec31st.tm_sec  = 59;
    end = mktime(&dec31st);

    tmp = time(NULL);
    localtime_r(&tmp, &local);

    memset(tzi, 0, sizeof(*tzi));

    tzi->Bias = -local.tm_gmtoff / 60;

    dlt = std = 0;
    tmp = find_dst_change2(start, end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;

    tmp = find_dst_change2(tmp, end, &is_dst);
    if (is_dst)
        dlt = tmp;
    else
        std = tmp;
    
    /* TODO: set some other fields before returning? */
    if (dlt == std || !dlt || !std)
        return local.tm_isdst;

    localtime_r(&dlt, &dlttm);
    memset(&mo1st, 0, sizeof(mo1st));
    mo1st.tm_year = dlttm.tm_year;
    mo1st.tm_mon = dlttm.tm_mon;
    mo1st.tm_mday = 1;
    mktime(&mo1st);
    memset(&molast, 0, sizeof(molast));
    molast.tm_year = dlttm.tm_year;
    molast.tm_mon = dlttm.tm_mon;
    molast.tm_mday = mdays[is_leap_year(dlttm.tm_year + 1900)][dlttm.tm_mon];
    mktime(&molast);

    week1st = (mo1st.tm_yday + jan1st.tm_wday) / 7;
    weeklast = (molast.tm_yday + jan1st.tm_wday) / 7;
    weeknum = (dlttm.tm_yday + jan1st.tm_wday) / 7;
    if (weeknum == weeklast)
        weeknum = 5;
    else
        weeknum -= week1st;

    dlt += local.tm_gmtoff;
    gmtime_r(&dlt, &dlttm);
    tzi->DaylightBias = -60;
    tzi->DaylightDate.wYear = 0;
    tzi->DaylightDate.wMonth = dlttm.tm_mon + 1;
    tzi->DaylightDate.wDayOfWeek = dlttm.tm_wday;
    tzi->DaylightDate.wDay = weeknum; 
    tzi->DaylightDate.wHour = dlttm.tm_hour;
    tzi->DaylightDate.wMinute = dlttm.tm_min;
    tzi->DaylightDate.wSecond = dlttm.tm_sec;

    localtime_r(&std, &stdtm);
    memset(&mo1st, 0, sizeof(mo1st));
    mo1st.tm_year = stdtm.tm_year;
    mo1st.tm_mon = stdtm.tm_mon;
    mo1st.tm_mday = 1;
    mktime(&mo1st);
    memset(&molast, 0, sizeof(molast));
    molast.tm_year = stdtm.tm_year;
    molast.tm_mon = stdtm.tm_mon;
    molast.tm_mday = mdays[is_leap_year(stdtm.tm_year + 1900)][stdtm.tm_mon];
    mktime(&molast);

    week1st = (mo1st.tm_yday + jan1st.tm_wday) / 7;
    weeklast = (molast.tm_yday + jan1st.tm_wday) / 7;
    weeknum = (stdtm.tm_yday + jan1st.tm_wday) / 7;
    if (weeknum == weeklast)
        weeknum = 5;
    else
        weeknum -= week1st;

    std += local.tm_gmtoff - tzi->DaylightBias * 60;
    gmtime_r(&std, &stdtm);
    tzi->StandardBias = 0;
    tzi->StandardDate.wYear = 0;
    tzi->StandardDate.wMonth = stdtm.tm_mon + 1;
    tzi->StandardDate.wDayOfWeek = stdtm.tm_wday;
    tzi->StandardDate.wDay = weeknum;
    tzi->StandardDate.wHour = stdtm.tm_hour;
    tzi->StandardDate.wMinute = stdtm.tm_min;
    tzi->StandardDate.wSecond = stdtm.tm_sec;

    return local.tm_isdst;
}

static int init_tz_info(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi, int year)
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

    tm->tm_isdst = 0;
    tm->tm_mday = 1;
    tm->tm_mon = tm->tm_hour = tm->tm_min = tm->tm_sec = tm->tm_wday = tm->tm_yday = 0;
    year_start = mktime(tm);

    tm->tm_mday = tm->tm_wday = tm->tm_yday = 0;
    tm->tm_mon = 11;
    tm->tm_hour = 23;
    tm->tm_min = tm->tm_sec = 59;
    year_end = mktime(tm);

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

    if (dlt == std || !dlt || !std)
        return current_is_dst;

    tmp = dlt - tzi->Bias * 60;
    tm = gmtime(&tmp);

    tzi->DaylightBias = -60;
    tzi->DaylightDate.wYear = tm->tm_year + 1900;
    tzi->DaylightDate.wMonth = tm->tm_mon + 1;
    tzi->DaylightDate.wDayOfWeek = tm->tm_wday;
    tzi->DaylightDate.wDay = tm->tm_mday;
    tzi->DaylightDate.wHour = tm->tm_hour;
    tzi->DaylightDate.wMinute = tm->tm_min;
    tzi->DaylightDate.wSecond = tm->tm_sec;
    tzi->DaylightDate.wMilliseconds = 0;

    tmp = std - tzi->Bias * 60 - tzi->DaylightBias * 60;
    tm = gmtime(&tmp);

    tzi->StandardBias = 0;
    tzi->StandardDate.wYear = tm->tm_year + 1900;
    tzi->StandardDate.wMonth = tm->tm_mon + 1;
    tzi->StandardDate.wDayOfWeek = tm->tm_wday;
    tzi->StandardDate.wDay = tm->tm_mday;
    tzi->StandardDate.wHour = tm->tm_hour;
    tzi->StandardDate.wMinute = tm->tm_min;
    tzi->StandardDate.wSecond = tm->tm_sec;
    tzi->StandardDate.wMilliseconds = 0;

    convert_to_non_absolute(&tzi->DaylightDate);
    convert_to_non_absolute(&tzi->StandardDate);

    return current_is_dst;
}

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define TIME_ZONE_KEY_SIZE  (ARRAY_SIZE(((RTL_DYNAMIC_TIME_ZONE_INFORMATION*)0)->TimeZoneKeyName))

static void dump_timezone(const char *win_tz, const char *unix_tz, unsigned int tzid)
{
    time_t today;
    struct tm *tm;
    REGTIMEZONEINFORMATION tzi;
    char dlt[TIME_ZONE_KEY_SIZE], *p;
    const DWORD dynamic_start = 2004;
    DWORD last_year, first_year, cur_year;
    RTL_DYNAMIC_TIME_ZONE_INFORMATION dtzi, last;

    setenv("TZ", unix_tz, TRUE);
    tzset();

    init_tz_info2(&dtzi, 0);

    tzi.Bias = dtzi.Bias;
    tzi.StandardBias = dtzi.StandardBias;
    tzi.DaylightBias = dtzi.DaylightBias;
    tzi.StandardDate = dtzi.StandardDate;
    tzi.DaylightDate = dtzi.DaylightDate;

    strcpy(dlt, win_tz);
    p = strstr(dlt,"Standard");
    if (p)
        memcpy(p,"Daylight",8);
    printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s,\"Display\",,\"%s\"\n", win_tz, unix_tz);
    printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s,\"Dlt\",,\"%s\"\n", win_tz, dlt);
    if (tzid)
    {
        printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s,\"MUI_Dlt\",,\"@tzres.dll,-%u\"\n", win_tz, tzid+1);
        printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s,\"MUI_Std\",,\"@tzres.dll,-%u\"\n", win_tz, tzid);
    }
    printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s,\"Std\",,\"%s\"\n", win_tz, win_tz);
    printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s,\"TZI\",1,", win_tz);
    print_tzi(&tzi);

    today = time(NULL);
    tm = localtime(&today);
    cur_year = tm->tm_year + 1900;

    init_tz_info2(&last, dynamic_start);
    for (first_year = dynamic_start + 1; first_year <= cur_year; first_year++)
    {
        init_tz_info2(&dtzi, first_year);
        if (memcmp(&dtzi, &last, sizeof(last)))
        {
            --first_year;
            break;
        }
    }

    if (first_year > cur_year)
        return; /* no dynamic dst */

    init_tz_info2(&last, cur_year);
    for (last_year = cur_year - 1; last_year >= first_year; last_year--)
    {
        init_tz_info2(&dtzi, last_year);
        if (memcmp(&dtzi, &last, sizeof(last)))
        {
            ++last_year;
            break;
        }
    }

    for (cur_year = first_year; cur_year <= last_year; cur_year++)
    {
        init_tz_info2(&dtzi, cur_year);

        tzi.Bias = dtzi.Bias;
        tzi.StandardBias = dtzi.StandardBias;
        tzi.DaylightBias = dtzi.DaylightBias;
        tzi.StandardDate = dtzi.StandardDate;
        tzi.DaylightDate = dtzi.DaylightDate;
        printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s\\Dynamic DST,\"%d\",1,", win_tz, cur_year);
        print_tzi(&tzi);
    }

    printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s\\Dynamic DST,\"FirstEntry\",0x10001,\"%d\"\n", win_tz, first_year);
    printf("HKLM,%%CurrentVersionNT%%\\Time Zones\\%s\\Dynamic DST,\"LastEntry\",0x10001,\"%d\"\n", win_tz, last_year);
}

int main(void)
{
    const tzdata *tz = zoneinfo;

    while (tz->wintz)
    {
        dump_timezone(tz->wintz, tz->unixtz, tz->tzid);
        tz++;
    }

    return 0;
}

