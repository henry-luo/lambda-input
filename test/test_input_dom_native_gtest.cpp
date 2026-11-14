#include <gtest/gtest.h>
#include "lambda/input/css/dom_element.hpp"
#include "lambda/input/css/dom_xml_serializer.h"
#include "lib/strbuf.h"
#include "lambda/input/css/dom_svg_context.h"
#include "lambda/input/input.hpp"
#include "lambda/io/mark_builder.hpp"
#include "lambda/core/mark_reader.hpp"
#include "lib/mem.h"
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

TEST(InputDomNativeTest, ViewOccurrencesOwnBackingAndLeaveSourceUnchanged) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    input->ui_mode = true;
    MarkBuilder builder(input);
    Item child = builder.element("span").attr("id", "child").text("content").final();
    Item source = builder.element("div").attr("id", "original").child(child).final();
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    DomElement* first = build_dom_tree_from_element(source.element, document, nullptr);
    DomElement* second = build_dom_tree_from_element(source.element, document, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(dom_element_backing(first), source.element);
    EXPECT_NE(dom_element_backing(second), source.element);
    EXPECT_NE(dom_element_backing(first), dom_element_backing(second));
    EXPECT_EQ(dom_element_render_source(first), source.element);
    EXPECT_EQ(dom_find_element_for_source(first, source.element), first);
    ASSERT_TRUE(first->set_attribute("id", "changed"));
    EXPECT_STREQ(ElementReader(source.element).get_attr("id").cstring(), "original");
    EXPECT_STREQ(ElementReader(dom_element_backing(second)).get_attr("id").cstring(), "original");
    EXPECT_STREQ(ElementReader(dom_element_backing(first)).get_attr("id").cstring(), "changed");
    ASSERT_NE(first->first_child, nullptr);
    ASSERT_NE(second->first_child, nullptr);
    DomElement* first_child = first->first_child->as_element();
    DomElement* second_child = second->first_child->as_element();
    ASSERT_NE(first_child, nullptr);
    ASSERT_NE(second_child, nullptr);
    ASSERT_TRUE(first_child->set_attribute("id", "changed-child"));
    EXPECT_STREQ(ElementReader(child.element).get_attr("id").cstring(), "child");
    EXPECT_STREQ(ElementReader(dom_element_backing(second_child)).get_attr("id").cstring(), "child");
    dom_document_destroy(document);
    pool_destroy(pool);
}

TEST(InputDomNativeTest, NonViewWrapperKeepsLiveBackingAndRenderSourceDistinct) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    MarkBuilder builder(input);
    Item source = builder.element("div").attr("id", "source").final();
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    DomElement* node = DomElement::create(document, "div", source.element);
    ASSERT_NE(node, nullptr);
    EXPECT_NE(dom_element_backing(node), source.element);
    EXPECT_EQ(dom_element_render_source(node), source.element);
    ASSERT_TRUE(node->set_attribute("id", "live"));
    EXPECT_STREQ(ElementReader(dom_element_backing(node)).get_attr("id").cstring(), "live");
    EXPECT_EQ(dom_element_render_source(node), source.element);
    EXPECT_EQ(dom_find_element_for_source(node, source.element), node);
    dom_document_destroy(document);
    pool_destroy(pool);
}

TEST(InputDomNativeTest, NonViewWrapperShapeChangesRetainSourceStorage) {
    for (bool private_shape : {false, true}) {
        SCOPED_TRACE(private_shape);
        Pool* pool = pool_create();
        ASSERT_NE(pool, nullptr);
        Input* input = Input::create(pool);
        ASSERT_NE(input, nullptr);
        MarkBuilder builder(input);
        Item source = builder.element("div").attr("id", "source").final();
        if (private_shape) {
            TypeElmt* type = elmt_clone_type_for_mutation(source.element, pool, true);
            ASSERT_NE(type, nullptr);
            ASSERT_NE(type->shape, nullptr);
            Type* contract = (Type*)pool_calloc(pool, sizeof(Type));
            ASSERT_NE(contract, nullptr);
            *contract = *type->shape->type;
            shape_entry_set_type(type->shape, contract);
        }
        DomDocument* document = dom_document_create(input);
        ASSERT_NE(document, nullptr);
        DomElement* node = DomElement::create(document, "div", source.element);
        ASSERT_NE(node, nullptr);
        void* original_data = source.element->data;
        void* original_type = source.element->type;
        ASSERT_TRUE(pool_owns(pool, original_data));
        ASSERT_TRUE(node->set_attribute("new-attribute", "value"));
        EXPECT_EQ(source.element->data, original_data);
        EXPECT_EQ(source.element->type, original_type);
        EXPECT_FALSE(ElementReader(source.element).has_attr("new-attribute"));
        bool source_data_live = pool_owns(pool, original_data);
        EXPECT_TRUE(source_data_live);
        if (source_data_live)
            EXPECT_STREQ(ElementReader(source.element).get_attr("id").cstring(), "source");
        EXPECT_STREQ(ElementReader(dom_element_backing(node)).get_attr("new-attribute").cstring(), "value");
        dom_document_destroy(document);
        pool_destroy(pool);
    }
}

