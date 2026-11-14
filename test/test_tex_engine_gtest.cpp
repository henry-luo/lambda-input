#include <gtest/gtest.h>
#include "lambda/input/input-tex.hpp"
#include "lambda/io/security_io.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct ResourceAccess {
    unsigned classifications;
    unsigned reads;
    bool deny_reads;
};

SecurityIOResult classify_bundled_file(void* context, const char*, int32_t* kind) {
    ResourceAccess* access = static_cast<ResourceAccess*>(context);
    ++access->classifications;
    if (access->deny_reads) {
        *kind = SECURITY_OBJECT_FILE;
        return {SECURITY_ALLOWED, 0, false, {}};
    }
    return {SECURITY_DENIED, EACCES, false, {}};
}

SecurityIOResult deny_bundled_read(void* context, const char*, bool, uint64_t,
                                   MemCategory, char**, size_t*) {
    ++static_cast<ResourceAccess*>(context)->reads;
    return {SECURITY_DENIED, EACCES, false, {}};
}

bool is_expl3_preload(const char* path) {
    const char* name = strrchr(path, '/');
    return name && strcmp(name + 1, "expl3.ltx") == 0;
}

SecurityIOResult allow_preload_kind(void*, const char* path, int32_t* kind) {
    if (!is_expl3_preload(path)) return {SECURITY_ALLOWED, ENOENT, false, {}};
    *kind = SECURITY_OBJECT_FILE;
    return {SECURITY_ALLOWED, 0, false, {}};
}

SecurityIOResult read_preload(void* context, const char* path, bool, uint64_t limit,
                              MemCategory category, char** data, size_t* size) {
    if (!is_expl3_preload(path)) return {SECURITY_ALLOWED, ENOENT, false, {}};
    const char* content = "\\def\\ExplSyntaxOn{}\\def\\ExplSyntaxOff{}\\def\\demo{5}";
    size_t length = strlen(content);
    if (length > limit) return {SECURITY_ALLOWED, EFBIG, false, {}};
    *data = mem_strdup(content, category);
    if (!*data) return {SECURITY_ALLOWED, ENOMEM, false, {}};
    *size = length;
    ++*static_cast<unsigned*>(context);
    return {SECURITY_ALLOWED, 0, false, {}};
}

class TexEngineTest : public ::testing::Test {
protected:
    void SetUp() override { tex::set_resource_dir("test"); }
    void TearDown() override { tex::set_resource_dir(nullptr); }
};

TEST_F(TexEngineTest, Expl3ReferenceCaseExpansionTerminates) {
    const char* resources = getenv("LAMBDA_INPUT_EXPL3_TEST_DIR");
    if (!resources || !*resources) GTEST_SKIP() << "Set LAMBDA_INPUT_EXPL3_TEST_DIR to the bundled TeX resource directory";
    tex::set_resource_dir(resources);
    tex::EngineOptions options = {};
    options.expl3 = true;
    for (const char* source : {
        "\\ExplSyntaxOn\\text_uppercase:n{aB}\\ExplSyntaxOff",
        "\\ExplSyntaxOn\\text_uppercase:n{\\ref{aB}}\\ExplSyntaxOff",
        "\\ExplSyntaxOn\\text_uppercase:n{\\pageref{aB}}\\ExplSyntaxOff",
        "\\ExplSyntaxOn\\text_uppercase:n{\\missingcommand{aB}}\\ExplSyntaxOff",
        "\\ExplSyntaxOn\\text_uppercase:n{\\relax{aB}}\\ExplSyntaxOff",
        "\\def\\eqref#1{(#1)}\\ExplSyntaxOn\\text_uppercase:n{\\eqref{aB}}\\ExplSyntaxOff",
        "\\ExplSyntaxOn\\text_uppercase:n{\\eqref{aB}}\\ExplSyntaxOff",
        "\\ExplSyntaxOn\\text_uppercase:n{\\ref{aB}/\\pageref{aB}/\\eqref{aB}}\\ExplSyntaxOff"
    }) {
        SCOPED_TRACE(source);
        tex::Engine* engine = tex::engine_create(&options);
        ASSERT_NE(engine, nullptr);
        tex::Result result = {};
        EXPECT_TRUE(tex::engine_run(engine, source, strlen(source), &result));
        EXPECT_GT(result.length, 0u);
        for (size_t i = 0; i < result.diagnostic_count; i++) {
            EXPECT_STRNE(result.diagnostics[i].code, "tex-budget");
            EXPECT_STRNE(result.diagnostics[i].code, "tex-missing-number");
            EXPECT_STRNE(result.diagnostics[i].code, "tex-missing-brace");
        }
        tex::engine_destroy(engine);
    }
}

