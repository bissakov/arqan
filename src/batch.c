/** Sequential tool batches: bounded inputs and structured results. */
#include "agent.h"

#include <math.h>
#include <stdio.h>

static b8 schema_equal(const JVal *a, const JVal *b) {
    if (!a || !b || a->type != b->type) return false;
    switch (a->type) {
        case J_NULL: return true;
        case J_BOOL: return a->u.b == b->u.b;
        case J_NUM: return a->u.n == b->u.n;
        case J_STR: return str_eq(a->u.s, b->u.s);
        default: return false;
    }
}

static const char *schema_type(const JVal *value) {
    switch (value->type) {
        case J_NULL: return "null";
        case J_BOOL: return "a boolean";
        case J_NUM: return "a number";
        case J_STR: return "a string";
        case J_ARR: return "an array";
        case J_OBJ: return "an object";
    }
    return "valid JSON";
}

static b8 schema_is_type(const JVal *value, Str type) {
    if (str_eq(type, STR("null"))) return value->type == J_NULL;
    if (str_eq(type, STR("boolean"))) return value->type == J_BOOL;
    if (str_eq(type, STR("number"))) return value->type == J_NUM;
    if (str_eq(type, STR("integer")))
        return value->type == J_NUM && isfinite(value->u.n)
               && floor(value->u.n) == value->u.n;
    if (str_eq(type, STR("string"))) return value->type == J_STR;
    if (str_eq(type, STR("array"))) return value->type == J_ARR;
    if (str_eq(type, STR("object"))) return value->type == J_OBJ;
    return true;
}

static b8 schema_error(char *err, size_t cap, const char *path,
                       const char *message) {
    if (!cap) return false;
    size_t n = 0;
    while (path[n] && n + 1 < cap) {
        err[n] = path[n];
        n++;
    }
    if (n + 1 < cap) err[n++] = ' ';
    size_t at = 0;
    while (message[at] && n + 1 < cap) err[n++] = message[at++];
    err[n] = '\0';
    return false;
}

static b8 schema_path(char *out, size_t cap, const char *base, Str key) {
    Str clipped = str_clip_utf8(key, 128);
    i32 n = snprintf(out, cap, "%s.%.*s", base, (i32)clipped.n, clipped.p);
    return n > 0 && (size_t)n < cap;
}

static b8 schema_validate(const JVal *value, const JVal *schema,
                          const char *path, char *err, size_t err_cap,
                          size_t depth) {
    if (!schema || schema->type != J_OBJ || depth > 32) return true;
    const JVal *type = json_get(schema, STR("type"));
    if (type && type->type == J_STR && !schema_is_type(value, type->u.s)) {
        char message[96];
        const char *want = str_eq(type->u.s, STR("integer"))   ? "an integer"
                           : str_eq(type->u.s, STR("array"))   ? "an array"
                           : str_eq(type->u.s, STR("object"))  ? "an object"
                           : str_eq(type->u.s, STR("boolean")) ? "a boolean"
                           : str_eq(type->u.s, STR("number"))  ? "a number"
                           : str_eq(type->u.s, STR("null"))    ? "null"
                                                               : "a string";
        snprintf(message, sizeof message, "must be %s, not %s", want,
                 schema_type(value));
        return schema_error(err, err_cap, path, message);
    }
    const JVal *constant = json_get(schema, STR("const"));
    if (constant && !schema_equal(value, constant))
        return schema_error(err, err_cap, path, "has an unsupported value");
    const JVal *values = json_get(schema, STR("enum"));
    if (values && values->type == J_ARR) {
        b8 found = false;
        for (size_t i = 0; i < values->u.arr.n; i++)
            if (schema_equal(value, &values->u.arr.items[i])) found = true;
        if (!found)
            return schema_error(err, err_cap, path,
                                "has a value outside the allowed list");
    }
    if (value->type == J_NUM) {
        const JVal *minimum = json_get(schema, STR("minimum"));
        const JVal *maximum = json_get(schema, STR("maximum"));
        char message[96];
        if (minimum && minimum->type == J_NUM && value->u.n < minimum->u.n) {
            snprintf(message, sizeof message, "must be at least %.17g",
                     minimum->u.n);
            return schema_error(err, err_cap, path, message);
        }
        if (maximum && maximum->type == J_NUM && value->u.n > maximum->u.n) {
            snprintf(message, sizeof message, "must be at most %.17g",
                     maximum->u.n);
            return schema_error(err, err_cap, path, message);
        }
    }
    if (value->type == J_ARR) {
        const JVal *minimum = json_get(schema, STR("minItems"));
        const JVal *maximum = json_get(schema, STR("maxItems"));
        const JVal *items = json_get(schema, STR("items"));
        if (minimum && minimum->type == J_NUM
            && (f64)value->u.arr.n < minimum->u.n)
            return schema_error(err, err_cap, path, "has too few items");
        if (maximum && maximum->type == J_NUM
            && (f64)value->u.arr.n > maximum->u.n)
            return schema_error(err, err_cap, path, "has too many items");
        for (size_t i = 0; items && i < value->u.arr.n; i++) {
            char child[256];
            i32 n = snprintf(child, sizeof child, "%s[%zu]", path, i);
            if (n <= 0 || (size_t)n >= sizeof child)
                return schema_error(err, err_cap, path, "is nested too deeply");
            if (!schema_validate(&value->u.arr.items[i], items, child, err,
                                 err_cap, depth + 1))
                return false;
        }
    }
    if (value->type != J_OBJ) return true;
    const JVal *required = json_get(schema, STR("required"));
    if (required && required->type == J_ARR) {
        for (size_t i = 0; i < required->u.arr.n; i++) {
            const JVal *key = &required->u.arr.items[i];
            if (key->type != J_STR || json_get(value, key->u.s)) continue;
            char child[256];
            if (!schema_path(child, sizeof child, path, key->u.s))
                return schema_error(err, err_cap, path, "is nested too deeply");
            return schema_error(err, err_cap, child, "is required");
        }
    }
    const JVal *properties = json_get(schema, STR("properties"));
    const JVal *additional = json_get(schema, STR("additionalProperties"));
    for (const JVal *field = value->u.obj.head; field; field = field->next) {
        const JVal *field_schema = json_get(properties, field->key);
        char child[256];
        if (!schema_path(child, sizeof child, path, field->key))
            return schema_error(err, err_cap, path, "is nested too deeply");
        if (!field_schema) {
            if (additional && additional->type == J_BOOL && !additional->u.b)
                return schema_error(err, err_cap, child, "is not allowed");
            continue;
        }
        if (!schema_validate(field, field_schema, child, err, err_cap,
                             depth + 1))
            return false;
    }
    return true;
}

