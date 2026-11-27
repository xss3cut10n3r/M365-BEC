#ifndef BEC_APP_H
#define BEC_APP_H

#include <glib.h>

typedef struct {
    char *case_path;
    char *filter;
    char *export_path;
    gint64 log;
    gboolean headless;
    char *mark;
    char **mark_ips;
    char **files;
} Options;

int cli_run(const Options *options);
void cli_console(void);

#endif
