#include "util.h"
#include "datatypes.h"
#include "ds.h"
#include "parse.h"
#include "value.h"
#include "vm.h"

static const char UNEXPECTED_CLOSING_DELIM[] = "Unexpected closing delimiter";

/* The type of a ParseState */
typedef enum ParseType {
    PTYPE_ROOT,
    PTYPE_FORM,
    PTYPE_STRING,
    PTYPE_TOKEN
} ParseType;

/* Contain a parse state that goes on the parse stack */
struct GstParseState {
    ParseType type;
    union {
        struct {
			uint8_t endDelimiter;
            GstArray * array;
        } form;
        struct {
            GstBuffer * buffer;
            enum {
                STRING_STATE_BASE,
                STRING_STATE_ESCAPE,
                STRING_STATE_ESCAPE_UNICODE,
                STRING_STATE_ESCAPE_HEX
            } state;
        } string;
    } buf;
};

/* Handle error in parsing */
#define p_error(p, e) ((p)->error = (e), (p)->status = GST_PARSER_ERROR)

/* Get the top ParseState in the parse stack */
static GstParseState *parser_peek(GstParser *p) {
    if (!p->count) {
        p_error(p, "parser stack underflow");
        return NULL;
    }
    return p->data + p->count - 1;
}

/* Remove the top state from the ParseStack */
static GstParseState *parser_pop(GstParser * p) {
    if (!p->count) {
        p_error(p, "parser stack underflow");
        return NULL;
    }
    return p->data + --p->count;
}

/* Add a new, empty ParseState to the ParseStack. */
static void parser_push(GstParser *p, ParseType type, uint8_t character) {
    GstParseState *top;
    if (p->count >= p->cap) {
        uint32_t newCap = 2 * p->count;
        GstParseState *data = gst_alloc(p->vm, newCap);
        p->data = data;
        p->cap = newCap;
    }
    ++p->count;
    top = parser_peek(p);
    if (!top) return;
    top->type = type;
    switch (type) {
        case PTYPE_ROOT:
            break;
        case PTYPE_STRING:
            top->buf.string.state = STRING_STATE_BASE;
        case PTYPE_TOKEN:
            top->buf.string.buffer = gst_buffer(p->vm, 10);
            break;
        case PTYPE_FORM:
            top->buf.form.array = gst_array(p->vm, 10);
            if (character == '(') top->buf.form.endDelimiter = ')';
            if (character == '[') {
                top->buf.form.endDelimiter = ']';
                gst_array_push(p->vm, top->buf.form.array, gst_load_cstring(p->vm, "array"));
            }
            if (character == '{') {
                top->buf.form.endDelimiter = '}';
                gst_array_push(p->vm, top->buf.form.array, gst_load_cstring(p->vm, "obj"));
            }
            break;
    }
}

/* Append a value to the top-most state in the Parser's stack. */
static void parser_append(GstParser *p, GstValue x) {
    GstParseState *top = parser_peek(p);
    if (!top) return;
    switch (top->type) {
        case PTYPE_ROOT:
            p->value = x;
            p->status = GST_PARSER_FULL;
            break;
        case PTYPE_FORM:
            gst_array_push(p->vm, top->buf.form.array, x);
            break;
        default:
            p_error(p, "Expected container type.");
            break;
    }
}

/* Check if a character is whitespace */
static int is_whitespace(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0' || c == ',';
}

/* Check if a character is a valid symbol character */
static int is_symbol_char(uint8_t c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= ':') return 1;
    if (c >= '<' && c <= '@') return 1;
    if (c >= '*' && c <= '/') return 1;
    if (c >= '#' && c <= '&') return 1;
    if (c == '_') return 1;
    if (c == '^') return 1;
    if (c == '!') return 1;
    return 0;
}

