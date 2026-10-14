#define _POSIX_C_SOURCE 200809L

#include "ai_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/*
 * parse_string — parse a JSON string starting at *p (which must point at '"').
 *
 * Writes up to n-1 decoded bytes into buf and null-terminates it.
 * Handles escape sequences: \\ \" \/ \n \r \t \b \f and \uXXXX (→ '?').
 * If the decoded length would exceed n-1, sets *truncated (when non-NULL)
 * and continues scanning so the caller can report the error.
 * Returns a pointer past the closing '"', or NULL on syntax error.
 */
static const char *parse_string(const char *p, char *buf, size_t n, int *truncated)
{
    if (truncated) *truncated = 0;
    if (*p != '"') return NULL;
    p++;

    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) return NULL;

            if (*p == 'u') {
                /* \uXXXX — verify four hex digits, replace with '?' */
                p++;
                for (int j = 0; j < 4; j++, p++) {
                    if (!isxdigit((unsigned char)*p)) return NULL;
                }
                if (i < n - 1) buf[i++] = '?';
                else if (truncated) *truncated = 1;
                continue;
            }

            char c;
            switch (*p) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                default:   return NULL; /* unrecognised escape */
            }
            if (i < n - 1) buf[i++] = c;
            else if (truncated) *truncated = 1;
            p++;
        } else {
            if (i < n - 1) buf[i++] = *p;
            else if (truncated) *truncated = 1;
            p++;
        }
    }

    if (*p != '"') return NULL; /* unterminated string */
    buf[i] = '\0';
    return p + 1;
}

/*
 * parse_number — parse a JSON number (integer or decimal) using strtod.
 * Returns a pointer past the number, or NULL if no valid number was found.
 */
static const char *parse_number(const char *p, double *out)
{
    char *end;
    *out = strtod(p, &end);
    return (end == p) ? NULL : end;
}

/*
 * skip_value — advance past one JSON value of any type without storing it.
 * Handles strings, objects, arrays, and scalars (numbers/booleans/null).
 * Used to skip unknown fields so the parser can continue.
 */
static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (!*p) return NULL;

    if (*p == '"') {
        /* String */
        p++;
        while (*p && *p != '"') {
            if (*p == '\\') { p++; if (!*p) return NULL; }
            p++;
        }
        return (*p == '"') ? p + 1 : NULL;
    }

    if (*p == '{' || *p == '[') {
        /* Object or array: track depth, respecting strings */
        char open  = *p;
        char close = (open == '{') ? '}' : ']';
        int  depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                /* Skip string contents to avoid counting brackets inside them */
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') { p++; if (!*p) return NULL; }
                    p++;
                }
                if (!*p) return NULL;
                /* p now points to the closing '"' */
            } else if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }

    /* Scalar: number, true, false, null */
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

/* -------------------------------------------------------------------------
 * Code-fence stripping
 * ---------------------------------------------------------------------- */

/*
 * strip_code_fence — remove a markdown code fence if the input has one.
 *
 * Claude sometimes wraps its JSON response in a markdown code block despite
 * the prompt asking it not to.  This function undoes that so the parser
 * always receives a bare JSON object.
 *
 * When a fence is found:
 *   1. Skips leading whitespace.
 *   2. Consumes "```" plus any optional language tag (e.g., "json") and the
 *      trailing newline.
 *   3. Copies the inner content into buf, stopping just before the closing
 *      "```" (or at end-of-string if the closing fence is absent).
 *   4. Trims trailing whitespace from the inner content.
 *   5. Returns buf.
 *
 * When no fence is found, returns raw unchanged (buf is not written).
 *
 * buf must be at least bufsz bytes; bufsz should be large enough to hold
 * the full inner content (matching the caller's stdout buffer size).
 */