TEST(InputDomNativeTest, NonViewChildBackingTracksTheLiveWrapper) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    MarkBuilder builder(input);
    Item child = builder.element("span").attr("id", "original").final();
    Item parent = builder.element("div").child(child).final();
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    DomElement* root = build_dom_tree_from_element(parent.element, document, nullptr);
    ASSERT_NE(root, nullptr);
    DomElement* second = build_dom_tree_from_element(parent.element, document, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(second->first_child, nullptr);
    DomElement* second_child = second->first_child->as_element();
    ASSERT_NE(second_child, nullptr);
    ASSERT_NE(root->first_child, nullptr);
    DomElement* view_child = root->first_child->as_element();
    ASSERT_NE(view_child, nullptr);
    EXPECT_EQ(dom_element_render_source(view_child), child.element);
    EXPECT_EQ(dom_backed_node_index(root, view_child), 0);
    ASSERT_TRUE(view_child->set_attribute("added", "value"));
    ElementReader child_of_root = ElementReader(dom_element_backing(root)).findChildElement("span");
    ASSERT_TRUE(child_of_root.isValid());
    EXPECT_STREQ(child_of_root.get_attr("added").cstring(), "value");
    EXPECT_FALSE(ElementReader(child.element).has_attr("added"));
    EXPECT_FALSE(ElementReader(dom_element_backing(second_child)).has_attr("added"));
    ASSERT_TRUE(dom_remove_prepared_backed_child(root, view_child));
    EXPECT_EQ(dom_element_backing(root)->length, 0);
    EXPECT_EQ(parent.element->length, 1);
    EXPECT_EQ(parent.element->items[0].element, child.element);
    dom_document_destroy(document);
    pool_destroy(pool);
}

TEST(InputDomNativeTest, CopiedTreeKeepsContentAfterSourceInputIsDestroyed) {
    for (bool ui_mode : {false, true}) {
        SCOPED_TRACE(ui_mode);
        Pool* source_pool = pool_create();
        Pool* document_pool = pool_create();
        ASSERT_NE(source_pool, nullptr);
        ASSERT_NE(document_pool, nullptr);
        Input* source_input = Input::create(source_pool);
        Input* document_input = Input::create(document_pool);
        ASSERT_NE(source_input, nullptr);
        ASSERT_NE(document_input, nullptr);
        document_input->ui_mode = ui_mode;
        MarkBuilder builder(source_input);
        Item child = builder.element("span").attr("title", "child").text("body").final();
        Item source = builder.element("div").attr("id", "source").child(child).final();
        DomDocument* document = dom_document_create(document_input);
        ASSERT_NE(document, nullptr);
        DomElement* root = build_dom_tree_from_element(source.element, document, nullptr);
        ASSERT_NE(root, nullptr);
        ASSERT_NE(root->first_child, nullptr);
        DomElement* descendant = root->first_child->as_element();
        ASSERT_NE(descendant, nullptr);
        pool_destroy(source_pool);
        EXPECT_STREQ(ElementReader(dom_element_backing(root)).get_attr("id").cstring(), "source");
        EXPECT_STREQ(ElementReader(dom_element_backing(descendant)).get_attr("title").cstring(), "child");
        ASSERT_TRUE(root->set_attribute("added", "value"));
        ASSERT_TRUE(descendant->set_attribute("added-child", "value"));
        EXPECT_STREQ(ElementReader(dom_element_backing(root)).get_attr("added").cstring(), "value");
        dom_document_destroy(document);
        pool_destroy(document_pool);
    }
}