/* Get an integer power of 10 */
static double exp10(int power) {
    if (power == 0) return 1;
    if (power > 0) {
        double result = 10;
        int currentPower = 1;
        while (currentPower * 2 <= power) {
            result = result * result;
            currentPower *= 2;
        }
        return result * exp10(power - currentPower);
    } else {
        return 1 / exp10(-power);
    }
}

/* Read a number from a string. Returns if successfuly
 * parsed a number from the enitre input string.
 * If returned 1, output is int ret.*/
static int read_number(const uint8_t *string, const uint8_t *end, double *ret, int forceInt) {
    int sign = 1, x = 0;
    double accum = 0, exp = 1, place = 1;
    /* Check the sign */
    if (*string == '-') {
        sign = -1;
        ++string;
    } else if (*string == '+') {
        ++string;
    }
    if (string >= end) return 0;
    while (string < end) {
        if (*string == '.' && !forceInt) {
            place = 0.1;
        } else if (!forceInt && (*string == 'e' || *string == 'E')) {
            /* Read the exponent */
            ++string;
            if (string >= end) return 0;
            if (!read_number(string, end, &exp, 1))
                return 0;
            exp = exp10(exp);
            break;
        } else {
            x = *string;
            if (x < '0' || x > '9') return 0;
            x -= '0';
            if (place < 1) {
                accum += x * place;
                place *= 0.1;
            } else {
                accum *= 10;
                accum += x;
            }
        }
        ++string;
    }
    *ret = accum * sign * exp;
    return 1;
}

/* Checks if a string slice is equal to a string constant */
static int check_str_const(const char *ref, const uint8_t *start, const uint8_t *end) {
    while (*ref && start < end) {
        if (*ref != *(char *)start) return 0;
        ++ref;
        ++start;
    }
    return !*ref && start == end;
}

/* Build from the token buffer */
static GstValue build_token(GstParser *p, GstBuffer *buf) {
    GstValue x;
    GstNumber number;
    uint8_t * data = buf->data;
    uint8_t * back = data + buf->count;
    if (read_number(data, back, &number, 0)) {
        x.type = GST_NUMBER;
        x.data.number = number;
    } else if (check_str_const("nil", data, back)) {
        x.type = GST_NIL;
        x.data.boolean = 0;
    } else if (check_str_const("false", data, back)) {
        x.type = GST_BOOLEAN;
        x.data.boolean = 0;
    } else if (check_str_const("true", data, back)) {
        x.type = GST_BOOLEAN;
        x.data.boolean = 1;
    } else {
        if (buf->data[0] >= '0' && buf->data[0] <= '9') {
            p_error(p, "Symbols cannot start with digits.");
            x.type = GST_NIL;
        } else {
            x.type = GST_STRING;
            x.data.string = gst_buffer_to_string(p->vm, buf);
        }
    }
    return x;
}

/* Handle parsing a token */
static int token_state(GstParser *p, uint8_t c) {
    GstParseState *top = parser_peek(p);
    GstBuffer *buf = top->buf.string.buffer;
    if (is_whitespace(c) || c == ')' || c == ']' || c == '}') {
        parser_pop(p);
        parser_append(p, build_token(p, buf));
        return !(c == ')' || c == ']' || c == '}');
    } else if (is_symbol_char(c)) {
        gst_buffer_push(p->vm, buf, c);
        return 1;
    } else {
        p_error(p, "Expected symbol character.");
        return 1;
    }
}

