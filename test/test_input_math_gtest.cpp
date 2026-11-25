#include <gtest/gtest.h>
#include "test_input_test_helpers.hpp"
#include "lambda/input/input.hpp"
#include "lambda/input/input-latex-scanner.h"
#include "lambda/core/mark_reader.hpp"
#include "lib/str.h"
#include <locale.h>
#include <stdlib.h>
#include <string.h>

class NativeMathInputTest : public ::testing::Test {
protected:
    Input* parse(const char* source, const char* flavor) {
        String* type = create_lambda_string("math");
        String* name = create_lambda_string(flavor);
        Input* input = input_from_source_n(source, strlen(source), nullptr, type, name);
        free(type);
        free(name);
        return input;
    }
    void TearDown() override { InputManager::destroy_global(); }
};

TEST_F(NativeMathInputTest, TexDocumentsRequireRuntimeOrDeferTheirSource) {
    const char* source = "\\begin{document}Hello\\end{document}";
    const char* formats[] = {"tex", "latex", "latex-ts"};
    for (const char* format : formats) {
        String* type = create_lambda_string(format);
        Input* direct = input_from_source_n(source, strlen(source), nullptr, type, nullptr);
        ASSERT_NE(direct, nullptr);
        EXPECT_TRUE(direct->parse_failed);
        EXPECT_EQ(get_type_id(direct->root), LMD_TYPE_ERROR);
        const char* deferred = nullptr;
        Input* captured = input_from_source_n_with_deferred_tex(source, strlen(source), nullptr,
            type, nullptr, nullptr, nullptr, &deferred);
        ASSERT_NE(captured, nullptr);
        ASSERT_FALSE(captured->parse_failed);
        ASSERT_NE(deferred, nullptr);
        EXPECT_STREQ(deferred, !strcmp(format, "tex") ? "tex" : "latex");
        EXPECT_EQ(get_type_id(captured->root), LMD_TYPE_STRING);
        free(type);
    }
}

TEST_F(NativeMathInputTest, AsciiParserBuildsOperatorTree) {
    Input* input = parse("x + y", "ascii");
    ASSERT_NE(input, nullptr);
    ASSERT_FALSE(input->parse_failed);
    ElementReader root(input->root);
    ASSERT_TRUE(root.hasTag("math"));
    ASSERT_EQ(root.childCount(), 3);
    EXPECT_STREQ(root.childAt(0).cstring(), "x");
    ElementReader operation = root.childAt(1).asElement();
    ASSERT_TRUE(operation.hasTag("operator"));
    EXPECT_STREQ(operation.get_attr_string("value"), "+");
    EXPECT_STREQ(root.childAt(2).cstring(), "y");
}

// The fraction AST itself is checked through the runtime in test_math_script_public.
TEST_F(NativeMathInputTest, LatexMathRequiresRuntimeOrDefersItsSource) {
    Input* input = parse("\\frac{a}{b}", "latex");
    ASSERT_NE(input, nullptr);
    EXPECT_TRUE(input->parse_failed);
    String* type = create_lambda_string("math");
    String* flavor = create_lambda_string("latex");
    const char* deferred = nullptr;
    const char* source = "\\frac{a}{b}";
    Input* captured = input_from_source_n_with_deferred_tex(source, strlen(source), nullptr,
        type, flavor, nullptr, nullptr, &deferred);
    ASSERT_NE(captured, nullptr);
    EXPECT_FALSE(captured->parse_failed);
    ASSERT_NE(deferred, nullptr);
    EXPECT_STREQ(deferred, "math");
    EXPECT_EQ(get_type_id(captured->root), LMD_TYPE_STRING);
    free(type);
    free(flavor);
}

TEST_F(NativeMathInputTest, MarkdownRetainsTexMathForRuntimeParsing) {
    const char* source = "Before $x^2$ after.";
    String* type = create_lambda_string("markdown");
    InputParseOptions options = {};
    options.embedded_math = true;
    Input* input = input_from_source_with_options(source, nullptr, type, nullptr, &options);
    free(type);
    ASSERT_NE(input, nullptr);
    ASSERT_FALSE(input->parse_failed);
    ASSERT_NE(input->embedded_math, nullptr);
    ASSERT_EQ(input->embedded_math->length, 1);
    Element* node = input->embedded_math->items[0].element;
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->get_attr("ast").type_id(), LMD_TYPE_NULL);
    ASSERT_EQ(node->length, 1);
    EXPECT_STREQ(node->items[0].get_chars(), "x^2");
}

// The retained scanner is also used by AsciiMath, independent of the retired engine.
TEST(LatexScannerTest, CommandNameClassificationIsLocaleIndependent) {
    const char* current = setlocale(LC_CTYPE, nullptr);
    char* saved = current ? str_dup(current, strlen(current)) : nullptr;
    ASSERT_NE(saved, nullptr);
    const char* locales[] = {"C", "C.UTF-8", "en_US.UTF-8", "fr_FR.UTF-8", "fr_FR.ISO8859-1"};
    for (const char* locale_name : locales) {
        if (!setlocale(LC_CTYPE, locale_name)) continue;
        SCOPED_TRACE(locale_name);
        for (unsigned int byte = 0; byte <= 255; byte++) {
            const char source[] = {'\\', (char)byte, 'A', '@', 'x'};
            char name[16] = {}, full[16] = {};
            size_t end = latex_scan_command(source, sizeof(source), 0, name, sizeof(name), full, sizeof(full));
            bool letter = byte != 0 && strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@", (int)byte) != nullptr;
            EXPECT_EQ(end, letter ? 5u : 2u);
            EXPECT_EQ((unsigned char)name[0], byte);
            EXPECT_EQ((unsigned char)full[1], byte);
            EXPECT_EQ(name[letter ? 4 : 1], '\0');
            EXPECT_EQ(full[letter ? 5 : 2], '\0');
        }
    }
    setlocale(LC_CTYPE, saved);
    free(saved);
}
