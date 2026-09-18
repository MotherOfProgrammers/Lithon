#!/usr/bin/env python3
"""
Lithon frontend: Python source -> Lithon IR text.

Uses Python's own `ast` module as a shortcut -- not a permanent
architecture choice, replaced by a native Lithon parser at M10.

Two modes, both supported by the same builder:
  - Untyped input (bare "x = 10", no annotations): emits IR exactly
    as before, with no type fields. Existing tests/programs/*.py
    (the M1/M2 regression suite) use this path unchanged.
  - Typed input (AnnAssign "x: int[8] = 10", typed function
    signatures): emits IR carrying type_kind/type_width per V1_SPEC
    0.6, consumed by the type-checker before interpretation.

Supports: assignment (typed and untyped), int/float/bool literals,
+/-/*//, comparisons (single, non-chained), and/or (exactly two
operands), not, print(), if/elif/else, while, for ... in range(...),
function calls, return.
"""
import ast
import sys


class Block:
    def __init__(self, label):
        self.label = label
        self.lines = []


def render_type_suffix(kind, width):
    if not kind:
        return ""
    if width is None or width == -1:
        return f" : {kind}"
    return f" : {kind}[{width}]"


def parse_type_annotation(node):
    """Returns (kind, width) from an annotation AST node, or raises
    NotImplementedError for forms this frontend slice doesn't handle.
    Mirrors tools/typecheck.py's parse_annotation, kept independent
    since this is a separate scaffolding tool (frontend vs checker)."""
    if isinstance(node, ast.Name):
        if node.id == "bool":
            return "bool", -1
        raise NotImplementedError(f"type '{node.id}' requires an explicit size, e.g. {node.id}[64]")

    if isinstance(node, ast.Subscript):
        if not isinstance(node.value, ast.Name):
            raise NotImplementedError("unsupported type annotation form")
        base = node.value.id
        if base not in ("int", "float", "str"):
            raise NotImplementedError(f"unknown type '{base}'")
        size_node = node.slice
        if not isinstance(size_node, ast.Constant) or not isinstance(size_node.value, int):
            raise NotImplementedError(f"{base}[N] requires a literal integer size")
        return base, size_node.value

    raise NotImplementedError("unsupported type annotation form")


