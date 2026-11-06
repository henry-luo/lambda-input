#include <gtest/gtest.h>
#include "../lambda/input/css/dom_element.hpp"
#include "../lambda/input/css/dom_xml_serializer.h"
#include "../lib/strbuf.h"
#include "../lambda/input/css/dom_svg_context.h"
#include "../lambda/input/input.hpp"
#include "../lib/mem.h"

TEST(InputDomNativeTest, CreatesDetachedBackedElementsWithDocumentNamespaces) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(dom_create_backed_element_identity(nullptr, "B", nullptr, false), nullptr);
    EXPECT_EQ(dom_create_backed_element_identity(document, "", nullptr, false), nullptr);
    DomElement* html = dom_create_backed_element_identity(document, "B", nullptr, false);
    ASSERT_NE(html, nullptr);
    EXPECT_STREQ(html->tag_name, "b");
    EXPECT_STREQ(dom_element_namespace_uri(html), "http://www.w3.org/1999/xhtml");
    EXPECT_NE(dom_element_backing(html), nullptr);
    EXPECT_EQ(html->parent, nullptr);
    DomElement* svg = dom_create_backed_element_identity(
        document, "svg:linearGradient", "http://www.w3.org/2000/svg", true);
    ASSERT_NE(svg, nullptr);
    EXPECT_STREQ(svg->local_name(), "linearGradient");
    EXPECT_STREQ(dom_element_namespace_uri(svg), "http://www.w3.org/2000/svg");
    EXPECT_NE(dom_element_backing(svg), nullptr);
    document->xml_document = true;
    DomElement* xml = dom_create_backed_element_identity(document, "B", nullptr, false);
    ASSERT_NE(xml, nullptr);
    EXPECT_STREQ(xml->tag_name, "B");
    EXPECT_STREQ(dom_element_namespace_uri(xml), "");
    EXPECT_NE(dom_element_backing(xml), nullptr);
    dom_document_destroy(document);
    pool_destroy(pool);
}

