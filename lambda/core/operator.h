#pragma once

// type descriptors and syntax trees share one operator identity.
typedef enum Operator {
    // unary
    OPERATOR_NOT,
    OPERATOR_NEG,
    OPERATOR_POS,
    OPERATOR_SPREAD,
    OPERATOR_PROPAGATE,
    // PTH32: postfix `#`, the force step. Postfix in source, but a one-operand
    // node like the rest of this group.
    OPERATOR_FORCE,
    // PTH40: prefix `&`, address-of. Reads the reference the operand carries as
    // its identity, or null; performs no I/O and mints no identity.
    OPERATOR_ADDRESS_OF,

    // binary
    OPERATOR_ADD,
    OPERATOR_JOIN,
    OPERATOR_SUB,
    OPERATOR_MUL,
    OPERATOR_POW,
    OPERATOR_DIV,
    OPERATOR_IDIV,
    OPERATOR_MOD,

    OPERATOR_AND,
    OPERATOR_OR,

    OPERATOR_EQ,
    OPERATOR_NE,
    OPERATOR_LT,
    OPERATOR_LE,
    OPERATOR_GT,
    OPERATOR_GE,
    // PTH45v2: `a === b` is reference equality, `&a != null and &a == &b`.
    OPERATOR_REF_EQ,
    OPERATOR_ELEM_EQ,
    OPERATOR_ELEM_NE,
    OPERATOR_ELEM_LT,
    OPERATOR_ELEM_LE,
    OPERATOR_ELEM_GT,
    OPERATOR_ELEM_GE,

    OPERATOR_TO,
    OPERATOR_UNION,
    OPERATOR_INTERSECT,
    OPERATOR_EXCLUDE,
    OPERATOR_IS,
    OPERATOR_SUBTYPE,
    OPERATOR_IS_NAN,
    OPERATOR_IN,
    OPERATOR_AT,
    OPERATOR_OF,        // position membership: the IntKey axis (S8.1.1v2)

    // pipe operators
    OPERATOR_PIPE,
    OPERATOR_FILTER,    // `|:` keeps the members for which the body is truthy (S10.1.6)
    OPERATOR_THAT,      // `x that p` is x when p holds of it, else null (S10.1.5v3)
    OPERATOR_PIPE_FILE,
    OPERATOR_PIPE_APPEND,

    // occurrence
    OPERATOR_OPTIONAL,
    OPERATOR_ONE_MORE,
    OPERATOR_ZERO_MORE,
    OPERATOR_REPEAT,
    // `T[]` is a value-array contract, distinct from validator occurrence
    // counts such as `T[3]` (D4.1.1).
    OPERATOR_ARRAY,

    OPERATOR_ASSIGN,

    // JavaScript-only operator superset, dormant for Lambda until JS adopts Operator.
    OPERATOR_JS_STRICT_EQ,
    OPERATOR_JS_STRICT_NE,
    OPERATOR_JS_EXP,
    OPERATOR_JS_BIT_AND,
    OPERATOR_JS_BIT_OR,
    OPERATOR_JS_BIT_XOR,
    OPERATOR_JS_LSHIFT,
    OPERATOR_JS_RSHIFT,
    OPERATOR_JS_URSHIFT,
    OPERATOR_JS_BIT_NOT,
    OPERATOR_JS_TYPEOF,
    OPERATOR_JS_VOID,
    OPERATOR_JS_DELETE,
    OPERATOR_JS_INCREMENT,
    OPERATOR_JS_DECREMENT,
    OPERATOR_JS_ADD_ASSIGN,
    OPERATOR_JS_SUB_ASSIGN,
    OPERATOR_JS_MUL_ASSIGN,
    OPERATOR_JS_DIV_ASSIGN,
    OPERATOR_JS_MOD_ASSIGN,
    OPERATOR_JS_EXP_ASSIGN,
    OPERATOR_JS_BIT_AND_ASSIGN,
    OPERATOR_JS_BIT_OR_ASSIGN,
    OPERATOR_JS_BIT_XOR_ASSIGN,
    OPERATOR_JS_LSHIFT_ASSIGN,
    OPERATOR_JS_RSHIFT_ASSIGN,
    OPERATOR_JS_URSHIFT_ASSIGN,
    OPERATOR_JS_INSTANCEOF,
    OPERATOR_JS_NULLISH_COALESCE,
    OPERATOR_JS_NULLISH_ASSIGN,
    OPERATOR_JS_AND_ASSIGN,
    OPERATOR_JS_OR_ASSIGN,
    // reserve frontend-specific operator tags as enum members so switch
    // labels remain constant expressions across Clang's Windows ABI.
    OPERATOR_PY_MATMUL = 3000,
    OPERATOR_PY_NOT_IN,
    OPERATOR_PY_IS_NOT,
    OPERATOR_PY_FLOOR_DIV_ASSIGN,
    OPERATOR_PY_MATMUL_ASSIGN,
} Operator;
