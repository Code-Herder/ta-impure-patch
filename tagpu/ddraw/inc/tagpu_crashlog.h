#ifndef TAGPU_CRASHLOG_H
#define TAGPU_CRASHLOG_H
/* An unhandled exception writes log\crash-report.txt: tagpu_crashlog.c. After tagpu_log_init. */
void tagpu_crashlog_install(void);
#endif