TEST(InputDomNativeTest, FindsBackedChildSlotsAcrossNodeKinds) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    DomElement* parent = dom_create_backed_element_identity(document, "div", nullptr, false);
    DomElement* child = dom_create_backed_element_identity(document, "span", nullptr, false);
    DomElement* detached = dom_create_backed_element_identity(document, "b", nullptr, false);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(detached, nullptr);
    ASSERT_TRUE(parent->append_child(child));
    DomText* text = parent->append_text("text");
    DomComment* comment = parent->append_comment("comment");
    ASSERT_NE(text, nullptr);
    ASSERT_NE(comment, nullptr);
    EXPECT_EQ(dom_node_ancestor_document(child), document);
    EXPECT_EQ(dom_node_ancestor_document(text), document);
    EXPECT_EQ(dom_node_ancestor_document(detached), document);
    EXPECT_EQ(dom_node_ancestor_document(nullptr), nullptr);
    EXPECT_EQ(dom_backed_node_index(parent, child), 0);
    EXPECT_EQ(dom_backed_node_index(parent, text), 1);
    EXPECT_EQ(dom_backed_node_index(parent, comment), 2);
    EXPECT_EQ(dom_backed_node_index(parent, detached), -1);
    EXPECT_EQ(dom_backed_node_index(nullptr, child), -1);
    EXPECT_EQ(dom_text_get_child_index(text), 1);
    EXPECT_EQ(dom_comment_get_child_index(comment), 2);
    ASSERT_TRUE(dom_insert_detached_backed_element(parent, detached, child));
    EXPECT_EQ(parent->first_child, detached);
    EXPECT_EQ(dom_backed_node_index(parent, detached), 0);
    EXPECT_EQ(dom_backed_node_index(parent, child), 1);
    EXPECT_FALSE(dom_insert_detached_backed_element(parent, detached, child));
    ASSERT_TRUE(dom_remove_prepared_backed_child(parent, detached));
    DomText* inserted = DomText::create_detached_copy(document, "new", 3);
    ASSERT_NE(inserted, nullptr);
    EXPECT_EQ(dom_node_ancestor_document(inserted), nullptr);
    ASSERT_TRUE(dom_insert_prepared_backed_text(parent, inserted, child));
    EXPECT_EQ(parent->first_child, inserted);
    EXPECT_EQ(dom_backed_node_index(parent, inserted), 0);
    EXPECT_EQ(dom_backed_node_index(parent, child), 1);
    EXPECT_EQ(dom_backed_node_index(parent, text), 2);
    DomElement* destination = dom_create_backed_element_identity(document, "div", nullptr, false);
    ASSERT_NE(destination, nullptr);
    ASSERT_TRUE(dom_insert_prepared_backed_text(destination, inserted, nullptr));
    EXPECT_EQ(destination->first_child, inserted);
    EXPECT_EQ(dom_backed_node_index(destination, inserted), 0);
    EXPECT_EQ(dom_element_backing(parent)->length, 3);
    ASSERT_TRUE(dom_insert_prepared_backed_text(parent, inserted, child));
    EXPECT_EQ(dom_element_backing(destination)->length, 0);
    EXPECT_EQ(parent->first_child, inserted);
    ASSERT_TRUE(dom_remove_prepared_backed_child(parent, inserted));
    EXPECT_FALSE(dom_remove_prepared_backed_child(parent, detached));
    ASSERT_TRUE(dom_remove_prepared_backed_child(parent, comment));
    EXPECT_EQ(dom_element_backing(parent)->length, 2);
    EXPECT_EQ(dom_backed_node_index(parent, text), 1);
    ASSERT_TRUE(dom_remove_prepared_backed_child(parent, text));
    EXPECT_EQ(dom_element_backing(parent)->length, 1);
    ASSERT_TRUE(dom_remove_prepared_backed_child(parent, child));
    EXPECT_EQ(dom_element_backing(parent)->length, 0);
    EXPECT_EQ(parent->first_child, nullptr);
    DomElement* synthetic = DomElement::create(document, "div", nullptr);
    DomElement* synthetic_child = DomElement::create(document, "span", nullptr);
    ASSERT_NE(synthetic, nullptr);
    ASSERT_NE(synthetic_child, nullptr);
    ASSERT_TRUE(synthetic->DomNode::append_child(synthetic_child));
    EXPECT_TRUE(dom_remove_prepared_backed_child(synthetic, synthetic_child));
    EXPECT_EQ(synthetic->first_child, nullptr);
    dom_document_destroy(document);
    pool_destroy(pool);
}

TEST(InputDomNativeTest, FindsElementsByIdWithinTheSelectedTree) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    EXPECT_EQ(dom_mutation_epoch(nullptr), 0u);
    document->mutation_epoch = UINT64_C(0x100000001);
    EXPECT_EQ(dom_mutation_epoch(document), UINT64_C(0x100000001));
    document->mutation_epoch = 0;
    DomElement* root = DomElement::create(document, "html", nullptr);
    DomElement* parent = DomElement::create(document, "section", nullptr);
    DomElement* first = DomElement::create(document, "div", nullptr);
    DomElement* second = DomElement::create(document, "div", nullptr);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    document->root = lam::up(root);
    ASSERT_TRUE(root->append_child(parent));
    ASSERT_TRUE(parent->append_child(first));
    ASSERT_TRUE(root->append_child(second));
    ASSERT_TRUE(first->set_attribute("id", "target"));
    ASSERT_TRUE(second->set_attribute("id", "target"));
    EXPECT_EQ(dom_find_element_by_id(root, "target"), first);
    EXPECT_EQ(dom_find_element_by_id(second, "target"), second);
    EXPECT_EQ(dom_find_element_by_id(parent, "missing"), nullptr);
    EXPECT_EQ(dom_find_element_by_id(nullptr, "target"), nullptr);
    dom_document_destroy(document);
    pool_destroy(pool);
}