TEST_F(TexEngineTest, TextEncodingCommandsDispatchAndProtect) {
    const char* source =
        "\\makeatletter\\def\\cf@encoding{TST}"
        "\\def\\simple{A}\\@current@cmd\\simple\\simple"
        "\\expandafter\\def\\csname TST\\string\\localcmd\\endcsname{L}"
        "\\expandafter\\def\\csname ?\\string\\fallbackcmd\\endcsname{F}"
        "\\@changed@cmd\\localcmd\\relax\\@changed@cmd\\fallbackcmd\\relax"
        "\\let\\protect\\@unexpandable@protect"
        "\\edef\\saved{\\@current@cmd\\simple\\relax\\@changed@cmd\\localcmd\\relax}"
        "\\let\\protect\\@typeset@protect\\def\\simple{P}\\def\\localcmd{Q}\\saved"
        "\\makeatother";
    tex::EngineOptions options = {};
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    tex::Result result = {};
    EXPECT_TRUE(tex::engine_run(engine, source, strlen(source), &result));
    EXPECT_EQ(result.diagnostic_count, 0u);
    EXPECT_NE(strstr(result.text, "ALF"), nullptr) << result.text;
    EXPECT_NE(strstr(result.text, "PQ"), nullptr) << result.text;
    tex::engine_destroy(engine);
}

TEST_F(TexEngineTest, TextEncodingCommandReportsUnavailableSymbol) {
    const char* source = "\\makeatletter\\def\\cf@encoding{TST}"
        "\\def\\missingtext{X}\\@changed@cmd\\missingtext\\relax\\makeatother";
    tex::EngineOptions options = {};
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    tex::Result result = {};
    tex::engine_run(engine, source, strlen(source), &result);
    bool reported = false;
    for (size_t i = 0; i < result.diagnostic_count; i++) {
        if (strcmp(result.diagnostics[i].code, "tex-errmessage") == 0 &&
            strstr(result.diagnostics[i].message, "unavailable in encoding TST")) reported = true;
    }
    EXPECT_TRUE(reported);
    tex::engine_destroy(engine);
}

TEST_F(TexEngineTest, ExpandafterUnexpandedRetainsMacroInEdef) {
    const char* source = "\\def\\a{A}\\edef\\b{\\expandafter\\unexpanded{\\a}}\\def\\a{B}\\b";
    tex::EngineOptions options = {};
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    tex::Result result = {};
    EXPECT_TRUE(tex::engine_run(engine, source, strlen(source), &result));
    EXPECT_EQ(result.diagnostic_count, 0u);
    ASSERT_GT(result.length, 0u);
    EXPECT_EQ(result.text[result.length - 1], 'B') << result.text;
    tex::engine_destroy(engine);
}

TEST_F(TexEngineTest, Expl3UndefinedTokenIsNotExpandable) {
    const char* resources = getenv("LAMBDA_INPUT_EXPL3_TEST_DIR");
    if (!resources || !*resources) GTEST_SKIP() << "Set LAMBDA_INPUT_EXPL3_TEST_DIR to the bundled TeX resource directory";
    tex::set_resource_dir(resources);
    tex::EngineOptions options = {};
    options.expl3 = true;
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    const char* source = "\\ExplSyntaxOn X\\token_if_expandable:NTF\\missingcommand{T}{F}Y"
        "X\\cs_if_exist:cTF{l__text_expand_\\token_to_str:N\\missingcommand _tl}{T}{F}Y"
        "X\\str_if_eq:nnTF{\\missingcommand}{\\missingcommand}{T}{F}Y"
        "X\\str_if_eq:nnTF{\\missingcommand}{\\ref}{T}{F}Y\\ExplSyntaxOff";
    tex::Result result = {};
    EXPECT_TRUE(tex::engine_run(engine, source, strlen(source), &result));
    EXPECT_EQ(result.diagnostic_count, 0u);
    EXPECT_NE(strstr(result.text, "XFYXFYXTYXFY"), nullptr) << result.text;
    tex::engine_destroy(engine);
}

