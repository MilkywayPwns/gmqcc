#include <stdio.h>
#include <stdarg.h>

#include "gmqcc.h"
#include "lexer.h"

typedef struct {
    char *name;
    ast_expression *var;
} varentry_t;

typedef struct {
    lex_file *lex;
    int      tok;

    MEM_VECTOR_MAKE(varentry_t, globals);
    MEM_VECTOR_MAKE(ast_function*, functions);
    MEM_VECTOR_MAKE(ast_value*, imm_float);
    MEM_VECTOR_MAKE(ast_value*, imm_string);
    MEM_VECTOR_MAKE(ast_value*, imm_vector);

    ast_function *function;
    MEM_VECTOR_MAKE(varentry_t, locals);
    size_t blocklocal;

    size_t errors;
} parser_t;

MEM_VEC_FUNCTIONS(parser_t, varentry_t, globals)
MEM_VEC_FUNCTIONS(parser_t, ast_value*, imm_float)
MEM_VEC_FUNCTIONS(parser_t, ast_value*, imm_string)
MEM_VEC_FUNCTIONS(parser_t, ast_value*, imm_vector)
MEM_VEC_FUNCTIONS(parser_t, varentry_t, locals)
MEM_VEC_FUNCTIONS(parser_t, ast_function*, functions)

void parseerror(parser_t *parser, const char *fmt, ...)
{
	va_list ap;

	parser->errors++;

    if (parser)
	    printf("error %s:%lu: ", parser->lex->tok->ctx.file, (unsigned long)parser->lex->tok->ctx.line);
	else
	    printf("error: ");

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	printf("\n");
}

bool parser_next(parser_t *parser)
{
    /* lex_do kills the previous token */
    parser->tok = lex_do(parser->lex);
    if (parser->tok == TOKEN_EOF || parser->tok >= TOKEN_ERROR)
        return false;
    return true;
}

/* lift a token out of the parser so it's not destroyed by parser_next */
token *parser_lift(parser_t *parser)
{
    token *tok = parser->lex->tok;
    parser->lex->tok = NULL;
    return tok;
}

#define parser_tokval(p) (p->lex->tok->value)
#define parser_token(p)  (p->lex->tok)
#define parser_ctx(p)    (p->lex->tok->ctx)

ast_value* parser_const_float(parser_t *parser, double d)
{
    size_t i;
    ast_value *out;
    for (i = 0; i < parser->imm_float_count; ++i) {
        if (parser->imm_float[i]->constval.vfloat == d)
            return parser->imm_float[i];
    }
    out = ast_value_new(parser_ctx(parser), "#IMMEDIATE", TYPE_FLOAT);
    out->isconst = true;
    out->constval.vfloat = d;
    if (!parser_t_imm_float_add(parser, out)) {
        ast_value_delete(out);
        return NULL;
    }
    return out;
}

ast_value* parser_const_string(parser_t *parser, const char *str)
{
    size_t i;
    ast_value *out;
    for (i = 0; i < parser->imm_string_count; ++i) {
        if (!strcmp(parser->imm_string[i]->constval.vstring, str))
            return parser->imm_string[i];
    }
    out = ast_value_new(parser_ctx(parser), "#IMMEDIATE", TYPE_STRING);
    out->isconst = true;
    out->constval.vstring = util_strdup(str);
    if (!parser_t_imm_string_add(parser, out)) {
        ast_value_delete(out);
        return NULL;
    }
    return out;
}

ast_value* parser_const_vector(parser_t *parser, vector v)
{
    size_t i;
    ast_value *out;
    for (i = 0; i < parser->imm_vector_count; ++i) {
        if (!memcmp(&parser->imm_vector[i]->constval.vvec, &v, sizeof(v)))
            return parser->imm_vector[i];
    }
    out = ast_value_new(parser_ctx(parser), "#IMMEDIATE", TYPE_VECTOR);
    out->isconst = true;
    out->constval.vvec = v;
    if (!parser_t_imm_vector_add(parser, out)) {
        ast_value_delete(out);
        return NULL;
    }
    return out;
}

ast_expression* parser_find_global(parser_t *parser, const char *name)
{
    size_t i;
    for (i = 0; i < parser->globals_count; ++i) {
        if (!strcmp(parser->globals[i].name, name))
            return parser->globals[i].var;
    }
    return NULL;
}

ast_expression* parser_find_local(parser_t *parser, const char *name, size_t upto)
{
    size_t i;
    ast_value *fun;
    for (i = parser->locals_count; i > upto;) {
        --i;
        if (!strcmp(parser->locals[i].name, name))
            return parser->locals[i].var;
    }
    fun = parser->function->vtype;
    for (i = 0; i < fun->expression.params_count; ++i) {
        if (!strcmp(fun->expression.params[i]->name, name))
            return (ast_expression*)(fun->expression.params[i]);
    }
    return NULL;
}