TEST(InputDomNativeTest, CopiedTreeOwnsForeignFieldTypeContract) {
    for (bool ui_mode : {false, true}) {
        SCOPED_TRACE(ui_mode);
        Pool* source_pool = pool_create();
        Pool* document_pool = pool_create();
        ASSERT_NE(source_pool, nullptr);
        ASSERT_NE(document_pool, nullptr);
        Input* source_input = Input::create(source_pool);
        Input* document_input = Input::create(document_pool);
        ASSERT_NE(source_input, nullptr);
        ASSERT_NE(document_input, nullptr);
        document_input->ui_mode = ui_mode;
        MarkBuilder builder(source_input);
        Item source = builder.element("div").attr("id", "source").final();
        TypeElmt* source_type = elmt_clone_type_for_mutation(source.element, source_pool, true);
        ASSERT_NE(source_type, nullptr);
        ASSERT_NE(source_type->shape, nullptr);
        Type* contract = (Type*)pool_alloc(source_pool, sizeof(Type));
        ASSERT_NE(contract, nullptr);
        *contract = *source_type->shape->type;
        shape_entry_set_type(source_type->shape, contract);
        DomDocument* document = dom_document_create(document_input);
        ASSERT_NE(document, nullptr);
        DomElement* root = build_dom_tree_from_element(source.element, document, nullptr);
        ASSERT_NE(root, nullptr);
        TypeElmt* copied_type = (TypeElmt*)dom_element_backing(root)->type;
        ASSERT_NE(copied_type, nullptr);
        ASSERT_NE(copied_type->shape, nullptr);
        EXPECT_FALSE(pool_owns(source_pool, copied_type->shape->type));
        const LaneStorageDesc* storage = shape_entry_storage(copied_type->shape);
        EXPECT_FALSE(pool_owns(source_pool, storage->semantic_contract));
        EXPECT_FALSE(pool_owns(source_pool, storage->base_contract));
        EXPECT_STREQ(ElementReader(dom_element_backing(root)).get_attr("id").cstring(), "source");
        dom_document_destroy(document);
        pool_destroy(document_pool);
        pool_destroy(source_pool);
    }
}

TEST(InputDomNativeTest, CopiedTreeOwnsForeignNullableFieldContractGraph) {
    for (bool ui_mode : {false, true}) {
        SCOPED_TRACE(ui_mode);
        Pool* source_pool = pool_create();
        Pool* document_pool = pool_create();
        ASSERT_NE(source_pool, nullptr);
        ASSERT_NE(document_pool, nullptr);
        Input* source_input = Input::create(source_pool);
        Input* document_input = Input::create(document_pool);
        ASSERT_NE(source_input, nullptr);
        ASSERT_NE(document_input, nullptr);
        document_input->ui_mode = ui_mode;
        MarkBuilder builder(source_input);
        Item source = builder.element("div").attr("count", (int64_t)7).final();
        TypeElmt* source_type = elmt_clone_type_for_mutation(source.element, source_pool, true);
        ASSERT_NE(source_type, nullptr);
        ASSERT_NE(source_type->shape, nullptr);
        Type* base = (Type*)pool_alloc(source_pool, sizeof(Type));
        TypeUnary* optional = (TypeUnary*)pool_calloc(source_pool, sizeof(TypeUnary));
        ASSERT_NE(base, nullptr);
        ASSERT_NE(optional, nullptr);
        *base = TYPE_INT;
        optional->type_id = LMD_TYPE_TYPE;
        optional->kind = TYPE_KIND_UNARY;
        optional->op = OPERATOR_OPTIONAL;
        optional->operand = base;
        shape_entry_set_type(source_type->shape, optional);
        DomDocument* document = dom_document_create(document_input);
        ASSERT_NE(document, nullptr);
        DomElement* root = build_dom_tree_from_element(source.element, document, nullptr);
        ASSERT_NE(root, nullptr);
        TypeElmt* copied_type = (TypeElmt*)dom_element_backing(root)->type;
        ASSERT_NE(copied_type, nullptr);
        ASSERT_NE(copied_type->shape, nullptr);
        const LaneStorageDesc* storage = shape_entry_storage(copied_type->shape);
        EXPECT_TRUE(storage->nullable);
        EXPECT_FALSE(pool_owns(source_pool, copied_type->shape->type));
        EXPECT_FALSE(pool_owns(source_pool, storage->semantic_contract));
        EXPECT_FALSE(pool_owns(source_pool, storage->base_contract));
        ItemReader value = ElementReader(dom_element_backing(root)).get_attr("count");
        EXPECT_TRUE(value.isInt());
        EXPECT_EQ(value.asInt(), 7);
        dom_document_destroy(document);
        pool_destroy(document_pool);
        pool_destroy(source_pool);
    }
}

