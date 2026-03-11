#ifdef _WIN32
#define PGDLLIMPORT __declspec(dllimport)
#endif
// Version Windows plus sûre qui évite les fonctions mémoire PostgreSQL problématiques
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>

#define popen _popen
#define pclose _pclose
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif
#endif

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "lib/stringinfo.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "utils/rel.h"
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif

#ifdef _WIN32
#define BINARY_NAME "FastBCP.exe"
#else
#define BINARY_NAME "FastBCP"
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

// Clé symétrique partagée pour le chiffrement/déchiffrement
static const char *PGFBCP_ENCRYPTION_KEY = "key";


//###########################################################################################
//## Decrypt C Function
//###########################################################################################

char *decrypt_password(text *cipher_text, const char *key) {
    // ... votre code de déchiffrement existant ...
    // Note: Mettez à jour le message d'erreur pour qu'il soit spécifique à FastBcp
    const char *sql = "SELECT pgp_sym_decrypt(decode($1, 'base64'), $2)";
    Oid argtypes[2] = { TEXTOID, TEXTOID };
    Datum values[2] = {
        PointerGetDatum(cipher_text),
        CStringGetTextDatum(key)
    };

    char *decrypted = NULL;
    int ret;
    bool isnull;
    Datum result;
    text *txt;
    
    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR, (errmsg("Failed to connect to SPI for decryption")));
    }
    
    ret = SPI_execute_with_args(sql, 2, argtypes, values, NULL, true, 1);

    if (ret != SPI_OK_SELECT || SPI_processed != 1) {
        SPI_finish();
        ereport(ERROR, (errmsg("Decryption failed via pgp_sym_decrypt. Check encrypted data or key.")));
    }
    
    result = SPI_getbinval(SPI_tuptable->vals[0],
                            SPI_tuptable->tupdesc,
                            1,
                            &isnull);

    if (!isnull) {
        txt = DatumGetTextPP(result);
        decrypted =  pstrdup(text_to_cstring(txt));
    } else {
        ereport(LOG, (errmsg("pg_fastbcp: Decryption returned NULL.")));
    }

    SPI_finish();

    return decrypted;
}



//###########################################################################################
//## Run FastBcp Function
//###########################################################################################

PG_FUNCTION_INFO_V1(xp_RunFastBcp_secure);