ast_expression* parser_find_var(parser_t *parser, const char *name)
{
    ast_expression *v;
    v         = parser_find_local(parser, name, 0);
    if (!v) v = parser_find_global(parser, name);
    return v;
}

typedef struct {
    MEM_VECTOR_MAKE(ast_value*, p);
} paramlist_t;
MEM_VEC_FUNCTIONS(paramlist_t, ast_value*, p)

static ast_value *parser_parse_type(parser_t *parser, int basetype, bool *isfunc)
{
    paramlist_t params;
    ast_value *var;
    lex_ctx   ctx = parser_ctx(parser);
    int vtype = basetype;
    int temptype;
    size_t i;

    MEM_VECTOR_INIT(&params, p);

    *isfunc = false;

    if (parser->tok == '(') {
        *isfunc = true;
        while (true) {
            ast_value *param;
            bool dummy;

            if (!parser_next(parser))
                goto on_error;

            if (parser->tok == ')')
                break;

            temptype = parser_token(parser)->constval.t;
            if (!parser_next(parser))
                goto on_error;

            param = parser_parse_type(parser, temptype, &dummy);
            (void)dummy;

            if (!param)
                goto on_error;

            if (parser->tok == TOKEN_IDENT) {
                /* named parameter */
                if (!ast_value_set_name(param, parser_tokval(parser)))
                    goto on_error;
                if (!parser_next(parser))
                    goto on_error;
            }

            if (!paramlist_t_p_add(&params, param)) {
                parseerror(parser, "Out of memory while parsing typename");
                goto on_error;
            }

            if (parser->tok == ',')
                continue;
            if (parser->tok == ')')
                break;
            parseerror(parser, "Unexpected token");
            goto on_error;
        }
        if (!parser_next(parser))
            goto on_error;
    }

    var = ast_value_new(ctx, "<unnamed>", vtype);
    if (!var)
        goto on_error;
    MEM_VECTOR_MOVE(&params, p, &var->expression, params);
    return var;
on_error:
    for (i = 0; i < params.p_count; ++i)
        ast_value_delete(params.p[i]);
    MEM_VECTOR_CLEAR(&params, p);
    return NULL;
}

typedef struct
{
    size_t etype; /* 0 = expression, others are operators */
    int             paren;
    size_t          off;
    ast_expression *out;
    ast_block      *block; /* for commas and function calls */
    lex_ctx ctx;
} sy_elem;
typedef struct
{
    MEM_VECTOR_MAKE(sy_elem, out);
    MEM_VECTOR_MAKE(sy_elem, ops);
} shunt;
MEM_VEC_FUNCTIONS(shunt, sy_elem, out)
MEM_VEC_FUNCTIONS(shunt, sy_elem, ops)

static sy_elem syexp(lex_ctx ctx, ast_expression *v) {
    sy_elem e;
    e.etype = 0;
    e.out   = v;
    e.block = NULL;
    e.ctx   = ctx;
    e.paren = 0;
    return e;
}

static sy_elem syblock(lex_ctx ctx, ast_block *v) {
    sy_elem e;
    e.etype = 0;
    e.out   = (ast_expression*)v;
    e.block = v;
    e.ctx   = ctx;
    e.paren = 0;
    return e;
}

static sy_elem syop(lex_ctx ctx, const oper_info *op) {
    sy_elem e;
    e.etype = 1 + (op - operators);
    e.out   = NULL;
    e.block = NULL;
    e.ctx   = ctx;
    e.paren = 0;
    return e;
}

static sy_elem syparen(lex_ctx ctx, int p, size_t off) {
    sy_elem e;
    e.etype = 0;
    e.off   = off;
    e.out   = NULL;
    e.block = NULL;
    e.ctx   = ctx;
    e.paren = p;
    return e;
}

