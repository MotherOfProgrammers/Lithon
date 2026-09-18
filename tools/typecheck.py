#!/usr/bin/env python3
"""
Lithon type-checker.

Static analysis over Python's ast -- no execution. Enforces, for
top-level code (no functions yet):

  0.6.1  -- every first assignment must carry an explicit annotation
  0.6.4  -- re-assignment is checked against the already-declared type
  0.6.5  -- integer literal overflow is a compile-time error
  0.6.10 -- definite assignment + type agreement across if/else
            branches (function scoping, not block scoping). A name
            with a type but no assigned value (e.g. "i: int[8]"
            alone) is NOT considered definitely assigned -- it must
            actually receive a value before use.
  0.6.11 -- conversions: widening/same-width automatic, narrowing
            never allowed; int -> float automatic, float -> int
            never allowed; no cast syntax exists anywhere
  0.6.12 -- range()'s produced values are checked against the loop
            variable's declared width at compile time when the bound
            is a literal. The loop variable must be declared with a
            real starting value beforehand (e.g. "i: int[8] = 0"),
            same as any ordinary variable -- range() then overwrites
            it each iteration, 0..N-1.

Scaffolding note: since Python's own ast has no annotation slot on a
for-target (V1_SPEC 0.6.6's custom grammar is M10 work), this checker
requires the loop variable to be pre-declared via AnnAssign on a
statement before the for-loop, enforcing 0.6.12's semantics now
without yet building the custom parser.
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


def infer_expr_type(node, scope, context):
    kind = literal_kind(node)
    if kind == "bool":
        return LType("bool")
    if kind == "int":
        return LType("int", 64)
    if kind == "float":
        return LType("float", 64)
    if kind == "str":
        return LType("str", len(node.value.encode("utf-8")))

    if isinstance(node, ast.Name):
        if node.id not in scope:
            raise RCRError(
                f"{context}: '{node.id}' is not definitely assigned here (V1_SPEC 0.6.10)")
        return scope[node.id]

    raise RCRError(f"{context}: cannot statically determine the type of this expression yet")


def check_assignment_compatible(source: LType, target: LType, context: str):
    if source.kind == target.kind:
        if source.width is None and target.width is None:
            return
        if source.width is not None and target.width is not None:
            if target.width >= source.width:
                return
            raise RCRError(
                f"{context}: cannot narrow {source} into {target} -- "
                f"narrowing is never allowed (V1_SPEC 0.6.11)")
        raise RCRError(f"{context}: incompatible {source} and {target}")

    if source.kind == "int" and target.kind == "float":
        return

    if source.kind == "float" and target.kind == "int":
        raise RCRError(
            f"{context}: float -> int conversion does not exist in Lithon "
            f"(V1_SPEC 0.6.11) -- no cast can perform this")

    raise RCRError(f"{context}: cannot convert {source} to {target} -- no such conversion exists")


def check_literal_overflow(value_node, declared: LType, context: str):
    if declared.kind == "int" and literal_kind(value_node) == "int":
        lo, hi = int_range(declared.width)
        val = value_node.value
        if not (lo <= val <= hi):
            raise RCRError(
                f"{context}: literal {val} does not fit {declared} "
                f"(valid range {lo}..{hi}) -- V1_SPEC 0.6.5")


class TypeChecker:
    def __init__(self):
        # Tracks, per name, whether it currently holds an actual
        # assigned value (True) or only a type with no value yet
        # (False) -- e.g. right after "i: int[8]" with no "= ...".
        # This is what 0.6.10's definite-assignment check reads.
        self.has_value = {}

    def check_expr_names(self, node, scope, context):
        """Recursively verifies every Name *load* in this expression
        exists in scope AND currently holds an assigned value --
        a declared-but-unassigned name (0.6.10) is rejected here."""
        if isinstance(node, ast.Name):
            if node.id not in scope:
                raise RCRError(
                    f"{context}: '{node.id}' is not definitely assigned here "
                    f"(V1_SPEC 0.6.10 -- may be unassigned on some path, or never declared)")
            if not self.has_value.get(node.id, False):
                raise RCRError(
                    f"{context}: '{node.id}' was declared with a type but never assigned "
                    f"a value (V1_SPEC 0.6.10) -- 'i: int[8]' alone does not give i a value")
            return
        if isinstance(node, ast.Constant):
            return
        if isinstance(node, ast.BinOp):
            self.check_expr_names(node.left, scope, context)
            self.check_expr_names(node.right, scope, context)
            return
        if isinstance(node, ast.Compare):
            self.check_expr_names(node.left, scope, context)
            for c in node.comparators:
                self.check_expr_names(c, scope, context)
            return
        if isinstance(node, ast.BoolOp):
            for v in node.values:
                self.check_expr_names(v, scope, context)
            return
        if isinstance(node, ast.UnaryOp):
            self.check_expr_names(node.operand, scope, context)
            return
        if isinstance(node, ast.Call):
            for a in node.args:
                self.check_expr_names(a, scope, context)
            return
        raise RCRError(f"{context}: expression node {type(node).__name__} not supported yet")

    def check_assign_value(self, value_node, declared, scope, context):
        self.check_expr_names(value_node, scope, context)
        if literal_kind(value_node) is not None:
            check_literal_overflow(value_node, declared, context)
        else:
            source_type = infer_expr_type(value_node, scope, context)
            check_assignment_compatible(source_type, declared, context)

    def check_stmt(self, node, scope):
        if isinstance(node, ast.AnnAssign):
            name = node.target.id if isinstance(node.target, ast.Name) else None
            if name is None:
                raise RCRError("only simple name targets are supported for annotations")
            declared = parse_annotation(node.annotation)
            if node.value is not None:
                self.check_assign_value(node.value, declared, scope, f"declaration of '{name}'")
                self.has_value[name] = True
            else:
                self.has_value[name] = False
            scope[name] = declared
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
            self.check_assign_value(node.value, declared, scope, f"re-assignment of '{name}'")
            self.has_value[name] = True
            return

        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            for a in node.value.args:
                self.check_expr_names(a, scope, "call argument")
            return

        if isinstance(node, ast.If):
            self.check_expr_names(node.test, scope, "if condition")

            then_scope = dict(scope)
            for stmt in node.body:
                self.check_stmt(stmt, then_scope)

            else_scope = dict(scope)
            for stmt in node.orelse:
                self.check_stmt(stmt, else_scope)

            merged = {}
            for name in set(then_scope) | set(else_scope):
                if name in then_scope and name in else_scope:
                    if then_scope[name] != else_scope[name]:
                        raise RCRError(
                            f"type of '{name}' disagrees across if/else branches: "
                            f"{then_scope[name]} vs {else_scope[name]} (V1_SPEC 0.6.10)")
                    merged[name] = then_scope[name]

            scope.clear()
            scope.update(merged)
            return

        if isinstance(node, ast.For):
            if not isinstance(node.target, ast.Name):
                raise RCRError("only a single name for-target is supported")
            loop_var = node.target.id

            if loop_var not in scope:
                raise RCRError(
                    f"for-loop variable '{loop_var}' must be declared with a type "
                    f"annotation and a starting value before the loop, e.g. "
                    f"'{loop_var}: int[8] = 0' (V1_SPEC 0.6.1, 0.6.6)")
            declared = scope[loop_var]
            if declared.kind != "int":
                raise RCRError(
                    f"for-loop variable '{loop_var}' must be an int type, got {declared}")

            if not self.has_value.get(loop_var, False):
                raise RCRError(
                    f"for-loop variable '{loop_var}' was declared with a type but never "
                    f"given a starting value -- write '{loop_var}: {declared} = 0' or "
                    f"similar before the loop (V1_SPEC 0.6.10)")

            if not (isinstance(node.iter, ast.Call)
                    and isinstance(node.iter.func, ast.Name)
                    and node.iter.func.id == "range"
                    and len(node.iter.args) == 1):
                raise RCRError("only 'for x in range(N)' is supported in this slice")

            bound_node = node.iter.args[0]
            self.check_expr_names(bound_node, scope, "range() argument")

            if literal_kind(bound_node) == "int":
                n = bound_node.value
                lo, hi = int_range(declared.width)
                max_produced = n - 1
                if max_produced > hi or 0 < lo:
                    raise RCRError(
                        f"for-loop: range({n}) produces values up to {max_produced}, "
                        f"which does not fit {declared} (valid range {lo}..{hi}) "
                        f"-- V1_SPEC 0.6.12")
            # else: bound isn't statically known -- 0.6.12 says this becomes a
            # runtime trap instead of a compile-time rejection (a deliberate,
            # documented gap in this static-only slice).

            self.has_value[loop_var] = True  # range() overwrites it each iteration
            for stmt in node.body:
                self.check_stmt(stmt, scope)
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
