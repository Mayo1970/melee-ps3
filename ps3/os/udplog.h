// Log lines by UDP to the PC (PS3_LOG_IP, port 18194), one datagram each, after DolphinPS3's ps3/log.cpp.
// Debug builds only (DEBUG=1). Standard C types only, so SDK sources can include it next to their own headers.
#ifndef PS3_OS_UDPLOG_H
#define PS3_OS_UDPLOG_H

void ps3_log_init(void);
void ps3_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// A line already formatted, sent as it is.
void ps3_log_text(const char* text, int len);

#endif