Datum
xp_RunFastBcp_secure(PG_FUNCTION_ARGS)
{
    TupleDesc tupdesc;
    Datum values[5]; // Gardez la même structure de retour
    bool nulls[5] = {false, false, false, false, false};
    HeapTuple tuple;

    static int exit_code = 0;
    static long total_rows = -1;
    static int total_columns = -1; // FastBcp ne retourne pas ce paramètre de manière standard. Nous pourrions le laisser à -1 ou l'enlever.
    static long total_time = -1;
    
    StringInfo command = makeStringInfo();
    StringInfo result_output = makeStringInfo();

    char binary_path[1024];
    
    // NOUVEAU: Mettre à jour les noms des arguments et leur nombre
    // Le nombre de paramètres est 38 (36 + license + path)
    const char *arg_names[] = {
    "--connectiontype", "--sourceconnectstring", "--dsn", "--provider",
    "--server", "--user", "--password", "--trusted", "--database",
    "--applicationintent", "--fileinput", "--query", "--sourceschema",
    "--sourcetable", "--fileoutput", "--directory", "--timestamped",
    "--encoding", "--delimiter", "--usequotes", "--dateformat",
    "--decimalseparator", "--boolformat", "--noheader", "--parquetcompression",
    "--cloudprofile", "--parallelmethod", "--paralleldegree", "--distributekeycolumn",
    "--datadrivenquery", "--merge", "--loglevel", "--runid",
    "--settingsfile", "--config", "--nobanner", "--license", "__fastbcp_path"
    };

    // NOUVEAU: Mettre à jour les paramètres booléens et entiers
    const char *flag_params[] = {
        "--trusted", "--timestamped", "--usequotes", "--noheader", "--nobanner"
    };
    const char *int_params[] = {
        "--paralleldegree"
    };
    const char *password_params[] = {
        "--password"
    };

    static const char *value_bool_params[] = {
        "--merge",
    };
    
    FILE *fp;
    char buffer[1024];
    const char *pg_path;
    char numbuf[34];
    int i, b, x, p, f;
    bool is_flag,is_value_bool, is_int, is_password;
    const char *val = NULL;
    int status;
    text *enc;
    char *token;

    #ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
    #endif

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("The function should return a record")));
    
    // Le nombre de paramètres dans PG_GETARG_ pour l'index du chemin sera maintenant 37
    if (fcinfo->nargs > 37 && !PG_ARGISNULL(37)) {
        pg_path = text_to_cstring(PG_GETARG_TEXT_PP(37));
        #ifdef _WIN32
        snprintf(binary_path, sizeof(binary_path), "%s\\%s", pg_path, BINARY_NAME);
        #else
        snprintf(binary_path, sizeof(binary_path), "%s/%s", pg_path, BINARY_NAME);
        #endif
    } else {
        #ifdef _WIN32
        snprintf(binary_path, sizeof(binary_path), ".\\%s", BINARY_NAME);
        #else
        snprintf(binary_path, sizeof(binary_path), "./%s", BINARY_NAME);
        #endif
    }

    for (i = 0; binary_path[i] != '\0'; i++) {
        if (isspace((unsigned char)binary_path[i])) {
            ereport(ERROR, (errmsg("The path of the executable must not contain spaces.")));
            break;
        }
    }
    
    appendStringInfo(command, "%s", binary_path);

    // Boucle pour itérer sur les arguments. Le nombre d'arguments est maintenant 37.
    for (i = 0; i < 37; i++) {
        if (PG_ARGISNULL(i)) continue;

        // Vérifie si le paramètre est un booléen de type "switch"
        is_flag = false;
        for (f = 0; f < sizeof(flag_params) / sizeof(char *); f++) {
            if (strcmp(arg_names[i], flag_params[f]) == 0) {
                is_flag = true;
                break;
            }
        }
        if (is_flag) {
            // Si le paramètre est un switch et est true, on l'ajoute sans valeur
            if (PG_GETARG_BOOL(i)) {
                appendStringInfo(command, " %s", arg_names[i]);
            }
            continue;
        }

        // Vérifie si le paramètre est un booléen qui nécessite une valeur
        is_value_bool = false;
        for (b = 0; b < sizeof(value_bool_params) / sizeof(char *); b++) {
            if (strcmp(arg_names[i], value_bool_params[b]) == 0) {
                is_value_bool = true;
                break;
            }
        }

        if (is_value_bool) {
            // Si le paramètre est un booléen avec valeur, on ajoute la valeur
            val = PG_GETARG_BOOL(i) ? "true" : "false";
            appendStringInfo(command, " %s \"%s\"", arg_names[i], val);
            continue;
        }
        
        is_int = false;
        for (x = 0; x < sizeof(int_params) / sizeof(char *); x++) {
            if (strcmp(arg_names[i], int_params[x]) == 0) {
                snprintf(numbuf, sizeof(numbuf), "%d", PG_GETARG_INT32(i));
                val = numbuf;
                is_int = true;
                break;
            }
        }
        
        if (is_int) {
            appendStringInfo(command, " %s \"%s\"", arg_names[i], val);
        } else {
            is_password = false;
            for (p = 0; p < sizeof(password_params) / sizeof(char*); p++) {
                if (strcmp(arg_names[i], password_params[p]) == 0) {
                    is_password = true;
                    break;
                }
            }

            if (is_password) {
                enc = PG_GETARG_TEXT_PP(i);
                val = decrypt_password(enc, PGFBCP_ENCRYPTION_KEY);
            } else {
                val = text_to_cstring(PG_GETARG_TEXT_PP(i));
            }
            
            if (val && strlen(val) > 0) {
                appendStringInfo(command, " %s \"%s\"", arg_names[i], val);
            }
        }
    }
    
    appendStringInfo(command, " 2>&1");
    
    PG_TRY();
    {
        fp = popen(command->data, "r");
        if (!fp) {
            ereport(FATAL, (errmsg("pg_fastbcp: unable to execute FastBcp. Check the binary path, permissions, and environment variables.")));
        } else {
            while (fgets(buffer, sizeof(buffer), fp) != NULL) {
                appendStringInfoString(result_output, buffer);
            }
            
            status = pclose(fp);
            fp = NULL;

            // Analyser le code de sortie
            #ifdef _WIN32
            exit_code = status;
            #else
            if (WIFEXITED(status)) {
                exit_code = WEXITSTATUS(status);
            } else {
                exit_code = -2;
            }
            #endif
    
            // Parsing de la sortie : Cherchez les labels pertinents
            char *out = result_output->data;
            char *token = NULL;
            if (out && out[0] != '\0') {
                /* Total rows */
                token = strstr(out, "Total data rows : ");
                if (token) {
                    char *p = token + strlen("Total data rows : ");
                    char *endptr = NULL;
                    long v = strtol(p, &endptr, 10);
                    if (endptr != p) total_rows = v;
                }

                /* Total columns */
                token = strstr(out, "Total data columns : ");
                if (token) {
                    char *p = token + strlen("Total data columns : ");
                    char *endptr = NULL;
                    long v = strtol(p, &endptr, 10);
                    if (endptr != p) total_columns = (int)v;
                }


                /* Total time */
                token = strstr(out, "Total time : Elapsed=");
                if (token) {
                    char *eq = strchr(token, '=');
                    if (eq) {
                        char *endptr = NULL;
                        long v = strtol(eq + 1, &endptr, 10);
                        if (endptr != (eq + 1)) total_time = v;
                    }
                }
            }
        }
    }
    PG_CATCH();
    {
        ErrorData *errdata;
        MemoryContext oldcxt = MemoryContextSwitchTo(ErrorContext);
        errdata = CopyErrorData();
        MemoryContextSwitchTo(oldcxt);
        FreeErrorData(errdata);

        exit_code = -3;
        resetStringInfo(result_output);
        appendStringInfoString(result_output, "An internal error occurred during data transfer. Check PostgreSQL logs for details.\n");
    }
    PG_END_TRY();

    // Retourner les résultats
    values[0] = Int32GetDatum(exit_code);
    values[1] = CStringGetTextDatum(result_output->data);
    values[2] = Int64GetDatum(total_rows);
    values[3] = Int32GetDatum(total_columns); 
    values[4] = Int64GetDatum(total_time);
    
    tuple = heap_form_tuple(tupdesc, values, nulls);
    
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}


//###########################################################################################
//## Encrypt Function
//###########################################################################################


PG_FUNCTION_INFO_V1(pg_fastbcp_encrypt);

Datum
pg_fastbcp_encrypt(PG_FUNCTION_ARGS)
{
    text *input_text;
    const char *key = PGFBCP_ENCRYPTION_KEY;
    Datum result;
    const char *sql;
    Oid argtypes[2];
    Datum values[2];
    int ret;
    bool isnull;

    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    input_text = PG_GETARG_TEXT_PP(0);

    SPI_connect();

    sql = "SELECT encode(pgp_sym_encrypt($1, $2), 'base64')";
    argtypes[0] = TEXTOID;
    argtypes[1] = TEXTOID;
    values[0] = PointerGetDatum(input_text);
    values[1] = CStringGetTextDatum(key);

    ret = SPI_execute_with_args(sql, 2, argtypes, values, NULL, true, 1);

    if (ret != SPI_OK_SELECT || SPI_processed != 1) {
        SPI_finish();
        ereport(ERROR, (errmsg("Encryption with pgp_sym_encrypt failed")));
    }

    result = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);

    SPI_finish();

    if (isnull)
        PG_RETURN_NULL();

    PG_RETURN_DATUM(result);
}