static const char *strip_code_fence(const char *raw, char *buf, size_t bufsz)
{
    /* Skip leading whitespace */
    while (*raw == ' ' || *raw == '\t' || *raw == '\n' || *raw == '\r')
        raw++;

    /* Not a code fence */
    if (strncmp(raw, "```", 3) != 0)
        return raw;

    raw += 3; /* skip the opening "```" */

    /* Skip optional language tag (e.g., "json") up to and including \n */
    while (*raw && *raw != '\n')
        raw++;
    if (*raw == '\n')
        raw++;

    /* Find the closing "```" (first occurrence in the remaining text) */
    const char *close = strstr(raw, "```");
    const char *end   = close ? close : (raw + strlen(raw));

    /* Trim trailing whitespace before the closing fence (or end of string) */
    while (end > raw && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\n' || end[-1] == '\r'))
        end--;

    size_t len = (size_t)(end - raw);
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, raw, len);
    buf[len] = '\0';
    return buf;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

ai_json_status_t ai_json_parse_result(const char *json,
                                      ai_result_t *out,
                                      char *err,
                                      size_t err_len)
{
    int has_command = 0, has_explanation = 0, has_risk = 0, has_confidence = 0;
    int has_intent  = 0;
    int truncated   = 0;

    memset(out, 0, sizeof(*out));

    /* Strip markdown code fence if Claude wrapped its JSON response in one. */
    char fence_buf[8192];
    const char *s = strip_code_fence(json, fence_buf, sizeof(fence_buf));

    const char *p = skip_ws(s);
    if (*p != '{') {
        snprintf(err, err_len, "expected '{', got '%c'", *p ? *p : '?');
        return AI_JSON_PARSE_ERROR;
    }
    p++;
    p = skip_ws(p);

    while (*p != '}') {
        if (*p == '\0') {
            snprintf(err, err_len, "unexpected end of input");
            return AI_JSON_PARSE_ERROR;
        }

        /* Parse key string */
        char key[64];
        p = parse_string(p, key, sizeof(key), NULL);
        if (!p) {
            snprintf(err, err_len, "invalid or unterminated key string");
            return AI_JSON_PARSE_ERROR;
        }

        /* Colon separator */
        p = skip_ws(p);
        if (*p != ':') {
            snprintf(err, err_len, "expected ':' after key");
            return AI_JSON_PARSE_ERROR;
        }
        p++;
        p = skip_ws(p);

        /* Dispatch on key name */
        if (strcmp(key, "intent") == 0) {
            int t = 0;
            p = parse_string(p, out->intent, sizeof(out->intent), &t);
            if (!p) {
                snprintf(err, err_len, "invalid value for 'intent'");
                return AI_JSON_PARSE_ERROR;
            }
            if (t) truncated = 1;
            has_intent = 1;

        } else if (strcmp(key, "command") == 0) {
            int t = 0;
            p = parse_string(p, out->command, sizeof(out->command), &t);
            if (!p) {
                snprintf(err, err_len, "invalid value for 'command'");
                return AI_JSON_PARSE_ERROR;
            }
            if (t) truncated = 1;
            has_command = 1;

        } else if (strcmp(key, "explanation") == 0) {
            int t = 0;
            p = parse_string(p, out->explanation, sizeof(out->explanation), &t);
            if (!p) {
                snprintf(err, err_len, "invalid value for 'explanation'");
                return AI_JSON_PARSE_ERROR;
            }
            if (t) truncated = 1;
            has_explanation = 1;

        } else if (strcmp(key, "risk") == 0) {
            int t = 0;
            p = parse_string(p, out->risk, sizeof(out->risk), &t);
            if (!p) {
                snprintf(err, err_len, "invalid value for 'risk'");
                return AI_JSON_PARSE_ERROR;
            }
            if (t) truncated = 1;
            has_risk = 1;

        } else if (strcmp(key, "confidence") == 0) {
            p = parse_number(p, &out->confidence);
            if (!p) {
                snprintf(err, err_len, "invalid value for 'confidence'");
                return AI_JSON_PARSE_ERROR;
            }
            has_confidence = 1;

        } else {
            /* Unknown key — skip value without error */
            p = skip_value(p);
            if (!p) {
                snprintf(err, err_len, "invalid value for unknown key '%s'", key);
                return AI_JSON_PARSE_ERROR;
            }
        }

        /* Expect ',' (more pairs) or '}' (end of object) */
        p = skip_ws(p);
        if (*p == ',') {
            p++;
            p = skip_ws(p);
        } else if (*p != '}') {
            snprintf(err, err_len, "expected ',' or '}' after value");
            return AI_JSON_PARSE_ERROR;
        }
    }
    /* p now points at '}' — no need to advance; caller doesn't use it */

    /* Truncation takes priority: report it before checking other errors */
    if (truncated) {
        snprintf(err, err_len, "string value exceeds buffer limit");
        return AI_JSON_TRUNCATED;
    }

    /* 'intent' is optional — default to "shell_command" when absent */
    if (!has_intent)
        strncpy(out->intent, "shell_command", sizeof(out->intent) - 1);

    /* Validate intent before required-field checks so the error is clear */
    if (strcmp(out->intent, "shell_command") != 0 &&
        strcmp(out->intent, "unsupported") != 0) {
        snprintf(err, err_len, "invalid intent value: '%s'", out->intent);
        return AI_JSON_INVALID_FIELD;
    }

    /* Required field presence */
    if (!has_command)     { snprintf(err, err_len, "missing field: command");     return AI_JSON_MISSING_FIELD; }
    if (!has_explanation) { snprintf(err, err_len, "missing field: explanation"); return AI_JSON_MISSING_FIELD; }
    if (!has_risk)        { snprintf(err, err_len, "missing field: risk");        return AI_JSON_MISSING_FIELD; }
    if (!has_confidence)  { snprintf(err, err_len, "missing field: confidence");  return AI_JSON_MISSING_FIELD; }

    /* Field value validation */
    if (out->command[0] == '\0' &&
        strcmp(out->intent, "unsupported") != 0) {
        snprintf(err, err_len, "empty field: command");
        return AI_JSON_INVALID_FIELD;
    }
    if (out->explanation[0] == '\0') {
        snprintf(err, err_len, "empty field: explanation");
        return AI_JSON_INVALID_FIELD;
    }

    static const char *const valid_risks[] = {
        "read_only", "write", "destructive", "network", "unknown", NULL
    };
    int risk_ok = 0;
    for (int i = 0; valid_risks[i]; i++) {
        if (strcmp(out->risk, valid_risks[i]) == 0) { risk_ok = 1; break; }
    }
    if (!risk_ok) {
        snprintf(err, err_len, "invalid risk value: '%s'", out->risk);
        return AI_JSON_INVALID_FIELD;
    }

    if (out->confidence < 0.0 || out->confidence > 1.0) {
        snprintf(err, err_len, "confidence out of range: %g", out->confidence);
        return AI_JSON_INVALID_FIELD;
    }

    err[0] = '\0';
    return AI_JSON_OK;
}

const char *ai_json_status_name(ai_json_status_t status)
{
    switch (status) {
        case AI_JSON_OK:            return "ok";
        case AI_JSON_PARSE_ERROR:   return "parse_error";
        case AI_JSON_MISSING_FIELD: return "missing_field";
        case AI_JSON_INVALID_FIELD: return "invalid_field";
        case AI_JSON_TRUNCATED:     return "truncated";
        default:                    return "unknown";
    }
}
