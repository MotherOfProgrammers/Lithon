#!/usr/bin/env python3
"""
Lithon type-checker.

Static analysis over Python's ast -- no execution. Enforces, for
top-level code (no functions yet):

  0.6.1  -- every first assignment must carry an explicit annotation
  0.6.4  -- re-assignment is checked against the already-declared type
  0.6.5  -- integer literal overflow is a compile-time error
  0.6.10 -- definite assignment + type agreement across if/else
            branches (function scoping, not block scoping)

Every Name reference (not just assignment targets) is checked
against the current scope, so a variable dropped from the merged
scope after an if/else (because only one branch assigned it) is
caught the moment it's used, not silently ignored.
"""
import ast
import sys


class RCRError(Exception):
    pass


class LType:
    def __init__(self, kind, width=None):
        self.kind = kind
        self.width = width

    def __eq__(self, other):
        return self.kind == other.kind and self.width == other.width

    def __repr__(self):
        if self.width is None:
            return self.kind
        return f"{self.kind}[{self.width}]"


def int_range(width):
    return -(2 ** (width - 1)), 2 ** (width - 1) - 1


def parse_annotation(node):
    if isinstance(node, ast.Name):
        if node.id == "bool":
            return LType("bool")
        raise RCRError(f"type '{node.id}' requires an explicit size, e.g. {node.id}[64]")

    if isinstance(node, ast.Subscript):
        if not isinstance(node.value, ast.Name):
            raise RCRError("unsupported type annotation form")
        base = node.value.id
        if base == "bool":
            raise RCRError("bool[N] is not allowed -- bool has no size parameter (V1_SPEC 0.6.3)")
        if base not in ("int", "float", "str"):
            raise RCRError(f"unknown type '{base}'")

        size_node = node.slice
        if not isinstance(size_node, ast.Constant) or not isinstance(size_node.value, int):
            raise RCRError(f"{base}[N] requires a literal integer size")
        width = size_node.value

        if base == "int" and width not in (8, 16, 32, 64):
            raise RCRError(f"int[{width}] is not a valid width -- must be 8, 16, 32, or 64")
        if base == "float" and width not in (32, 64):
            raise RCRError(f"float[{width}] is not a valid width -- must be 32 or 64")

        return LType(base, width)

    raise RCRError("every binding requires an explicit type annotation (V1_SPEC 0.6.1)")


def literal_kind(node):
    if isinstance(node, ast.Constant):
        if isinstance(node.value, bool):
            return "bool"
        if isinstance(node.value, int):
            return "int"
        if isinstance(node.value, float):
            return "float"
        if isinstance(node.value, str):
            return "str"
    return None


def check_literal_against_type(value_node, declared: LType, context: str):
    kind = literal_kind(value_node)
    if kind is None:
        return

    if kind != declared.kind:
        raise RCRError(
            f"{context}: literal of kind '{kind}' does not match declared type {declared}")

    if declared.kind == "int":
        lo, hi = int_range(declared.width)
        val = value_node.value
        if not (lo <= val <= hi):
            raise RCRError(
                f"{context}: literal {val} does not fit {declared} "
                f"(valid range {lo}..{hi}) -- V1_SPEC 0.6.5")


def check_expr_names(node, scope, context):
    """Recursively verifies every Name *load* in this expression exists
    in scope. This is what surfaces a 0.6.10 branch-merge drop as a
    real error at the point of use, not silently."""
    if isinstance(node, ast.Name):
        if node.id not in scope:
            raise RCRError(
                f"{context}: '{node.id}' is not definitely assigned here "
                f"(V1_SPEC 0.6.10 -- may be unassigned on some path, or never declared)")
        return
    if isinstance(node, ast.Constant):
        return
    if isinstance(node, ast.BinOp):
        check_expr_names(node.left, scope, context)
        check_expr_names(node.right, scope, context)
        return
    if isinstance(node, ast.Compare):
        check_expr_names(node.left, scope, context)
        for c in node.comparators:
            check_expr_names(c, scope, context)
        return
    if isinstance(node, ast.BoolOp):
        for v in node.values:
            check_expr_names(v, scope, context)
        return
    if isinstance(node, ast.UnaryOp):
        check_expr_names(node.operand, scope, context)
        return
    if isinstance(node, ast.Call):
        for a in node.args:
            check_expr_names(a, scope, context)
        return
    raise RCRError(f"{context}: expression node {type(node).__name__} not supported yet")


class TypeChecker:
    def check_stmt(self, node, scope):
        """Mutates `scope` in place to reflect this statement's effect."""
        if isinstance(node, ast.AnnAssign):
            name = node.target.id if isinstance(node.target, ast.Name) else None
            if name is None:
                raise RCRError("only simple name targets are supported for annotations")
            declared = parse_annotation(node.annotation)
            if node.value is not None:
                check_expr_names(node.value, scope, f"declaration of '{name}'")
                check_literal_against_type(node.value, declared, f"declaration of '{name}'")
            scope[name] = declared  # 0.6.4: always a fresh binding
            return

        if isinstance(node, ast.Assign):
            if len(node.targets) != 1 or not isinstance(node.targets[0], ast.Name):
                raise RCRError("only single-name assignment targets are supported")
            name = node.targets[0].id
            if name not in scope:
                raise RCRError(
                    f"'{name}' is assigned without ever being declared with a type "
                    f"annotation (V1_SPEC 0.6.1) -- write '{name}: <type> = ...' first")
            declared = scope[name]
            check_expr_names(node.value, scope, f"re-assignment of '{name}'")
            check_literal_against_type(node.value, declared, f"re-assignment of '{name}'")
            return

        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            for a in node.value.args:
                check_expr_names(a, scope, "call argument")
            return

        if isinstance(node, ast.If):
            check_expr_names(node.test, scope, "if condition")

            then_scope = dict(scope)
            for stmt in node.body:
                self.check_stmt(stmt, then_scope)

            else_scope = dict(scope)
            for stmt in node.orelse:
                self.check_stmt(stmt, else_scope)

            merged = {}
            for name in set(then_scope) | set(else_scope):
                in_then = name in then_scope
                in_else = name in else_scope
                if in_then and in_else:
                    if then_scope[name] != else_scope[name]:
                        raise RCRError(
                            f"type of '{name}' disagrees across if/else branches: "
                            f"{then_scope[name]} vs {else_scope[name]} (V1_SPEC 0.6.10)")
                    merged[name] = then_scope[name]
                # in only one branch -> dropped: not definitely assigned after this if

            scope.clear()
            scope.update(merged)
            return

        raise RCRError(f"statement {type(node).__name__} not supported by the type-checker yet")

    def check_module(self, tree):
        scope = {}
        for stmt in tree.body:
            self.check_stmt(stmt, scope)
        return scope


def main():
    if len(sys.argv) != 2:
        print("usage: typecheck.py <source.py>", file=sys.stderr)
        return 1

    with open(sys.argv[1]) as f:
        source = f.read()

    tree = ast.parse(source)
    checker = TypeChecker()
    try:
        final_scope = checker.check_module(tree)
    except RCRError as e:
        print(f"RCR error: {e}", file=sys.stderr)
        return 1

    print("OK -- type-checks cleanly")
    for name, t in final_scope.items():
        print(f"  {name}: {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