#if defined(__unix__) || defined(__APPLE__)
TEST(InputDomNativeTest, CopiedTreeEditDoesNotReadSourceHeaderAgain) {
    Pool* source_pool = pool_create();
    Pool* document_pool = pool_create();
    ASSERT_NE(source_pool, nullptr);
    ASSERT_NE(document_pool, nullptr);
    Input* source_input = Input::create(source_pool);
    Input* document_input = Input::create(document_pool);
    ASSERT_NE(source_input, nullptr);
    ASSERT_NE(document_input, nullptr);
    MarkBuilder builder(source_input);
    Item source = builder.element("div").attr("id", "source").final();
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    ASSERT_NE(page, MAP_FAILED);
    Element* external = static_cast<Element*>(page);
    *external = *source.element;
    DomDocument* document = dom_document_create(document_input);
    ASSERT_NE(document, nullptr);
    DomElement* root = build_dom_tree_from_element(external, document, nullptr);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(mprotect(page, page_size, PROT_NONE), 0);
    EXPECT_TRUE(root->set_attribute("added", "value"));
    ASSERT_EQ(mprotect(page, page_size, PROT_READ | PROT_WRITE), 0);
    EXPECT_STREQ(ElementReader(dom_element_backing(root)).get_attr("added").cstring(), "value");
    dom_document_destroy(document);
    EXPECT_EQ(munmap(page, page_size), 0);
    pool_destroy(document_pool);
    pool_destroy(source_pool);
}
#endif

TEST(InputDomNativeTest, NestedArrayEditsKeepOwnedChildOccurrences) {
    for (bool ui_mode : {false, true}) {
        SCOPED_TRACE(ui_mode);
        Pool* pool = pool_create();
        ASSERT_NE(pool, nullptr);
        Input* input = Input::create(pool);
        ASSERT_NE(input, nullptr);
        input->ui_mode = ui_mode;
        MarkBuilder builder(input);
        Item child = builder.element("span").attr("id", "original").final();
        Item nested = builder.array().append(child).final();
        Item parent = builder.element("div").child(nested).final();
        DomDocument* document = dom_document_create(input);
        ASSERT_NE(document, nullptr);
        DomElement* root = build_dom_tree_from_element(parent.element, document, nullptr);
        ASSERT_NE(root, nullptr);
        ASSERT_NE(root->first_child, nullptr);
        DomElement* view_child = root->first_child->as_element();
        ASSERT_NE(view_child, nullptr);
        Item copied_array = dom_element_backing(root)->items[0];
        ASSERT_EQ(get_type_id(copied_array), LMD_TYPE_ARRAY);
        ASSERT_NE(copied_array.array, nested.array);
        ASSERT_EQ(copied_array.array->length, 1);
        EXPECT_EQ(copied_array.array->items[0].element, dom_element_backing(view_child));
        EXPECT_EQ(nested.array->items[0].element, child.element);
        ASSERT_TRUE(view_child->set_attribute("added", "value"));
        EXPECT_STREQ(ElementReader(copied_array.array->items[0]).get_attr("added").cstring(), "value");
        EXPECT_FALSE(ElementReader(child.element).has_attr("added"));
        DomElement* inserted = dom_create_backed_element_identity(document, "b", nullptr, false);
        ASSERT_NE(inserted, nullptr);
        Item* saved_slots = dom_element_backing(root)->items;
        ASSERT_TRUE(dom_insert_detached_backed_element(root, inserted, view_child));
        Item before_insert = copied_array;
        copied_array = dom_element_backing(root)->items[0];
        EXPECT_NE(dom_element_backing(root)->items, saved_slots);
        EXPECT_EQ(saved_slots[0].array, before_insert.array);
        EXPECT_NE(copied_array.array, before_insert.array);
        EXPECT_EQ(before_insert.array->length, 1);
        ASSERT_EQ(copied_array.array->length, 2);
        EXPECT_EQ(copied_array.array->items[0].element, dom_element_backing(inserted));
        EXPECT_EQ(copied_array.array->items[1].element, dom_element_backing(view_child));
        DomText* text = DomText::create_detached_copy(document, "prefix", 6);
        ASSERT_NE(text, nullptr);
        ASSERT_TRUE(dom_insert_prepared_backed_text(root, text, view_child));
        Item before_text = copied_array;
        copied_array = dom_element_backing(root)->items[0];
        EXPECT_NE(copied_array.array, before_text.array);
        EXPECT_EQ(before_text.array->length, 2);
        ASSERT_EQ(copied_array.array->length, 3);
        EXPECT_EQ(copied_array.array->items[1].get_string(), text->native_string);
        EXPECT_EQ(root->first_child, inserted);
        EXPECT_EQ(inserted->next_sibling, text);
        EXPECT_EQ(text->next_sibling, view_child);
        ASSERT_TRUE(dom_remove_prepared_backed_child(root, inserted));
        ASSERT_TRUE(dom_text_remove(text));
        ASSERT_TRUE(dom_remove_prepared_backed_child(root, view_child));
        copied_array = dom_element_backing(root)->items[0];
        EXPECT_EQ(copied_array.array->length, 0);
        EXPECT_EQ(nested.array->length, 1);
        dom_document_destroy(document);
        pool_destroy(pool);
    }
}