TEST_F(TexEngineTest, CounterExpressionIsBoundedBeforeFollowingExpansion) {
    struct Case { const char* value; const char* expected; };
    for (const Case& test : {Case{"\\numexpr1+2", "3"},
                              Case{"\\numexpr\\numexpr1+2\\relax+3", "6"},
                              Case{"4", "4"}}) {
        char source[256];
        snprintf(source, sizeof(source), "\\begin{document}\\newcounter{n}\\setcounter{n}{%s}\\arabic{n}\\end{document}", test.value);
        SCOPED_TRACE(test.value);
        tex::EngineOptions options = {};
        tex::Engine* engine = tex::engine_create(&options);
        ASSERT_NE(engine, nullptr);
        tex::Result result = {};
        EXPECT_TRUE(tex::engine_run(engine, source, strlen(source), &result));
        EXPECT_EQ(result.diagnostic_count, 0u);
        const char* tail = strstr(result.text, "\\end{document}");
        ASSERT_NE(tail, nullptr) << result.text;
        ASSERT_GT(tail - result.text, 0);
        EXPECT_EQ(tail[-1], test.expected[0]) << result.text;
        tex::engine_destroy(engine);
    }
}

TEST_F(TexEngineTest, UnskipKeepsDiscardedInsertWithinItsBoundary) {
    struct Case { const char* source; const char* expected; };
    for (const Case& test : {
        Case{"\\begin{document}A \\insert0{\\vadjust{\\unskip}X}B\\end{document}", "AB"},
        Case{"\\begin{document}A \\insert0{\\vadjust{\\unskip}\\global\\def\\saved{Y}X}\\saved B\\end{document}", "AYB"}
    }) {
        SCOPED_TRACE(test.source);
        tex::EngineOptions options = {};
        tex::Engine* engine = tex::engine_create(&options);
        ASSERT_NE(engine, nullptr);
        tex::Result result = {};
        EXPECT_TRUE(tex::engine_run(engine, test.source, strlen(test.source), &result));
        EXPECT_EQ(result.diagnostic_count, 0u);
        EXPECT_EQ(strchr(result.text, 'X'), nullptr) << result.text;
        EXPECT_EQ(strstr(result.text, "\\def"), nullptr) << result.text;
        EXPECT_NE(strstr(result.text, test.expected), nullptr) << result.text;
        tex::engine_destroy(engine);
    }
}

TEST_F(TexEngineTest, CustomEnvironmentInsideConstructorClosesItsGroup) {
    struct Case { const char* source; const char* expected; };
    for (const Case& test : {
        Case{"\\newenvironment{foo}{B}{E}\\begin{document}\\textbf{A\\begin{foo}X\\end{foo}Z}\\end{document}", "ABXEZ"},
        Case{"\\newenvironment{foo}{B}{E}\\begin{document}\\textbf{\\begin{foo}X\\end{foo}\\begin{foo}Y\\end{foo}}\\end{document}", "BXEBYE"}
    }) {
        SCOPED_TRACE(test.source);
        tex::EngineOptions options = {};
        tex::Engine* engine = tex::engine_create(&options);
        ASSERT_NE(engine, nullptr);
        tex::Result result = {};
        EXPECT_TRUE(tex::engine_run(engine, test.source, strlen(test.source), &result));
        EXPECT_EQ(result.diagnostic_count, 0u);
        EXPECT_NE(strstr(result.text, test.expected), nullptr) << result.text;
        tex::engine_destroy(engine);
    }
}

TEST_F(TexEngineTest, UnavailableExpl3StopsAtTheRequest) {
    ResourceAccess access = {};
    SecurityIOClient io = {};
    io.version = 1;
    io.context = &access;
    io.input_kind = classify_bundled_file;
    io.read = deny_bundled_read;

    const char* sources[] = {
        "\\ExplSyntaxOn\\int_eval:n{2+3}\\ExplSyntaxOff",
        "\\usepackage{expl3}",
        "\\ProvidesExplPackage{example}{2026-01-01}{1}{example}"
    };
    for (const char* source : sources) {
        SCOPED_TRACE(source);
        tex::EngineOptions options = {};
        options.io = &io;
        options.expl3 = true;
        options.max_expansions = 1000;
        tex::Engine* engine = tex::engine_create(&options);
        ASSERT_NE(engine, nullptr);
        tex::Result result = {};
        EXPECT_FALSE(tex::engine_run(engine, source, strlen(source), &result));
        ASSERT_EQ(result.diagnostic_count, 1U);
        EXPECT_STREQ(result.diagnostics[0].code, "tex-expl3-unavailable");
        EXPECT_FALSE(tex::engine_wants_expl3(engine));
        EXPECT_EQ(strstr(result.text, "5"), nullptr);
        tex::engine_destroy(engine);
    }
    EXPECT_GT(access.classifications, 0U);
    EXPECT_EQ(access.reads, 0U);

    // A successful file classification is not authorization to read bytes.
    access = {};
    access.deny_reads = true;
    tex::EngineOptions options = {};
    options.io = &io;
    options.expl3 = true;
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    tex::Result result = {};
    EXPECT_FALSE(tex::engine_run(engine, sources[0], strlen(sources[0]), &result));
    ASSERT_EQ(result.diagnostic_count, 1U);
    EXPECT_STREQ(result.diagnostics[0].code, "tex-expl3-unavailable");
    EXPECT_GT(access.reads, 0U);
    tex::engine_destroy(engine);
}

