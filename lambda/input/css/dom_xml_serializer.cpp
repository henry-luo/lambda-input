#include "dom_xml_serializer.h"
#include "dom_element.hpp"
#include "../../../lib/escape.h"
#include "../../../lib/mem_factory.h"
#include "../../../lib/strbuf.h"
#include <stdio.h>
#include <string.h>

bool dom_serializes_html_void(DomElement* element) {
    if (strcmp(dom_element_namespace_uri(element), "http://www.w3.org/1999/xhtml")) return false;
    const char* local = element->local_name();
    static const char* const names[] = {"area", "base", "basefont", "bgsound", "br", "col", "embed", "frame", "hr", "img", "input", "keygen", "link", "meta", "param", "source", "track", "wbr"};
    for (const char* name : names) if (!strcmp(local, name)) return true;
    return false;
}

static void collect_xml_attr_value(const char* value, StrBuf* sb) {
    if (!value || !sb) return;
    for (const char* p = value; *p; p++) {
        if (*p == '&') strbuf_append_str(sb, "&amp;");
        else if (*p == '<') strbuf_append_str(sb, "&lt;");
        else if (*p == '"') strbuf_append_str(sb, "&quot;");
        else if (*p == '\t') strbuf_append_str(sb, "&#9;");
        else if (*p == '\n') strbuf_append_str(sb, "&#xA;");
        else if (*p == '\r') strbuf_append_str(sb, "&#xD;");
        else strbuf_append_char(sb, *p);
    }
}

static void collect_xml_text_value(const char* value, size_t length, StrBuf* sb) {
    escape_append_html_text(sb, value, length);
}

static const char* const DOM_XML_NAMESPACE = "http://www.w3.org/XML/1998/namespace";
static const char* const DOM_XMLNS_NAMESPACE = "http://www.w3.org/2000/xmlns/";

struct DomXmlPrefixBinding {
    StrView prefix;
    const char* uri;
    DomXmlPrefixBinding* next;
};
struct DomXmlSerialization {
    Pool* pool;
    DomXmlPrefixBinding* bindings;
    uint64_t prefix_index;
    StrBuf* output;
};
struct DomXmlFrame {
    DomElement* element;
    DomNode* child;
    const char* default_namespace;
    StrView prefix;
    DomXmlPrefixBinding* inherited_bindings;
    DomXmlFrame* parent;
    bool fragment;
};

static bool dom_xml_prefix_equal(StrView a, StrView b) {
    return a.length == b.length && (!a.length || !memcmp(a.str, b.str, a.length));
}

static StrView dom_xml_name_prefix(const char* name, const char* local) {
    const char* colon = name ? strchr(name, ':') : nullptr;
    return colon && !strcmp(colon + 1, local) ? StrView{name, (size_t)(colon - name)} : StrView{};
}

static DomXmlPrefixBinding* dom_xml_prefix_binding(DomXmlSerialization* context, StrView prefix) {
    for (DomXmlPrefixBinding* binding = context->bindings; binding; binding = binding->next)
        if (dom_xml_prefix_equal(binding->prefix, prefix)) return binding;
    return nullptr;
}

static void dom_xml_bind_prefix(DomXmlSerialization* context, StrView prefix, const char* uri) {
    DomXmlPrefixBinding* binding = (DomXmlPrefixBinding*)pool_calloc(context->pool, sizeof(DomXmlPrefixBinding));
    binding->prefix = prefix; binding->uri = uri; binding->next = context->bindings;
    context->bindings = binding;
}

static void dom_xml_append_name(StrBuf* output, StrView prefix, const char* local) {
    if (prefix.length) { strbuf_append_str_n(output, prefix.str, prefix.length); strbuf_append_char(output, ':'); }
    strbuf_append_str(output, local);
}

static void dom_xml_append_declaration(DomXmlSerialization* context, StrView prefix, const char* uri) {
    strbuf_append_str(context->output, " xmlns");
    if (prefix.length) { strbuf_append_char(context->output, ':'); strbuf_append_str_n(context->output, prefix.str, prefix.length); }
    strbuf_append_str(context->output, "=\"");
    collect_xml_attr_value(uri, context->output);
    strbuf_append_char(context->output, '"');
}