class IRBuilder:
    def __init__(self):
        self.reg_counter = 0
        self.block_counter = 0
        self.blocks = []
        self.current = None

    def new_reg(self):
        r = self.reg_counter
        self.reg_counter += 1
        return f"%{r}"

    def reserve_label(self):
        label = f"block{self.block_counter}"
        self.block_counter += 1
        return label

    def start_block(self, label):
        b = Block(label)
        self.blocks.append(b)
        self.current = b
        return b

    def emit(self, line):
        self.current.lines.append(f"    {line}")

    def build_expr(self, node):
        if isinstance(node, ast.Constant) and isinstance(node.value, bool):
            r = self.new_reg()
            self.emit(f"{r} = const_bool {1 if node.value else 0}")
            return r

        if isinstance(node, ast.Constant) and isinstance(node.value, float):
            r = self.new_reg()
            self.emit(f"{r} = const_f64 {node.value}")
            return r

        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            r = self.new_reg()
            self.emit(f"{r} = const_i64 {node.value}")
            return r

        if isinstance(node, ast.Name):
            r = self.new_reg()
            self.emit(f"{r} = load {node.id}")
            return r

        if isinstance(node, ast.BinOp):
            left = self.build_expr(node.left)
            right = self.build_expr(node.right)
            op_map = {ast.Add: "add", ast.Sub: "sub", ast.Mult: "mul", ast.Div: "div"}
            op_type = type(node.op)
            if op_type not in op_map:
                raise NotImplementedError(f"operator {op_type.__name__} not supported yet")
            r = self.new_reg()
            self.emit(f"{r} = {op_map[op_type]} {left}, {right}")
            return r

        if isinstance(node, ast.Compare):
            if len(node.ops) != 1 or len(node.comparators) != 1:
                raise NotImplementedError("chained comparisons (a < b < c) not supported yet")
            left = self.build_expr(node.left)
            right = self.build_expr(node.comparators[0])
            op_map = {ast.Lt: "lt", ast.Gt: "gt", ast.Eq: "eq"}
            op_type = type(node.ops[0])
            if op_type not in op_map:
                raise NotImplementedError(f"comparison {op_type.__name__} not supported yet")
            r = self.new_reg()
            self.emit(f"{r} = {op_map[op_type]} {left}, {right}")
            return r

        if isinstance(node, ast.BoolOp):
            if len(node.values) != 2:
                raise NotImplementedError("and/or with exactly two operands supported for now")
            left = self.build_expr(node.values[0])
            right = self.build_expr(node.values[1])
            op_name = "and" if isinstance(node.op, ast.And) else "or"
            r = self.new_reg()
            self.emit(f"{r} = {op_name} {left}, {right}")
            return r

        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            operand = self.build_expr(node.operand)
            r = self.new_reg()
            self.emit(f"{r} = not {operand}")
            return r

        if isinstance(node, ast.Call):
            if not isinstance(node.func, ast.Name):
                raise NotImplementedError("only direct name calls are supported")
            if node.func.id == "print":
                raise NotImplementedError("print() is a statement in this slice, not an expression")
            arg_regs = [self.build_expr(a) for a in node.args]
            r = self.new_reg()
            if arg_regs:
                self.emit(f"{r} = call {node.func.id}, {', '.join(arg_regs)}")
            else:
                self.emit(f"{r} = call {node.func.id}")
            return r

        raise NotImplementedError(f"expression node {type(node).__name__} not supported yet")

    def build_if(self, node):
        cond_reg = self.build_expr(node.test)
        branch_block = self.current

        then_label = self.reserve_label()
        else_label = self.reserve_label() if node.orelse else None
        merge_label = self.reserve_label()

        target_else = else_label if else_label else merge_label
        branch_block.lines.append(f"    branch {cond_reg}, {then_label}, {target_else}")

        self.start_block(then_label)
        for stmt in node.body:
            self.build_stmt(stmt)
        self.emit(f"jump {merge_label}")

        if node.orelse:
            self.start_block(else_label)
            for stmt in node.orelse:
                self.build_stmt(stmt)
            self.emit(f"jump {merge_label}")

        self.start_block(merge_label)

    def build_while(self, node):
        header_label = self.reserve_label()
        body_label = self.reserve_label()
        exit_label = self.reserve_label()

        self.emit(f"jump {header_label}")

        self.start_block(header_label)
        cond_reg = self.build_expr(node.test)
        self.emit(f"branch {cond_reg}, {body_label}, {exit_label}")

        self.start_block(body_label)
        for stmt in node.body:
            self.build_stmt(stmt)
        self.emit(f"jump {header_label}")

        self.start_block(exit_label)

    def build_for(self, node):
        if not isinstance(node.target, ast.Name):
            raise NotImplementedError("only a single name for-target is supported")
        if not (isinstance(node.iter, ast.Call)
                and isinstance(node.iter.func, ast.Name)
                and node.iter.func.id == "range"
                and len(node.iter.args) == 1):
            raise NotImplementedError("only for x in range(N) is supported in this slice")

        loop_var = node.target.id
        limit_reg = self.build_expr(node.iter.args[0])

        zero_reg = self.new_reg()
        self.emit(f"{zero_reg} = const_i64 0")
        self.emit(f"store {loop_var}, {zero_reg}")

        header_label = self.reserve_label()
        body_label = self.reserve_label()
        exit_label = self.reserve_label()

        self.emit(f"jump {header_label}")

        self.start_block(header_label)
        i_reg = self.new_reg()
        self.emit(f"{i_reg} = load {loop_var}")
        cond_reg = self.new_reg()
        self.emit(f"{cond_reg} = lt {i_reg}, {limit_reg}")
        self.emit(f"branch {cond_reg}, {body_label}, {exit_label}")

        self.start_block(body_label)
        for stmt in node.body:
            self.build_stmt(stmt)
        i_reg2 = self.new_reg()
        self.emit(f"{i_reg2} = load {loop_var}")
        one_reg = self.new_reg()
        self.emit(f"{one_reg} = const_i64 1")
        inc_reg = self.new_reg()
        self.emit(f"{inc_reg} = add {i_reg2}, {one_reg}")
        self.emit(f"store {loop_var}, {inc_reg}")
        self.emit(f"jump {header_label}")

        self.start_block(exit_label)

    def build_stmt(self, node):
        if isinstance(node, ast.AnnAssign):
            if not isinstance(node.target, ast.Name):
                raise NotImplementedError("only simple name targets are supported for annotations")
            name = node.target.id
            kind, width = parse_type_annotation(node.annotation)
            if node.value is not None:
                value_reg = self.build_expr(node.value)
                suffix = render_type_suffix(kind, width)
                self.emit(f"store {name}, {value_reg}{suffix}")
            # annotation-only (no value) emits nothing -- the checker
            # tracks the type from the annotation itself; nothing to
            # store at runtime until a real value is assigned.
            return

        if isinstance(node, ast.Assign):
            if len(node.targets) != 1 or not isinstance(node.targets[0], ast.Name):
                raise NotImplementedError("only single-name assignment targets are supported")
            value_reg = self.build_expr(node.value)
            name = node.targets[0].id
            self.emit(f"store {name}, {value_reg}")
            return

        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            call = node.value
            if isinstance(call.func, ast.Name) and call.func.id == "print":
                if len(call.args) != 1:
                    raise NotImplementedError("print() with exactly one argument is supported")
                arg_reg = self.build_expr(call.args[0])
                self.emit(f"call print, {arg_reg}")
                return
            self.build_expr(call)
            return

        if isinstance(node, ast.If):
            self.build_if(node)
            return

        if isinstance(node, ast.While):
            self.build_while(node)
            return

        if isinstance(node, ast.For):
            self.build_for(node)
            return

        if isinstance(node, ast.Return):
            if node.value is not None:
                val_reg = self.build_expr(node.value)
                self.emit(f"return {val_reg}")
            else:
                self.emit("return")
            return

        raise NotImplementedError(f"statement node {type(node).__name__} not supported yet")

    def render(self, header_line):
        out = [header_line]
        for b in self.blocks:
            out.append(f"{b.label}:")
            out.extend(b.lines)
        return "\n".join(out)


