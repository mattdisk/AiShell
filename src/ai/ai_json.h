#ifndef AISHELL_AI_JSON_H
#define AISHELL_AI_JSON_H

#include <stddef.h>

/* -------------------------------------------------------------------------
 * Status codes returned by ai_json_parse_result().
 * ---------------------------------------------------------------------- */
typedef enum {
    AI_JSON_OK = 0,        /* parsed and validated successfully */
    AI_JSON_PARSE_ERROR,   /* malformed JSON (bad syntax) */
    AI_JSON_MISSING_FIELD, /* a required field is absent */
    AI_JSON_INVALID_FIELD, /* a field is present but has an invalid value */
    AI_JSON_TRUNCATED      /* a string value exceeded its destination buffer */
} ai_json_status_t;

/* -------------------------------------------------------------------------
 * ai_result_t — the structured output of a successful parse.
 *
 * This is the canonical type for an AI-generated command proposal.
 * It is defined here (not in ai_generate.h) so that the JSON parser,
 * the AI generator, and any future Claude client share one definition.
 *
 * Valid intent values: "shell_command" (default), "unsupported"
 * Valid risk values:   "read_only", "write", "destructive", "network", "unknown"
 * ---------------------------------------------------------------------- */
typedef struct {
    char   intent[32];       /* "shell_command" or "unsupported"; default "shell_command" */
    char   command[512];     /* proposed shell command; non-empty unless intent is "unsupported" */
    char   explanation[512]; /* one-sentence human explanation; non-empty */
    char   risk[32];         /* risk label; one of the five allowed values */
    double confidence;       /* model confidence in [0.0, 1.0] */
} ai_result_t;

/* -------------------------------------------------------------------------
 * ai_json_parse_result — parse and validate one AI response JSON object.
 *
 * Parses the null-terminated string 'json' into *out.
 * On any error, writes a human-readable message into err[0..err_len-1].
 *
 * Rules enforced:
 *   - command, explanation, risk, and confidence must be present.
 *   - 'intent' is optional; absent means "shell_command".
 *   - 'intent' must be "shell_command" or "unsupported".
 *   - 'command' must be non-empty unless intent is "unsupported".
 *   - 'explanation' must be non-empty.
 *   - 'risk' must be exactly one of the five documented values.
 *   - 'confidence' must be in [0.0, 1.0].
 *   - String values longer than their destination buffers are rejected.
 *   - Unknown extra fields are silently ignored.
 *   - Leading/trailing whitespace around the JSON object is accepted.
 * ---------------------------------------------------------------------- */
ai_json_status_t ai_json_parse_result(const char *json,
                                      ai_result_t *out,
                                      char *err,
                                      size_t err_len);

/* Return a short ASCII name for a status code (for error messages). */
const char *ai_json_status_name(ai_json_status_t status);

#endif /* AISHELL_AI_JSON_H */
