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

/* some time zones (Pacific/Bougainville) redefine standard time */
static time_t find_dst_change(time_t min, time_t max, int *is_dst)
{
    time_t pos;
    struct tm tm;
    long gmtoff;

    pos = min;
    localtime_r(&pos, &tm);
    gmtoff = tm.tm_gmtoff;
    *is_dst = !tm.tm_isdst;

    while (min <= max)
    {
        pos = (min + max) / 2;
        localtime_r(&pos, &tm);
        if (tm.tm_gmtoff == gmtoff)
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

static void init_tz_info(RTL_DYNAMIC_TIME_ZONE_INFORMATION *tzi, int year)
{
    static int mdays[2][12] =
    {
        { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
        { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
    };
    int is_dst;
    int redef_std;
    struct tm dlttm;
    struct tm stdtm;
    struct tm local;
    struct tm jan1st;
    struct tm dec31st;
    time_t start, end, tmp, dlt, std;

    memset(&local, 0, sizeof(local));
    memset(&jan1st, 0, sizeof(jan1st));
    memset(&dec31st, 0, sizeof(dec31st));
    if (!year)
    {
        tmp = time(NULL);
        localtime_r(&tmp, &local);
        year = local.tm_year + 1900;
    }
    jan1st.tm_year = year - 1900;
    jan1st.tm_mday = 1;
    start = mktime(&jan1st);

    dec31st.tm_year = year - 1900;
    dec31st.tm_mday = 31;
    dec31st.tm_mon  = 11;
    dec31st.tm_hour = 23;
    dec31st.tm_min  = 59;
    dec31st.tm_sec  = 59;
    end = mktime(&dec31st);

    memset(tzi, 0, sizeof(*tzi));

    redef_std = jan1st.tm_gmtoff != dec31st.tm_gmtoff &&
                !strcmp(jan1st.tm_zone, dec31st.tm_zone);

    dlt = std = 0;
    tmp = find_dst_change(start, end, &is_dst);
    if (tmp <= end)
    {
        if (is_dst)
            dlt = tmp;
        else
            std = tmp;
    }
    else
        tmp = start;

    tmp = find_dst_change(tmp, end, &is_dst);
    if (tmp <= end)
    {
        if (is_dst)
            dlt = tmp;
        else
            std = tmp;
    }

    if (!dlt) dlt = start;
    if (!std) std = start;

    localtime_r(&std, &local);
    tzi->Bias = -local.tm_gmtoff / 60;

    if (dlt == std)
        return;

    localtime_r(&dlt, &dlttm);
    localtime_r(&std, &stdtm);
    tzi->DaylightBias = (stdtm.tm_gmtoff - dlttm.tm_gmtoff) / 60;

    tmp = dlt - tzi->Bias * 60;
    gmtime_r(&tmp, &dlttm);
    tzi->DaylightDate.wYear = 0;
    tzi->DaylightDate.wMonth = dlttm.tm_mon + 1;
    tzi->DaylightDate.wDayOfWeek = dlttm.tm_wday;
    tzi->DaylightDate.wDay = (dlttm.tm_mday - 1) / 7 + 1;
    if (dlt != start)
    {
        /* only fill out transition time if one was found */
        tzi->DaylightDate.wHour = dlttm.tm_hour;
        tzi->DaylightDate.wMinute = dlttm.tm_min;
        tzi->DaylightDate.wSecond = dlttm.tm_sec;
    }

    if (tzi->DaylightDate.wDay == 4)
    {
        if (dlttm.tm_mday + 7 > mdays[is_leap_year(dlttm.tm_year + 1900)][dlttm.tm_mon])
            tzi->DaylightDate.wDay++;
    }

    if (redef_std)
        tmp = std - tzi->Bias * 60;
    else
        tmp = std - tzi->Bias * 60 - tzi->DaylightBias * 60;
    gmtime_r(&tmp, &stdtm);
    tzi->StandardBias = 0;
    tzi->StandardDate.wYear = 0;
    tzi->StandardDate.wMonth = stdtm.tm_mon + 1;
    tzi->StandardDate.wDayOfWeek = stdtm.tm_wday;
    tzi->StandardDate.wDay = (stdtm.tm_mday - 1) / 7 + 1;
    if (std != start)
    {
        tzi->StandardDate.wHour = stdtm.tm_hour;
        tzi->StandardDate.wMinute = stdtm.tm_min;
        tzi->StandardDate.wSecond = stdtm.tm_sec;
    }

    if (tzi->StandardDate.wDay == 4)
    {
        if (stdtm.tm_mday + 7 > mdays[is_leap_year(stdtm.tm_year + 1900)][stdtm.tm_mon])
            tzi->StandardDate.wDay++;
    }
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

    init_tz_info(&dtzi, 0);

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

    init_tz_info(&last, dynamic_start);
    for (first_year = dynamic_start + 1; first_year <= cur_year; first_year++)
    {
        init_tz_info(&dtzi, first_year);
        if (memcmp(&dtzi, &last, sizeof(last)))
        {
            --first_year;
            break;
        }
    }

    if (first_year > cur_year)
        return; /* no dynamic dst */

    init_tz_info(&last, cur_year);
    for (last_year = cur_year - 1; last_year >= first_year; last_year--)
    {
        init_tz_info(&dtzi, last_year);
        if (memcmp(&dtzi, &last, sizeof(last)))
        {
            ++last_year;
            break;
        }
    }

    for (cur_year = first_year; cur_year <= last_year; cur_year++)
    {
        init_tz_info(&dtzi, cur_year);

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

    printf("STRINGTABLE\n{\n");
    tz = zoneinfo;
    while (tz->wintz)
    {
        char dlt[TIME_ZONE_KEY_SIZE], *p;
        strcpy(dlt, tz->wintz);
        p = strstr(dlt,"Standard");
        if (p)
            memcpy(p,"Daylight",8);
        printf("%7d \"%s\"\n", tz->tzid, tz->wintz);
        printf("%7d \"%s\"\n", tz->tzid+1, dlt);
        tz++;
    }
    printf("}\n");

    return 0;
}

