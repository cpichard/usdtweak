#include "SpanTree.h"

#include <algorithm>

const char *SpanKindName(SpanKind kind) {
    switch (kind) {
    case SpanKind::Layer:
        return "layer";
    case SpanKind::LayerMetadata:
        return "layer metadata";
    case SpanKind::Prim:
        return "prim";
    case SpanKind::Property:
        return "property";
    case SpanKind::VariantSet:
        return "variant set";
    case SpanKind::Variant:
        return "variant";
    }
    return "";
}

int SpanNodeAbsoluteFirstLine(const SpanNode *node) {
    int line = 0;
    for (; node; node = node->parent) {
        line += node->lineOffsetInParent;
    }
    return line;
}

SpanNode *SpanNodeAtLine(SpanNode *root, int absoluteLine) {
    if (!root || absoluteLine < 0 || absoluteLine >= root->lineCount + root->lineOffsetInParent) {
        // root offset is expected to be 0; keep the check cheap and tolerant
        if (!root || absoluteLine < root->lineOffsetInParent ||
            absoluteLine >= root->lineOffsetInParent + root->lineCount) {
            return nullptr;
        }
    }
    SpanNode *node = root;
    int relativeLine = absoluteLine - root->lineOffsetInParent;
    while (true) {
        // Find the last child whose offset is <= relativeLine
        auto it = std::upper_bound(node->children.begin(), node->children.end(), relativeLine,
                                   [](int line, const std::unique_ptr<SpanNode> &child) {
                                       return line < child->lineOffsetInParent;
                                   });
        if (it == node->children.begin()) {
            return node;
        }
        SpanNode *child = std::prev(it)->get();
        if (relativeLine >= child->lineOffsetInParent + child->lineCount) {
            return node; // falls in a gap between children
        }
        node = child;
        relativeLine -= child->lineOffsetInParent;
    }
}

void SpanTreeShiftAfter(SpanNode *node, int delta) {
    if (!delta) {
        return;
    }
    for (SpanNode *n = node; n->parent; n = n->parent) {
        bool after = false;
        for (auto &sibling : n->parent->children) {
            if (after) {
                sibling->lineOffsetInParent += delta;
            }
            if (sibling.get() == n) {
                after = true;
            }
        }
        n->parent->lineCount += delta;
    }
}

int SpanNodeIndent(const SpanNode *node) {
    int indent = 0;
    for (const SpanNode *n = node->parent; n; n = n->parent) {
        if (n->kind == SpanKind::Prim || n->kind == SpanKind::VariantSet || n->kind == SpanKind::Variant) {
            ++indent;
        }
    }
    return indent;
}

void SpanTreeBuildPathMap(SpanNode *root,
                          std::unordered_map<SdfPath, SpanNode *, SdfPath::Hash> &pathToNode) {
    if (!root) {
        return;
    }
    if (!root->path.IsEmpty()) {
        pathToNode[root->path] = root;
    }
    for (auto &child : root->children) {
        SpanTreeBuildPathMap(child.get(), pathToNode);
    }
}
