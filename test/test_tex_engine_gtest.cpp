#include <gtest/gtest.h>
#include "lambda/input/input-tex.hpp"
#include "lambda/io/security_io.h"
#include <errno.h>
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