static bool parser_sy_pop(parser_t *parser, shunt *sy)
{
    const oper_info *op;
    lex_ctx ctx;
    ast_expression *out = NULL;
    ast_expression *exprs[3];
    ast_block      *blocks[3];
    size_t i;

    if (!sy->ops_count) {
        parseerror(parser, "internal error: missing operator");
        return false;
    }

    if (sy->ops[sy->ops_count-1].paren) {
        parseerror(parser, "unmatched parenthesis");
        return false;
    }

    op = &operators[sy->ops[sy->ops_count-1].etype - 1];
    ctx = sy->ops[sy->ops_count-1].ctx;

    if (sy->out_count < op->operands) {
        parseerror(parser, "internal error: not enough operands: %i", sy->out_count);
        return false;
    }

    sy->ops_count--;

    sy->out_count -= op->operands;
    for (i = 0; i < op->operands; ++i) {
        exprs[i]  = sy->out[sy->out_count+i].out;
        blocks[i] = sy->out[sy->out_count+i].block;
    }

    if (blocks[0] && !blocks[0]->exprs_count && op->id != opid1(',')) {
        parseerror(parser, "internal error: operator cannot be applied on empty blocks");
        return false;
    }

    switch (op->id)
    {
        default:
            parseerror(parser, "internal error: unhandled operand");
            return false;

        case opid1(','):
            if (blocks[0]) {
                if (!ast_block_exprs_add(blocks[0], exprs[1]))
                    return false;
            } else {
                blocks[0] = ast_block_new(ctx);
                if (!ast_block_exprs_add(blocks[0], exprs[0]) ||
                    !ast_block_exprs_add(blocks[0], exprs[1]))
                {
                    return false;
                }
            }
            if (!ast_block_set_type(blocks[0], exprs[1]))
                return false;

            sy->out[sy->out_count++] = syblock(ctx, blocks[0]);
            return true;

        case opid1('+'):
            if (exprs[0]->expression.vtype != exprs[1]->expression.vtype) {
                parseerror(parser, "Cannot add type %s and %s",
                           type_name[exprs[0]->expression.vtype],
                           type_name[exprs[1]->expression.vtype]);
                return false;
            }
            switch (exprs[0]->expression.vtype) {
                case TYPE_FLOAT:
                    out = (ast_expression*)ast_binary_new(ctx, INSTR_ADD_F, exprs[0], exprs[1]);
                    break;
                case TYPE_VECTOR:
                    out = (ast_expression*)ast_binary_new(ctx, INSTR_ADD_V, exprs[0], exprs[1]);
                    break;
                default:
                    parseerror(parser, "Cannot add type %s and %s",
                               type_name[exprs[0]->expression.vtype],
                               type_name[exprs[1]->expression.vtype]);
                    return false;
            };
            break;
        case opid1('-'):
            if (exprs[0]->expression.vtype != exprs[1]->expression.vtype) {
                parseerror(parser, "Cannot subtract type %s from %s",
                           type_name[exprs[1]->expression.vtype],
                           type_name[exprs[0]->expression.vtype]);
                return false;
            }
            switch (exprs[0]->expression.vtype) {
                case TYPE_FLOAT:
                    out = (ast_expression*)ast_binary_new(ctx, INSTR_SUB_F, exprs[0], exprs[1]);
                    break;
                case TYPE_VECTOR:
                    out = (ast_expression*)ast_binary_new(ctx, INSTR_SUB_V, exprs[0], exprs[1]);
                    break;
                default:
                    parseerror(parser, "Cannot add type %s from %s",
                               type_name[exprs[1]->expression.vtype],
                               type_name[exprs[0]->expression.vtype]);
                    return false;
            };
            break;
        case opid1('*'):
            if (exprs[0]->expression.vtype != exprs[1]->expression.vtype &&
                exprs[0]->expression.vtype != TYPE_VECTOR &&
                exprs[0]->expression.vtype != TYPE_FLOAT &&
                exprs[1]->expression.vtype != TYPE_VECTOR &&
                exprs[1]->expression.vtype != TYPE_FLOAT)
            {
                parseerror(parser, "Cannot multiply type %s from %s",
                           type_name[exprs[1]->expression.vtype],
                           type_name[exprs[0]->expression.vtype]);
                return false;
            }
            switch (exprs[0]->expression.vtype) {
                case TYPE_FLOAT:
                    if (exprs[1]->expression.vtype == TYPE_VECTOR)
                        out = (ast_expression*)ast_binary_new(ctx, INSTR_MUL_FV, exprs[0], exprs[1]);
                    else
                        out = (ast_expression*)ast_binary_new(ctx, INSTR_MUL_F, exprs[0], exprs[1]);
                    break;
                case TYPE_VECTOR:
                    if (exprs[1]->expression.vtype == TYPE_FLOAT)
                        out = (ast_expression*)ast_binary_new(ctx, INSTR_MUL_VF, exprs[0], exprs[1]);
                    else
                        out = (ast_expression*)ast_binary_new(ctx, INSTR_MUL_V, exprs[0], exprs[1]);
                    break;
                default:
                    parseerror(parser, "Cannot add type %s from %s",
                               type_name[exprs[1]->expression.vtype],
                               type_name[exprs[0]->expression.vtype]);
                    return false;
            };
            break;
        case opid1('/'):
            if (exprs[0]->expression.vtype != exprs[1]->expression.vtype ||
                exprs[0]->expression.vtype != TYPE_FLOAT)
            {
                parseerror(parser, "Cannot divide types %s and %s",
                           type_name[exprs[0]->expression.vtype],
                           type_name[exprs[1]->expression.vtype]);
                return false;
            }
            out = (ast_expression*)ast_binary_new(ctx, INSTR_DIV_F, exprs[0], exprs[1]);
            break;


        case opid1('='):
            out = (ast_expression*)ast_store_new(ctx,
                                                 type_store_instr[exprs[0]->expression.vtype],
                                                 exprs[0], exprs[1]);
            break;
    }

    if (!out) {
        parseerror(parser, "failed to apply operand %s", op->op);
        return false;
    }

    sy->out[sy->out_count++] = syexp(ctx, out);
    return true;
}

