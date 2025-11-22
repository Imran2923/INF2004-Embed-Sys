#ifndef WEB_ACTIONS_H
#define WEB_ACTIONS_H

// Web-specific implementations of menu actions
// These functions capture output for web display instead of serial

void web_test_connection(void);
void web_read_results(void);
void web_erase_last_session(void);
void web_identify_chip(void);
void web_run_benchmark(void);
void web_run_benchmark_save(void);
void web_run_benchmark_100(void);
void web_show_status(void);

// Fast benchmark - runs on Core 0 (no dual-core complexity)
void web_run_fast_benchmark(void);

#endif // WEB_ACTIONS_H