TEST_F(TexEngineTest, FirstExpl3RequestAsksCallerToRestart) {
    tex::EngineOptions options = {};
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    const char* source = "\\ExplSyntaxOn";
    tex::Result result = {};
    EXPECT_FALSE(tex::engine_run(engine, source, strlen(source), &result));
    EXPECT_TRUE(tex::engine_wants_expl3(engine));
    EXPECT_EQ(result.diagnostic_count, 0U);
    tex::engine_destroy(engine);
}

TEST_F(TexEngineTest, AuthorizedClientCanLoadItsOwnFormat) {
    unsigned reads = 0;
    SecurityIOClient io = {};
    io.version = 1;
    io.context = &reads;
    io.input_kind = allow_preload_kind;
    io.read = read_preload;
    tex::EngineOptions options = {};
    options.io = &io;
    options.expl3 = true;
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    const char* source = "\\ExplSyntaxOn\\immediate\\write16{LT:\\demo}\\ExplSyntaxOff";
    tex::Result result = {};
    EXPECT_TRUE(tex::engine_run(engine, source, strlen(source), &result));
    EXPECT_EQ(result.diagnostic_count, 0U);
    ASSERT_EQ(result.message_count, 1U);
    EXPECT_STREQ(result.messages[0], "LT:5");
    EXPECT_GT(reads, 0U);
    tex::engine_destroy(engine);
}

TEST_F(TexEngineTest, MacroBodiesKeepTheirMeaningAcrossRedefinitionAndSubstitution) {
    struct Example { const char* source; const char* first; const char* second; };
#define TEX_TEST_CATCODES "\\catcode`\\{=1 \\catcode`\\}=2 \\catcode`\\#=6 "
    const Example examples[] = {
        {TEX_TEST_CATCODES "\\def\\a{\\def\\a{B}\\immediate\\write16{C}}\\a\\immediate\\write16{\\a}", "C", "B"},
        {TEX_TEST_CATCODES "\\def\\pair#1,#2;{[#2:#1:#2]}\\immediate\\write16{\\pair X,Y;}", "[Y:X:Y]", nullptr},
        {TEX_TEST_CATCODES "\\def\\duplicate#1{#1#1}\\immediate\\write16{\\duplicate{ab}}", "abab", nullptr},
        {TEX_TEST_CATCODES "\\def\\identity#1{#1}\\immediate\\write16{L\\identity{}R}", "LR", nullptr},
        {TEX_TEST_CATCODES "\\def\\old{X}\\edef\\copy{\\old}\\def\\old{Y}\\immediate\\write16{\\copy\\old}", "XY", nullptr},
    };
#undef TEX_TEST_CATCODES
    for (const Example& example : examples) {
        SCOPED_TRACE(example.source);
        tex::EngineOptions options = {};
        options.ini = true;
        tex::Engine* engine = tex::engine_create(&options);
        ASSERT_NE(engine, nullptr);
        tex::Result result = {};
        EXPECT_TRUE(tex::engine_run(engine, example.source, strlen(example.source), &result));
        EXPECT_EQ(result.diagnostic_count, 0U);
        ASSERT_EQ(result.message_count, example.second ? 2U : 1U);
        EXPECT_STREQ(result.messages[0], example.first);
        if (example.second) EXPECT_STREQ(result.messages[1], example.second);
        tex::engine_destroy(engine);
    }
}

TEST_F(TexEngineTest, ParameterlessRecursionStillUsesTheExpansionBudget) {
    tex::EngineOptions options = {};
    options.ini = true;
    options.max_expansions = 3;
    tex::Engine* engine = tex::engine_create(&options);
    ASSERT_NE(engine, nullptr);
    const char* source = "\\catcode`\\{=1 \\catcode`\\}=2 \\def\\a{\\a}\\a";
    tex::Result result = {};
    EXPECT_FALSE(tex::engine_run(engine, source, strlen(source), &result));
    ASSERT_EQ(result.diagnostic_count, 1U);
    EXPECT_STREQ(result.diagnostics[0].code, "tex-budget");
    tex::engine_destroy(engine);
}

} // namespace