static bool parser_close_call(parser_t *parser, shunt *sy)
{
    /* was a function call */
    ast_expression *fun;
    ast_call       *call;

    size_t          fid;
    size_t          paramcount;

    sy->ops_count--;
    fid = sy->ops[sy->ops_count].off;

    /* out[fid] is the function
     * everything above is parameters...
     * 0 params = nothing
     * 1 params = ast_expression
     * more = ast_block
     */

    if (sy->out_count < 1 || sy->out_count <= fid) {
        parseerror(parser, "internal error: function call needs function and parameter list...");
        return false;
    }

    fun = sy->out[fid].out;

    call = ast_call_new(sy->ops[sy->ops_count].ctx, fun);
    if (!call) {
        parseerror(parser, "out of memory");
        return false;
    }

    if (fid+1 == sy->out_count) {
        /* no arguments */
        paramcount = 0;
    } else if (fid+2 == sy->out_count) {
        ast_block *params;
        sy->out_count--;
        params = sy->out[sy->out_count].block;
        if (!params) {
            /* 1 param */
            paramcount = 1;
            if (!ast_call_params_add(call, sy->out[sy->out_count].out)) {
                ast_delete(sy->out[sy->out_count].out);
                parseerror(parser, "out of memory");
                return false;
            }
        } else {
            paramcount = params->exprs_count;
            MEM_VECTOR_MOVE(params, exprs, call, params);
            ast_delete(params);
        }
    } else {
        parseerror(parser, "invalid function call");
        return false;
    }

    /* overwrite fid, the function, with a call */
    sy->out[fid] = syexp(call->expression.node.context, (ast_expression*)call);

    if (fun->expression.vtype != TYPE_FUNCTION) {
        parseerror(parser, "not a function");
        return false;
    }

    if (!fun->expression.next) {
        parseerror(parser, "could not determine function return type");
        return false;
    } else {
        if (fun->expression.params_count != paramcount) {
            parseerror(parser, "expected %i parameters, got %i", (int)fun->expression.params_count, paramcount);
            return false;
        }
    }

    return true;
}

static bool parser_close_paren(parser_t *parser, shunt *sy, bool functions_only)
{
    if (!sy->ops_count) {
        parseerror(parser, "unmatched closing paren");
        return false;
    }
    if (sy->ops[sy->ops_count-1].paren == 1) {
        parseerror(parser, "empty parenthesis expression");
        return false;
    }
    while (sy->ops_count) {
        if (sy->ops[sy->ops_count-1].paren == 'f') {
            if (!parser_close_call(parser, sy))
                return false;
            break;
        }
        if (sy->ops[sy->ops_count-1].paren == 1) {
            sy->ops_count--;
            return !functions_only;
        }
        if (!parser_sy_pop(parser, sy))
            return false;
    }
    return true;
}

