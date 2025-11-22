#pragma once
int  get_choice_blocking(void);
void action_show_network_status(void);
void print_menu(void);
void action_test_connection(void);
void run_benchmarks(bool save_csv);
void run_benchmarks_100(bool save_csv);
void action_show_network_status(void);

// new backup/restore menu actions
void action_backup_flash(void);
void action_restore_flash(void);
