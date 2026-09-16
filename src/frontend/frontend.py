#!/usr/bin/env python3
"""
Lithon frontend, first slice: Python source -> Lithon IR text.

Uses Python's own `ast` module as a shortcut -- not a permanent
architecture choice, replaced by a native Lithon parser at M10.

Scope of this slice, deliberately narrow: top-level statements only,
treated as the body of an implicit `main` function in one basic
block. Supports: assignment, int/float literals, +/-/*//, print().
No control flow, no functions, no annotations yet.
"""
import ast
import sys


class IRBuilder:
    def __init__(self):
        self.lines = []
        self.reg_counter = 0

    def new_reg(self):
        r = self.reg_counter
        self.reg_counter += 1
        return f"%{r}"

    def emit(self, line):
        self.lines.append(f"    {line}")

    def build_expr(self, node):
        if isinstance(node, ast.Constant) and isinstance(node.value, bool):
            # bool is a subclass of int in Python -- must check before int.
            raise NotImplementedError("bool literals not supported yet")

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

        raise NotImplementedError(f"expression node {type(node).__name__} not supported yet")

    def build_stmt(self, node):
        if isinstance(node, ast.Assign):
            if len(node.targets) != 1 or not isinstance(node.targets[0], ast.Name):
                raise NotImplementedError("only single-name assignment targets are supported")
            value_reg = self.build_expr(node.value)
            name = node.targets[0].id
            self.emit(f"store {name}, {value_reg}")
            return

        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call):
            call = node.value
            if not isinstance(call.func, ast.Name) or call.func.id != "print":
                raise NotImplementedError("only print() calls are supported")
            if len(call.args) != 1:
                raise NotImplementedError("print() with exactly one argument is supported")
            arg_reg = self.build_expr(call.args[0])
            self.emit(f"call print, {arg_reg}")
            return

        raise NotImplementedError(f"statement node {type(node).__name__} not supported yet")

    def build_module(self, tree):
        self.lines.append("function main:")
        self.lines.append("block0:")
        for stmt in tree.body:
            self.build_stmt(stmt)
        self.emit("return")
        return "\n".join(self.lines) + "\n"


def main():
    if len(sys.argv) != 2:
        print("usage: frontend.py <source.py>", file=sys.stderr)
        return 1

    with open(sys.argv[1], "r") as f:
        source = f.read()

    tree = ast.parse(source)
    builder = IRBuilder()
    ir_text = builder.build_module(tree)
    sys.stdout.write(ir_text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