static ast_expression* parser_expression(parser_t *parser)
{
    ast_expression *expr = NULL;
    shunt sy;
    bool wantop = false;

    MEM_VECTOR_INIT(&sy, out);
    MEM_VECTOR_INIT(&sy, ops);

    while (true)
    {
        if (!wantop)
        {
            bool nextwant = true;
            if (parser->tok == TOKEN_IDENT)
            {
                /* variable */
                ast_expression *var = parser_find_var(parser, parser_tokval(parser));
                if (!var) {
                    parseerror(parser, "unexpected ident: %s", parser_tokval(parser));
                    goto onerr;
                }
                if (!shunt_out_add(&sy, syexp(parser_ctx(parser), var))) {
                    parseerror(parser, "out of memory");
                    goto onerr;
                }
            }
            else if (parser->tok == TOKEN_FLOATCONST) {
                ast_value *val = parser_const_float(parser, (parser_token(parser)->constval.f));
                if (!val)
                    return false;
                if (!shunt_out_add(&sy, syexp(parser_ctx(parser), (ast_expression*)val))) {
                    parseerror(parser, "out of memory");
                    goto onerr;
                }
            }
            else if (parser->tok == TOKEN_INTCONST) {
                ast_value *val = parser_const_float(parser, (double)(parser_token(parser)->constval.i));
                if (!val)
                    return false;
                if (!shunt_out_add(&sy, syexp(parser_ctx(parser), (ast_expression*)val))) {
                    parseerror(parser, "out of memory");
                    goto onerr;
                }
            }
            else if (parser->tok == TOKEN_STRINGCONST) {
                ast_value *val = parser_const_string(parser, parser_tokval(parser));
                if (!val)
                    return false;
                if (!shunt_out_add(&sy, syexp(parser_ctx(parser), (ast_expression*)val))) {
                    parseerror(parser, "out of memory");
                    goto onerr;
                }
            }
            else if (parser->tok == TOKEN_VECTORCONST) {
                ast_value *val = parser_const_vector(parser, parser_token(parser)->constval.v);
                if (!val)
                    return false;
                if (!shunt_out_add(&sy, syexp(parser_ctx(parser), (ast_expression*)val))) {
                    parseerror(parser, "out of memory");
                    goto onerr;
                }
            }
            else if (parser->tok == '(') {
                nextwant = false; /* not expecting an operator next */
                if (!shunt_ops_add(&sy, syparen(parser_ctx(parser), 1, 0))) {
                    parseerror(parser, "out of memory");
                    goto onerr;
                }
            }
            else if (parser->tok == ')') {
                /* allowed for function calls */
                if (!parser_close_paren(parser, &sy, true))
                    goto onerr;
            }
            else {
                /* TODO: prefix operators */
                parseerror(parser, "expected statement");
                goto onerr;
            }
            wantop = nextwant;
            parser->lex->flags.noops = !wantop;
        } else {
            if (parser->tok == '(') {
                /* we expected an operator, this is the function-call operator */
                if (!shunt_ops_add(&sy, syparen(parser_ctx(parser), 'f', sy.out_count-1))) {
                    parseerror(parser, "out of memory");
                    goto onerr;
                }
            }
            else if (parser->tok == ')') {
                /* we do expect an operator next */
                /* closing an opening paren */
                if (!parser_close_paren(parser, &sy, false))
                    goto onerr;
            }
            else if (parser->tok != TOKEN_OPERATOR) {
                parseerror(parser, "expected operator or end of statement");
                goto onerr;
            }
            else {
                /* classify the operator */
                /* TODO: suffix operators */
                const oper_info *op;
                const oper_info *olast = NULL;
                size_t o;
                for (o = 0; o < operator_count; ++o) {
                    if (!(operators[o].flags & OP_PREFIX) &&
                        !(operators[o].flags & OP_SUFFIX) && /* remove this */
                        !strcmp(parser_tokval(parser), operators[o].op))
                    {
                        break;
                    }
                }
                if (o == operator_count) {
                    /* no operator found... must be the end of the statement */
                    break;
                }
                /* found an operator */
                op = &operators[o];

                if (sy.ops_count && !sy.ops[sy.ops_count-1].paren)
                    olast = &operators[sy.ops[sy.ops_count-1].etype-1];

                while (olast && (
                        (op->prec < olast->prec) ||
                        (op->assoc == ASSOC_LEFT && op->prec <= olast->prec) ) )
                {
                    if (!parser_sy_pop(parser, &sy))
                        goto onerr;
                    if (sy.ops_count && !sy.ops[sy.ops_count-1].paren)
                        olast = &operators[sy.ops[sy.ops_count-1].etype-1];
                }

                if (!shunt_ops_add(&sy, syop(parser_ctx(parser), op)))
                    goto onerr;
            }
            wantop = false;
            parser->lex->flags.noops = true;
        }
        if (!parser_next(parser)) {
            goto onerr;
        }
        if (parser->tok == ';') {
            break;
        }
    }
    if (!parser_next(parser)) {
        parseerror(parser, "Unexpected end of file");
        goto onerr;
    }

    while (sy.ops_count) {
        if (!parser_sy_pop(parser, &sy))
            goto onerr;
    }

    parser->lex->flags.noops = true;
    if (!sy.out_count) {
        parseerror(parser, "empty expression");
        expr = NULL;
    } else
        expr = sy.out[0].out;
    MEM_VECTOR_CLEAR(&sy, out);
    MEM_VECTOR_CLEAR(&sy, ops);
    return expr;

onerr:
    parser->lex->flags.noops = true;
    MEM_VECTOR_CLEAR(&sy, out);
    MEM_VECTOR_CLEAR(&sy, ops);
    return NULL;
}

