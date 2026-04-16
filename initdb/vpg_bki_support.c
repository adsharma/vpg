/*
 * vpg_bki_support.c
 *
 * BKI file helpers used by vpg_bootstrap.c.
 * Extracted from initdb.c / vpg_initdb.c; renamed with vpg_bki_ prefix so
 * they can be called from vpg_bootstrap.c without conflicting with the static
 * copies that still live in vpg_initdb.c.
 *
 * All functions here use palloc/pg_malloc from the backend ABI — exactly the
 * same as the rest of the single-process build.
 */

#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/fe_memutils.h"   /* pg_malloc / pg_strdup / pg_realloc */
#include "common/string.h"        /* pg_get_line_buf */
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"          /* pg_valid_server_encoding */

/*
 * Read a text file into a malloc'd NULL-terminated array of malloc'd strings.
 */
char **
vpg_bki_readfile(const char *path)
{
    FILE       *infile;
    char      **result;
    StringInfoData line;
    int         maxlines;
    int         n;

    if ((infile = fopen(path, "r")) == NULL)
    {
        fprintf(stderr, "vpg_bootstrap: could not open \"%s\": %m\n", path);
        return NULL;
    }

    initStringInfo(&line);
    maxlines = 1024;
    result = (char **) pg_malloc(maxlines * sizeof(char *));

    n = 0;
    while (pg_get_line_buf(infile, &line))
    {
        if (n >= maxlines - 1)
        {
            maxlines *= 2;
            result = (char **) pg_realloc(result, maxlines * sizeof(char *));
        }
        result[n++] = pg_strdup(line.data);
    }
    result[n] = NULL;

    pfree(line.data);
    fclose(infile);
    return result;
}

/*
 * Replace every occurrence of `token` with `replacement` in each line.
 * Modifies the array in-place; old line strings are freed.
 */
char **
vpg_bki_replace_token(char **lines, const char *token, const char *replacement)
{
    int toklen  = strlen(token);
    int replen  = strlen(replacement);
    int diff    = replen - toklen;

    for (int i = 0; lines[i]; i++)
    {
        char *where;
        char *newline;
        int   pre;

        if ((where = strstr(lines[i], token)) == NULL)
            continue;

        newline = (char *) pg_malloc(strlen(lines[i]) + diff + 1);
        pre = where - lines[i];
        memcpy(newline, lines[i], pre);
        memcpy(newline + pre, replacement, replen);
        strcpy(newline + pre + replen, lines[i] + pre + toklen);
        free(lines[i]);
        lines[i] = newline;
    }
    return lines;
}

/*
 * Return the numeric encoding id as a string (palloc'd).
 */
const char *
vpg_bki_encodingid_to_string(int enc)
{
    char result[32];
    snprintf(result, sizeof(result), "%d", enc);
    return pg_strdup(result);
}

/*
 * Escape single quotes, then wrap value in single quotes, for BKI use.
 * Returns a pg_malloc'd string.
 */
const char *
vpg_bki_escape_quotes(const char *src)
{
    /* escape_single_quotes_ascii is from libpgcommon */
    char *escaped = escape_single_quotes_ascii(src);
    char *result;

    if (!escaped)
    {
        fprintf(stderr, "vpg_bootstrap: out of memory\n");
        exit(1);
    }

    result = pg_malloc(strlen(escaped) + 3);
    result[0] = '\'';
    strcpy(result + 1, escaped);
    result[strlen(escaped) + 1] = '\'';
    result[strlen(escaped) + 2] = '\0';
    free(escaped);
    return result;
}