/* Handle parsing a string literal */
static int string_state(GstParser *p, uint8_t c) {
    GstParseState *top = parser_peek(p);
    switch (top->buf.string.state) {
        case STRING_STATE_BASE:
            if (c == '\\') {
                top->buf.string.state = STRING_STATE_ESCAPE;
            } else if (c == '"') {
                /* Load a quote form to get the string literal */
                GstValue x, array;
                x.type = GST_STRING;
                x.data.string = gst_buffer_to_string(p->vm, top->buf.string.buffer);
                array.type = GST_ARRAY;
                array.data.array = gst_array(p->vm, 2);
                gst_array_push(p->vm, array.data.array, gst_load_cstring(p->vm, "quote"));
                gst_array_push(p->vm, array.data.array, x);
                parser_pop(p);
                parser_append(p, array);
            } else {
                gst_buffer_push(p->vm, top->buf.string.buffer, c);
            }
            break;
        case STRING_STATE_ESCAPE:
            {
                uint8_t next;
                switch (c) {
                    case 'n': next = '\n'; break;
                    case 'r': next = '\r'; break;
                    case 't': next = '\t'; break;
                    case 'f': next = '\f'; break;
                    case '0': next = '\0'; break;
                    case '"': next = '"'; break;
                    case '\'': next = '\''; break;
                    case 'z': next = '\0'; break;
                    default:
                              p_error(p, "Unknown string escape sequence.");
                              return 1;
                }
                gst_buffer_push(p->vm, top->buf.string.buffer, next);
                top->buf.string.state = STRING_STATE_BASE;
            }
            break;
        case STRING_STATE_ESCAPE_HEX:
            break;
        case STRING_STATE_ESCAPE_UNICODE:
            break;
    }
    return 1;
}

/* Root state of the parser */
static int root_state(GstParser *p, uint8_t c) {
    if (c == ']' || c == ')' || c == '}') {
        p_error(p, UNEXPECTED_CLOSING_DELIM);
        return 1;
    }
    if (c == '(' || c == '[' || c == '{') {
        parser_push(p, PTYPE_FORM, c);
        return 1;
    }
    if (c == '"') {
        parser_push(p, PTYPE_STRING, c);
        return 1;
    }
    if (is_whitespace(c)) return 1;
    if (is_symbol_char(c)) {
        parser_push(p, PTYPE_TOKEN, c);
        return 0;
    }
    p_error(p, "Unexpected character.");
    return 1;
}

/* Handle parsing a form */
static int form_state(GstParser *p, uint8_t c) {
    GstParseState *top = parser_peek(p);
    if (c == top->buf.form.endDelimiter) {
        GstArray *array = top->buf.form.array;
        GstValue x;
        x.type = GST_ARRAY;
        x.data.array = array;
    	parser_pop(p);
        parser_append(p, x);
        return 1;
    }
    return root_state(p, c);
}

/* Handle a character */
static int dispatch_char(GstParser *p, uint8_t c) {
    int done = 0;
    while (!done && p->status == GST_PARSER_PENDING) {
        GstParseState *top = parser_peek(p);
        switch (top->type) {
            case PTYPE_ROOT:
                done = root_state(p, c);
                break;
            case PTYPE_TOKEN:
                done = token_state(p, c);
                break;
            case PTYPE_FORM:
                done = form_state(p, c);
                break;
            case PTYPE_STRING:
                done = string_state(p, c);
                break;
        }
    }
    ++p->index;
    return !done;
}

/* Parse a C style string. The first value encountered when parsed is put
 * in p->value. The string variable is then updated to the next char that
 * was not read. Returns 1 if any values were read, otherwise returns 0.
 * Returns the number of bytes read.
 */
int gst_parse_cstring(GstParser *p, const char *string) {
    int bytesRead = 0;
    p->status = GST_PARSER_PENDING;
    while ((p->status == GST_PARSER_PENDING) && (string[bytesRead] != '\0')) {
        dispatch_char(p, string[bytesRead++]);
    }
    return bytesRead;
}

/* Parser initialization (memory allocation) */
void gst_parser(GstParser *p, Gst *vm) {
    p->vm = vm;
    GstParseState *data = gst_alloc(vm, sizeof(GstParseState) * 10);
    p->data = data;
    p->count = 0;
    p->cap = 10;
    p->index = 0;
    p->error = NULL;
    p->status = GST_PARSER_PENDING;
    p->value.type = GST_NIL;
    parser_push(p, PTYPE_ROOT, ' ');
}