static bool parser_variable(parser_t *parser, ast_block *localblock);
static ast_block* parser_parse_block(parser_t *parser);
static bool parser_parse_statement(parser_t *parser, ast_block *block, ast_expression **out)
{
    if (parser->tok == TOKEN_TYPENAME)
    {
        /* local variable */
        if (!block) {
            parseerror(parser, "cannot declare a variable from here");
            return false;
        }
        if (!parser_variable(parser, block))
            return false;
        *out = NULL;
        return true;
    }
    else if (parser->tok == TOKEN_KEYWORD)
    {
        if (!strcmp(parser_tokval(parser), "return"))
        {
            ast_expression *exp = NULL;
            ast_return     *ret = NULL;
            ast_value      *expected = parser->function->vtype;

            if (!parser_next(parser)) {
                parseerror(parser, "expected return expression");
                return false;
            }

            if (parser->tok != ';') {
                exp = parser_expression(parser);
                if (!exp)
                    return false;

                if (exp->expression.vtype != expected->expression.next->expression.vtype) {
                    parseerror(parser, "return with invalid expression");
                }

                ret = ast_return_new(exp->expression.node.context, exp);
                if (!ret) {
                    ast_delete(exp);
                    return false;
                }

                *out = (ast_expression*)ret;
            } else if (!parser_next(parser)) {
                parseerror(parser, "expected semicolon");
                if (expected->expression.next->expression.vtype != TYPE_VOID) {
                    parseerror(parser, "return without value");
                }
            }
            return true;
        }
        parseerror(parser, "Unexpected keyword");
        return false;
    }
    else if (parser->tok == '{')
    {
        ast_block *inner;
        inner = parser_parse_block(parser);
        if (!inner)
            return false;
        *out = (ast_expression*)inner;
        return true;
    }
    else
    {
        ast_expression *exp = parser_expression(parser);
        if (!exp)
            return false;
        *out = exp;
        return true;
    }
}

static void parser_pop_local(parser_t *parser)
{
    parser->locals_count--;
    mem_d(parser->locals[parser->locals_count].name);
}

static ast_block* parser_parse_block(parser_t *parser)
{
    size_t oldblocklocal;
    ast_block *block = NULL;

    oldblocklocal = parser->blocklocal;
    parser->blocklocal = parser->locals_count;

    if (!parser_next(parser)) { /* skip the '{' */
        parseerror(parser, "expected function body");
        goto cleanup;
    }

    block = ast_block_new(parser_ctx(parser));

    while (parser->tok != TOKEN_EOF && parser->tok < TOKEN_ERROR)
    {
        ast_expression *expr;
        if (parser->tok == '}')
            break;

        if (!parser_parse_statement(parser, block, &expr)) {
            ast_block_delete(block);
            block = NULL;
            goto cleanup;
        }
        if (!expr)
            continue;
        if (!ast_block_exprs_add(block, expr)) {
            ast_delete(expr);
            ast_block_delete(block);
            block = NULL;
            goto cleanup;
        }
    }

    if (parser->tok != '}') {
        ast_block_delete(block);
        block = NULL;
    } else {
        (void)parser_next(parser);
    }

cleanup:
    parser->blocklocal = oldblocklocal;
    /* unroll the local vector */
    while (parser->locals_count > parser->blocklocal)
        parser_pop_local(parser);
    return block;
}

static ast_expression* parser_parse_statement_or_block(parser_t *parser)
{
    ast_expression *expr;
    if (parser->tok == '{')
        return (ast_expression*)parser_parse_block(parser);
    if (!parser_parse_statement(parser, NULL, &expr))
        return NULL;
    return expr;
}