TEST(InputDomNativeTest, SvgConditionsMatchLanguageTagBoundaries) {
    EXPECT_TRUE(dom_svg_conditions_match(nullptr, nullptr, ""));
    EXPECT_FALSE(dom_svg_conditions_match("", nullptr, "en"));
    EXPECT_FALSE(dom_svg_conditions_match(nullptr, "", "en"));
    EXPECT_TRUE(dom_svg_conditions_match(nullptr, "zz, EN-gb", "en"));
    EXPECT_FALSE(dom_svg_conditions_match(nullptr, "english", "en"));
    EXPECT_FALSE(dom_svg_conditions_match(nullptr, "en", "en-GB"));
}

TEST(InputDomNativeTest, SvgSwitchUsesDocumentLanguageAndDefinitionExceptions) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    document->services.preferred_languages = mem_strdup("en", MEM_CAT_LAYOUT);
    ASSERT_NE(document->services.preferred_languages, nullptr);
    DomElement* root = DomElement::create(document, "switch", nullptr);
    DomElement* french = DomElement::create(document, "g", nullptr);
    DomElement* english = DomElement::create(document, "g", nullptr);
    DomElement* definitions = DomElement::create(document, "defs", nullptr);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(french, nullptr);
    ASSERT_NE(english, nullptr);
    ASSERT_NE(definitions, nullptr);
    const char* svg = "http://www.w3.org/2000/svg";
    ASSERT_TRUE(dom_element_set_namespace_identity(root, svg, "switch"));
    ASSERT_TRUE(dom_element_set_namespace_identity(french, svg, "g"));
    ASSERT_TRUE(dom_element_set_namespace_identity(english, svg, "g"));
    ASSERT_TRUE(dom_element_set_namespace_identity(definitions, svg, "defs"));
    ASSERT_TRUE(french->set_attribute("systemLanguage", "fr", true));
    ASSERT_TRUE(english->set_attribute("systemLanguage", "en-GB", true));
    ASSERT_TRUE(definitions->set_attribute("requiredExtensions", "unsupported", true));
    ASSERT_TRUE(root->append_child(french));
    ASSERT_TRUE(root->append_child(english));
    EXPECT_FALSE(dom_svg_element_is_eligible(french));
    EXPECT_TRUE(dom_svg_element_is_eligible(english));
    EXPECT_TRUE(dom_svg_element_is_eligible(definitions));
    EXPECT_EQ(dom_svg_switch_selected_child(root), english);
    dom_document_destroy(document);
    pool_destroy(pool);
}

TEST(InputDomNativeTest, SerializesSvgNamespacesAndEscapesAttributesWithoutRuntime) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    DomElement* root = DomElement::create(document, "svg", nullptr);
    DomElement* use = DomElement::create(document, "use", nullptr);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(use, nullptr);
    const char* svg = "http://www.w3.org/2000/svg";
    ASSERT_TRUE(dom_element_set_namespace_identity(root, svg, "svg"));
    ASSERT_TRUE(dom_element_set_namespace_identity(use, svg, "use"));
    ASSERT_TRUE(dom_element_set_attribute_ns(use, "http://www.w3.org/1999/xlink", "xlink:href", "#a&b"));
    document->root = lam::up(root);
    ASSERT_TRUE(root->append_child(use));
    StrBuf* output = strbuf_new();
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(dom_serialize_xml(root, output, nullptr));
    EXPECT_STREQ(output->str, "<svg xmlns=\"http://www.w3.org/2000/svg\"><use xmlns:xlink=\"http://www.w3.org/1999/xlink\" xlink:href=\"#a&amp;b\"/></svg>");
    strbuf_free(output);
    dom_document_destroy(document);
    pool_destroy(pool);
}
