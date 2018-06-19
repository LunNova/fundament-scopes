/*
    The Scopes Compiler Infrastructure
    This file is distributed under the MIT License.
    See LICENSE.md for details.
*/

#include "stream_ast.hpp"
#include "ast.hpp"
#include "type.hpp"
#include "dyn_cast.inc"

#include <unordered_set>

namespace scopes {

static const char INDENT_SEP[] = "⁞";

//------------------------------------------------------------------------------
// AST PRINTER
//------------------------------------------------------------------------------

StreamASTFormat::StreamASTFormat() :
    anchors(None),
    depth(0)
{}

//------------------------------------------------------------------------------

struct StreamAST : StreamAnchors {
    StreamASTFormat fmt;
    bool line_anchors;
    bool atom_anchors;

    std::unordered_set<ASTNode *> visited;

    StreamAST(StyledStream &_ss, const StreamASTFormat &_fmt)
        : StreamAnchors(_ss), fmt(_fmt) {
        line_anchors = (fmt.anchors == StreamASTFormat::Line);
        atom_anchors = (fmt.anchors == StreamASTFormat::All);
    }

    void stream_indent(int depth = 0) {
        const int N = 50;
        if (depth > N) {
            int levels = depth / N;
            depth = depth % N;
            ss << "[" << (levels * N) << "]";
        }
        if (depth >= 1) {
            ss << Style_Comment << "  ";
            for (int i = 2; i <= depth; ++i) {
                ss << INDENT_SEP << " ";
            }
            ss << Style_None;
        }
    }

    void write_arguments(CallLike *val, int depth, int maxdepth) {
        for (int i = 0; i < val->args.size(); ++i) {
            ss << std::endl;
            auto &&arg = val->args[i];
            if (arg.key == SYM_Unnamed) {
                walk(val->args[i].expr, depth+1, maxdepth);
            } else {
                stream_indent(depth+1);
                ss << arg.key << " "
                    << Style_Operator << "=" << Style_None << std::endl;
                walk(val->args[i].expr, depth+2, maxdepth);
            }
        }
    }