static bool parser_variable(parser_t *parser, ast_block *localblock)
{
    bool          isfunc = false;
    ast_function *func = NULL;
    lex_ctx       ctx;
    ast_value    *var;
    varentry_t    varent;

    int basetype = parser_token(parser)->constval.t;

    while (true)
    {
        if (!parser_next(parser)) { /* skip basetype or comma */
            parseerror(parser, "expected variable declaration");
            return false;
        }

        isfunc = false;
        func = NULL;
        ctx = parser_ctx(parser);
        var = parser_parse_type(parser, basetype, &isfunc);

        if (!var)
            return false;

        if (parser->tok != TOKEN_IDENT) {
            parseerror(parser, "expected variable name\n");
            return false;
        }

        if (!localblock && parser_find_global(parser, parser_tokval(parser))) {
            ast_value_delete(var);
            parseerror(parser, "global already exists: %s\n", parser_tokval(parser));
            return false;
        }

        if (localblock && parser_find_local(parser, parser_tokval(parser), parser->blocklocal)) {
            ast_value_delete(var);
            parseerror(parser, "local variable already exists: %s\n", parser_tokval(parser));
            return false;
        }

        if (!ast_value_set_name(var, parser_tokval(parser))) {
            parseerror(parser, "failed to set variable name\n");
            ast_value_delete(var);
            return false;
        }

        if (isfunc) {
            /* a function was defined */
            ast_value *fval;

            /* turn var into a value of TYPE_FUNCTION, with the old var
             * as return type
             */
            fval = ast_value_new(ctx, var->name, TYPE_FUNCTION);
            func = ast_function_new(ctx, var->name, fval);
            if (!fval || !func) {
                ast_value_delete(var);
                if (fval) ast_value_delete(fval);
                if (func) ast_function_delete(func);
                return false;
            }

            fval->expression.next = (ast_expression*)var;
            MEM_VECTOR_MOVE(&var->expression, params, &fval->expression, params);

            if (!parser_t_functions_add(parser, func)) {
                ast_value_delete(var);
                if (fval) ast_value_delete(fval);
                if (func) ast_function_delete(func);
                return false;
            }

            var = fval;
        }

        varent.name = util_strdup(var->name);
        varent.var = (ast_expression*)var;
        if (var->expression.vtype == TYPE_VECTOR)
        {
            size_t len = strlen(varent.name);
            varentry_t vx, vy, vz;
            vx.var = (ast_expression*)ast_member_new(var->expression.node.context, (ast_expression*)var, 0);
            vy.var = (ast_expression*)ast_member_new(var->expression.node.context, (ast_expression*)var, 1);
            vz.var = (ast_expression*)ast_member_new(var->expression.node.context, (ast_expression*)var, 2);
            vx.name = mem_a(len+3);
            vy.name = mem_a(len+3);
            vz.name = mem_a(len+3);
            strcpy(vx.name, varent.name);
            strcpy(vy.name, varent.name);
            strcpy(vz.name, varent.name);
            vx.name[len] = vy.name[len] = vz.name[len] = '_';
            vx.name[len+1] = 'x';
            vy.name[len+1] = 'y';
            vz.name[len+1] = 'z';
            vx.name[len+2] = vy.name[len+2] = vz.name[len+2] = 0;

            if (!localblock) {
                (void)!parser_t_globals_add(parser, varent);
                (void)!parser_t_globals_add(parser, vx);
                (void)!parser_t_globals_add(parser, vy);
                (void)!parser_t_globals_add(parser, vz);
            } else {
                (void)!parser_t_locals_add(parser, varent);
                (void)!parser_t_locals_add(parser, vx);
                (void)!parser_t_locals_add(parser, vy);
                (void)!parser_t_locals_add(parser, vz);
            }
        }
        else
        {
            if ( (!localblock && !parser_t_globals_add(parser, varent)) ||
                 ( localblock && !parser_t_locals_add(parser, varent)) )
            {
                ast_value_delete(var);
                return false;
            }
        }
        if (localblock && !ast_block_locals_add(localblock, var))
        {
            parser_pop_local(parser);
            ast_value_delete(var);
            return false;
        }

        if (!parser_next(parser)) {
            ast_value_delete(var);
            return false;
        }

        if (parser->tok == ';') {
            if (!parser_next(parser))
                return parser->tok == TOKEN_EOF;
            return true;
        }

        if (parser->tok == ',') {
            /* another var */
            continue;
        }

        if (parser->tok != '=') {
            parseerror(parser, "expected '=' or ';'");
            return false;
        }

        if (!parser_next(parser))
            return false;

        if (parser->tok == '#') {
            if (localblock) {
                parseerror(parser, "cannot declare builtins within functions");
                return false;
            }
            if (!isfunc || !func) {
                parseerror(parser, "unexpected builtin number, '%s' is not a function", var->name);
                return false;
            }
            if (!parser_next(parser)) {
                parseerror(parser, "expected builtin number");
                return false;
            }
            if (parser->tok != TOKEN_INTCONST) {
                parseerror(parser, "builtin number must be an integer constant");
                return false;
            }
            if (parser_token(parser)->constval.i <= 0) {
                parseerror(parser, "builtin number must be positive integer greater than zero");
                return false;
            }

            func->builtin = -parser_token(parser)->constval.i;
        } else if (parser->tok == '{') {
            /* function body */
            ast_block *block;
            ast_function *old = parser->function;

            if (localblock) {
                parseerror(parser, "cannot declare functions within functions");
                return false;
            }

            parser->function = func;
            block = parser_parse_block(parser);
            parser->function = old;

            if (!block)
                return false;

            if (!ast_function_blocks_add(func, block)) {
                ast_block_delete(block);
                return false;
            }
            return true;
        } else {
            parseerror(parser, "TODO, const assignment");
        }

        if (!parser_next(parser))
            return false;

        if (parser->tok == ',') {
            /* another */
            continue;
        }

        if (parser->tok != ';') {
            parseerror(parser, "expected semicolon");
            return false;
        }

        (void)parser_next(parser);

        return true;
    }
}

