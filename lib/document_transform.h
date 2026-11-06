#pragma once

// How the transform export receives its document argument. PARSED transforms
// get `input(target, input_type)`; PATH transforms get the resolved local path
// and perform their own effectful read in a procedural entry (S12.1.1v2).
enum LambdaDocumentTransformSource {
    LAMBDA_DOCUMENT_TRANSFORM_SOURCE_PARSED = 0,
    LAMBDA_DOCUMENT_TRANSFORM_SOURCE_PATH,
};

struct LambdaDocumentTransformConfig {
    const char* input_type;
    const char* package_module;
    const char* function_name;
    LambdaDocumentTransformSource source;
    // A stateless transform keeps nothing between documents, so a window runs
    // it on its one shared loader runtime; a stateful application (edit) keeps
    // a runtime per document.
    bool stateless;
};

enum LambdaDocumentTransformOptionKind {
    LAMBDA_DOCUMENT_TRANSFORM_OPTION_STRING,
    LAMBDA_DOCUMENT_TRANSFORM_OPTION_BOOL,
};

// Native callers pass typed transform options without constructing Lambda source.
struct LambdaDocumentTransformOption {
    const char* name;
    LambdaDocumentTransformOptionKind kind;
    const char* string_value;
    bool bool_value;
};