    void walk(ASTNode *node, int depth, int maxdepth) {
        const Anchor *anchor = node->anchor();

        stream_indent(depth);
        if (atom_anchors) {
            stream_anchor(anchor);
        }

        bool is_new = !visited.count(node);
        if (is_new)
            visited.insert(node);

        switch(node->kind()) {
        case ASTK_Function: {
            auto val = cast<ASTFunction>(node);
            ss << Style_Keyword << "Function" << Style_None;
            if (val->is_inline()) {
                ss << " " << Style_Keyword << "inline" << Style_None;
            }
            if (!val->body) {
                ss << " " << Style_Keyword << "forward-decl" << Style_None;
            }
            ss << " ";
            ss << Style_Symbol << val->name.name()->data
                << "λ" << (void *)val << Style_None;
            if (is_new) {
                for (int i = 0; i < val->params.size(); ++i) {
                    ss << std::endl;
                    walk(val->params[i], depth+1, maxdepth);
                }
                if (val->body) {
                    ss << std::endl;
                    walk(val->body, depth+1, maxdepth);
                }
            } else {
                ss << " <...>";
            }
        } break;
        case ASTK_Block: {
            auto val = cast<Block>(node);
            ss << Style_Keyword << "Block" << Style_None;
            for (int i = 0; i < val->body.size(); ++i) {
                ss << std::endl;
                walk(val->body[i], depth+1, maxdepth);
            }
        } break;
        case ASTK_If: {
            auto val = cast<If>(node);
            ss << Style_Keyword << "If" << Style_None;
            for (int i = 0; i < val->clauses.size(); ++i) {
                ss << std::endl;
                auto &&expr = val->clauses[i];
                if (expr.cond)
                    walk(expr.cond, depth+1, maxdepth);
                else {
                    stream_indent(depth+1);
                    ss << Style_Keyword << "Else" << Style_None;
                }
                ss << std::endl;
                walk(expr.body, depth+1, maxdepth);
            }
        } break;
        case ASTK_Symbol: {
            auto val = cast<ASTSymbol>(node);
            ss << Style_Symbol << val->name.name()->data
                << "$" << (void *)val << Style_None;
            if (val->is_variadic()) {
                ss << "…";
            }
            if (val->type != TYPE_Unknown) {
                ss << Style_Operator << ":" << Style_None;
                ss << val->type;
            }
        } break;
        case ASTK_Call: {
            auto val = cast<Call>(node);
            ss << Style_Keyword << "Call" << Style_None;
            if (val->flags & CF_RawCall) {
                ss << Style_Keyword << " rawcall" << Style_None;
            }
            if (val->flags & CF_TryCall) {
                ss << Style_Keyword << " trycall" << Style_None;
            }
            ss << std::endl;
            walk(val->callee, depth+1, maxdepth);
            write_arguments(val, depth, maxdepth);
        } break;
        case ASTK_Let: {
            auto val = cast<Let>(node);
            ss << Style_Keyword << "Let" << Style_None;
            for (int i = 0; i < val->bindings.size(); ++i) {
                ss << std::endl;
                auto &&arg = val->bindings[i];
                walk(arg.sym, depth+1, maxdepth);
                ss << std::endl;
                walk(arg.expr, depth+2, maxdepth);
            }
            if (val->has_variadic_section()) {
                for (int i = 0; i < val->variadic.syms.size(); ++i) {
                    ss << std::endl;
                    walk(val->variadic.syms[i], depth + 1, maxdepth);
                }
                ss << std::endl;
                walk(val->variadic.expr, depth+2, maxdepth);
            }
            ss << std::endl;
            walk(val->body, depth+1, maxdepth);
        } break;
        case ASTK_Loop: {
            auto val = cast<Loop>(node);
            ss << Style_Keyword << "Loop" << Style_None;
            for (int i = 0; i < val->bindings.size(); ++i) {
                ss << std::endl;
                auto &&arg = val->bindings[i];
                walk(arg.sym, depth+1, maxdepth);
                ss << std::endl;
                walk(arg.expr, depth+2, maxdepth);
            }
            ss << std::endl;
            walk(val->body, depth+1, maxdepth);
        } break;
        case ASTK_Const: {
            auto val = cast<Const>(node);
            ss << Style_Keyword << "Const" << Style_None << " ";
            ss << val->value;
        } break;
        case ASTK_Break: {
            auto val = cast<Break>(node);
            ss << Style_Keyword << "Break" << Style_None;
            write_arguments(val, depth, maxdepth);
        } break;
        case ASTK_Repeat: {
            auto val = cast<Repeat>(node);
            ss << Style_Keyword << "Repeat" << Style_None;
            write_arguments(val, depth, maxdepth);
        } break;
        case ASTK_Return: {
            auto val = cast<Return>(node);
            ss << Style_Keyword << "Return" << Style_None;
            write_arguments(val, depth, maxdepth);
        } break;
        case ASTK_SyntaxExtend: {
            auto val = cast<SyntaxExtend>(node);
            ss << Style_Keyword << "SyntaxExtend" << Style_None;
            ss << std::endl;
            walk(val->func, depth+1, maxdepth);
        } break;
        default:
            ss << Style_Error << "<unknown AST node type>" << Style_None;
        }
    }

    void stream(ASTNode *node) {
        visited.clear();
        walk(node, fmt.depth, -1);
        ss << std::endl;
    }
};

//------------------------------------------------------------------------------

void stream_ast(
    StyledStream &_ss, ASTNode *node, const StreamASTFormat &_fmt) {
    StreamAST streamer(_ss, _fmt);
    streamer.stream(node);
}

//------------------------------------------------------------------------------

} // namespace scopes