TEST(InputDomNativeTest, NestedArrayRemovalPreservesSiblingNodesAndSource) {
    for (bool ui_mode : {false, true}) {
        SCOPED_TRACE(ui_mode);
        Pool* pool = pool_create();
        ASSERT_NE(pool, nullptr);
        Input* input = Input::create(pool);
        ASSERT_NE(input, nullptr);
        input->ui_mode = ui_mode;
        MarkBuilder builder(input);
        Item comment = builder.element("!--").text("note").final();
        Item child = builder.element("span").final();
        Item inner = builder.array().append("text").append(comment).append(child).final();
        Item outer = builder.array().append(inner).final();
        Item parent = builder.element("div").child(outer).final();
        DomDocument* document = dom_document_create(input);
        ASSERT_NE(document, nullptr);
        DomElement* root = build_dom_tree_from_element(parent.element, document, nullptr);
        ASSERT_NE(root, nullptr);
        ASSERT_NE(root->first_child, nullptr);
        DomText* text = root->first_child->as_text();
        ASSERT_NE(text, nullptr);
        ASSERT_NE(text->next_sibling, nullptr);
        DomComment* note = text->next_sibling->as_comment();
        ASSERT_NE(note, nullptr);
        ASSERT_NE(note->next_sibling, nullptr);
        DomElement* element = note->next_sibling->as_element();
        ASSERT_NE(element, nullptr);
        Item copied_outer = dom_element_backing(root)->items[0];
        ASSERT_EQ(get_type_id(copied_outer), LMD_TYPE_ARRAY);
        Item copied_inner = copied_outer.array->items[0];
        ASSERT_EQ(get_type_id(copied_inner), LMD_TYPE_ARRAY);
        ASSERT_EQ(copied_inner.array->length, 3);
        ASSERT_TRUE(dom_text_remove(text));
        Item before_remove = copied_outer;
        copied_outer = dom_element_backing(root)->items[0];
        EXPECT_NE(copied_outer.array, before_remove.array);
        EXPECT_EQ(copied_inner.array->length, 3);
        copied_inner = copied_outer.array->items[0];
        EXPECT_EQ(copied_inner.array->length, 2);
        EXPECT_EQ(root->first_child, note);
        ASSERT_TRUE(dom_comment_remove(note));
        copied_outer = dom_element_backing(root)->items[0];
        copied_inner = copied_outer.array->items[0];
        EXPECT_EQ(copied_inner.array->length, 1);
        EXPECT_EQ(root->first_child, element);
        ASSERT_TRUE(dom_remove_prepared_backed_child(root, element));
        copied_outer = dom_element_backing(root)->items[0];
        copied_inner = copied_outer.array->items[0];
        EXPECT_EQ(copied_inner.array->length, 0);
        EXPECT_EQ(root->first_child, nullptr);
        EXPECT_EQ(inner.array->length, 3);
        EXPECT_EQ(outer.array->items[0].array, inner.array);
        dom_document_destroy(document);
        pool_destroy(pool);
    }
}

