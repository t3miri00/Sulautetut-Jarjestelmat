#include "TimeParser.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int time_parse(char *time)
{
    if (time == NULL) {
        return TIME_ERROR_NULL;
    }

    if (strlen(time) != 6) {
        return TIME_ERROR_LENGTH;
    }

    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)time[i])) {
            return TIME_ERROR_NOT_NUMERIC;
        }
    }

    char hh_str[3] = { time[0], time[1], '\0' };
    char mm_str[3] = { time[2], time[3], '\0' };
    char ss_str[3] = { time[4], time[5], '\0' };

    int hh = atoi(hh_str);
    int mm = atoi(mm_str);
    int ss = atoi(ss_str);

    if (hh < 0 || hh > 23) return TIME_ERROR_HOUR_RANGE;
    if (mm < 0 || mm > 59) return TIME_ERROR_MINUTE_RANGE;
    if (ss < 0 || ss > 59) return TIME_ERROR_SECOND_RANGE;

    if (hh == 0 && mm == 0 && ss == 0) return TIME_ERROR_ZERO_TIME;

    int total_seconds = (hh * 3600) + (mm * 60) + ss;
    return total_seconds;
}