static StrView dom_xml_choose_prefix(DomXmlSerialization* context, const char* uri,
        StrView preferred, DomXmlPrefixBinding* inherited) {
    DomXmlPrefixBinding* wanted = preferred.length ? dom_xml_prefix_binding(context, preferred) : nullptr;
    if (wanted && !strcmp(wanted->uri, uri)) return preferred;
    for (DomXmlPrefixBinding* binding = context->bindings; binding; binding = binding->next)
        if (!strcmp(binding->uri, uri) && dom_xml_prefix_binding(context, binding->prefix) == binding)
            return binding->prefix;
    // inherited names can be rebound; names used by this element or its attributes cannot.
    bool local_conflict = false;
    for (DomXmlPrefixBinding* binding = context->bindings; binding != inherited; binding = binding->next)
        if (dom_xml_prefix_equal(binding->prefix, preferred)) { local_conflict = true; break; }
    StrView prefix = preferred;
    if (!prefix.length || local_conflict) {
        char generated[32];
        do {
            snprintf(generated, sizeof(generated), "ns%llu", (unsigned long long)context->prefix_index++);
            prefix = {generated, strlen(generated)};
        } while (dom_xml_prefix_binding(context, prefix));
        char* owned = pool_strdup(context->pool, generated);
        prefix = {owned, strlen(owned)};
    }
    dom_xml_bind_prefix(context, prefix, uri);
    dom_xml_append_declaration(context, prefix, uri);
    return prefix;
}

static void dom_xml_append_attribute(DomXmlSerialization* context, DomAttributeView attribute,
    DomXmlPrefixBinding* inherited) {
    StrView prefix = {};
    if (!strcmp(attribute.namespace_uri, DOM_XML_NAMESPACE)) prefix = {"xml", 3};
    else if (*attribute.namespace_uri) prefix = dom_xml_choose_prefix(context, attribute.namespace_uri,
        dom_xml_name_prefix(attribute.qualified_name, attribute.local_name), inherited);
    if (prefix.length) dom_xml_bind_prefix(context, prefix, attribute.namespace_uri);
    strbuf_append_char(context->output, ' ');
    dom_xml_append_name(context->output, prefix, attribute.local_name);
    strbuf_append_str(context->output, "=\"");
    collect_xml_attr_value(attribute.value, context->output);
    strbuf_append_char(context->output, '"');
}

