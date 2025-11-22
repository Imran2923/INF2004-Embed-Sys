#ifndef WEB_ACTIONS_H
#define WEB_ACTIONS_H

// Web-specific implementations of menu actions
// These functions capture output for web display instead of serial

/**
 * @brief Web version of test connection - outputs to web buffer
 */
void web_test_connection(void);

/**
 * @brief Web version of read results - outputs CSV to web buffer  
 */
void web_read_results(void);

/**
 * @brief Web version of erase last session - outputs confirmation to web buffer
 */
void web_erase_last_session(void);

/**
 * @brief Web version of identify chip - outputs status to web buffer
 */
void web_identify_chip(void);

// Add these declarations
void web_run_benchmark(void);
void web_run_benchmark_save(void); 
void web_run_benchmark_100(void);
void web_erase_last_session(void);
void web_identify_chip(void);
void web_show_status(void);

#endif // WEB_ACTIONS_H