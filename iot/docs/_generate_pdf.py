#!/usr/bin/env python3
"""
Script auxiliar para gerar RELATORIO_PARTE1.pdf a partir do Markdown.
Usa fpdf2 para criar um PDF formatado com o conteúdo do relatório.
"""
import os
import re
import sys

from fpdf import FPDF


class RelatorioPDF(FPDF):
    """PDF customizado para o relatório CardioIA."""

    def __init__(self):
        super().__init__()
        self.set_auto_page_break(auto=True, margin=20)

    def header(self):
        self.set_font("Helvetica", "I", 8)
        self.cell(0, 5, "CardioIA - Fase 3 - Relatorio Parte 1 (Edge Computing)", align="C")
        self.ln(8)

    def footer(self):
        self.set_y(-15)
        self.set_font("Helvetica", "I", 8)
        self.cell(0, 10, f"Pagina {self.page_no()}/{{nb}}", align="C")

    def chapter_title(self, title, level=1):
        if level == 1:
            self.set_font("Helvetica", "B", 14)
            self.ln(4)
        elif level == 2:
            self.set_font("Helvetica", "B", 12)
            self.ln(3)
        else:
            self.set_font("Helvetica", "B", 10)
            self.ln(2)
        self.multi_cell(0, 6, title)
        self.ln(2)

    def body_text(self, text):
        self.set_font("Helvetica", "", 10)
        self.multi_cell(0, 5, text)
        self.ln(2)

    def code_block(self, text):
        self.set_font("Courier", "", 8)
        self.set_fill_color(240, 240, 240)
        for line in text.split("\n"):
            self.cell(0, 4, line, new_x="LMARGIN", new_y="NEXT", fill=True)
        self.ln(3)

    def bold_text(self, text):
        self.set_font("Helvetica", "B", 10)
        self.multi_cell(0, 5, text)
        self.set_font("Helvetica", "", 10)
        self.ln(1)

    def table_row(self, cells, header=False):
        if header:
            self.set_font("Helvetica", "B", 9)
        else:
            self.set_font("Helvetica", "", 9)
        col_width = (self.w - 20) / len(cells)
        for cell in cells:
            self.cell(col_width, 5, sanitize(cell), border=1, align="C")
        self.ln()


def sanitize(text):
    """Remove caracteres que fpdf2 nao consegue renderizar com fontes built-in."""
    replacements = {
        "\u2014": "-",   # em dash
        "\u2013": "-",   # en dash
        "\u2018": "'",
        "\u2019": "'",
        "\u201c": '"',
        "\u201d": '"',
        "\u2026": "...",
        "\u2022": "*",
        "\u00b0": "o",   # degree sign -> o
        "\u2264": "<=",
        "\u2265": ">=",
        "\u00d7": "x",
        "\u2248": "~",
        "\u2192": "->",
        "\u00e9": "e",
        "\u00e3": "a",
        "\u00e7": "c",
        "\u00e1": "a",
        "\u00ed": "i",
        "\u00f3": "o",
        "\u00fa": "u",
        "\u00ea": "e",
        "\u00f4": "o",
        "\u00e2": "a",
        "\u00f5": "o",
        "\u00e0": "a",
        "\u00fc": "u",
        "\u00c9": "E",
        "\u00c3": "A",
        "\u00c7": "C",
        "\u00c1": "A",
        "\u00cd": "I",
        "\u00d3": "O",
        "\u00da": "U",
        "\u00ca": "E",
        "\u00d4": "O",
        "\u00c2": "A",
        "\u00d5": "O",
        "\u00c0": "A",
    }
    for k, v in replacements.items():
        text = text.replace(k, v)
    # Remove any remaining non-latin1 characters
    return text.encode("latin-1", errors="replace").decode("latin-1")


def parse_md_and_generate_pdf(md_path, pdf_path):
    """Parse the markdown file and generate a formatted PDF."""
    with open(md_path, "r", encoding="utf-8") as f:
        content = f.read()

    pdf = RelatorioPDF()
    pdf.alias_nb_pages()
    pdf.add_page()

    lines = content.split("\n")
    i = 0
    in_code_block = False
    code_buffer = []

    while i < len(lines):
        line = lines[i]

        # Code block start/end
        if line.strip().startswith("```"):
            if in_code_block:
                pdf.code_block(sanitize("\n".join(code_buffer)))
                code_buffer = []
                in_code_block = False
            else:
                in_code_block = True
            i += 1
            continue

        if in_code_block:
            code_buffer.append(line)
            i += 1
            continue

        # Headers
        if line.startswith("# "):
            pdf.chapter_title(sanitize(line[2:].strip()), level=1)
        elif line.startswith("## "):
            pdf.chapter_title(sanitize(line[3:].strip()), level=2)
        elif line.startswith("### "):
            pdf.chapter_title(sanitize(line[4:].strip()), level=3)
        # Horizontal rule
        elif line.strip() == "---":
            pdf.ln(3)
            pdf.set_draw_color(200, 200, 200)
            pdf.line(10, pdf.get_y(), pdf.w - 10, pdf.get_y())
            pdf.ln(3)
        # Table rows
        elif "|" in line and not line.strip().startswith("|--"):
            cells = [c.strip() for c in line.split("|") if c.strip()]
            if cells and not all(c.replace("-", "").replace(":", "") == "" for c in cells):
                is_header = i + 1 < len(lines) and "---" in lines[i + 1]
                pdf.table_row(cells[:4], header=is_header)  # limit columns
        # Table separator (skip)
        elif line.strip().startswith("|--") or ("|" in line and all(
            c.strip().replace("-", "").replace(":", "") == ""
            for c in line.split("|") if c.strip()
        )):
            pass
        # Bold lines
        elif line.strip().startswith("**") and line.strip().endswith("**"):
            pdf.bold_text(sanitize(line.strip().strip("*")))
        # Empty line
        elif line.strip() == "":
            pdf.ln(2)
        # Italic/figure caption
        elif line.strip().startswith("*") and line.strip().endswith("*"):
            pdf.set_font("Helvetica", "I", 9)
            pdf.multi_cell(0, 4, sanitize(line.strip().strip("*")))
            pdf.set_font("Helvetica", "", 10)
            pdf.ln(2)
        # Regular text (including list items)
        else:
            text = sanitize(line.strip())
            if text.startswith("- "):
                text = "  * " + text[2:]
            elif re.match(r"^\d+\.", text):
                text = "  " + text
            pdf.body_text(text)

        i += 1

    pdf.output(pdf_path)
    print(f"PDF gerado com sucesso: {pdf_path}")
    print(f"Paginas: {pdf.page_no()}")


if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    md_file = os.path.join(script_dir, "RELATORIO_PARTE1.md")
    pdf_file = os.path.join(script_dir, "RELATORIO_PARTE1.pdf")
    parse_md_and_generate_pdf(md_file, pdf_file)