static void schema_write_compact(Buf *out, const JVal *value) {
    if (!value) return;
    if (value->type == J_OBJ) {
        buf_putc(out, '{');
        b8 first = true;
        for (const JVal *field = value->u.obj.head; field;
             field = field->next) {
            if (str_eq(field->key, STR("description"))) continue;
            if (!first) buf_putc(out, ',');
            first = false;
            buf_json_str(out, field->key);
            buf_putc(out, ':');
            schema_write_compact(out, field);
        }
        buf_putc(out, '}');
        return;
    }
    if (value->type == J_ARR) {
        buf_putc(out, '[');
        for (size_t i = 0; i < value->u.arr.n; i++) {
            if (i) buf_putc(out, ',');
            schema_write_compact(out, &value->u.arr.items[i]);
        }
        buf_putc(out, ']');
        return;
    }
    json_write(out, value);
}

Str batch_compact_schema(Str schema, Arena *persist, Arena *scratch) {
    size_t mark = scratch->off;
    const JVal *value = json_parse(scratch, schema);
    if (!value) {
        scratch->off = mark;
        return schema;
    }
    Buf out;
    buf_init(&out, persist, schema.n);
    schema_write_compact(&out, value);
    scratch->off = mark;
    return buf_ok(&out) ? buf_finish(&out) : schema;
}

static size_t batch_schema_choices(const ToolRegistry *r, AgentMode mode,
                                   ToolAudience audience) {
    size_t n = 0;
    for (size_t i = 0; i < r->n; i++)
        if (tools_available_to(r, i, mode, audience) && tools_batchable(r, i))
            n++;
    return n;
}

void batch_write_schema(Buf *out, const ToolRegistry *r, AgentMode mode,
                        ToolAudience audience) {
    size_t choices = batch_schema_choices(r, mode, audience);
    if (!choices) {
        size_t batch = tools_find(r, STR("batch"));
        if (batch != TOOL_NONE) buf_puts(out, r->schema[batch]);
        return;
    }
    buf_puts(out, STR("{\"type\":\"object\",\"properties\":{\"steps\":{"
                      "\"type\":\"array\",\"minItems\":1,\"maxItems\":8,"
                      "\"items\":{\"oneOf\":["));
    b8 first = true;
    for (size_t i = 0; i < r->n; i++) {
        if (!tools_available_to(r, i, mode, audience) || !tools_batchable(r, i))
            continue;
        if (!first) buf_putc(out, ',');
        first = false;
        buf_puts(out, STR("{\"type\":\"object\",\"properties\":{\"tool\":{"
                          "\"const\":"));
        buf_json_str(out, r->name[i]);
        buf_puts(out, STR("},\"args\":"));
        buf_puts(out, r->batch_schema[i]);
        buf_puts(out, STR("},\"required\":[\"tool\",\"args\"],"
                          "\"additionalProperties\":false}"));
    }
    buf_puts(out, STR("]}}},\"required\":[\"steps\"],"
                      "\"additionalProperties\":false}"));
}

size_t batch_schema_bytes(const ToolRegistry *r, AgentMode mode,
                          ToolAudience audience) {
    size_t n = 192;
    for (size_t i = 0; i < r->n; i++) {
        if (!tools_available_to(r, i, mode, audience) || !tools_batchable(r, i))
            continue;
        n += 128 + r->name[i].n * 6 + r->batch_schema[i].n;
    }
    return n;
}