static bool parser_do(parser_t *parser)
{
    if (parser->tok == TOKEN_TYPENAME)
    {
        return parser_variable(parser, NULL);
    }
    else if (parser->tok == TOKEN_KEYWORD)
    {
        /* handle 'var' and 'const' */
        return false;
    }
    else if (parser->tok == '.')
    {
        /* entity-member declaration */
        return false;
    }
    else
    {
        parseerror(parser, "unexpected token: %s", parser->lex->tok->value);
        return false;
    }
    return true;
}

static parser_t *parser;

bool parser_init()
{
    parser = (parser_t*)mem_a(sizeof(parser_t));
    if (!parser)
        return false;

    memset(parser, 0, sizeof(parser));

    MEM_VECTOR_INIT(parser, globals);
    MEM_VECTOR_INIT(parser, locals);
    return true;
}

bool parser_compile(const char *filename)
{
    parser->lex = lex_open(filename);
    if (!parser->lex) {
        printf("failed to open file \"%s\"\n", filename);
        return false;
    }

    /* initial lexer/parser state */
    parser->lex->flags.noops = true;

    if (parser_next(parser))
    {
        while (parser->tok != TOKEN_EOF && parser->tok < TOKEN_ERROR)
        {
            if (!parser_do(parser)) {
                if (parser->tok == TOKEN_EOF)
                    parseerror(parser, "unexpected eof");
                else
                    parseerror(parser, "parse error\n");
                lex_close(parser->lex);
                mem_d(parser);
                return false;
            }
        }
    }

    lex_close(parser->lex);

    return !parser->errors;
}

void parser_cleanup()
{
    size_t i;
    for (i = 0; i < parser->functions_count; ++i) {
        ast_delete(parser->functions[i]);
    }
    for (i = 0; i < parser->imm_vector_count; ++i) {
        ast_delete(parser->imm_vector[i]);
    }
    for (i = 0; i < parser->imm_string_count; ++i) {
        ast_delete(parser->imm_string[i]);
    }
    for (i = 0; i < parser->imm_float_count; ++i) {
        ast_delete(parser->imm_float[i]);
    }
    for (i = 0; i < parser->globals_count; ++i) {
        ast_delete(parser->globals[i].var);
        mem_d(parser->globals[i].name);
    }
    MEM_VECTOR_CLEAR(parser, globals);

    mem_d(parser);
}

bool parser_finish(const char *output)
{
    size_t i;
    ir_builder *ir;

    if (!parser->errors)
    {
        ir = ir_builder_new("gmqcc_out");
        if (!ir) {
            printf("failed to allocate builder\n");
            return false;
        }

        for (i = 0; i < parser->imm_float_count; ++i) {
            if (!ast_global_codegen(parser->imm_float[i], ir)) {
                printf("failed to generate global %s\n", parser->imm_float[i]->name);
                ir_builder_delete(ir);
                return false;
            }
        }
        for (i = 0; i < parser->imm_string_count; ++i) {
            if (!ast_global_codegen(parser->imm_string[i], ir)) {
                printf("failed to generate global %s\n", parser->imm_string[i]->name);
                ir_builder_delete(ir);
                return false;
            }
        }
        for (i = 0; i < parser->imm_vector_count; ++i) {
            if (!ast_global_codegen(parser->imm_vector[i], ir)) {
                printf("failed to generate global %s\n", parser->imm_vector[i]->name);
                ir_builder_delete(ir);
                return false;
            }
        }
        for (i = 0; i < parser->globals_count; ++i) {
            if (!ast_istype(parser->globals[i].var, ast_value))
                continue;
            if (!ast_global_codegen((ast_value*)(parser->globals[i].var), ir)) {
                printf("failed to generate global %s\n", parser->globals[i].name);
                ir_builder_delete(ir);
                return false;
            }
        }
        for (i = 0; i < parser->functions_count; ++i) {
            if (!ast_function_codegen(parser->functions[i], ir)) {
                printf("failed to generate function %s\n", parser->functions[i]->name);
                ir_builder_delete(ir);
                return false;
            }
            if (!ir_function_finalize(parser->functions[i]->ir_func)) {
                printf("failed to finalize function %s\n", parser->functions[i]->name);
                ir_builder_delete(ir);
                return false;
            }
        }

        ir_builder_dump(ir, printf);

        if (!ir_builder_generate(ir, output)) {
            printf("*** failed to generate output file\n");
            ir_builder_delete(ir);
            return false;
        }

        ir_builder_delete(ir);
        return true;
    }

    printf("*** there were compile errors\n");
    return false;
}