TEST(InputDomNativeTest, NestedArrayEditsDeclineForeignStorageWithoutDetachingNodes) {
    Pool* source_pool = pool_create();
    Pool* document_pool = pool_create();
    ASSERT_NE(source_pool, nullptr);
    ASSERT_NE(document_pool, nullptr);
    Input* source_input = Input::create(source_pool);
    Input* document_input = Input::create(document_pool);
    ASSERT_NE(source_input, nullptr);
    ASSERT_NE(document_input, nullptr);
    MarkBuilder builder(source_input);
    Item child = builder.element("span").final();
    Item nested = builder.array().append(child).final();
    Item source = builder.element("div").child(nested).final();
    DomDocument* document = dom_document_create(document_input);
    ASSERT_NE(document, nullptr);
    DomElement* root = DomElement::create(document, "div", source.element);
    DomElement* reference = DomElement::create(document, "span", child.element);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(reference, nullptr);
    // a registered node in foreign array storage must not authorize mutation.
    nested.array->items[0] = {.element = dom_element_backing(reference)};
    ASSERT_TRUE(root->DomNode::append_child(reference));
    DomElement* inserted = dom_create_backed_element_identity(document, "b", nullptr, false);
    ASSERT_NE(inserted, nullptr);
    EXPECT_FALSE(dom_insert_detached_backed_element(root, inserted, reference));
    DomElement* origin = dom_create_backed_element_identity(document, "p", nullptr, false);
    ASSERT_NE(origin, nullptr);
    DomText* text = origin->append_text("retained");
    ASSERT_NE(text, nullptr);
    EXPECT_FALSE(dom_insert_prepared_backed_text(root, text, reference));
    EXPECT_EQ(text->parent, origin);
    EXPECT_FALSE(dom_remove_prepared_backed_child(root, reference));
    EXPECT_EQ(root->first_child, reference);
    EXPECT_EQ(reference->parent, root);
    EXPECT_EQ(nested.array->length, 1);
    EXPECT_EQ(nested.array->items[0].element, dom_element_backing(reference));
    dom_document_destroy(document);
    pool_destroy(document_pool);
    pool_destroy(source_pool);
}

TEST(InputDomNativeTest, NestedArrayEditPreservesWideScalarAndSnapshot) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    MarkBuilder builder(input);
    const int64_t number = INT64_C(1099511627777);
    Item child = builder.element("span").final();
    Item nested = builder.array().append(builder.createLong(number)).append(child).final();
    Item source = builder.element("div").child(nested).final();
    DomDocument* document = dom_document_create(input);
    ASSERT_NE(document, nullptr);
    DomElement* root = build_dom_tree_from_element(source.element, document, nullptr);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(root->first_child, nullptr);
    DomElement* occurrence = root->first_child->as_element();
    ASSERT_NE(occurrence, nullptr);
    Array* snapshot = dom_element_backing(root)->items[0].array;
    ASSERT_EQ(snapshot->length, 2);
    ASSERT_TRUE(dom_remove_prepared_backed_child(root, occurrence));
    Array* current = dom_element_backing(root)->items[0].array;
    ASSERT_NE(current, snapshot);
    ASSERT_EQ(current->length, 1);
    EXPECT_EQ(array_item_read(current, 0).get_int64(), number);
    EXPECT_EQ(array_item_read(snapshot, 0).get_int64(), number);
    EXPECT_EQ(snapshot->length, 2);
    EXPECT_EQ(nested.array->length, 2);
    dom_document_destroy(document);
    pool_destroy(pool);
}

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
