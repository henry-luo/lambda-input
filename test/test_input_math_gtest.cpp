#include <gtest/gtest.h>
#include "test_input_test_helpers.hpp"
#include "lambda/input/input.hpp"
#include "lambda/core/mark_reader.hpp"
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

TEST_F(NativeMathInputTest, LatexParserBuildsFractionTree) {
    Input* input = parse("\\frac{a}{b}", "latex");
    ASSERT_NE(input, nullptr);
    ASSERT_FALSE(input->parse_failed);
    ElementReader root(input->root);
    ASSERT_TRUE(root.hasTag("math"));
    ASSERT_EQ(root.childCount(), 1);
    ElementReader fraction = root.childAt(0).asElement();
    ASSERT_TRUE(fraction.hasTag("fraction"));
    EXPECT_STREQ(fraction.get_attr_string("cmd"), "\\frac");
    ElementReader numerator = fraction.get_attr("numer").asElement();
    ElementReader denominator = fraction.get_attr("denom").asElement();
    ASSERT_TRUE(numerator.hasTag("group"));
    ASSERT_TRUE(denominator.hasTag("group"));
    EXPECT_STREQ(numerator.childAt(0).cstring(), "a");
    EXPECT_STREQ(denominator.childAt(0).cstring(), "b");
}
