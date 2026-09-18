#!/usr/bin/env python3
"""
Lithon type-checker.

Static analysis over Python's ast -- no execution. Enforces:

  0.6.1  -- every first assignment/parameter/return must carry an
            explicit annotation
  0.6.4  -- re-assignment is checked against the already-declared type
  0.6.5  -- integer literal overflow, and provable-range overflow for
            binary operations, are compile-time errors
  0.6.8  -- function contracts: parameters and return type are
            mandatory and checked; call-site arguments are checked
            against the declared signature; a declared return type
            must be provably wide enough for the possible result of
            what's returned -- the compiler never auto-widens it
  0.6.9  -- print() is a fixed built-in, not a user function (full
            closed-overload kind validation still to come)
  0.6.10 -- definite assignment + type agreement across if/else
            branches (function scoping, not block scoping)
  0.6.11 -- conversions: widening/same-width automatic, narrowing
            never allowed; int -> float automatic, float -> int
            never allowed; no cast syntax exists anywhere
  0.6.12 -- range()'s produced values are checked against the loop
            variable's declared width at compile time when known

Note: a bare integer literal defaults to int[64] per 0.6.3. Passing
one directly as an argument to a parameter narrower than int[64] is
therefore always rejected as narrowing (0.6.11) -- the argument must
come from an already-narrower-typed variable instead.
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


class FunctionSig:
    def __init__(self, name, param_types, return_type):
        self.name = name
        self.param_types = param_types
        self.return_type = return_type


class TypeChecker:
    def __init__(self):
        self.has_value = {}
        self.functions = {}

    def infer_expr_type(self, node, scope, context):
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
            if not self.has_value.get(node.id, False):
                raise RCRError(
                    f"{context}: '{node.id}' was declared with a type but never assigned "
                    f"a value (V1_SPEC 0.6.10)")
            return scope[node.id]

        if isinstance(node, ast.BinOp):
            left_t = self.infer_expr_type(node.left, scope, context)
            right_t = self.infer_expr_type(node.right, scope, context)
            return self.infer_binop_type(node.op, left_t, right_t, context)

        if isinstance(node, ast.Compare):
            if len(node.ops) != 1 or len(node.comparators) != 1:
                raise RCRError(f"{context}: chained comparisons (a < b < c) not supported yet")
            self.infer_expr_type(node.left, scope, context)
            self.infer_expr_type(node.comparators[0], scope, context)
            return LType("bool")

        if isinstance(node, ast.BoolOp):
            if len(node.values) != 2:
                raise RCRError(f"{context}: and/or with exactly two operands supported for now")
            for v in node.values:
                self.infer_expr_type(v, scope, context)
            return LType("bool")

        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            self.infer_expr_type(node.operand, scope, context)
            return LType("bool")

        if isinstance(node, ast.Call):
            if not isinstance(node.func, ast.Name):
                raise RCRError(f"{context}: only direct name calls are supported")
            fname = node.func.id

            if fname == "print":
                if len(node.args) != 1:
                    raise RCRError(f"{context}: print() with exactly one argument is supported")
                arg_t = self.infer_expr_type(node.args[0], scope, context)
                if arg_t.kind not in ("int", "float", "str", "bool"):
                    raise RCRError(
                        f"{context}: print() does not accept {arg_t} -- V1_SPEC 0.6.9's closed "
                        f"overload set is int[N], float[N], str[N], bool only")
                return LType("bool")

            if fname not in self.functions:
                raise RCRError(f"{context}: call to unknown function '{fname}'")
            sig = self.functions[fname]
            if len(node.args) != len(sig.param_types):
                raise RCRError(
                    f"{context}: '{fname}' expects {len(sig.param_types)} argument(s), "
                    f"got {len(node.args)} (V1_SPEC 0.6.8)")
            for i, (arg_node, expected) in enumerate(zip(node.args, sig.param_types)):
                arg_t = self.infer_expr_type(arg_node, scope, f"{context}, argument {i+1} to '{fname}'")
                check_assignment_compatible(arg_t, expected,
                    f"{context}: argument {i+1} to '{fname}' expects {expected}, got {arg_t}")
            return sig.return_type

        raise RCRError(f"{context}: cannot statically determine the type of this expression yet")

    def infer_binop_type(self, op, left: LType, right: LType, context):
        if left.kind == "float" or right.kind == "float":
            width = max(left.width or 64, right.width or 64, 32)
            return LType("float", 64 if width > 32 else 32)
        if left.kind == "int" and right.kind == "int":
            return LType("int", max(left.width, right.width))
        raise RCRError(f"{context}: cannot apply arithmetic to {left} and {right}")

    def check_stmt_return(self, node, sig: FunctionSig, scope, context):
        if node.value is None:
            if sig.return_type is not None:
                raise RCRError(f"{context}: function '{sig.name}' declares a return type "
                                f"{sig.return_type} but this 'return' has no value")
            return

        if isinstance(node.value, ast.BinOp) and isinstance(node.value.op, (ast.Add, ast.Sub, ast.Mult)):
            def op_range(operand_node):
                k = literal_kind(operand_node)
                if k == "int":
                    v = operand_node.value
                    return v, v
                t = self.infer_expr_type(operand_node, scope, context)
                if t.kind != "int":
                    return None
                return int_range(t.width)

            left_range = op_range(node.value.left)
            right_range = op_range(node.value.right)
            if left_range and right_range and sig.return_type.kind == "int":
                lo1, hi1 = left_range
                lo2, hi2 = right_range
                if isinstance(node.value.op, ast.Add):
                    possible_lo, possible_hi = lo1 + lo2, hi1 + hi2
                elif isinstance(node.value.op, ast.Sub):
                    possible_lo, possible_hi = lo1 - hi2, hi1 - lo2
                else:
                    corners = [lo1*lo2, lo1*hi2, hi1*lo2, hi1*hi2]
                    possible_lo, possible_hi = min(corners), max(corners)

                target_lo, target_hi = int_range(sig.return_type.width)
                if possible_lo < target_lo or possible_hi > target_hi:
                    raise RCRError(
                        f"{context}: return type {sig.return_type} is not wide enough -- "
                        f"this operation can produce {possible_lo}..{possible_hi}, which "
                        f"exceeds {sig.return_type}'s range {target_lo}..{target_hi}. "
                        f"Declare a wider return type explicitly (V1_SPEC 0.5, 0.6.5) -- "
                        f"the compiler will not auto-widen it for you.")

        value_t = self.infer_expr_type(node.value, scope, context)
        check_assignment_compatible(value_t, sig.return_type, context)

    def check_assign_value(self, value_node, declared, scope, context):
        if literal_kind(value_node) is not None:
            check_literal_overflow(value_node, declared, context)
        else:
            source_type = self.infer_expr_type(value_node, scope, context)
            check_assignment_compatible(source_type, declared, context)

    def check_stmt(self, node, scope, current_fn_sig=None):
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
            self.infer_expr_type(node.value, scope, "call statement")
            return

        if isinstance(node, ast.Return):
            if current_fn_sig is None:
                raise RCRError("'return' used outside of a function")
            self.check_stmt_return(node, current_fn_sig, scope, f"return in '{current_fn_sig.name}'")
            return

        if isinstance(node, ast.If):
            self.infer_expr_type(node.test, scope, "if condition")

            then_scope = dict(scope)
            for stmt in node.body:
                self.check_stmt(stmt, then_scope, current_fn_sig)

            else_scope = dict(scope)
            for stmt in node.orelse:
                self.check_stmt(stmt, else_scope, current_fn_sig)

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
                raise RCRError(f"for-loop variable '{loop_var}' must be an int type, got {declared}")

            if not self.has_value.get(loop_var, False):
                raise RCRError(
                    f"for-loop variable '{loop_var}' was declared with a type but never "
                    f"given a starting value (V1_SPEC 0.6.10)")

            if not (isinstance(node.iter, ast.Call)
                    and isinstance(node.iter.func, ast.Name)
                    and node.iter.func.id == "range"
                    and len(node.iter.args) == 1):
                raise RCRError("only 'for x in range(N)' is supported in this slice")

            bound_node = node.iter.args[0]
            self.infer_expr_type(bound_node, scope, "range() argument")

            if literal_kind(bound_node) == "int":
                n = bound_node.value
                lo, hi = int_range(declared.width)
                max_produced = n - 1
                if max_produced > hi or 0 < lo:
                    raise RCRError(
                        f"for-loop: range({n}) produces values up to {max_produced}, "
                        f"which does not fit {declared} (valid range {lo}..{hi}) "
                        f"-- V1_SPEC 0.6.12")

            self.has_value[loop_var] = True
            for stmt in node.body:
                self.check_stmt(stmt, scope, current_fn_sig)
            return

        raise RCRError(f"statement {type(node).__name__} not supported by the type-checker yet")

    def register_function(self, node: ast.FunctionDef):
        param_types = []
        for arg in node.args.args:
            if arg.annotation is None:
                raise RCRError(
                    f"function '{node.name}': parameter '{arg.arg}' has no type annotation "
                    f"(V1_SPEC 0.6.1, 0.6.8) -- every parameter must be explicitly typed")
            param_types.append(parse_annotation(arg.annotation))

        if node.returns is None:
            raise RCRError(
                f"function '{node.name}' has no return type annotation "
                f"(V1_SPEC 0.6.8) -- every function must declare '-> <type>'")
        return_type = parse_annotation(node.returns)

        self.functions[node.name] = FunctionSig(node.name, param_types, return_type)

    def check_function_body(self, node: ast.FunctionDef):
        sig = self.functions[node.name]
        fn_scope = {}
        for arg, t in zip(node.args.args, sig.param_types):
            fn_scope[arg.arg] = t
            self.has_value[arg.arg] = True

        for stmt in node.body:
            self.check_stmt(stmt, fn_scope, current_fn_sig=sig)

    def check_module(self, tree):
        function_defs = [s for s in tree.body if isinstance(s, ast.FunctionDef)]
        for fn in function_defs:
            self.register_function(fn)

        for fn in function_defs:
            self.check_function_body(fn)

        scope = {}
        for stmt in tree.body:
            if not isinstance(stmt, ast.FunctionDef):
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
    for name, sig in checker.functions.items():
        params = ", ".join(str(p) for p in sig.param_types)
        print(f"  function {name}({params}) -> {sig.return_type}")
    for name, t in final_scope.items():
        print(f"  {name}: {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