def build_program(tree):
    module_parts = []
    main_builder = IRBuilder()
    main_builder.start_block(main_builder.reserve_label())

    for stmt in tree.body:
        if isinstance(stmt, ast.FunctionDef):
            fb = IRBuilder()
            fb.start_block(fb.reserve_label())

            param_strs = []
            for arg in stmt.args.args:
                if arg.annotation is not None:
                    kind, width = parse_type_annotation(arg.annotation)
                    suffix = render_type_suffix(kind, width).replace(" : ", ":")
                    param_strs.append(f"{arg.arg}{suffix}")
                else:
                    param_strs.append(arg.arg)

            return_suffix = ""
            if stmt.returns is not None:
                kind, width = parse_type_annotation(stmt.returns)
                return_suffix = f" -> {kind}" + (f"[{width}]" if width not in (None, -1) else "")

            header = f"function {stmt.name}({', '.join(param_strs)}){return_suffix}:"

            for s in stmt.body:
                fb.build_stmt(s)
            fb.emit("return")
            module_parts.append(fb.render(header))
        else:
            main_builder.build_stmt(stmt)

    main_builder.emit("return")
    module_parts.append(main_builder.render("function main():"))

    return "\n\n".join(module_parts) + "\n"


def main():
    if len(sys.argv) != 2:
        print("usage: frontend.py <source.py>", file=sys.stderr)
        return 1

    with open(sys.argv[1], "r") as f:
        source = f.read()

    tree = ast.parse(source)
    ir_text = build_program(tree)
    sys.stdout.write(ir_text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