b8 batch_parse(ToolBatch *batch, const ToolRegistry *r, AgentMode mode,
               Str args, Arena *scratch, char *err, size_t err_cap) {
    *batch = (ToolBatch){0};
    const JVal *root = json_parse(scratch, args);
    const JVal *steps = json_get(root, STR("steps"));
    if (!root || root->type != J_OBJ || !steps || steps->type != J_ARR
        || !steps->u.arr.n || steps->u.arr.n > AGENT_MAX_BATCH_STEPS) {
        snprintf(err, err_cap, "batch requires 1 through %u steps in an array",
                 AGENT_MAX_BATCH_STEPS);
        return false;
    }
    batch->n = steps->u.arr.n;
    if (!root->u.obj.head || root->u.obj.head->next
        || !str_eq(root->u.obj.head->key, STR("steps"))) {
        snprintf(err, err_cap, "batch accepts only the steps field");
        return false;
    }
    for (size_t i = 0; i < batch->n; i++) {
        const JVal *item = &steps->u.arr.items[i];
        const JVal *args_value = json_get(item, STR("args"));
        BatchStep *step = &batch->steps[i];
        step->name = json_str(item, STR("tool"));
        if (item->type != J_OBJ || !step->name.n || !args_value
            || args_value->type != J_OBJ) {
            snprintf(err, err_cap,
                     "step %zu requires a string tool and object args", i + 1);
            return false;
        }
        size_t tool_fields = 0, args_fields = 0;
        for (const JVal *field = item->u.obj.head; field; field = field->next) {
            if (str_eq(field->key, STR("tool")))
                tool_fields++;
            else if (str_eq(field->key, STR("args")))
                args_fields++;
            else
                break;
        }
        const JVal *first = item->u.obj.head;
        if (tool_fields != 1 || args_fields != 1 || !first || !first->next
            || first->next->next) {
            snprintf(err, err_cap, "step %zu accepts only tool and args fields",
                     i + 1);
            return false;
        }
        step->tool = tools_find(r, step->name);
        if (step->tool == TOOL_NONE || !tools_available(r, step->tool, mode)) {
            snprintf(err, err_cap,
                     "step %zu: %.*s is not available in this mode or session",
                     i + 1, (i32)str_clip_utf8(step->name, 128).n,
                     step->name.p);
            return false;
        }
        if (!tools_batchable(r, step->tool)) {
            snprintf(
                err, err_cap,
                "step %zu: %.*s cannot run inside batch; call it separately",
                i + 1, (i32)str_clip_utf8(step->name, 128).n, step->name.p);
            return false;
        }
        const JVal *schema = json_parse(scratch, r->schema[step->tool]);
        char detail[256] = {0};
        if (!schema
            || !schema_validate(args_value, schema, "args", detail,
                                sizeof detail, 0)) {
            snprintf(err, err_cap, "step %zu %s", i + 1,
                     detail[0] ? detail : "has an invalid tool schema");
            return false;
        }
        Buf encoded;
        buf_init(&encoded, scratch, 256);
        json_write(&encoded, args_value);
        if (!buf_ok(&encoded)) {
            snprintf(err, err_cap, "out of memory preparing step %zu", i + 1);
            return false;
        }
        step->args = buf_finish(&encoded);
        if (conv_args_are_stub(step->args, scratch)) {
            snprintf(
                err, err_cap,
                "step %zu: elided arguments are not an input; send complete args",
                i + 1);
            return false;
        }
    }
    return true;
}

void batch_write(Buf *out, const ToolBatch *batch, Str status, size_t shown) {
    buf_puts(out, STR("{\"steps\":["));
    for (size_t i = 0; i < shown && i < batch->n; i++) {
        const BatchStep *step = &batch->steps[i];
        if (i) buf_putc(out, ',');
        buf_puts(out, STR("{\"tool\":"));
        buf_json_str(out, step->name);
        buf_puts(out, STR(",\"status\":"));
        buf_json_str(out, step->status);
        buf_puts(out, STR(",\"result\":"));
        buf_json_str(out, step->result);
        buf_putf(out, ",\"ms\":%u", step->ms);
        if (step->execution.job_id)
            buf_putf(out, ",\"job_id\":%u", step->execution.job_id);
        if (step->execution.exit_code)
            buf_putf(out, ",\"exit_code\":%d", step->execution.exit_code);
        buf_putc(out, '}');
    }
    buf_puts(out, STR("],\"status\":"));
    buf_json_str(out, status);
    buf_putf(out, ",\"attempted\":%zu,\"skipped\":%zu,\"total\":%zu}",
             batch->attempted, batch->n - batch->attempted, batch->n);
}

void batch_summary(Buf *out, Str status, size_t attempted, size_t total) {
    buf_putf(out, "batch %.*s: %zu/%zu steps attempted", (i32)status.n,
             status.p, attempted, total);
    if (total > attempted)
        buf_putf(out, "; %zu %s", total - attempted,
                 str_eq(status, STR("running")) ? "remaining" : "skipped");
}
