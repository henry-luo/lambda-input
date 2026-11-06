#pragma once

#include <gtest/gtest.h>
#include "../lambda/io/mark_builder.hpp"
#include "../lib/test_utils.h"

class FormatMarkupTest : public ::testing::Test {
protected:
    Pool* pool_;
    Input* input_;
    MarkBuilder* mb_;

    void SetUp() override {
        pool_ = tu_setup_pool();
        input_ = Input::create(pool_);
        mb_ = new MarkBuilder(input_);
    }

    void TearDown() override {
        delete mb_;
        tu_teardown_pool(pool_);
    }

    // Build a simple document with a heading and paragraph
    Item build_heading_paragraph() {
        ElementBuilder html = mb_->element("html");
        html.child(mb_->element("h1").text("Hello World").final());
        html.child(mb_->element("p").text("This is a paragraph.").final());
        return html.final();
    }

    // Build a document with bold, italic, code inline
    Item build_inline_formatting() {
        ElementBuilder p = mb_->element("p");
        p.text("Text with ");
        p.child(mb_->element("strong").text("bold").final());
        p.text(" and ");
        p.child(mb_->element("em").text("italic").final());
        p.text(" and ");
        p.child(mb_->element("code").text("code").final());
        p.text(".");
        ElementBuilder html = mb_->element("html");
        html.child(p.final());
        return html.final();
    }

    // Build a document with a link
    Item build_link() {
        ElementBuilder p = mb_->element("p");
        p.text("Click ");
        p.child(mb_->element("a").attr("href", "https://example.com").text("here").final());
        p.text(".");
        ElementBuilder html = mb_->element("html");
        html.child(p.final());
        return html.final();
    }

    // Build an unordered list
    Item build_unordered_list() {
        ElementBuilder ul = mb_->element("ul");
        ul.child(mb_->element("li").text("Item one").final());
        ul.child(mb_->element("li").text("Item two").final());
        ul.child(mb_->element("li").text("Item three").final());
        ElementBuilder html = mb_->element("html");
        html.child(ul.final());
        return html.final();
    }

    // Build an ordered list
    Item build_ordered_list() {
        ElementBuilder ol = mb_->element("ol");
        ol.child(mb_->element("li").text("First").final());
        ol.child(mb_->element("li").text("Second").final());
        ElementBuilder html = mb_->element("html");
        html.child(ol.final());
        return html.final();
    }

    // Build a code block
    Item build_code_block() {
        ElementBuilder html = mb_->element("html");
        html.child(mb_->element("pre").attr("language", "python").text("print('hello')").final());
        return html.final();
    }

    // Build a blockquote
    Item build_blockquote() {
        ElementBuilder blockquote = mb_->element("blockquote");
        blockquote.child(mb_->element("p").text("Quoted text.").final());
        ElementBuilder html = mb_->element("html");
        html.child(blockquote.final());
        return html.final();
    }

    // Build a horizontal rule
    Item build_hr() {
        ElementBuilder html = mb_->element("html");
        html.child(mb_->element("hr").final());
        return html.final();
    }

    // Build a simple table
    Item build_table() {
        ElementBuilder tr1 = mb_->element("tr");
        tr1.child(mb_->element("th").text("Name").final());
        tr1.child(mb_->element("th").text("Age").final());
        ElementBuilder tr2 = mb_->element("tr");
        tr2.child(mb_->element("td").text("Alice").final());
        tr2.child(mb_->element("td").text("30").final());
        ElementBuilder table = mb_->element("table");
        table.child(tr1.final());
        table.child(tr2.final());
        ElementBuilder html = mb_->element("html");
        html.child(table.final());
        return html.final();
    }
};

