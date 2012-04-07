#ifndef __TIME_H__
#define __TIME_H__

typedef long clock_t;

struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

int get_cmostime(struct tm *t);

#endif	/* __TIME_H__ */