extern "C" bool dom_serialize_xml(void* opaque, StrBuf* output, const DomXmlProjection* projection) {
    DomNode* node = (DomNode*)opaque;
    if (!node || !output) return false;
    Pool* pool = mem_pool_create(nullptr, MEM_ROLE_CSS, "dom.xml_serialization");
    if (!pool) return false;
    DomXmlSerialization context = {pool, nullptr, 1, output};
    dom_xml_bind_prefix(&context, {"xml", 3}, DOM_XML_NAMESPACE);
    DomXmlFrame* frame = nullptr;
    DomNode* current = node;
    while (current || frame) {
        if (!current) {
            if (frame->child) {
                current = frame->child;
                frame->child = dom_source_sibling(current, true);
                continue;
            }
            if (!frame->fragment) {
                if (projection && projection->append_children &&
                    !projection->append_children(projection->context, frame->element, output)) {
                    pool_destroy(pool); return false;
                }
                strbuf_append_str(output, "</");
                dom_xml_append_name(output, frame->prefix, frame->element->local_name());
                strbuf_append_char(output, '>');
            }
            context.bindings = frame->inherited_bindings;
            DomXmlFrame* parent = frame->parent;
            pool_free(pool, frame);
            frame = parent;
            continue;
        }
        DomNode* next = current;
        current = nullptr;
        if (dom_is_generated_pseudo_node(next)) continue;
        if (next->is_text()) {
            DomText* text = next->as_text();
            collect_xml_text_value(text->text, text->length, output);
            continue;
        }
        if (next->is_comment()) {
            DomComment* comment = next->as_comment();
            strbuf_append_str(output, "<!--");
            if (comment->content) strbuf_append_str_n(output, comment->content, comment->length);
            strbuf_append_str(output, "-->");
            continue;
        }
        if (!next->is_element()) continue;
        DomElement* element = next->as_element();
        if (projection && projection->include_element &&
            !projection->include_element(projection->context, element)) continue;
        DomXmlAttributeOverride* overrides = projection && projection->attributes
            ? projection->attributes(projection->context, element) : nullptr;
        DomXmlFrame* opened = (DomXmlFrame*)pool_calloc(pool, sizeof(DomXmlFrame));
        if (!opened) { pool_destroy(pool); return false; }
        opened->element = element;
        opened->parent = frame;
        opened->inherited_bindings = context.bindings;
        opened->default_namespace = frame ? frame->default_namespace : "";
        opened->child = dom_source_child(element, true);
        opened->fragment = !strcmp(element->tag_name, "#document-fragment");
        if (opened->fragment) { frame = opened; continue; }

        // declarations are recorded before choosing names, without changing the DOM.
        const char* declared_default = nullptr;
        DomAttributeIterator iterator = dom_element_attribute_iterator(element);
        DomAttributeView attribute;
        while (dom_element_next_attribute(&iterator, &attribute)) {
            if (strcmp(attribute.namespace_uri, DOM_XMLNS_NAMESPACE)) continue;
            const char* value = attribute.value ? attribute.value : "";
            if (!strcmp(attribute.local_name, "xmlns")) declared_default = value;
            else if (strcmp(value, DOM_XML_NAMESPACE))
                dom_xml_bind_prefix(&context, {attribute.local_name, strlen(attribute.local_name)}, value);
        }
        const char* uri = dom_element_namespace_uri(element);
        bool omit_default = false;
        bool append_default = false;
        StrView preferred = dom_xml_name_prefix(element->tag_name, element->local_name());
        if (!strcmp(uri, opened->default_namespace)) {
            omit_default = true;
        } else if (!strcmp(uri, DOM_XML_NAMESPACE)) {
            opened->prefix = {"xml", 3};
            if (declared_default) opened->default_namespace = declared_default;
        } else {
            DomXmlPrefixBinding* candidate = nullptr;
            if (*uri) {
                candidate = preferred.length ? dom_xml_prefix_binding(&context, preferred) : nullptr;
                if (candidate && strcmp(candidate->uri, uri)) candidate = nullptr;
                if (!candidate) for (DomXmlPrefixBinding* binding = context.bindings; binding; binding = binding->next)
                    if (!strcmp(binding->uri, uri) && dom_xml_prefix_binding(&context, binding->prefix) == binding) { candidate = binding; break; }
            }
            if (candidate) opened->prefix = candidate->prefix;
            else if (*uri && preferred.length) opened->prefix = preferred; // chosen after the opening name below
            else {
                opened->default_namespace = uri;
                omit_default = !declared_default || strcmp(declared_default, uri);
                append_default = omit_default;
            }
            if (opened->prefix.length && declared_default) opened->default_namespace = declared_default;
        }
        // prefix generation emits declarations, so reserve output for the name first.
        StrBuf* declarations = strbuf_new();
        if (!declarations) { pool_destroy(pool); return false; }
        context.output = declarations;
        if (opened->prefix.length) {
            opened->prefix = dom_xml_choose_prefix(&context, uri, opened->prefix, opened->inherited_bindings);
            // reserve a reused ancestor prefix against later attribute rebindings.
            dom_xml_bind_prefix(&context, opened->prefix, uri);
        }
        context.output = output;
        strbuf_append_char(output, '<');
        dom_xml_append_name(output, opened->prefix, element->local_name());
        strbuf_append_str_n(output, declarations->str, declarations->length);
        strbuf_free(declarations);
        if (append_default) dom_xml_append_declaration(&context, {}, uri);

        iterator = dom_element_attribute_iterator(element);
        while (dom_element_next_attribute(&iterator, &attribute)) {
            if (dom_attribute_is_internal(attribute.qualified_name)) continue;
            if (!strcmp(attribute.namespace_uri, DOM_XMLNS_NAMESPACE)) {
                if (!strcmp(attribute.local_name, "xmlns")) {
                    if (!omit_default) dom_xml_append_declaration(&context, {}, attribute.value);
                } else {
                    StrView declared = {attribute.local_name, strlen(attribute.local_name)};
                    DomXmlPrefixBinding* ancestor = nullptr;
                    for (DomXmlPrefixBinding* binding = opened->inherited_bindings; binding; binding = binding->next)
                        if (dom_xml_prefix_equal(binding->prefix, declared)) { ancestor = binding; break; }
                    const char* value = attribute.value ? attribute.value : "";
                    if (strcmp(value, DOM_XML_NAMESPACE) && (!ancestor || strcmp(ancestor->uri, value)))
                        dom_xml_append_declaration(&context, declared, value);
                }
                continue;
            }
            if (!*attribute.namespace_uri && omit_default && !strcmp(attribute.local_name, "xmlns")) continue;
            for (DomXmlAttributeOverride* replacement = overrides; replacement; replacement = replacement->next)
                if (!strcmp(replacement->name, attribute.qualified_name)) {
                    attribute.value = replacement->value; replacement->written = true; break;
                }
            dom_xml_append_attribute(&context, attribute, opened->inherited_bindings);
        }
        for (DomXmlAttributeOverride* replacement = overrides; replacement; replacement = replacement->next) {
            if (replacement->written) continue;
            const char* colon = strchr(replacement->name, ':');
            const char* local = colon ? colon + 1 : replacement->name;
            DomAttributeView attribute = {replacement->namespace_uri ? replacement->namespace_uri : "",
                replacement->name, local, replacement->value};
            dom_xml_append_attribute(&context, attribute, opened->inherited_bindings);
        }
        if (!opened->child && !(projection && projection->append_children) &&
            (strcmp(uri, "http://www.w3.org/1999/xhtml") || dom_serializes_html_void(element))) {
            strbuf_append_str(output, dom_serializes_html_void(element) ? " />" : "/>");
            context.bindings = opened->inherited_bindings;
            pool_free(pool, opened);
        } else {
            strbuf_append_char(output, '>');
            frame = opened;
        }
    }
    pool_destroy(pool);
    return true;
}
