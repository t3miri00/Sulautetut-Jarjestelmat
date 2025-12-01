#ifndef TIMEPARSER_H
#define TIMEPARSER_H

#ifdef __cplusplus
extern "C" {
#endif

#define TIME_ERROR_NULL           -1
#define TIME_ERROR_LENGTH         -2
#define TIME_ERROR_NOT_NUMERIC    -3
#define TIME_ERROR_HOUR_RANGE     -4
#define TIME_ERROR_MINUTE_RANGE   -5
#define TIME_ERROR_SECOND_RANGE   -6
#define TIME_ERROR_ZERO_TIME      -7

int time_parse(char *time);

#ifdef __cplusplus
}
#endif

#endif /* TIMEPARSER_H */